#pragma once

#include <stdbool.h>

#include <ev.h>
#include <uwsc/uwsc.h>

#include "edge_config.h"
#include "edge_protocol.h"
#include "edge_spool.h"

typedef struct edge_ws_app edge_ws_app;

typedef struct {
    struct uwsc_client client;
    struct ev_timer reconnect_timer;
    struct ev_timer heartbeat_timer;
    struct ev_timer firmware_timer;
    struct ev_timer reload_timer;
    struct ev_timer terminal_timer;
    edge_ws_app *app;
    const edge_platform_config *config;
    char transport_url[EDGE_URL_MAX + 32U];
    uint8_t node_id[16];
    uint8_t terminal_id[16];
    uint64_t session_epoch;
    uint64_t active_revision;
    edge_spool spool;
    uint64_t sequence;
    uint16_t heartbeat_interval_sec;
    bool client_active;
    bool websocket_open;
    bool enrolled;
    bool terminal_open;
} edge_ws_session;

struct edge_ws_app {
    struct ev_loop *loop;
    const edge_app_config *config;
    edge_ws_session sessions[EDGE_MAX_PLATFORMS];
    iot_edge_v1_Envelope envelope;
    uint8_t wire[EDGENODE_MAX_WS_MESSAGE];
};

bool edge_ws_app_init(edge_ws_app *app, struct ev_loop *loop,
                      const edge_app_config *config);
void edge_ws_app_start(edge_ws_app *app);
void edge_ws_app_stop(edge_ws_app *app);

/*
 * Queues a complete Envelope only in the origin platform's tmpfs outbox. ack_id
 * is the TelemetryRecord record_id or RawPacket packet_id acknowledged upstream.
 */
bool edge_ws_app_enqueue(edge_ws_app *app, const uint8_t origin_platform_id[16],
                         const uint8_t ack_id[16],
                         const iot_edge_v1_Envelope *envelope);
