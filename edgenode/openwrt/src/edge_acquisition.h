#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"
#include "edge_runtime_config.h"

typedef struct edge_acquisition edge_acquisition;

typedef bool (*edge_acquisition_telemetry_callback)(
    void *context, const iot_edge_v1_TelemetryRecord *record);
typedef bool (*edge_acquisition_command_callback)(
    void *context, const iot_edge_v1_CommandResult *result);

edge_acquisition *edge_acquisition_create(
    edge_acquisition_telemetry_callback telemetry,
    edge_acquisition_command_callback command, void *callback_context);

/*
 * Builds a replacement runtime without touching the currently running one.
 * Only Modbus RTU/TCP and S7 TCP Client are accepted. On success the old
 * connections are closed and the new config becomes live atomically.
 */
bool edge_acquisition_apply(edge_acquisition *acquisition,
                            const edge_runtime_config *config,
                            uint64_t now_ms, char *error, size_t error_size);

/* Fixed one-second DTU acquisition tick; reporting uses each device interval. */
void edge_acquisition_tick(edge_acquisition *acquisition, uint64_t now_ms);

bool edge_acquisition_command(edge_acquisition *acquisition,
                              const iot_edge_v1_CommandRequest *request,
                              char *error, size_t error_size);

void edge_acquisition_destroy(edge_acquisition *acquisition);
