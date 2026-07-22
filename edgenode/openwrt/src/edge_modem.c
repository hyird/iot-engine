#include "edge_modem.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <uci.h>

#include "edge_protocol.h"

#define EDGE_MODEM_RESPONSE_CAPACITY 2048U
#define EDGE_MODEM_TIMEOUT_MS 5000

static volatile sig_atomic_t monitor_stop;

static int64_t monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static bool write_all(int fd, const char *data, size_t size) {
    while (size != 0U) {
        const ssize_t written = write(fd, data, size);
        if (written > 0) {
            data += (size_t)written;
            size -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static bool configure_port(int fd, struct termios *original) {
    if (tcgetattr(fd, original) != 0)
        return false;
    struct termios value = *original;
    value.c_iflag = IGNPAR;
    value.c_oflag = 0;
    value.c_lflag = 0;
    value.c_cflag &= (tcflag_t)~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    value.c_cflag &= (tcflag_t)~CRTSCTS;
#endif
    value.c_cflag |= CS8 | CREAD | CLOCAL;
    value.c_cc[VMIN] = 0;
    value.c_cc[VTIME] = 0;
    if (cfsetispeed(&value, B115200) != 0 || cfsetospeed(&value, B115200) != 0)
        return false;
    if (tcsetattr(fd, TCSANOW, &value) != 0)
        return false;
    tcflush(fd, TCIOFLUSH);
    return true;
}

static bool copy_digit_token(const char *start, size_t minimum, size_t maximum,
                             char *output, size_t capacity) {
    while (*start == ' ' || *start == '\t')
        ++start;
    size_t length = 0U;
    while (start[length] >= '0' && start[length] <= '9')
        ++length;
    if (length < minimum || length > maximum || length + 1U > capacity)
        return false;
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool parse_imei(const char *response, char output[16]) {
    for (const char *cursor = response; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9' ||
            (cursor != response && cursor[-1] >= '0' && cursor[-1] <= '9'))
            continue;
        size_t length = 0U;
        while (cursor[length] >= '0' && cursor[length] <= '9')
            ++length;
        if (length == 15U && !(cursor[length] >= '0' && cursor[length] <= '9')) {
            memcpy(output, cursor, 15U);
            output[15] = '\0';
            if (edge_protocol_validate_imei(output))
                return true;
            output[0] = '\0';
        }
        cursor += length != 0U ? length - 1U : 0U;
    }
    return false;
}

static void parse_response(const char *response, edge_modem_info *info) {
    parse_imei(response, info->imei);

    const char *iccid = strstr(response, "+QCCID:");
    if (iccid != NULL)
        copy_digit_token(iccid + strlen("+QCCID:"), 18U, 22U,
                         info->iccid, sizeof(info->iccid));

    const char *registration = strstr(response, "+CEREG:");
    int mode = 0;
    int status = -1;
    if (registration != NULL && sscanf(registration, "+CEREG: %d,%d", &mode, &status) == 2) {
        (void)mode;
        info->registration_status = status;
        info->registered = status == 1 || status == 5;
    }

    const char *signal = strstr(response, "+CSQ:");
    int csq = 99;
    int error_rate = 99;
    if (signal != NULL && sscanf(signal, "+CSQ: %d,%d", &csq, &error_rate) == 2) {
        (void)error_rate;
        if ((csq >= 0 && csq <= 31) || csq == 99) {
            info->csq = csq;
            if (csq <= 31) {
                info->rssi_dbm = -113 + 2 * csq;
                info->signal_percent = (unsigned)(csq * 100 + 15) / 31U;
            }
        }
    }
}

bool edge_modem_probe(const char *port, edge_modem_info *info) {
    if (port == NULL || info == NULL)
        return false;
    memset(info, 0, sizeof(*info));
    info->registration_status = -1;
    info->csq = 99;
    info->rssi_dbm = -1;

    const int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct termios original;
    if (!configure_port(fd, &original)) {
        close(fd);
        return false;
    }

    static const char commands[] = "ATE0\rAT+CEREG?\rAT+GSN\rAT+QCCID\rAT+CSQ\r";
    if (!write_all(fd, commands, sizeof(commands) - 1U)) {
        tcsetattr(fd, TCSANOW, &original);
        close(fd);
        return false;
    }

    char response[EDGE_MODEM_RESPONSE_CAPACITY];
    size_t used = 0U;
    const int64_t deadline = monotonic_milliseconds() + EDGE_MODEM_TIMEOUT_MS;
    while (used + 1U < sizeof(response)) {
        const int64_t remaining = deadline - monotonic_milliseconds();
        if (remaining <= 0)
            break;
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        const int ready = poll(&poll_fd, 1, (int)remaining);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;
        const ssize_t count = read(fd, response + used, sizeof(response) - used - 1U);
        if (count > 0) {
            used += (size_t)count;
            response[used] = '\0';
            if (strstr(response, "+CEREG:") != NULL && strstr(response, "+QCCID:") != NULL &&
                strstr(response, "+CSQ:") != NULL && parse_imei(response, info->imei))
                break;
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        break;
    }
    response[used] = '\0';
    parse_response(response, info);
    tcsetattr(fd, TCSANOW, &original);
    close(fd);
    return info->imei[0] != '\0' || info->iccid[0] != '\0' || info->csq != 99 ||
           info->registration_status >= 0;
}

static struct uci_section *lookup_section(struct uci_context *context,
                                          struct uci_package *package,
                                          const char *name) {
    struct uci_element *element;
    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);
        if (strcmp(section->e.name, name) == 0)
            return section;
    }
    (void)context;
    return NULL;
}

static bool set_option_if_changed(struct uci_context *context, struct uci_package *package,
                                  struct uci_section *section, const char *name,
                                  const char *value, bool *changed) {
    const char *current = uci_lookup_option_string(context, section, name);
    if (current != NULL && strcmp(current, value) == 0)
        return true;
    struct uci_ptr pointer = {
        .p = package,
        .s = section,
        .option = name,
        .value = value,
    };
    if (uci_set(context, &pointer) != UCI_OK)
        return false;
    *changed = true;
    return true;
}

bool edge_modem_save_identity(const edge_modem_info *info) {
    if (info == NULL || !edge_protocol_validate_imei(info->imei))
        return false;
    struct uci_context *context = uci_alloc_context();
    struct uci_package *package = NULL;
    if (context == NULL || uci_load(context, "edgenode", &package) != UCI_OK) {
        if (context != NULL)
            uci_free_context(context);
        return false;
    }
    struct uci_section *node = lookup_section(context, package, "node");
    struct uci_section *modem = lookup_section(context, package, "modem");
    bool changed = false;
    bool success = node != NULL && modem != NULL &&
                   set_option_if_changed(context, package, node, "imei", info->imei, &changed) &&
                   set_option_if_changed(context, package, modem, "imei", info->imei, &changed);
    if (success && info->iccid[0] != '\0')
        success = set_option_if_changed(context, package, modem, "iccid", info->iccid, &changed);
    if (success && changed)
        success = uci_save(context, package) == UCI_OK &&
                  uci_commit(context, &package, false) == UCI_OK;
    if (package != NULL)
        uci_unload(context, package);
    uci_free_context(context);
    return success;
}

bool edge_modem_write_status(const char *path, const edge_modem_info *info, bool available) {
    if (path == NULL || info == NULL)
        return false;
    if (mkdir("/tmp/edgenode", 0700) != 0 && errno != EEXIST)
        return false;
    char temporary[256];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) >=
        (int)sizeof(temporary))
        return false;
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    FILE *file = fdopen(fd, "w");
    if (file == NULL) {
        close(fd);
        unlink(temporary);
        return false;
    }
    const int result = fprintf(file,
                               "available=%u\nregistered=%u\nregistration_status=%d\n"
                               "imei=%s\niccid=%s\ncsq=%d\nrssi_dbm=%d\n"
                               "signal_percent=%u\nupdated_at=%lld\n",
                               available ? 1U : 0U, info->registered ? 1U : 0U,
                               info->registration_status, info->imei, info->iccid, info->csq,
                               info->rssi_dbm, info->signal_percent, (long long)time(NULL));
    const bool success = result > 0 && fclose(file) == 0 && rename(temporary, path) == 0;
    if (!success)
        unlink(temporary);
    return success;
}

bool edge_modem_read_status(const char *path, edge_modem_info *info, bool *available) {
    if (path == NULL || info == NULL || available == NULL)
        return false;
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return false;
    memset(info, 0, sizeof(*info));
    info->registration_status = -1;
    info->csq = 99;
    info->rssi_dbm = -1;
    *available = false;
    char line[128];
    while (fgets(line, sizeof(line), file) != NULL) {
        char *newline = strpbrk(line, "\r\n");
        if (newline != NULL)
            *newline = '\0';
        char *separator = strchr(line, '=');
        if (separator == NULL)
            continue;
        *separator++ = '\0';
        if (strcmp(line, "available") == 0)
            *available = strcmp(separator, "1") == 0;
        else if (strcmp(line, "registered") == 0)
            info->registered = strcmp(separator, "1") == 0;
        else if (strcmp(line, "registration_status") == 0)
            info->registration_status = atoi(separator);
        else if (strcmp(line, "imei") == 0 && strlen(separator) < sizeof(info->imei))
            memcpy(info->imei, separator, strlen(separator) + 1U);
        else if (strcmp(line, "iccid") == 0 && strlen(separator) < sizeof(info->iccid))
            memcpy(info->iccid, separator, strlen(separator) + 1U);
        else if (strcmp(line, "csq") == 0)
            info->csq = atoi(separator);
        else if (strcmp(line, "rssi_dbm") == 0)
            info->rssi_dbm = atoi(separator);
        else if (strcmp(line, "signal_percent") == 0)
            info->signal_percent = (unsigned)strtoul(separator, NULL, 10);
    }
    const bool success = !ferror(file);
    fclose(file);
    return success;
}

int edge_modem_initialize(const char *port, const char *status_path) {
    edge_modem_info info;
    const bool available = edge_modem_probe(port, &info);
    edge_modem_write_status(status_path, &info, available);
    if (!available || !edge_protocol_validate_imei(info.imei))
        return EXIT_FAILURE;
    return edge_modem_save_identity(&info) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void stop_monitor(int signal_number) {
    (void)signal_number;
    monitor_stop = 1;
}

int edge_modem_monitor(const char *port, const char *status_path, unsigned interval_sec) {
    monitor_stop = 0;
    signal(SIGTERM, stop_monitor);
    signal(SIGINT, stop_monitor);
    signal(SIGHUP, stop_monitor);
    if (interval_sec == 0U)
        interval_sec = 30U;

    while (!monitor_stop) {
        edge_modem_info info;
        const bool available = edge_modem_probe(port, &info);
        if (available && edge_protocol_validate_imei(info.imei))
            edge_modem_save_identity(&info);
        edge_modem_write_status(status_path, &info, available);
        for (unsigned elapsed = 0U; elapsed < interval_sec && !monitor_stop; ++elapsed)
            sleep(1U);
    }
    return EXIT_SUCCESS;
}
