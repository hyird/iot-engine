#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/modules/edge/edge.config.service.h"
#include "service/modules/edge/edge.protocol.h"
#include "service/modules/edge/edge.projector.h"
#include "service/modules/edge/edge.schema.h"

namespace service::edge {

class EdgeGatewayController final : public ruvia::Controller<EdgeGatewayController> {
  public:
    RUVIA_CONTROLLER_GROUP("/edge/v1")
    RUVIA_ROUTES_BEGIN
    const auto connectionOptions = ruvia::WebSocketRouteOptions{
        .lifecycle = {
            .heartbeat = ruvia::WebSocketHeartbeatPolicy::periodic(
                std::chrono::seconds(30), std::chrono::seconds(15)),
            .closeHandshakeTimeout = std::chrono::seconds(5),
        },
    };
    RUVIA_GET_WS_OPTIONS("/connect", connect, connectionOptions);
    RUVIA_GET_WS("/terminal", terminal, TerminalTicketValidator);
    RUVIA_ROUTES_END

  private:
    struct Session {
        std::string nodeId;
        std::array<std::uint8_t, 16> nodeBytes{};
        std::array<std::uint8_t, 16> platformBytes{};
        std::uint64_t epoch{};
        std::uint64_t inboundSequence{};
        std::uint64_t outboundSequence{};
        std::uint64_t configSentAtMs{};
        bool capabilitySeen{};
    };

    ruvia::Task<void> connect(ruvia::Context& c) {
        auto& socket = c.webSocket();
        auto first = co_await socket.read();
        if (!first || !first->binary()) {
            co_await socket.close(1002, "binary hello required");
            co_return;
        }
        iot_edge_v1_Envelope input = iot_edge_v1_Envelope_init_zero;
        if (!protocol::decode(first->payload(), input) || input.protocol_version != 1 ||
            input.which_payload != iot_edge_v1_Envelope_hello_tag ||
            input.platform_id.size != 16 || !platformMatches(input.platform_id.bytes) ||
            !protocol::validImei(input.payload.hello.imei)) {
            co_await socket.close(1002, "invalid hello");
            co_return;
        }
        co_await publishIngress(c, first->payload());
        const auto authKey = protocol::authKey(input.payload.hello.imei);
        const auto auth = co_await c.redis().get(authKey);
        const auto separator = auth ? auth->find('|') : std::string_view::npos;
        const std::string nodeId =
            auth && separator != std::string_view::npos
                ? std::string(auth->substr(0, separator))
                : service::common::nextUuidV7();
        const std::string status =
            auth && separator != std::string_view::npos
                ? std::string(auth->substr(separator + 1))
                : "pending";
        auto session = makeSession(nodeId);
        if (status != "approved") {
            auto reply = makeEnvelope(session);
            reply.which_payload = status == "rejected"
                                      ? iot_edge_v1_Envelope_enrollment_rejected_tag
                                      : iot_edge_v1_Envelope_enrollment_pending_tag;
            auto& enrollment = status == "rejected" ? reply.payload.enrollment_rejected
                                                       : reply.payload.enrollment_pending;
            copy(enrollment.code, status);
            copy(enrollment.message, status == "rejected" ? "registration rejected"
                                                            : "registration pending approval");
            co_await send(socket, reply);
            co_await socket.close(1008, status);
            co_return;
        }

        auto ack = makeEnvelope(session);
        ack.which_payload = iot_edge_v1_Envelope_hello_ack_tag;
        protocol::setBytes(&ack.payload.hello_ack.assigned_node_id,
                           sizeof(ack.payload.hello_ack.assigned_node_id.bytes),
                           session.nodeBytes.data(), session.nodeBytes.size());
        ack.payload.hello_ack.session_epoch = session.epoch;
        ack.payload.hello_ack.negotiated_protocol_version = 1;
        ack.payload.hello_ack.heartbeat_interval_sec = 5;
        ack.payload.hello_ack.max_message_size = iot_edge_v1_Envelope_size;
        ack.payload.hello_ack.platform_time_ms = protocol::nowMs();
        co_await send(socket, ack);
        co_await markOnline(c, session);
        co_await drain(c, socket, session);

        while (auto message = co_await socket.read()) {
            if (!message->binary() || !protocol::decode(message->payload(), input) ||
                !validInbound(input, session)) {
                co_await socket.close(1002, "invalid envelope");
                co_return;
            }
            session.inboundSequence = input.sequence;
            co_await publishIngress(c, message->payload());
            co_await handle(c, socket, session, input);
            co_await markOnline(c, session);
            co_await drain(c, socket, session);
        }
        (void)co_await c.redis().del(sessionKey(session.nodeId));
    }

    ruvia::Task<void> terminal(ruvia::Context& c) {
        auto& socket = c.webSocket();
        const auto& query = c.req().valid<TerminalTicketQuery>();
        const std::string ticket(query.ticket()->view());
        const auto ticketKey = "iot:edge:terminal:ticket:" + ticket;
        const auto node = co_await c.redis().getDel(ticketKey);
        if (!node) {
            co_await socket.close(1008, "invalid terminal ticket");
            co_return;
        }
        const std::string nodeId(*node);
        const auto active = co_await c.redis().get(sessionKey(nodeId));
        if (!active) {
            co_await socket.close(1013, "edge node offline");
            co_return;
        }
        const auto terminalBytes = protocol::randomUuidV7Bytes();
        const auto terminalId = protocol::uuidText(terminalBytes.data());
        auto open = protocol::outbound(nodeId);
        open.which_payload = iot_edge_v1_Envelope_terminal_open_tag;
        protocol::setBytes(&open.payload.terminal_open.terminal_id,
                           sizeof(open.payload.terminal_open.terminal_id.bytes),
                           terminalBytes.data(), terminalBytes.size());
        std::uint8_t ticketBytes[16]{};
        protocol::uuidBytes(ticket, ticketBytes);
        protocol::setBytes(&open.payload.terminal_open.ticket,
                           sizeof(open.payload.terminal_open.ticket.bytes), ticketBytes, 16);
        open.payload.terminal_open.columns = 120;
        open.payload.terminal_open.rows = 30;
        co_await queue(c, nodeId, open);
        co_await socket.text("ready");

        while (auto message = co_await socket.read()) {
            if (message->binary() && !message->payload().empty()) {
                std::string_view remaining = message->payload();
                while (!remaining.empty()) {
                    const auto size = std::min<std::size_t>(remaining.size(), 4096);
                    auto data = protocol::outbound(nodeId);
                    data.which_payload = iot_edge_v1_Envelope_terminal_data_tag;
                    protocol::setBytes(&data.payload.terminal_data.terminal_id,
                                       sizeof(data.payload.terminal_data.terminal_id.bytes),
                                       terminalBytes.data(), terminalBytes.size());
                    protocol::setBytes(&data.payload.terminal_data.data,
                                       sizeof(data.payload.terminal_data.data.bytes),
                                       reinterpret_cast<const std::uint8_t*>(remaining.data()),
                                       size);
                    co_await queue(c, nodeId, data);
                    remaining.remove_prefix(size);
                }
            }
            if (co_await drainTerminal(c, socket, terminalId))
                co_return;
        }
        auto close = protocol::outbound(nodeId);
        close.which_payload = iot_edge_v1_Envelope_terminal_close_tag;
        protocol::setBytes(&close.payload.terminal_close.terminal_id,
                           sizeof(close.payload.terminal_close.terminal_id.bytes),
                           terminalBytes.data(), terminalBytes.size());
        copy(close.payload.terminal_close.reason, "browser closed");
        co_await queue(c, nodeId, close);
    }

    static Session makeSession(std::string nodeId) {
        Session result;
        result.nodeId = std::move(nodeId);
        protocol::uuidBytes(result.nodeId, result.nodeBytes.data());
        protocol::uuidBytes(protocol::kBootstrapPlatformId, result.platformBytes.data());
        result.epoch = randomEpoch();
        return result;
    }

    static std::uint64_t randomEpoch() {
        static thread_local std::mt19937_64 random(std::random_device{}());
        auto value = random();
        return value == 0 ? 1 : value;
    }

    static bool platformMatches(const std::uint8_t* value) {
        std::uint8_t expected[16]{};
        return protocol::uuidBytes(protocol::kBootstrapPlatformId, expected) &&
               std::memcmp(value, expected, sizeof(expected)) == 0;
    }

    static bool validInbound(const iot_edge_v1_Envelope& input, const Session& session) {
        return input.protocol_version == 1 && input.node_id.size == 16 &&
               input.platform_id.size == 16 && input.session_epoch == session.epoch &&
               input.sequence > session.inboundSequence &&
               std::memcmp(input.node_id.bytes, session.nodeBytes.data(), 16) == 0 &&
               std::memcmp(input.platform_id.bytes, session.platformBytes.data(), 16) == 0;
    }

    static iot_edge_v1_Envelope makeEnvelope(Session& session) {
        return protocol::outbound(session.nodeId, session.epoch, ++session.outboundSequence);
    }

    static ruvia::Task<void> send(ruvia::WebSocket& socket,
                                  const iot_edge_v1_Envelope& envelope) {
        const auto wire = protocol::encode(envelope);
        if (wire.empty())
            throw std::runtime_error("edge envelope encode failed");
        co_await socket.binary(wire);
    }

    static ruvia::Task<void> markOnline(ruvia::Context& c, const Session& session) {
        const auto key = sessionKey(session.nodeId);
        const auto epoch = std::to_string(session.epoch);
        co_await c.redis().setEx(key, std::chrono::seconds(90), epoch);
    }

    static ruvia::Task<void> publishIngress(ruvia::Context& c, std::string_view wire) {
        std::vector<service::bridge::StreamField> fields;
        fields.push_back({"wire", std::string(wire)});
        (void)co_await service::bridge::redis_async::add(
            c.redis(), kEdgeIngressStream, fields, 100000);
    }

    static std::string sessionKey(std::string_view nodeId) {
        return "iot:edge:session:" + std::string(nodeId);
    }

    static ruvia::Task<void> drain(ruvia::Context& c, ruvia::WebSocket& socket,
                                   Session& session) {
        const auto configCount =
            co_await drainKey(c, socket, session, "iot:edge:config:" + session.nodeId, 64);
        if (configCount != 0)
            session.configSentAtMs = protocol::nowMs();
        (void)co_await drainKey(c, socket, session, "iot:edge:egress:" + session.nodeId, 16);
    }

    static ruvia::Task<int> drainKey(ruvia::Context& c, ruvia::WebSocket& socket,
                                     Session& session, const std::string& key, int limit) {
        int sent = 0;
        for (int count = 0; count < limit; ++count) {
            auto item = co_await c.redis().lpop(key);
            if (!item)
                break;
            iot_edge_v1_Envelope envelope = iot_edge_v1_Envelope_init_zero;
            if (!protocol::decode(*item, envelope))
                continue;
            envelope.session_epoch = session.epoch;
            envelope.sequence = ++session.outboundSequence;
            protocol::setBytes(&envelope.platform_id, sizeof(envelope.platform_id.bytes),
                               session.platformBytes.data(), 16);
            protocol::setBytes(&envelope.node_id, sizeof(envelope.node_id.bytes),
                               session.nodeBytes.data(), 16);
            co_await send(socket, envelope);
            ++sent;
        }
        co_return sent;
    }

    static ruvia::Task<void> queue(ruvia::Context& c, std::string_view nodeId,
                                   const iot_edge_v1_Envelope& envelope) {
        const auto wire = protocol::encode(envelope);
        if (wire.empty())
            throw std::runtime_error("edge terminal envelope encode failed");
        const auto key = "iot:edge:egress:" + std::string(nodeId);
        (void)co_await c.redis().rpush(key, wire);
        co_await c.redis().ltrim(key, -100, -1);
    }

    static ruvia::Task<bool> drainTerminal(ruvia::Context& c, ruvia::WebSocket& socket,
                                           std::string_view terminalId) {
        const auto key = "iot:edge:terminal:out:" + std::string(terminalId);
        for (int count = 0; count < 32; ++count) {
            auto item = co_await c.redis().lpop(key);
            if (!item)
                break;
            if (item->empty())
                continue;
            const auto type = static_cast<unsigned char>((*item)[0]);
            if (type == 0U) {
                co_await socket.binary(std::string_view(item->data() + 1, item->size() - 1));
            } else {
                co_await socket.text(std::string_view(item->data() + 1, item->size() - 1));
                co_await socket.close(1000, "terminal closed");
                co_return true;
            }
        }
        co_return false;
    }

    static ruvia::Task<void> handle(ruvia::Context& c, ruvia::WebSocket& socket,
                                    Session& session, const iot_edge_v1_Envelope& input) {
        switch (input.which_payload) {
        case iot_edge_v1_Envelope_heartbeat_tag: {
            constexpr std::uint64_t retryIntervalMs = 30000;
            const auto now = protocol::nowMs();
            if (session.configSentAtMs == 0 || now - session.configSentAtMs >= retryIntervalMs) {
                (void)co_await edgeConfigService().requeueIfStale(
                    c, session.nodeId, input.payload.heartbeat.active_config_version);
            }
            auto reply = makeEnvelope(session);
            reply.which_payload = iot_edge_v1_Envelope_heartbeat_ack_tag;
            reply.payload.heartbeat_ack.platform_time_ms = protocol::nowMs();
            reply.payload.heartbeat_ack.request_capability_report = !session.capabilitySeen;
            co_await send(socket, reply);
            break;
        }
        case iot_edge_v1_Envelope_capability_report_tag:
            session.capabilitySeen = true;
            break;
        case iot_edge_v1_Envelope_telemetry_batch_tag: {
            auto reply = makeEnvelope(session);
            reply.which_payload = iot_edge_v1_Envelope_telemetry_ack_tag;
            const auto& batch = input.payload.telemetry_batch;
            for (pb_size_t index = 0;
                 index < batch.records_count &&
                 reply.payload.telemetry_ack.accepted_record_ids_count < 1;
                 ++index) {
                const auto& record = batch.records[index];
                if (record.record_id.size != 16)
                    continue;
                auto& accepted = reply.payload.telemetry_ack.accepted_record_ids
                    [reply.payload.telemetry_ack.accepted_record_ids_count++];
                protocol::setBytes(&accepted, sizeof(accepted.bytes), record.record_id.bytes, 16);
            }
            co_await send(socket, reply);
            break;
        }
        case iot_edge_v1_Envelope_raw_packet_tag: {
            const auto& packet = input.payload.raw_packet;
            if (packet.packet_id.size != 16)
                break;
            auto reply = makeEnvelope(session);
            reply.which_payload = iot_edge_v1_Envelope_raw_packet_ack_tag;
            protocol::setBytes(&reply.payload.raw_packet_ack.packet_id,
                               sizeof(reply.payload.raw_packet_ack.packet_id.bytes),
                               packet.packet_id.bytes, 16);
            co_await send(socket, reply);
            break;
        }
        case iot_edge_v1_Envelope_command_result_tag: {
            const auto& result = input.payload.command_result;
            if (result.command_id.size != 16)
                break;
            auto reply = makeEnvelope(session);
            reply.which_payload = iot_edge_v1_Envelope_command_result_ack_tag;
            protocol::setBytes(&reply.payload.command_result_ack.command_id,
                               sizeof(reply.payload.command_result_ack.command_id.bytes),
                               result.command_id.bytes, 16);
            co_await send(socket, reply);
            break;
        }
        case iot_edge_v1_Envelope_ping_tag: {
            auto reply = makeEnvelope(session);
            reply.which_payload = iot_edge_v1_Envelope_pong_tag;
            reply.payload.pong.nonce = input.payload.ping.nonce;
            co_await send(socket, reply);
            break;
        }
        case iot_edge_v1_Envelope_terminal_data_tag:
            co_await saveTerminalData(c, input.payload.terminal_data);
            break;
        case iot_edge_v1_Envelope_terminal_close_tag:
            co_await saveTerminalClose(c, input.payload.terminal_close);
            break;
        default:
            break;
        }
    }

    static ruvia::Task<void> saveTerminalData(ruvia::Context& c,
                                              const iot_edge_v1_TerminalData& data) {
        if (data.terminal_id.size != 16 || data.data.size == 0)
            co_return;
        const auto id = protocol::uuidText(data.terminal_id.bytes);
        const auto key = "iot:edge:terminal:out:" + id;
        std::string value(1, '\0');
        value.append(reinterpret_cast<const char*>(data.data.bytes), data.data.size);
        (void)co_await c.redis().rpush(key, value);
        (void)co_await c.redis().expire(key, std::chrono::seconds(120));
    }

    static ruvia::Task<void> saveTerminalClose(ruvia::Context& c,
                                               const iot_edge_v1_TerminalClose& close) {
        if (close.terminal_id.size != 16)
            co_return;
        const auto id = protocol::uuidText(close.terminal_id.bytes);
        const auto key = "iot:edge:terminal:out:" + id;
        std::string value(1, '\1');
        value += close.reason;
        (void)co_await c.redis().rpush(key, value);
        (void)co_await c.redis().expire(key, std::chrono::seconds(120));
    }

    template <std::size_t Size> static void copy(char (&output)[Size], std::string_view input) {
        const auto size = std::min(input.size(), Size - 1);
        std::memcpy(output, input.data(), size);
        output[size] = '\0';
    }
};

} // namespace service::edge
