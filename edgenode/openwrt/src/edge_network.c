#include "edge_network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "edge_process.h"

#define NETWORK_BACKUP "/tmp/edgenode/network.backup"
#define NETWORK_PENDING "/tmp/edgenode/network.pending"
#define NETWORK_CONFIRMED "/tmp/edgenode/network.confirmed"

static void restore_network(void);

static void set_error(char *output, size_t capacity, const char *message) {
    if (output != NULL && capacity != 0U)
        snprintf(output, capacity, "%s", message != NULL ? message : "network error");
}

static bool parse_ipv4(const char *text, uint32_t *value) {
    struct in_addr address;
    if (text == NULL || inet_pton(AF_INET, text, &address) != 1)
        return false;
    *value = ntohl(address.s_addr);
    return true;
}

bool edge_network_validate_static(const char *ip, uint32_t prefix_length,
                                  const char *gateway, char *error, size_t error_size) {
    uint32_t address = 0U;
    if (!parse_ipv4(ip, &address)) {
        set_error(error, error_size, "br-lan IPv4 address is invalid");
        return false;
    }
    if (prefix_length == 0U || prefix_length > 30U) {
        set_error(error, error_size, "br-lan netmask prefix must be between 1 and 30");
        return false;
    }
    const uint32_t mask = 0xFFFFFFFFU << (32U - prefix_length);
    const uint32_t host = address & ~mask;
    if (host == 0U || host == ~mask) {
        set_error(error, error_size, "br-lan address cannot be the network or broadcast address");
        return false;
    }
    if (gateway != NULL && gateway[0] != '\0') {
        uint32_t gateway_value = 0U;
        if (!parse_ipv4(gateway, &gateway_value) ||
            (gateway_value & mask) != (address & mask) || gateway_value == address) {
            set_error(error, error_size, "br-lan gateway must be a different host in the subnet");
            return false;
        }
        const uint32_t gateway_host = gateway_value & ~mask;
        if (gateway_host == 0U || gateway_host == ~mask) {
            set_error(error, error_size, "br-lan gateway cannot be network or broadcast");
            return false;
        }
    }
    return true;
}

static bool run_uci(const char *operation, const char *argument) {
    const char *argv[5] = {"uci", NULL, NULL, NULL, NULL};
    size_t index = 1U;
    if (strcmp(operation, "delete") == 0)
        argv[index++] = "-q";
    argv[index++] = operation;
    if (argument != NULL)
        argv[index++] = argument;
    return edge_process_run(argv, -1, -1) == 0;
}

static bool backup_network(void) {
    const int output = open(NETWORK_BACKUP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0)
        return false;
    const char *argv[] = {"uci", "export", "network", NULL};
    const int result = edge_process_run(argv, -1, output);
    close(output);
    return result == 0;
}

static void prefix_to_mask(uint32_t prefix, char output[16]) {
    const uint32_t mask = htonl(0xFFFFFFFFU << (32U - prefix));
    (void)inet_ntop(AF_INET, &mask, output, 16U);
}

bool edge_network_prepare_br_lan(const iot_edge_v1_NetworkConfigRequest *request,
                                 char *error, size_t error_size) {
    if (request == NULL || request->request_id.size != 16U ||
        request->interfaces_count != 1U) {
        set_error(error, error_size, "network request must contain one br-lan interface");
        return false;
    }
    const iot_edge_v1_NetworkInterfaceConfig *config = &request->interfaces[0];
    if (strcmp(config->name, "br-lan") != 0 ||
        config->mode != iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_STATIC ||
        !config->bridge || config->delete_bridge) {
        set_error(error, error_size, "only static br-lan configuration is supported");
        return false;
    }
    if (!edge_network_validate_static(config->ip, config->prefix_length, config->gateway,
                                      error, error_size))
        return false;
    if (!backup_network()) {
        set_error(error, error_size, "cannot back up UCI network configuration");
        return false;
    }
    char ip[64];
    char mask[64];
    char gateway[64];
    char netmask[16];
    prefix_to_mask(config->prefix_length, netmask);
    snprintf(ip, sizeof(ip), "network.lan.ipaddr=%s", config->ip);
    snprintf(mask, sizeof(mask), "network.lan.netmask=%s", netmask);
    snprintf(gateway, sizeof(gateway), "network.lan.gateway=%s", config->gateway);
    const bool applied =
        run_uci("set", "network.lan.proto=static") &&
        run_uci("set", "network.lan.device=br-lan") && run_uci("set", ip) &&
        run_uci("set", mask) &&
        (config->gateway[0] != '\0' ? run_uci("set", gateway)
                                    : run_uci("delete", "network.lan.gateway")) &&
        run_uci("commit", "network");
    if (!applied) {
        restore_network();
        unlink(NETWORK_BACKUP);
        set_error(error, error_size, "uci rejected br-lan configuration");
        return false;
    }
    return true;
}

static void restore_network(void) {
    const int input = open(NETWORK_BACKUP, O_RDONLY);
    if (input >= 0) {
        const char *import[] = {"uci", "import", "network", NULL};
        if (edge_process_run(import, input, -1) == 0)
            (void)run_uci("commit", "network");
        close(input);
    }
    const char *reload[] = {"/etc/init.d/network", "reload", NULL};
    (void)edge_process_run(reload, -1, -1);
}

bool edge_network_activate(uint32_t rollback_timeout_sec) {
    if (rollback_timeout_sec < 30U || rollback_timeout_sec > 300U)
        return false;
    unlink(NETWORK_CONFIRMED);
    const int marker = open(NETWORK_PENDING, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (marker < 0)
        return false;
    close(marker);
    const pid_t child = fork();
    if (child < 0) {
        restore_network();
        unlink(NETWORK_PENDING);
        unlink(NETWORK_BACKUP);
        return false;
    }
    if (child == 0) {
        /* Leave enough time for the success result to leave over the old address. */
        sleep(1U);
        const char *reload[] = {"/etc/init.d/network", "reload", NULL};
        (void)edge_process_run(reload, -1, -1);
        sleep(rollback_timeout_sec);
        if (access(NETWORK_CONFIRMED, F_OK) != 0)
            restore_network();
        unlink(NETWORK_CONFIRMED);
        unlink(NETWORK_PENDING);
        unlink(NETWORK_BACKUP);
        _exit(0);
    }
    return true;
}

void edge_network_confirm(void) {
    if (access(NETWORK_PENDING, F_OK) != 0)
        return;
    const int marker = open(NETWORK_CONFIRMED, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (marker >= 0)
        close(marker);
}
