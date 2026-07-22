#include "edge_platform.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "edge_config.h"
#include "edge_process.h"

static void set_error(char *output, size_t capacity, const char *message) {
    if (output != NULL && capacity != 0U)
        snprintf(output, capacity, "%s", message != NULL ? message : "platform error");
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

static void platform_names(const uint8_t id[16], char section[35], char credential[96]) {
    static const char hex[] = "0123456789abcdef";
    section[0] = 'p';
    section[1] = '_';
    for (size_t index = 0; index < 16U; ++index) {
        section[2U + index * 2U] = hex[id[index] >> 4U];
        section[3U + index * 2U] = hex[id[index] & 0x0FU];
    }
    section[34] = '\0';
    snprintf(credential, 96U, "/etc/edgenode/credentials/%s.token", section + 2U);
}

static bool write_credential(const char *path, const char *token) {
    if (token == NULL || token[0] == '\0') {
        unlink(path);
        return true;
    }
    const int output = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0)
        return false;
    const size_t size = strlen(token);
    const bool ok = write(output, token, size) == (ssize_t)size &&
                    write(output, "\n", 1U) == 1 && fsync(output) == 0;
    close(output);
    return ok;
}

static bool set_value(const char *section, const char *name, const char *value) {
    char assignment[640];
    const int size = name != NULL && name[0] != '\0'
                         ? snprintf(assignment, sizeof(assignment), "edgenode.%s.%s=%s",
                                    section, name, value != NULL ? value : "")
                         : snprintf(assignment, sizeof(assignment), "edgenode.%s=%s",
                                    section, value != NULL ? value : "");
    if (size < 0 || size >= (int)sizeof(assignment))
        return false;
    return run_uci("set", assignment);
}

static bool valid_text(const char *value) {
    if (value == NULL || value[0] == '\0')
        return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor)
        if (*cursor < 0x20U || *cursor == 0x7fU)
            return false;
    return true;
}

bool edge_platform_apply(const iot_edge_v1_PlatformConfigRequest *request,
                         char *error, size_t error_size) {
    if (request == NULL || request->request_id.size != 16U ||
        request->target_platform_id.size != 16U) {
        set_error(error, error_size, "platform request id is invalid");
        return false;
    }
    uint8_t bootstrap[16];
    if (!edge_config_parse_uuid(EDGE_BOOTSTRAP_PLATFORM_ID, bootstrap) ||
        memcmp(bootstrap, request->target_platform_id.bytes, sizeof(bootstrap)) == 0) {
        set_error(error, error_size, "bootstrap platform cannot be modified");
        return false;
    }
    char section[35];
    char credential[96];
    platform_names(request->target_platform_id.bytes, section, credential);
    char section_path[64];
    snprintf(section_path, sizeof(section_path), "edgenode.%s", section);
    if (request->operation ==
        iot_edge_v1_PlatformConfigOperation_PLATFORM_CONFIG_DELETE) {
        if (!run_uci("delete", section_path) || !run_uci("commit", "edgenode")) {
            set_error(error, error_size, "uci could not delete platform");
            return false;
        }
        unlink(credential);
        return true;
    }
    if (request->operation !=
            iot_edge_v1_PlatformConfigOperation_PLATFORM_CONFIG_UPSERT ||
        !valid_text(request->name) || !valid_text(request->url) ||
        (strncmp(request->url, "https://", 8U) != 0 &&
         strncmp(request->url, "http://", 7U) != 0) ||
        request->reconnect_interval_sec < 1U || request->reconnect_interval_sec > 3600U ||
        request->outbox_max_bytes < 16U * 1024U ||
        request->outbox_max_bytes > 8U * 1024U * 1024U) {
        set_error(error, error_size, "platform configuration is invalid");
        return false;
    }
    char id[37];
    edge_config_format_uuid(request->target_platform_id.bytes, id);
    char priority[16];
    char reconnect[16];
    char outbox[16];
    snprintf(priority, sizeof(priority), "%u", (unsigned)request->priority);
    snprintf(reconnect, sizeof(reconnect), "%u", (unsigned)request->reconnect_interval_sec);
    snprintf(outbox, sizeof(outbox), "%u", (unsigned)request->outbox_max_bytes);
    const bool applied = set_value(section, NULL, "platform") &&
                         set_value(section, "id", id) &&
                         set_value(section, "name", request->name) &&
                         set_value(section, "url", request->url) &&
                         set_value(section, "enabled", request->enabled ? "1" : "0") &&
                         set_value(section, "network_owner", "0") &&
                         set_value(section, "priority", priority) &&
                         set_value(section, "reconnect_interval_sec", reconnect) &&
                         set_value(section, "outbox_max_bytes", outbox) &&
                         set_value(section, "enrollment_token_file", credential) &&
                         write_credential(credential, request->enrollment_token) &&
                         run_uci("commit", "edgenode");
    if (!applied) {
        set_error(error, error_size, "uci could not save platform configuration");
        return false;
    }
    return true;
}
