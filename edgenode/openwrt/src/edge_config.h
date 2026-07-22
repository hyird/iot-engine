#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGE_MAX_PLATFORMS 4U
#define EDGE_URL_MAX 255U

typedef struct {
    uint8_t id[16];
    char name[33];
    char url[EDGE_URL_MAX + 1U];
    char enrollment_token_file[129];
    bool enabled;
    bool network_owner;
    uint16_t priority;
    uint16_t reconnect_interval_sec;
    uint32_t outbox_max_bytes;
} edge_platform_config;

typedef struct {
    char imei[16];
    char model[129];
    uint16_t heartbeat_interval_sec;
    uint8_t platform_count;
    edge_platform_config platforms[EDGE_MAX_PLATFORMS];
} edge_app_config;

bool edge_config_parse_uuid(const char *text, uint8_t output[16]);
void edge_config_format_uuid(const uint8_t value[16], char output[37]);

/* Loads /etc/config/iot-edge through libuci. error never contains credentials. */
bool edge_config_load(edge_app_config *config, char *error, size_t error_size);
