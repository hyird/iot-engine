#include "edge_ws.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <pb_encode.h>

#include "edge_capability.h"
#include "edge_firmware.h"
#include "edge_modem.h"
#include "edge_network.h"
#include "edge_platform.h"
#include "edge_sha256.h"
#include "edge_terminal.h"

#define EDGE_SOFTWARE_VERSION "0.1.1"

static edge_ws_session *session_from_client(struct uwsc_client *client) {
    return (edge_ws_session *)((uint8_t *)client - offsetof(edge_ws_session, client));
}

static edge_ws_session *session_from_reconnect(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, reconnect_timer));
}

static edge_ws_session *session_from_heartbeat(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, heartbeat_timer));
}

static edge_ws_session *session_from_firmware(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, firmware_timer));
}

static edge_ws_session *session_from_reload(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, reload_timer));
}

static edge_ws_session *session_from_terminal(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, terminal_timer));
}

static edge_ws_session *session_from_acquisition(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, acquisition_timer));
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

static bool acquisition_telemetry(void *context,
                                  const iot_edge_v1_TelemetryRecord *record) {
    edge_ws_session *session = context;
    if (record == NULL || record->record_id.size != 16U)
        return false;
    uint8_t record_id[16];
    memcpy(record_id, record->record_id.bytes, sizeof(record_id));
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return false;
    envelope->which_payload = iot_edge_v1_Envelope_telemetry_batch_tag;
    envelope->payload.telemetry_batch.records_count = 1U;
    envelope->payload.telemetry_batch.records[0] = *record;
    return edge_ws_app_enqueue(session->app, session->config->id, record_id, envelope);
}

static bool acquisition_command_result(void *context,
                                       const iot_edge_v1_CommandResult *result) {
    edge_ws_session *session = context;
    if (result == NULL || result->command_id.size != 16U)
        return false;
    uint8_t command_id[16];
    memcpy(command_id, result->command_id.bytes, sizeof(command_id));
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return false;
    envelope->which_payload = iot_edge_v1_Envelope_command_result_tag;
    envelope->payload.command_result = *result;
    return edge_ws_app_enqueue(session->app, session->config->id, command_id, envelope);
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
    hello->supports_terminal = edge_capability_has_ttyd();
    hello->supports_firmware_update = session->config->bootstrap;
    hello->supports_platform_config = session->config->bootstrap;
    hello->supports_device_config = true;
    edge_modem_info modem;
    bool modem_available = false;
    hello->signal_csq = 99U;
    hello->signal_rssi_dbm = -1;
    hello->mobile_registration_status = -1;
    if (edge_modem_read_status(session->app->config->modem_status_path,
                               &modem, &modem_available) && modem_available) {
        safe_copy(hello->iccid, sizeof(hello->iccid), modem.iccid);
        hello->signal_csq = (uint32_t)modem.csq;
        hello->signal_rssi_dbm = modem.rssi_dbm;
        hello->signal_percent = modem.signal_percent;
        hello->mobile_registered = modem.registered;
        hello->mobile_registration_status = modem.registration_status;
    }
    const bool sent = send_envelope(session, envelope);
    memset(hello->enrollment_token, 0, sizeof(hello->enrollment_token));
    return sent;
}

static uint32_t ipv4_prefix_length(struct in_addr mask) {
    uint32_t value = ntohl(mask.s_addr);
    uint32_t prefix = 0U;
    while ((value & 0x80000000U) != 0U) {
        ++prefix;
        value <<= 1U;
    }
    return prefix;
}

static void read_interface_capability(const char *name,
                                      iot_edge_v1_InterfaceCapability *capability) {
    safe_copy(capability->name, sizeof(capability->name), name);
    safe_copy(capability->display_name, sizeof(capability->display_name), name);
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    struct ifreq request;
    memset(&request, 0, sizeof(request));
    safe_copy(request.ifr_name, sizeof(request.ifr_name), name);
    if (ioctl(fd, SIOCGIFFLAGS, &request) == 0)
        capability->up = (request.ifr_flags & IFF_UP) != 0;
#ifdef SIOCGIFHWADDR
    if (ioctl(fd, SIOCGIFHWADDR, &request) == 0)
        edge_protocol_set_bytes(&capability->mac, sizeof(capability->mac.bytes),
                                (const uint8_t *)request.ifr_hwaddr.sa_data, 6U);
#endif
    if (ioctl(fd, SIOCGIFADDR, &request) == 0) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)&request.ifr_addr;
        inet_ntop(AF_INET, &address->sin_addr, capability->ipv4, sizeof(capability->ipv4));
    }
    if (ioctl(fd, SIOCGIFNETMASK, &request) == 0) {
        const struct sockaddr_in *mask = (const struct sockaddr_in *)&request.ifr_netmask;
        capability->prefix_length = ipv4_prefix_length(mask->sin_addr);
    }
    close(fd);
}

static bool send_capability_report(edge_ws_session *session) {
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return false;
    envelope->which_payload = iot_edge_v1_Envelope_capability_report_tag;
    iot_edge_v1_CapabilityReport *report = &envelope->payload.capability_report;
    safe_copy(report->network_backend, sizeof(report->network_backend), "netifd");
    report->ttyd_available = edge_capability_has_ttyd();

    if (session->app->config->lan_interface[0] != '\0') {
        report->interfaces_count = 1U;
        read_interface_capability(session->app->config->lan_interface,
                                  &report->interfaces[0]);
        report->interfaces[0].bridge = session->app->config->bridge;
    }
    if (session->app->config->serial_port[0] != '\0') {
        report->serial_ports_count = 1U;
        iot_edge_v1_SerialCapability *serial = &report->serial_ports[0];
        safe_copy(serial->path, sizeof(serial->path), session->app->config->serial_port);
        safe_copy(serial->display_name, sizeof(serial->display_name),
                  session->app->config->serial_port);
        serial->available = access(session->app->config->serial_port, R_OK | W_OK) == 0;
        serial->rs485 = session->app->config->serial_rs485;
    }
    return send_envelope(session, envelope);
}

static void schedule_reconnect(edge_ws_session *session) {
    session->websocket_open = false;
    session->enrolled = false;
    session->client_active = false;
    ev_timer_stop(session->app->loop, &session->heartbeat_timer);
    ev_timer_stop(session->app->loop, &session->terminal_timer);
    if (session->terminal_open) {
        edge_terminal_close(session->terminal_id);
        session->terminal_open = false;
    }
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
    if (edge_sha256(encoded, canonical_size, actual, 0) != 0 ||
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
        out->payload.config_applied.endpoint_count = session->runtime_config.endpoint_count;
        out->payload.config_applied.device_count = session->runtime_config.device_count;
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
    edge_runtime_config candidate = {0};
    edge_acquisition *candidate_acquisition = NULL;
    char apply_error[256] = "configuration digest or item count failed";
    if (commit->sha256.size != 32U ||
        !edge_runtime_config_load(&candidate, &session->spool.staging_config,
                                  apply_error, sizeof(apply_error))) {
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_commit_invalid", apply_error);
        return;
    }
    candidate_acquisition = edge_acquisition_create(acquisition_telemetry,
                                                     acquisition_command_result, session);
    if (candidate_acquisition == NULL ||
        !edge_acquisition_apply(candidate_acquisition, &candidate, (uint64_t)now_ms(),
                                apply_error, sizeof(apply_error))) {
        edge_acquisition_destroy(candidate_acquisition);
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_runtime_invalid", apply_error);
        return;
    }
    if (!edge_spool_config_commit(&session->spool, commit->revision,
                                  commit->sha256.bytes)) {
        edge_acquisition_destroy(candidate_acquisition);
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_commit_invalid", "configuration tmpfs commit failed");
        return;
    }
    edge_acquisition_destroy(session->acquisition);
    edge_runtime_config_free(&session->runtime_config);
    session->acquisition = candidate_acquisition;
    session->runtime_config = candidate;
    session->active_revision = commit->revision;
    send_config_result(session, commit->revision, commit->sha256.bytes, true, NULL, NULL);
}

static void handle_device_command(edge_ws_session *session,
                                  const iot_edge_v1_CommandRequest *request) {
    uint8_t command_id[16] = {0};
    uint8_t device_id[16] = {0};
    if (request->command_id.size == 16U)
        memcpy(command_id, request->command_id.bytes, sizeof(command_id));
    if (request->device_id.size == 16U)
        memcpy(device_id, request->device_id.bytes, sizeof(device_id));
    char error[257] = {0};
    if (edge_acquisition_command(session->acquisition, request, error, sizeof(error)))
        return;
    iot_edge_v1_CommandResult result = iot_edge_v1_CommandResult_init_zero;
    edge_protocol_set_bytes(&result.command_id, sizeof(result.command_id.bytes),
                            command_id, sizeof(command_id));
    edge_protocol_set_bytes(&result.device_id, sizeof(result.device_id.bytes),
                            device_id, sizeof(device_id));
    result.state = iot_edge_v1_CommandState_COMMAND_STATE_REJECTED;
    result.completed_at_ms = now_ms();
    safe_copy(result.message, sizeof(result.message),
              error[0] != '\0' ? error : "edge command rejected");
    (void)acquisition_command_result(session, &result);
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

static void send_firmware_result(edge_ws_session *session, const uint8_t request_id[16],
                                 iot_edge_v1_FirmwareUpdateState state,
                                 const char *message) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_firmware_update_result_tag;
    edge_protocol_set_bytes(&output->payload.firmware_update_result.request_id,
                            sizeof(output->payload.firmware_update_result.request_id.bytes),
                            request_id, 16U);
    output->payload.firmware_update_result.state = state;
    safe_copy(output->payload.firmware_update_result.message,
              sizeof(output->payload.firmware_update_result.message), message);
    send_envelope(session, output);
}

static void handle_network_config(edge_ws_session *session,
                                  const iot_edge_v1_NetworkConfigRequest *request) {
    uint8_t request_id[16] = {0};
    if (request->request_id.size == 16U)
        memcpy(request_id, request->request_id.bytes, sizeof(request_id));
    char message[257] = {0};
    bool success = edge_network_prepare_br_lan(request, message, sizeof(message));
    if (success && !edge_network_activate(request->rollback_timeout_sec)) {
        success = false;
        safe_copy(message, sizeof(message), "could not activate br-lan rollback watchdog");
    }
    if (success)
        safe_copy(message, sizeof(message), "br-lan accepted; reconnect confirms the change");
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_network_config_result_tag;
    edge_protocol_set_bytes(&output->payload.network_config_result.request_id,
                            sizeof(output->payload.network_config_result.request_id.bytes),
                            request_id, sizeof(request_id));
    output->payload.network_config_result.success = success;
    output->payload.network_config_result.rolled_back = false;
    safe_copy(output->payload.network_config_result.message,
              sizeof(output->payload.network_config_result.message), message);
    send_envelope(session, output);
}

static void handle_firmware_update(edge_ws_session *session,
                                   const iot_edge_v1_FirmwareUpdateRequest *request) {
    uint8_t request_id[16] = {0};
    if (request->request_id.size == 16U)
        memcpy(request_id, request->request_id.bytes, sizeof(request_id));
    char message[257] = {0};
    const bool accepted = edge_firmware_start(session->config->id, request,
                                               message, sizeof(message));
    send_firmware_result(
        session, request_id,
        accepted ? iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_ACCEPTED
                 : iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
        accepted ? "firmware update accepted" : message);
    if (accepted) {
        ev_timer_stop(session->app->loop, &session->firmware_timer);
        ev_timer_set(&session->firmware_timer, 0.25, 0.5);
        ev_timer_start(session->app->loop, &session->firmware_timer);
    }
}

static void handle_platform_config(edge_ws_session *session,
                                   const iot_edge_v1_PlatformConfigRequest *request) {
    uint8_t request_id[16] = {0};
    if (request->request_id.size == 16U)
        memcpy(request_id, request->request_id.bytes, sizeof(request_id));
    char message[257] = {0};
    const bool success = edge_platform_apply(request, message, sizeof(message));
    if (success)
        safe_copy(message, sizeof(message), "platform configuration saved through UCI");
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_platform_config_result_tag;
    edge_protocol_set_bytes(&output->payload.platform_config_result.request_id,
                            sizeof(output->payload.platform_config_result.request_id.bytes),
                            request_id, sizeof(request_id));
    output->payload.platform_config_result.success = success;
    safe_copy(output->payload.platform_config_result.message,
              sizeof(output->payload.platform_config_result.message), message);
    send_envelope(session, output);
    if (success) {
        ev_timer_stop(session->app->loop, &session->reload_timer);
        ev_timer_set(&session->reload_timer, 0.5, 0.0);
        ev_timer_start(session->app->loop, &session->reload_timer);
    }
}

static void send_terminal_close(edge_ws_session *session, const uint8_t terminal_id[16],
                                int32_t exit_code, const char *reason) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_terminal_close_tag;
    edge_protocol_set_bytes(&output->payload.terminal_close.terminal_id,
                            sizeof(output->payload.terminal_close.terminal_id.bytes),
                            terminal_id, 16U);
    output->payload.terminal_close.exit_code = exit_code;
    safe_copy(output->payload.terminal_close.reason,
              sizeof(output->payload.terminal_close.reason), reason);
    send_envelope(session, output);
}

static void handle_terminal_open(edge_ws_session *session,
                                 const iot_edge_v1_TerminalOpen *request) {
    uint8_t terminal_id[16] = {0};
    if (request->terminal_id.size == 16U)
        memcpy(terminal_id, request->terminal_id.bytes, sizeof(terminal_id));
    char error[129] = {0};
    if (!edge_terminal_open(request, error, sizeof(error))) {
        send_terminal_close(session, terminal_id, -1, error);
        return;
    }
    memcpy(session->terminal_id, terminal_id, sizeof(session->terminal_id));
    session->terminal_open = true;
    /* The platform drains terminal input after node traffic. While a terminal
     * is active, use a short heartbeat so interactive input is not delayed by
     * the normal management heartbeat interval. */
    ev_timer_stop(session->app->loop, &session->heartbeat_timer);
    ev_timer_set(&session->heartbeat_timer, 0.25, 0.25);
    ev_timer_start(session->app->loop, &session->heartbeat_timer);
    ev_timer_stop(session->app->loop, &session->terminal_timer);
    ev_timer_set(&session->terminal_timer, 0.05, 0.05);
    ev_timer_start(session->app->loop, &session->terminal_timer);
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
        if (session->config->network_owner)
            edge_network_confirm();
        const unsigned heartbeat = ack->heartbeat_interval_sec != 0U
                                       ? ack->heartbeat_interval_sec
                                       : session->app->config->heartbeat_interval_sec;
        session->heartbeat_interval_sec = (uint16_t)heartbeat;
        ev_timer_stop(session->app->loop, &session->heartbeat_timer);
        ev_timer_set(&session->heartbeat_timer, (ev_tstamp)heartbeat, (ev_tstamp)heartbeat);
        ev_timer_start(session->app->loop, &session->heartbeat_timer);
        send_capability_report(session);
        send_outbox_first(session);
        break;
    }
    case iot_edge_v1_Envelope_heartbeat_ack_tag:
        if (session->enrolled && envelope->payload.heartbeat_ack.request_capability_report)
            send_capability_report(session);
        break;
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
    case iot_edge_v1_Envelope_command_result_ack_tag:
        if (envelope->payload.command_result_ack.command_id.size == 16U) {
            edge_spool_outbox_ack(
                &session->spool, envelope->payload.command_result_ack.command_id.bytes);
            send_outbox_first(session);
        }
        break;
    case iot_edge_v1_Envelope_command_request_tag:
        if (session->enrolled)
            handle_device_command(session, &envelope->payload.command_request);
        break;
    case iot_edge_v1_Envelope_ping_tag:
        send_pong(session, envelope);
        break;
    case iot_edge_v1_Envelope_network_config_request_tag:
        if (session->enrolled && session->config->network_owner)
            handle_network_config(session, &envelope->payload.network_config_request);
        break;
    case iot_edge_v1_Envelope_firmware_update_request_tag:
        if (session->enrolled && session->config->bootstrap)
            handle_firmware_update(session, &envelope->payload.firmware_update_request);
        break;
    case iot_edge_v1_Envelope_platform_config_request_tag:
        if (session->enrolled && session->config->bootstrap)
            handle_platform_config(session, &envelope->payload.platform_config_request);
        break;
    case iot_edge_v1_Envelope_terminal_open_tag:
        if (session->enrolled && session->config->bootstrap &&
            edge_capability_has_ttyd())
            handle_terminal_open(session, &envelope->payload.terminal_open);
        break;
    case iot_edge_v1_Envelope_terminal_data_tag:
        if (session->terminal_open)
            (void)edge_terminal_write(&envelope->payload.terminal_data);
        break;
    case iot_edge_v1_Envelope_terminal_resize_tag:
        if (session->terminal_open)
            (void)edge_terminal_resize(&envelope->payload.terminal_resize);
        break;
    case iot_edge_v1_Envelope_terminal_close_tag:
        if (session->terminal_open && envelope->payload.terminal_close.terminal_id.size == 16U) {
            edge_terminal_close(envelope->payload.terminal_close.terminal_id.bytes);
            ev_timer_stop(session->app->loop, &session->terminal_timer);
            session->terminal_open = false;
            ev_timer_stop(session->app->loop, &session->heartbeat_timer);
            ev_timer_set(&session->heartbeat_timer,
                         (ev_tstamp)session->heartbeat_interval_sec,
                         (ev_tstamp)session->heartbeat_interval_sec);
            ev_timer_start(session->app->loop, &session->heartbeat_timer);
        }
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
    iot_edge_v1_Heartbeat *heartbeat = &envelope->payload.heartbeat;
    heartbeat->signal_csq = 99U;
    heartbeat->signal_rssi_dbm = -1;
    heartbeat->mobile_registration_status = -1;
    edge_modem_info modem;
    bool modem_available = false;
    if (edge_modem_read_status(session->app->config->modem_status_path,
                               &modem, &modem_available) && modem_available) {
        safe_copy(heartbeat->iccid, sizeof(heartbeat->iccid), modem.iccid);
        heartbeat->signal_csq = (uint32_t)modem.csq;
        heartbeat->signal_rssi_dbm = modem.rssi_dbm;
        heartbeat->signal_percent = modem.signal_percent;
        heartbeat->mobile_registered = modem.registered;
        heartbeat->mobile_registration_status = modem.registration_status;
    }
    struct sysinfo info;
    if (sysinfo(&info) == 0)
        heartbeat->uptime_sec = (uint64_t)info.uptime;
    heartbeat->active_config_version = session->active_revision;
    edge_spool_maintain(&session->spool);
    heartbeat->outbox_records = session->spool.outbox.count;
    heartbeat->outbox_bytes = session->spool.outbox.bytes;
    send_envelope(session, envelope);
    send_outbox_first(session);
}

static void firmware_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_firmware(timer);
    if (!session->enrolled)
        return;
    iot_edge_v1_FirmwareUpdateResult result = iot_edge_v1_FirmwareUpdateResult_init_zero;
    if (!edge_firmware_read_status(session->config->id, &result))
        return;
    uint8_t request_id[16] = {0};
    if (result.request_id.size == 16U)
        memcpy(request_id, result.request_id.bytes, sizeof(request_id));
    const iot_edge_v1_FirmwareUpdateState state = result.state;
    char message[257];
    safe_copy(message, sizeof(message), result.message);
    send_firmware_result(session, request_id, state, message);
    if (state == iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FLASHING ||
        state == iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED)
        ev_timer_stop(session->app->loop, &session->firmware_timer);
}

static void reload_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_reload(timer);
    ev_timer_stop(session->app->loop, &session->reload_timer);
    raise(SIGHUP);
}

static void terminal_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_terminal(timer);
    uint8_t terminal_id[16] = {0};
    uint8_t data[4096];
    bool closed = false;
    int32_t exit_code = 0;
    const ssize_t size = edge_terminal_read(terminal_id, data, sizeof(data), &closed, &exit_code);
    if (size > 0) {
        iot_edge_v1_Envelope *output = &session->app->envelope;
        if (init_envelope(session, output)) {
            output->which_payload = iot_edge_v1_Envelope_terminal_data_tag;
            edge_protocol_set_bytes(&output->payload.terminal_data.terminal_id,
                                    sizeof(output->payload.terminal_data.terminal_id.bytes),
                                    terminal_id, sizeof(terminal_id));
            edge_protocol_set_bytes(&output->payload.terminal_data.data,
                                    sizeof(output->payload.terminal_data.data.bytes),
                                    data, (size_t)size);
            send_envelope(session, output);
        }
    }
    if (closed) {
        ev_timer_stop(session->app->loop, &session->terminal_timer);
        session->terminal_open = false;
        send_terminal_close(session, terminal_id, exit_code, "terminal closed");
        ev_timer_stop(session->app->loop, &session->heartbeat_timer);
        ev_timer_set(&session->heartbeat_timer,
                     (ev_tstamp)session->heartbeat_interval_sec,
                     (ev_tstamp)session->heartbeat_interval_sec);
        ev_timer_start(session->app->loop, &session->heartbeat_timer);
    }
}

static void acquisition_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_acquisition(timer);
    edge_acquisition_tick(session->acquisition, (uint64_t)now_ms());
}

static bool make_transport_url(const char *base, char *output, size_t capacity) {
    const char *host = NULL;
    const char *scheme = NULL;
    if (strncmp(base, "https://", 8U) == 0) {
        scheme = "wss://";
        host = base + 8U;
    } else if (strncmp(base, "http://", 7U) == 0) {
        scheme = "ws://";
        host = base + 7U;
    } else {
        return false;
    }
    size_t host_size = strlen(host);
    while (host_size != 0U && host[host_size - 1U] == '/')
        --host_size;
    const int size = snprintf(output, capacity, "%s%.*s/edge/v1/connect", scheme,
                              (int)host_size, host);
    return host_size != 0U && size > 0 && (size_t)size < capacity;
}

static void start_connection(edge_ws_session *session) {
    if (!make_transport_url(session->config->url, session->transport_url,
                            sizeof(session->transport_url)) ||
        uwsc_init(&session->client, session->app->loop, session->transport_url,
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
            for (size_t cleanup = 0; cleanup <= index; ++cleanup) {
                edge_acquisition_destroy(app->sessions[cleanup].acquisition);
                edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
                edge_spool_free(&app->sessions[cleanup].spool);
            }
            return false;
        }
        session->active_revision = session->spool.active_config.revision;
        if (session->active_revision != 0U) {
            char error[256];
            if (!edge_runtime_config_load(&session->runtime_config,
                                          &session->spool.active_config,
                                          error, sizeof(error))) {
                syslog(LOG_ERR, "platform %s active config rejected: %s",
                       session->config->name, error);
                for (size_t cleanup = 0; cleanup <= index; ++cleanup) {
                    edge_acquisition_destroy(app->sessions[cleanup].acquisition);
                    edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
                    edge_spool_free(&app->sessions[cleanup].spool);
                }
                return false;
            }
        }
        session->acquisition = edge_acquisition_create(
            acquisition_telemetry, acquisition_command_result, session);
        char acquisition_error[256] = {0};
        if (session->acquisition == NULL ||
            !edge_acquisition_apply(session->acquisition, &session->runtime_config,
                                    (uint64_t)now_ms(), acquisition_error,
                                    sizeof(acquisition_error))) {
            syslog(LOG_ERR, "platform %s acquisition config rejected: %s",
                   session->config->name, acquisition_error);
            for (size_t cleanup = 0; cleanup <= index; ++cleanup) {
                edge_acquisition_destroy(app->sessions[cleanup].acquisition);
                edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
                edge_spool_free(&app->sessions[cleanup].spool);
            }
            return false;
        }
        ev_timer_init(&session->reconnect_timer, reconnect_timer, 0.0, 0.0);
        ev_timer_init(&session->heartbeat_timer, heartbeat_timer, 0.0, 0.0);
        ev_timer_init(&session->firmware_timer, firmware_timer, 0.0, 0.0);
        ev_timer_init(&session->reload_timer, reload_timer, 0.0, 0.0);
        ev_timer_init(&session->terminal_timer, terminal_timer, 0.0, 0.0);
        ev_timer_init(&session->acquisition_timer, acquisition_timer, 0.0, 0.0);
    }
    return true;
}

void edge_ws_app_start(edge_ws_app *app) {
    if (app == NULL)
        return;
    for (size_t index = 0; index < app->config->platform_count; ++index) {
        ev_timer_set(&app->sessions[index].acquisition_timer, 0.0, 1.0);
        ev_timer_start(app->loop, &app->sessions[index].acquisition_timer);
        start_connection(&app->sessions[index]);
    }
}

void edge_ws_app_stop(edge_ws_app *app) {
    if (app == NULL)
        return;
    for (size_t index = 0; index < app->config->platform_count; ++index) {
        edge_ws_session *session = &app->sessions[index];
        ev_timer_stop(app->loop, &session->reconnect_timer);
        ev_timer_stop(app->loop, &session->heartbeat_timer);
        ev_timer_stop(app->loop, &session->firmware_timer);
        ev_timer_stop(app->loop, &session->reload_timer);
        ev_timer_stop(app->loop, &session->terminal_timer);
        ev_timer_stop(app->loop, &session->acquisition_timer);
        if (session->terminal_open) {
            edge_terminal_close(session->terminal_id);
            session->terminal_open = false;
        }
        if (session->client_active)
            session->client.free(&session->client);
        edge_spool_free(&session->spool);
        edge_acquisition_destroy(session->acquisition);
        session->acquisition = NULL;
        edge_runtime_config_free(&session->runtime_config);
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
