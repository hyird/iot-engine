#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <random>
#include <string>
#include <string_view>

#include <ruvia/core/TaskScope.h>
#include <ruvia/web/Controller.h>
#include <terminal.pb.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/edge/config.h"
#include "service/features/edge/protocol.h"
#include "service/features/edge/projector.h"
#include "service/domains/edge/edge.schema.h"

namespace service::edge {

namespace webpb = ::iot::edge::terminal::v1;

class GatewayController final : public ruvia::Controller<GatewayController> {
  public:
    RUVIA_CONTROLLER_GROUP("/edge/v1")
    RUVIA_ROUTES_BEGIN
    const auto webSocketOptions = ruvia::WebSocketRouteOptions{
        .lifecycle = {
            .heartbeat = ruvia::WebSocketHeartbeatPolicy::periodic(
                std::chrono::seconds(30), std::chrono::seconds(15)),
            .closeHandshakeTimeout = std::chrono::seconds(5),
        },
    };
    RUVIA_GET_WS_OPTIONS("/connect", connect, webSocketOptions);
    RUVIA_GET_WS_OPTIONS(
        "/terminal", terminal, webSocketOptions, TerminalTicketValidator);
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
        std::uint64_t onlineMarkedAtMs{};
        bool capabilitySeen{};
    };

    ruvia::Task<void> connect(ruvia::Context& c) {
        auto& socket = c.webSocket();
        auto first = co_await socket.read();
        if (!first || !first->binary()) {
            co_await socket.close(1002, "binary hello required");
            co_return;
        }
        pb::Envelope input;
        if (!protocol::decode(first->payload(), input) ||
            input.protocol_version() != protocol::kProtocolVersion ||
            input.payload_case() != pb::Envelope::kHello || input.platform_id().size() != 16 ||
            !platformMatches(input.platform_id()) || !protocol::validImei(input.hello().imei())) {
            co_await socket.close(1002, "invalid hello");
            co_return;
        }
        co_await publishIngress(c, first->payload());
        const auto authKey = protocol::authKey(input.hello().imei());
        const auto auth = co_await c.redis().get(authKey);
        const auto separator = auth ? auth->find('|') : std::string_view::npos;
        std::string nodeId =
            auth && separator != std::string_view::npos
                ? std::string(auth->substr(0, separator))
                : service::common::nextUuidV7();
        std::string status =
            auth && separator != std::string_view::npos
                ? std::string(auth->substr(separator + 1))
                : "pending";
        auto session = makeSession(nodeId);
        if (status != "approved") {
            co_await sendEnrollment(socket, session, status);
            if (status == "rejected") {
                co_await socket.close(1008, status);
                co_return;
            }

            session.inboundSequence = input.sequence();
            while (auto message = co_await socket.read()) {
                if (!message->binary() || !protocol::decode(message->payload(), input) ||
                    input.protocol_version() != protocol::kProtocolVersion ||
                    input.payload_case() != pb::Envelope::kHeartbeat ||
                    !input.node_id().empty() || input.session_epoch() != 0 ||
                    input.sequence() <= session.inboundSequence ||
                    input.platform_id().size() != 16 ||
                    !platformMatches(input.platform_id())) {
                    co_await socket.close(1002, "invalid pending heartbeat");
                    co_return;
                }
                session.inboundSequence = input.sequence();

                const auto refreshed = co_await c.redis().get(authKey);
                const auto refreshedSeparator =
                    refreshed ? refreshed->find('|') : std::string_view::npos;
                if (refreshed && refreshedSeparator != std::string_view::npos) {
                    nodeId = std::string(refreshed->substr(0, refreshedSeparator));
                    status = std::string(refreshed->substr(refreshedSeparator + 1));
                } else {
                    status = "pending";
                }

                if (status == "approved")
                    break;
                if (status == "rejected") {
                    co_await sendEnrollment(socket, session, status);
                    co_await socket.close(1008, status);
                    co_return;
                }

                auto heartbeat = makeEnvelope(session);
                heartbeat.mutable_heartbeat_ack()->set_platform_time_ms(protocol::nowMs());
                co_await send(socket, heartbeat);
            }
            if (status != "approved")
                co_return;
            session = makeSession(nodeId);
        }

        auto ack = makeEnvelope(session);
        auto* helloAck = ack.mutable_hello_ack();
        helloAck->set_assigned_node_id(
            protocol::bytes(session.nodeBytes.data(), session.nodeBytes.size()));
        helloAck->set_session_epoch(session.epoch);
        helloAck->set_negotiated_protocol_version(protocol::kProtocolVersion);
        helloAck->set_heartbeat_interval_sec(5);
        helloAck->set_max_message_size(static_cast<std::uint32_t>(protocol::kMaxMessageSize));
        helloAck->set_platform_time_ms(protocol::nowMs());
        co_await send(socket, ack);
        co_await markOnline(c, session);
        co_await drain(c, socket, session);

        while (auto message = co_await socket.read()) {
            if (!message->binary() || !protocol::decode(message->payload(), input) ||
                !validInbound(input, session)) {
                co_await socket.close(1002, "invalid envelope");
                co_return;
            }
            session.inboundSequence = input.sequence();
            if (shouldProject(input))
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
        auto* terminalOpen = open.mutable_terminal_open();
        terminalOpen->set_terminal_id(
            protocol::bytes(terminalBytes.data(), terminalBytes.size()));
        std::uint8_t ticketBytes[16]{};
        protocol::uuidBytes(ticket, ticketBytes);
        terminalOpen->set_ticket(protocol::bytes(ticketBytes, 16));
        terminalOpen->set_columns(120);
        terminalOpen->set_rows(30);
        co_await queue(c, nodeId, open);
        webpb::WebTerminalFrame ready;
        ready.mutable_ready();
        co_await sendWebTerminal(socket, ready);

        bool terminalClosed = false;
        ruvia::TaskScope outputScope(c.worker(), c.resource());
        outputScope.spawn(
            pumpTerminal(c, socket, terminalId, outputScope.stopToken(), terminalClosed));
        std::exception_ptr failure;
        std::uint16_t closeCode = 1000;
        std::string closeReason;
        try {
            while (auto message = co_await socket.read()) {
                if (!message->binary()) {
                    closeCode = 1003;
                    closeReason = "terminal frames must use protobuf";
                    break;
                }
                webpb::WebTerminalFrame frame;
                if (!frame.ParseFromArray(message->payload().data(),
                                          static_cast<int>(message->payload().size()))) {
                    closeCode = 1002;
                    closeReason = "invalid terminal protobuf";
                    break;
                }
                if (frame.payload_case() == webpb::WebTerminalFrame::kResize) {
                    const auto& size = frame.resize();
                    if (size.columns() < 20 || size.columns() > 300 || size.rows() < 5 ||
                        size.rows() > 100) {
                        closeCode = 1002;
                        closeReason = "invalid terminal size";
                        break;
                    }
                    auto resize = protocol::outbound(nodeId);
                    auto* terminalResize = resize.mutable_terminal_resize();
                    terminalResize->set_terminal_id(
                        protocol::bytes(terminalBytes.data(), terminalBytes.size()));
                    terminalResize->set_columns(size.columns());
                    terminalResize->set_rows(size.rows());
                    co_await queue(c, nodeId, resize);
                } else if (frame.payload_case() == webpb::WebTerminalFrame::kData) {
                    std::string_view remaining = frame.data().data();
                    while (!remaining.empty()) {
                        const auto size = std::min<std::size_t>(remaining.size(), 4096);
                        auto data = protocol::outbound(nodeId);
                        auto* terminalData = data.mutable_terminal_data();
                        terminalData->set_terminal_id(
                            protocol::bytes(terminalBytes.data(), terminalBytes.size()));
                        terminalData->set_data(remaining.data(), size);
                        co_await queue(c, nodeId, data);
                        remaining.remove_prefix(size);
                    }
                } else if (frame.payload_case() == webpb::WebTerminalFrame::kClose) {
                    closeReason = "browser closed";
                    break;
                } else {
                    closeCode = 1002;
                    closeReason = "invalid terminal payload";
                    break;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        outputScope.requestStop();
        try {
            co_await outputScope.join();
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
        if (!terminalClosed) {
            auto close = protocol::outbound(nodeId);
            auto* terminalClose = close.mutable_terminal_close();
            terminalClose->set_terminal_id(
                protocol::bytes(terminalBytes.data(), terminalBytes.size()));
            terminalClose->set_reason("browser closed");
            co_await queue(c, nodeId, close);
        }
        (void)co_await c.redis().del("iot:edge:terminal:out:" + terminalId);
        if (!terminalClosed && !closeReason.empty())
            co_await socket.close(closeCode, closeReason);
        if (failure)
            std::rethrow_exception(failure);
    }

    static Session makeSession(std::string nodeId) {
        Session result;
        result.nodeId = std::move(nodeId);
        protocol::uuidBytes(result.nodeId, result.nodeBytes.data());
        protocol::uuidBytes(protocol::kBootstrapPlatformId, result.platformBytes.data());
        result.epoch = randomEpoch();
        return result;
    }

    static ruvia::Task<void> sendEnrollment(ruvia::WebSocket& socket, Session& session,
                                            std::string_view status) {
        auto reply = makeEnvelope(session);
        auto* enrollment = status == "rejected" ? reply.mutable_enrollment_rejected()
                                                 : reply.mutable_enrollment_pending();
        enrollment->set_code(status);
        enrollment->set_message(status == "rejected" ? "registration rejected"
                                                       : "registration pending approval");
        co_await send(socket, reply);
    }

    static std::uint64_t randomEpoch() {
        static thread_local std::mt19937_64 random(std::random_device{}());
        auto value = random();
        return value == 0 ? 1 : value;
    }

    static bool platformMatches(std::string_view value) {
        std::uint8_t expected[16]{};
        return protocol::uuidBytes(protocol::kBootstrapPlatformId, expected) &&
               value == protocol::bytes(expected, sizeof(expected));
    }

    static bool validInbound(const pb::Envelope& input, const Session& session) {
        return input.protocol_version() == protocol::kProtocolVersion &&
               input.node_id().size() == 16 &&
               input.platform_id().size() == 16 && input.session_epoch() == session.epoch &&
               input.sequence() > session.inboundSequence &&
               input.node_id() == protocol::bytes(session.nodeBytes.data(), 16) &&
               input.platform_id() == protocol::bytes(session.platformBytes.data(), 16);
    }

    static bool shouldProject(const pb::Envelope& input) {
        switch (input.payload_case()) {
        case pb::Envelope::kHeartbeat:
        case pb::Envelope::kCapabilityReport:
        case pb::Envelope::kNetworkConfigResult:
        case pb::Envelope::kFirmwareUpdateResult:
        case pb::Envelope::kModemControlResult:
        case pb::Envelope::kPlatformConfigResult:
        case pb::Envelope::kConfigApplied:
        case pb::Envelope::kConfigRejected:
        case pb::Envelope::kTelemetryBatch:
        case pb::Envelope::kCommandResult:
            return true;
        default:
            return false;
        }
    }

    static pb::Envelope makeEnvelope(Session& session) {
        return protocol::outbound(session.nodeId, session.epoch, ++session.outboundSequence);
    }

    static ruvia::Task<void> send(ruvia::WebSocket& socket, const pb::Envelope& envelope) {
        const auto wire = protocol::encode(envelope);
        if (wire.empty())
            throw std::runtime_error("edge envelope encode failed");
        co_await socket.binary(wire);
    }

    static ruvia::Task<void> markOnline(ruvia::Context& c, Session& session) {
        constexpr std::uint64_t refreshIntervalMs = 10000;
        const auto now = protocol::nowMs();
        if (session.onlineMarkedAtMs != 0 &&
            now - session.onlineMarkedAtMs < refreshIntervalMs)
            co_return;
        const auto key = sessionKey(session.nodeId);
        const auto epoch = std::to_string(session.epoch);
        co_await c.redis().setEx(key, std::chrono::seconds(90), epoch);
        session.onlineMarkedAtMs = now;
    }

    static ruvia::Task<void> publishIngress(ruvia::Context& c, std::string_view wire) {
        std::vector<service::message::StreamField> fields;
        fields.push_back({"wire", std::string(wire)});
        (void)co_await service::message::redis::add(
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
            pb::Envelope envelope;
            if (!protocol::decode(*item, envelope))
                continue;
            envelope.set_session_epoch(session.epoch);
            envelope.set_sequence(++session.outboundSequence);
            envelope.set_platform_id(protocol::bytes(session.platformBytes.data(), 16));
            envelope.set_node_id(protocol::bytes(session.nodeBytes.data(), 16));
            co_await send(socket, envelope);
            ++sent;
        }
        co_return sent;
    }

    static ruvia::Task<void> queue(ruvia::Context& c, std::string_view nodeId,
                                   const pb::Envelope& envelope) {
        const auto wire = protocol::encode(envelope);
        if (wire.empty())
            throw std::runtime_error("edge terminal envelope encode failed");
        const auto key = "iot:edge:egress:" + std::string(nodeId);
        (void)co_await c.redis().rpush(key, wire);
        co_await c.redis().ltrim(key, -100, -1);
    }

    static ruvia::Task<void> sendWebTerminal(ruvia::WebSocket& socket,
                                              const webpb::WebTerminalFrame& frame) {
        std::string wire;
        if (!frame.SerializeToString(&wire))
            throw std::runtime_error("web terminal protobuf encode failed");
        co_await socket.binary(wire);
    }

    static ruvia::Task<void> pumpTerminal(ruvia::Context& c, ruvia::WebSocket& socket,
                                          std::string terminalId, ruvia::StopToken stopToken,
                                          bool& terminalClosed) {
        const std::string key = "iot:edge:terminal:out:" + terminalId;
        const auto redis = c.redis();
        while (!stopToken.stopRequested()) {
            auto item = co_await redis.lpop(key);
            if (!item) {
                (void)co_await ruvia::sleepFor(c.worker(), std::chrono::milliseconds(10));
                continue;
            }
            webpb::WebTerminalFrame frame;
            if (!frame.ParseFromArray(item->data(), static_cast<int>(item->size()))) {
                webpb::WebTerminalFrame close;
                close.mutable_close()->set_reason("terminal stream protocol error");
                co_await sendWebTerminal(socket, close);
                terminalClosed = true;
                co_await socket.close(1011, "terminal stream protocol error");
                co_return;
            }
            co_await socket.binary(*item);
            if (frame.payload_case() == webpb::WebTerminalFrame::kClose) {
                terminalClosed = true;
                co_await socket.close(1000, "terminal closed");
                co_return;
            }
        }
    }

    static ruvia::Task<void> handle(ruvia::Context& c, ruvia::WebSocket& socket,
                                    Session& session, const pb::Envelope& input) {
        switch (input.payload_case()) {
        case pb::Envelope::kHeartbeat: {
            constexpr std::uint64_t retryIntervalMs = 30000;
            const auto now = protocol::nowMs();
            if (session.configSentAtMs == 0 || now - session.configSentAtMs >= retryIntervalMs) {
                (void)co_await configService().requeueIfStale(
                    c, session.nodeId, input.heartbeat().active_config_version());
            }
            auto reply = makeEnvelope(session);
            auto* heartbeatAck = reply.mutable_heartbeat_ack();
            heartbeatAck->set_platform_time_ms(protocol::nowMs());
            heartbeatAck->set_request_capability_report(!session.capabilitySeen);
            heartbeatAck->set_request_endpoint_status(input.heartbeat().managed_endpoint_count() > 0);
            co_await send(socket, reply);
            break;
        }
        case pb::Envelope::kCapabilityReport:
            session.capabilitySeen = true;
            break;
        case pb::Envelope::kTelemetryBatch: {
            auto reply = makeEnvelope(session);
            auto* telemetryAck = reply.mutable_telemetry_ack();
            for (const auto& record : input.telemetry_batch().records()) {
                if (record.record_id().size() != 16)
                    continue;
                telemetryAck->add_accepted_record_ids(record.record_id());
                break;
            }
            co_await send(socket, reply);
            break;
        }
        case pb::Envelope::kRawPacket: {
            const auto& packet = input.raw_packet();
            if (packet.packet_id().size() != 16)
                break;
            auto reply = makeEnvelope(session);
            reply.mutable_raw_packet_ack()->set_packet_id(packet.packet_id());
            co_await send(socket, reply);
            break;
        }
        case pb::Envelope::kCommandResult: {
            const auto& result = input.command_result();
            if (result.command_id().size() != 16)
                break;
            auto reply = makeEnvelope(session);
            reply.mutable_command_result_ack()->set_command_id(result.command_id());
            co_await send(socket, reply);
            break;
        }
        case pb::Envelope::kPing: {
            if (input.ping().nonce() != 0) {
                auto reply = makeEnvelope(session);
                reply.mutable_pong()->set_nonce(input.ping().nonce());
                co_await send(socket, reply);
            }
            break;
        }
        case pb::Envelope::kTerminalData:
            co_await saveTerminalData(c, input.terminal_data());
            break;
        case pb::Envelope::kTerminalClose:
            co_await saveTerminalClose(c, input.terminal_close());
            break;
        case pb::Envelope::kLogResult:
            co_await saveLogResult(c, input.log_result());
            break;
        default:
            break;
        }
    }

    static ruvia::Task<void> saveLogResult(ruvia::Context& c, const pb::LogResult& result) {
        if (result.request_id().size() != 16)
            co_return;
        std::string wire;
        if (!result.SerializeToString(&wire))
            co_return;
        const auto id = protocol::uuidText(result.request_id());
        co_await c.redis().setEx("iot:edge:logs:" + id, std::chrono::seconds(60), wire);
    }

    static ruvia::Task<void> saveTerminalData(ruvia::Context& c, const pb::TerminalData& data) {
        if (data.terminal_id().size() != 16 || data.data().empty())
            co_return;
        const auto id = protocol::uuidText(data.terminal_id());
        const auto key = "iot:edge:terminal:out:" + id;
        webpb::WebTerminalFrame frame;
        frame.mutable_data()->set_data(data.data());
        std::string wire;
        if (!frame.SerializeToString(&wire))
            co_return;
        (void)co_await c.redis().rpush(key, wire);
        (void)co_await c.redis().expire(key, std::chrono::seconds(120));
    }

    static ruvia::Task<void> saveTerminalClose(ruvia::Context& c,
                                               const pb::TerminalClose& close) {
        if (close.terminal_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(close.terminal_id());
        const auto key = "iot:edge:terminal:out:" + id;
        webpb::WebTerminalFrame frame;
        auto* terminalClose = frame.mutable_close();
        terminalClose->set_exit_code(close.exit_code());
        terminalClose->set_reason(close.reason());
        std::string wire;
        if (!frame.SerializeToString(&wire))
            co_return;
        (void)co_await c.redis().rpush(key, wire);
        (void)co_await c.redis().expire(key, std::chrono::seconds(120));
    }
};

} // namespace service::edge
