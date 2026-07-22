#include "edge_ws.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/sha256.h>
#include <pb_encode.h>

#define EDGE_SOFTWARE_VERSION "0.1.0"

static edge_ws_session *session_from_client(struct uwsc_client *client) {
    return (edge_ws_session *)((uint8_t *)client - offsetof(edge_ws_session, client));
}

static edge_ws_session *session_from_reconnect(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, reconnect_timer));
}

static edge_ws_session *session_from_heartbeat(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, heartbeat_timer));
}

static int64_t now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0)
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static bool random_bytes(uint8_t *output, size_t size) {
    FILE *input = fopen("/dev/urandom", "rb");
    if (input == NULL)
        return false;
    const bool ok = fread(output, 1U, size, input) == size;
    fclose(input);
    return ok;
}

static void safe_copy(char *output, size_t capacity, const char *input) {
    if (capacity == 0U)
        return;
    snprintf(output, capacity, "%s", input != NULL ? input : "");
}

static bool read_secret(const char *path, char *output, size_t capacity) {
    output[0] = '\0';
    if (path == NULL || path[0] == '\0')
        return true;
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || (info.st_mode & 0077U) != 0U) {
        syslog(LOG_ERR, "enrollment token file must be a regular owner-only file");
        return false;
    }
    FILE *input = fopen(path, "rb");
    if (input == NULL)
        return false;
    size_t size = fread(output, 1U, capacity - 1U, input);
    const bool eof = feof(input) != 0;
    fclose(input);
    if (!eof) {
        memset(output, 0, capacity);
        return false;
    }
    while (size != 0U && (output[size - 1U] == '\r' || output[size - 1U] == '\n' ||
                          output[size - 1U] == ' ' || output[size - 1U] == '\t'))
        output[--size] = '\0';
    output[capacity - 1U] = '\0';
    return true;
}

static bool init_envelope(edge_ws_session *session, iot_edge_v1_Envelope *envelope) {
    uint8_t random[10];
    if (!random_bytes(random, sizeof(random)))
        return false;
    return edge_protocol_init_envelope(
        envelope, session->config->id,
        session->enrolled ? session->node_id : NULL,
        session->session_epoch, ++session->sequence, now_ms(), random);
}

static bool send_envelope(edge_ws_session *session, iot_edge_v1_Envelope *envelope) {
    size_t wire_size = 0U;
    const char *error = NULL;
    if (!session->websocket_open ||
        !edge_protocol_encode(envelope, session->app->wire, sizeof(session->app->wire),
                              &wire_size, &error)) {
        syslog(LOG_ERR, "cannot encode edge envelope: %s", error != NULL ? error : "closed");
        return false;
    }
    return session->client.send(&session->client, session->app->wire, wire_size,
                                UWSC_OP_BINARY) == 0;
}

static void send_outbox_first(edge_ws_session *session) {
    if (!session->enrolled || !session->websocket_open)
        return;
    const edge_memory_message *message = edge_spool_outbox_first(&session->spool);
    if (message == NULL)
        return;
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    const char *error = NULL;
    if (!edge_protocol_decode(message->payload, message->payload_size, envelope, &error)) {
        syslog(LOG_ERR, "cannot decode tmpfs outbox envelope: %s",
               error != NULL ? error : "unknown error");
        return;
    }
    edge_protocol_set_bytes(&envelope->node_id, sizeof(envelope->node_id.bytes),
                            session->node_id, sizeof(session->node_id));
    envelope->session_epoch = session->session_epoch;
    envelope->sequence = ++session->sequence;
    send_envelope(session, envelope);
}

static bool send_hello(edge_ws_session *session) {
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return false;
    envelope->which_payload = iot_edge_v1_Envelope_hello_tag;
    iot_edge_v1_Hello *hello = &envelope->payload.hello;
    safe_copy(hello->imei, sizeof(hello->imei), session->app->config->imei);
    safe_copy(hello->model, sizeof(hello->model), session->app->config->model);
    safe_copy(hello->software_version, sizeof(hello->software_version), EDGE_SOFTWARE_VERSION);
    gethostname(hello->hostname, sizeof(hello->hostname) - 1U);
    struct utsname system;
    if (uname(&system) == 0) {
        safe_copy(hello->architecture, sizeof(hello->architecture), system.machine);
        safe_copy(hello->openwrt_release, sizeof(hello->openwrt_release), system.release);
    }
    if (!read_secret(session->config->enrollment_token_file, hello->enrollment_token,
                     sizeof(hello->enrollment_token)))
        return false;
    hello->last_applied_config_version = session->active_revision;
    hello->supported_protocol_versions_count = 1U;
    hello->supported_protocol_versions[0] = EDGENODE_PROTOCOL_VERSION;
    hello->supports_tcp = true;
    hello->supports_serial = true;
    hello->supports_network_config = session->config->network_owner;
    const bool sent = send_envelope(session, envelope);
    memset(hello->enrollment_token, 0, sizeof(hello->enrollment_token));
    return sent;
}

static void schedule_reconnect(edge_ws_session *session) {
    session->websocket_open = false;
    session->enrolled = false;
    session->client_active = false;
    ev_timer_stop(session->app->loop, &session->heartbeat_timer);
    ev_timer_stop(session->app->loop, &session->reconnect_timer);
    ev_timer_set(&session->reconnect_timer,
                 (ev_tstamp)session->config->reconnect_interval_sec, 0.0);
    ev_timer_start(session->app->loop, &session->reconnect_timer);
}

static void websocket_open(struct uwsc_client *client) {
    edge_ws_session *session = session_from_client(client);
    session->websocket_open = true;
    if (!send_hello(session)) {
        client->send_close(client, UWSC_CLOSE_STATUS_UNEXPECTED_CONDITION, "hello failed");
        return;
    }
    syslog(LOG_INFO, "platform %s WebSocket connected", session->config->name);
}

static void websocket_error(struct uwsc_client *client, int error, const char *message) {
    edge_ws_session *session = session_from_client(client);
    syslog(LOG_WARNING, "platform %s WebSocket error %d: %s", session->config->name,
           error, message != NULL ? message : "");
    schedule_reconnect(session);
}

static void websocket_close(struct uwsc_client *client, int code, const char *reason) {
    edge_ws_session *session = session_from_client(client);
    syslog(LOG_WARNING, "platform %s WebSocket closed %d: %s", session->config->name,
           code, reason != NULL ? reason : "");
    schedule_reconnect(session);
}

static bool valid_origin(const edge_ws_session *session,
                         const iot_edge_v1_Envelope *envelope) {
    if (envelope->platform_id.size != 16U ||
        memcmp(envelope->platform_id.bytes, session->config->id, 16U) != 0)
        return false;
    if (session->enrolled &&
        (envelope->node_id.size != 16U ||
         memcmp(envelope->node_id.bytes, session->node_id, 16U) != 0 ||
         envelope->session_epoch != session->session_epoch))
        return false;
    return true;
}

static bool encode_config_item(const iot_edge_v1_ConfigItem *item, uint8_t *output,
                               size_t capacity, size_t *output_size) {
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    if (!pb_encode(&stream, iot_edge_v1_ConfigItem_fields, item))
        return false;
    *output_size = stream.bytes_written;
    return true;
}

static bool verify_config_item(iot_edge_v1_ConfigItem *item, uint8_t *encoded,
                               size_t capacity, size_t *encoded_size) {
    if (item->sha256.size != 32U)
        return false;
    uint8_t expected[32];
    memcpy(expected, item->sha256.bytes, sizeof(expected));
    item->sha256.size = 0U;
    size_t canonical_size = 0U;
    if (!encode_config_item(item, encoded, capacity, &canonical_size)) {
        memcpy(item->sha256.bytes, expected, sizeof(expected));
        item->sha256.size = sizeof(expected);
        return false;
    }
    uint8_t actual[32];
    if (mbedtls_sha256(encoded, canonical_size, actual, 0) != 0 ||
        memcmp(expected, actual, sizeof(actual)) != 0) {
        memcpy(item->sha256.bytes, expected, sizeof(expected));
        item->sha256.size = sizeof(expected);
        return false;
    }
    memcpy(item->sha256.bytes, expected, sizeof(expected));
    item->sha256.size = sizeof(expected);
    return encode_config_item(item, encoded, capacity, encoded_size);
}

static void send_config_result(edge_ws_session *session, uint64_t revision,
                               const uint8_t digest[32], bool applied,
                               const char *code, const char *message) {
    uint8_t digest_copy[32];
    memcpy(digest_copy, digest, sizeof(digest_copy));
    iot_edge_v1_Envelope *out = &session->app->envelope;
    if (!init_envelope(session, out))
        return;
    if (applied) {
        out->which_payload = iot_edge_v1_Envelope_config_applied_tag;
        out->payload.config_applied.revision = revision;
        edge_protocol_set_bytes(&out->payload.config_applied.sha256,
                                sizeof(out->payload.config_applied.sha256.bytes), digest_copy, 32U);
    } else {
        out->which_payload = iot_edge_v1_Envelope_config_rejected_tag;
        out->payload.config_rejected.revision = revision;
        safe_copy(out->payload.config_rejected.code,
                  sizeof(out->payload.config_rejected.code), code);
        safe_copy(out->payload.config_rejected.message,
                  sizeof(out->payload.config_rejected.message), message);
    }
    send_envelope(session, out);
}

static void handle_config(edge_ws_session *session, iot_edge_v1_Envelope *envelope) {
    if (envelope->which_payload == iot_edge_v1_Envelope_config_begin_tag) {
        iot_edge_v1_ConfigBegin *begin = &envelope->payload.config_begin;
        if (begin->sha256.size != 32U || begin->revision <= session->active_revision ||
            !edge_spool_config_begin(&session->spool, begin->revision,
                                     begin->item_count, begin->sha256.bytes))
            send_config_result(session, begin->revision, begin->sha256.bytes, false,
                               "config_begin_invalid", "configuration begin was rejected");
        return;
    }
    if (envelope->which_payload == iot_edge_v1_Envelope_config_item_tag) {
        iot_edge_v1_ConfigItem *item = &envelope->payload.config_item;
        uint8_t encoded[iot_edge_v1_ConfigItem_size];
        size_t encoded_size = 0U;
        if (!verify_config_item(item, encoded, sizeof(encoded), &encoded_size) ||
            !edge_spool_config_put(&session->spool, item->revision, item->index,
                                   item->sha256.bytes, encoded, encoded_size))
            send_config_result(session, item->revision, item->sha256.bytes, false,
                               "config_item_invalid", "configuration item was rejected");
        return;
    }
    iot_edge_v1_ConfigCommit *commit = &envelope->payload.config_commit;
    if (commit->sha256.size != 32U ||
        !edge_spool_config_commit(&session->spool, commit->revision,
                                  commit->sha256.bytes)) {
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_commit_invalid", "configuration digest or item count failed");
        return;
    }
    session->active_revision = commit->revision;
    send_config_result(session, commit->revision, commit->sha256.bytes, true, NULL, NULL);
}

static void send_pong(edge_ws_session *session, const iot_edge_v1_Envelope *input) {
    const uint64_t nonce = input->payload.ping.nonce;
    uint8_t message_id[16];
    memcpy(message_id, input->message_id.bytes, sizeof(message_id));
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_pong_tag;
    output->payload.pong.nonce = nonce;
    edge_protocol_set_bytes(&output->causation_id, sizeof(output->causation_id.bytes),
                            message_id, sizeof(message_id));
    send_envelope(session, output);
}

static void websocket_message(struct uwsc_client *client, void *data, size_t size, bool binary) {
    edge_ws_session *session = session_from_client(client);
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    const char *error = NULL;
    if (!binary || !edge_protocol_decode(data, size, envelope, &error) ||
        !valid_origin(session, envelope)) {
        syslog(LOG_WARNING, "platform %s sent invalid nanopb envelope: %s",
               session->config->name, error != NULL ? error : "wrong origin");
        client->send_close(client, UWSC_CLOSE_STATUS_PROTOCOL_ERR, "invalid envelope");
        return;
    }

    switch (envelope->which_payload) {
    case iot_edge_v1_Envelope_hello_ack_tag: {
        iot_edge_v1_HelloAck *ack = &envelope->payload.hello_ack;
        if (ack->assigned_node_id.size != 16U ||
            ack->negotiated_protocol_version != EDGENODE_PROTOCOL_VERSION ||
            ack->session_epoch == 0U) {
            client->send_close(client, UWSC_CLOSE_STATUS_PROTOCOL_ERR, "invalid hello ack");
            return;
        }
        memcpy(session->node_id, ack->assigned_node_id.bytes, 16U);
        session->session_epoch = ack->session_epoch;
        session->enrolled = true;
        const unsigned heartbeat = ack->heartbeat_interval_sec != 0U
                                       ? ack->heartbeat_interval_sec
                                       : session->app->config->heartbeat_interval_sec;
        ev_timer_stop(session->app->loop, &session->heartbeat_timer);
        ev_timer_set(&session->heartbeat_timer, (ev_tstamp)heartbeat, (ev_tstamp)heartbeat);
        ev_timer_start(session->app->loop, &session->heartbeat_timer);
        send_outbox_first(session);
        break;
    }
    case iot_edge_v1_Envelope_config_begin_tag:
    case iot_edge_v1_Envelope_config_item_tag:
    case iot_edge_v1_Envelope_config_commit_tag:
        if (session->enrolled)
            handle_config(session, envelope);
        break;
    case iot_edge_v1_Envelope_telemetry_ack_tag:
        for (pb_size_t index = 0; index < envelope->payload.telemetry_ack.accepted_record_ids_count;
             ++index) {
            const iot_edge_v1_TelemetryAck_accepted_record_ids_t *id =
                &envelope->payload.telemetry_ack.accepted_record_ids[index];
            if (id->size == 16U)
                edge_spool_outbox_ack(&session->spool, id->bytes);
        }
        send_outbox_first(session);
        break;
    case iot_edge_v1_Envelope_raw_packet_ack_tag:
        if (envelope->payload.raw_packet_ack.packet_id.size == 16U) {
            edge_spool_outbox_ack(
                &session->spool, envelope->payload.raw_packet_ack.packet_id.bytes);
            send_outbox_first(session);
        }
        break;
    case iot_edge_v1_Envelope_ping_tag:
        send_pong(session, envelope);
        break;
    case iot_edge_v1_Envelope_enrollment_pending_tag:
        syslog(LOG_INFO, "platform %s enrollment pending", session->config->name);
        break;
    case iot_edge_v1_Envelope_enrollment_rejected_tag:
        syslog(LOG_WARNING, "platform %s enrollment rejected", session->config->name);
        break;
    default:
        break;
    }
}

static void heartbeat_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_heartbeat(timer);
    if (!session->enrolled)
        return;
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return;
    envelope->which_payload = iot_edge_v1_Envelope_heartbeat_tag;
    struct sysinfo info;
    if (sysinfo(&info) == 0)
        envelope->payload.heartbeat.uptime_sec = (uint64_t)info.uptime;
    envelope->payload.heartbeat.active_config_version = session->active_revision;
    edge_spool_maintain(&session->spool);
    envelope->payload.heartbeat.outbox_records = session->spool.outbox.count;
    envelope->payload.heartbeat.outbox_bytes = session->spool.outbox.bytes;
    send_envelope(session, envelope);
    send_outbox_first(session);
}

static void start_connection(edge_ws_session *session) {
    if (uwsc_init(&session->client, session->app->loop, session->config->url,
                  session->app->config->heartbeat_interval_sec, NULL) != 0) {
        schedule_reconnect(session);
        return;
    }
    /* libuwsc-mbedtls 3.3.5 encrypts WSS but uses optional certificate verification. */
    session->client.onopen = websocket_open;
    session->client.onmessage = websocket_message;
    session->client.onerror = websocket_error;
    session->client.onclose = websocket_close;
    session->client_active = true;
}

static void reconnect_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_reconnect(timer);
    start_connection(session);
}

bool edge_ws_app_init(edge_ws_app *app, struct ev_loop *loop,
                      const edge_app_config *config) {
    if (app == NULL || loop == NULL || config == NULL)
        return false;
    memset(app, 0, sizeof(*app));
    app->loop = loop;
    app->config = config;
    for (size_t index = 0; index < config->platform_count; ++index) {
        edge_ws_session *session = &app->sessions[index];
        session->app = app;
        session->config = &config->platforms[index];
        if (!edge_spool_init(&session->spool, session->config->id,
                             session->config->outbox_max_bytes)) {
            for (size_t cleanup = 0; cleanup <= index; ++cleanup)
                edge_spool_free(&app->sessions[cleanup].spool);
            return false;
        }
        session->active_revision = session->spool.active_config.revision;
        ev_timer_init(&session->reconnect_timer, reconnect_timer, 0.0, 0.0);
        ev_timer_init(&session->heartbeat_timer, heartbeat_timer, 0.0, 0.0);
    }
    return true;
}

void edge_ws_app_start(edge_ws_app *app) {
    if (app == NULL)
        return;
    for (size_t index = 0; index < app->config->platform_count; ++index)
        start_connection(&app->sessions[index]);
}

void edge_ws_app_stop(edge_ws_app *app) {
    if (app == NULL)
        return;
    for (size_t index = 0; index < app->config->platform_count; ++index) {
        edge_ws_session *session = &app->sessions[index];
        ev_timer_stop(app->loop, &session->reconnect_timer);
        ev_timer_stop(app->loop, &session->heartbeat_timer);
        if (session->client_active)
            session->client.free(&session->client);
        edge_spool_free(&session->spool);
        session->client_active = false;
        session->websocket_open = false;
    }
}

bool edge_ws_app_enqueue(edge_ws_app *app, const uint8_t origin_platform_id[16],
                         const uint8_t ack_id[16],
                         const iot_edge_v1_Envelope *envelope) {
    if (app == NULL || origin_platform_id == NULL || ack_id == NULL || envelope == NULL ||
        envelope->platform_id.size != 16U ||
        memcmp(envelope->platform_id.bytes, origin_platform_id, 16U) != 0)
        return false;
    edge_ws_session *session = NULL;
    for (size_t index = 0; index < app->config->platform_count; ++index) {
        if (memcmp(app->sessions[index].config->id, origin_platform_id, 16U) == 0) {
            session = &app->sessions[index];
            break;
        }
    }
    if (session == NULL)
        return false;
    size_t wire_size = 0U;
    const char *error = NULL;
    if (!edge_protocol_encode(envelope, app->wire, sizeof(app->wire), &wire_size, &error) ||
        !edge_spool_outbox_put(&session->spool, ack_id, app->wire, wire_size)) {
        syslog(LOG_ERR, "cannot queue nanopb message for origin platform %s: %s",
               session->config->name, error != NULL ? error : "tmpfs spool rejected");
        return false;
    }
    send_outbox_first(session);
    return true;
}
