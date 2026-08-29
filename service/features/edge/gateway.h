#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
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
#include "service/features/edge/session.h"
#include "service/domains/edge/edge.schema.h"

namespace service::edge {

namespace webpb = ::iot::edge::terminal::v1;

class GatewayController final : public ruvia::Controller<GatewayController> {
  public:
    RUVIA_CONTROLLER_GROUP("/edge/v1")
    RUVIA_ROUTES_BEGIN
    const auto webSocketOptions = ruvia::WebSocketRouteConfig{
        .lifecycle = {
            .heartbeat = {
                .pingInterval = std::chrono::seconds(30),
                .pongTimeout = std::chrono::seconds(15),
            },
            .closeHandshakeTimeout = std::chrono::seconds(5),
        },
    };
    RUVIA_GET_WS_OPTIONS("/connect", connect, webSocketOptions);
    RUVIA_GET_WS_OPTIONS(
        "/terminal", terminal, webSocketOptions, TerminalTicketValidator);
    RUVIA_ROUTES_END

  private:
    struct TelemetryReceipt {
        std::array<std::uint8_t, 16> recordId{};
        std::uint64_t acknowledgedAtMs{};
        bool occupied{};
    };

    struct Session {
        std::string nodeId;
        std::array<std::uint8_t, 16> nodeBytes{};
        std::array<std::uint8_t, 16> platformBytes{};
        std::uint32_t protocolVersion{protocol::kProtocolVersion};
        std::uint64_t epoch{};
        std::uint64_t inboundSequence{};
        std::uint64_t outboundSequence{};
        std::uint64_t configSentAtMs{};
        std::deque<std::string> outbound;
        std::array<TelemetryReceipt, 64> telemetryReceipts{};
        std::size_t nextTelemetryReceipt{};
        bool capabilitySeen{};
    };

    struct TerminalSession {
        std::uint32_t columns{120};
        std::uint32_t rows{30};
        std::uint32_t protocolVersion{protocol::kProtocolVersion};
        std::uint64_t inputSequence{};
        bool opened{};
        bool nodeClosed{};
    };

    ruvia::Task<void> connect(ruvia::Context& c) {
        auto& socket = c.webSocket();
        auto first = co_await socket.read();
        if (!first || !first->binary()) {
            co_await socket.close(
                ruvia::WebSocketCloseOptions{.code = 1002, .reason = "binary hello required"});
            co_return;
        }
        pb::Envelope input;
        if (!protocol::decode(first->payload(), input) ||
            !protocol::supportsProtocolVersion(input.protocol_version()) ||
            input.payload_case() != pb::Envelope::kHello ||
            !protocol::validSessionPlatformId(input.platform_id()) ||
            !protocol::validImei(input.hello().imei())) {
            co_await socket.close(
                ruvia::WebSocketCloseOptions{.code = 1002, .reason = "invalid hello"});
            co_return;
        }
        co_await publishIngress(c, first->payload(), protocol::nowMs());
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
        auto session =
            makeSession(nodeId, input.protocol_version(), input.platform_id());
        if (status != "approved") {
            co_await sendEnrollment(socket, session, status);
            if (status == "rejected") {
                co_await socket.close(ruvia::WebSocketCloseOptions{.code = 1008, .reason = status});
                co_return;
            }

            session.inboundSequence = input.sequence();
            while (auto message = co_await socket.read()) {
                if (!message->binary() || !protocol::decode(message->payload(), input) ||
                    input.protocol_version() != session.protocolVersion ||
                    input.payload_case() != pb::Envelope::kHeartbeat ||
                    !input.node_id().empty() || input.session_epoch() != 0 ||
                    input.sequence() <= session.inboundSequence ||
                    input.platform_id() !=
                        protocol::bytes(session.platformBytes.data(),
                                        session.platformBytes.size())) {
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1002,
                        .reason = "invalid pending heartbeat",
                    });
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
                    co_await socket.close(
                        ruvia::WebSocketCloseOptions{.code = 1008, .reason = status});
                    co_return;
                }

                auto heartbeat = makeEnvelope(session);
                heartbeat.mutable_heartbeat_ack()->set_platform_time_ms(protocol::nowMs());
                co_await send(socket, heartbeat);
            }
            if (status != "approved")
                co_return;
            session = makeSession(
                nodeId, session.protocolVersion,
                protocol::bytes(session.platformBytes.data(), session.platformBytes.size()));
        }

        auto ack = makeEnvelope(session);
        auto* helloAck = ack.mutable_hello_ack();
        helloAck->set_assigned_node_id(
            protocol::bytes(session.nodeBytes.data(), session.nodeBytes.size()));
        helloAck->set_session_epoch(session.epoch);
        helloAck->set_negotiated_protocol_version(session.protocolVersion);
        helloAck->set_heartbeat_interval_sec(5);
        helloAck->set_max_message_size(static_cast<std::uint32_t>(protocol::kMaxMessageSize));
        helloAck->set_platform_time_ms(protocol::nowMs());
        co_await send(socket, ack);
        (void)co_await session_state::claim(c.redis(), session.nodeId, session.epoch,
                                            session.protocolVersion);
        std::exception_ptr sessionFailure;
        // Egress must not wait for the node to speak first: an idle terminal only
        // produces a heartbeat every few seconds, which would stall keystrokes for
        // that whole interval. The pump owns the egress key from here on.
        ruvia::TaskScope egressScope(
            c.worker(), ruvia::TaskScopeOptions{.resource = c.resource()});
        egressScope.spawn(pumpEgress(c, socket, session, egressScope.stopToken()));
        try {
            while (auto message = co_await socket.read()) {
                if (!message->binary() || !protocol::decode(message->payload(), input) ||
                    !validInbound(input, session)) {
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1002,
                        .reason = "invalid envelope",
                    });
                    break;
                }
                if (!co_await session_state::refresh(c.redis(), session.nodeId, session.epoch,
                                                     session.protocolVersion)) {
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1008,
                        .reason = "session replaced",
                    });
                    break;
                }
                session.inboundSequence = input.sequence();
                const auto telemetry = telemetryDecision(session, input);
                if (shouldProject(input) && telemetry.publish)
                    co_await publishIngress(c, message->payload(), protocol::nowMs());
                if (telemetry.acknowledge)
                    co_await handle(c, session, input);
            }
        } catch (...) {
            sessionFailure = std::current_exception();
        }
        egressScope.requestStop();
        try {
            co_await egressScope.join();
        } catch (...) {
            if (!sessionFailure)
                sessionFailure = std::current_exception();
        }
        try {
            (void)co_await session_state::release(c.redis(), session.nodeId, session.epoch,
                                                  session.protocolVersion);
        } catch (...) {
            if (!sessionFailure)
                sessionFailure = std::current_exception();
        }
        if (sessionFailure)
            std::rethrow_exception(sessionFailure);
    }

    ruvia::Task<void> terminal(ruvia::Context& c) {
        auto& socket = c.webSocket();
        const auto& query = c.req().validated<TerminalTicketQuery>();
        const std::string ticket(query.get<"ticket">()->view());
        const auto ticketKey = "iot:edge:terminal:ticket:" + ticket;
        const auto node = co_await c.redis().getDel(ticketKey);
        if (!node) {
            co_await socket.close(ruvia::WebSocketCloseOptions{
                .code = 1008,
                .reason = "invalid terminal ticket",
            });
            co_return;
        }
        const std::string nodeId(*node);
        const auto active = co_await c.redis().get(sessionKey(nodeId));
        if (!active) {
            co_await socket.close(
                ruvia::WebSocketCloseOptions{.code = 1013, .reason = "edge node offline"});
            co_return;
        }
        const auto terminalBytes = protocol::randomUuidV7Bytes();
        const auto terminalId = protocol::uuidText(terminalBytes.data());
        const std::string nodeSession(*active);
        const auto sessionProtocolVersion =
            session_state::protocolVersion(nodeSession);
        if (!sessionProtocolVersion ||
            !protocol::supportsProtocolVersion(*sessionProtocolVersion)) {
            co_await socket.close(ruvia::WebSocketCloseOptions{
                .code = 1013,
                .reason = "edge node protocol state unavailable",
            });
            co_return;
        }
        ruvia::RedisSetOptions terminalOptions;
        terminalOptions.expiration =
            ruvia::RedisSetExpiration::expiresAfter(std::chrono::seconds(120));
        co_await c.redis().set(terminalSessionKey(nodeId, terminalId), nodeSession,
                               std::move(terminalOptions));
        auto open = protocol::outbound(nodeId);
        auto* terminalOpen = open.mutable_terminal_open();
        terminalOpen->set_terminal_id(
            protocol::bytes(terminalBytes.data(), terminalBytes.size()));
        if (*sessionProtocolVersion <= 3)
            terminalOpen->set_ticket(ticket);
        terminalOpen->set_columns(120);
        terminalOpen->set_rows(30);
        co_await queue(c, nodeId, open);

        TerminalSession terminalSession;
        terminalSession.protocolVersion = *sessionProtocolVersion;
        if (terminalSession.protocolVersion <= 3) {
            webpb::WebTerminalFrame ready;
            ready.mutable_ready();
            co_await sendWebTerminal(socket, ready);
            terminalSession.opened = true;
        }
        ruvia::TaskScope outputScope(
            c.worker(), ruvia::TaskScopeOptions{.resource = c.resource()});
        outputScope.spawn(pumpTerminal(c, socket, nodeId, nodeSession, terminalId,
                                       terminalBytes, outputScope.stopToken(), terminalSession));
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
                if (!terminalSession.opened) {
                    closeCode = 1002;
                    closeReason = "terminal is not ready";
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
                    terminalSession.columns = size.columns();
                    terminalSession.rows = size.rows();
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
                        if (terminalSession.protocolVersion >= 5)
                            terminalData->set_sequence(++terminalSession.inputSequence);
                        co_await queue(c, nodeId, data);
                        if (terminalSession.protocolVersion >= 5 &&
                            !co_await waitTerminalInputAck(
                                c, nodeId, nodeSession, terminalId,
                                terminalSession.inputSequence, terminalSession)) {
                            closeCode = 1013;
                            closeReason = terminalSession.nodeClosed
                                              ? "edge node closed terminal"
                                              : "terminal input acknowledgement timed out";
                            break;
                        }
                        remaining.remove_prefix(size);
                    }
                    if (!closeReason.empty())
                        break;
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
        if (!terminalSession.nodeClosed) {
            auto close = protocol::outbound(nodeId);
            auto* terminalClose = close.mutable_terminal_close();
            terminalClose->set_terminal_id(
                protocol::bytes(terminalBytes.data(), terminalBytes.size()));
            terminalClose->set_reason("browser closed");
            co_await queue(c, nodeId, close);
        }
        co_await releaseTerminalSession(c, nodeId, terminalId, nodeSession);
        if (!terminalSession.nodeClosed && !closeReason.empty())
            co_await socket.close(
                ruvia::WebSocketCloseOptions{.code = closeCode, .reason = closeReason});
        if (failure)
            std::rethrow_exception(failure);
    }

    static Session makeSession(std::string nodeId, std::uint32_t protocolVersion,
                               std::string_view platformId) {
        Session result;
        result.nodeId = std::move(nodeId);
        result.protocolVersion = protocolVersion;
        protocol::uuidBytes(result.nodeId, result.nodeBytes.data());
        std::memcpy(result.platformBytes.data(), platformId.data(), result.platformBytes.size());
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

    static bool validInbound(const pb::Envelope& input, const Session& session) {
        return input.protocol_version() == session.protocolVersion &&
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
        case pb::Envelope::kDeviceStatusReport:
            return true;
        default:
            return false;
        }
    }

    struct TelemetryDecision {
        bool publish{true};
        bool acknowledge{true};
    };

    static TelemetryDecision telemetryDecision(Session& session,
                                                const pb::Envelope& input) {
        if (input.payload_case() != pb::Envelope::kTelemetryBatch ||
            input.telemetry_batch().records().empty() ||
            input.telemetry_batch().records(0).record_id().size() != 16)
            return {};
        constexpr std::uint64_t retryAckIntervalMs = 1000;
        const auto recordId = input.telemetry_batch().records(0).record_id();
        const auto now = protocol::nowMs();
        for (auto& receipt : session.telemetryReceipts) {
            if (!receipt.occupied ||
                std::memcmp(receipt.recordId.data(), recordId.data(), 16) != 0)
                continue;
            if (now - receipt.acknowledgedAtMs < retryAckIntervalMs)
                return {.publish = false, .acknowledge = false};
            receipt.acknowledgedAtMs = now;
            return {.publish = false, .acknowledge = true};
        }
        auto& receipt = session.telemetryReceipts[session.nextTelemetryReceipt];
        std::memcpy(receipt.recordId.data(), recordId.data(), 16);
        receipt.acknowledgedAtMs = now;
        receipt.occupied = true;
        session.nextTelemetryReceipt =
            (session.nextTelemetryReceipt + 1) % session.telemetryReceipts.size();
        return {};
    }

    static pb::Envelope makeEnvelope(Session& session) {
        auto result = protocol::outbound(session.nodeId, session.epoch,
                                         ++session.outboundSequence,
                                         session.protocolVersion);
        result.set_platform_id(
            protocol::bytes(session.platformBytes.data(), session.platformBytes.size()));
        return result;
    }

    static ruvia::Task<void> send(ruvia::WebSocket& socket, const pb::Envelope& envelope) {
        const auto wire = protocol::encode(envelope);
        if (wire.empty())
            throw std::runtime_error("edge envelope encode failed");
        co_await socket.binary(wire);
    }

    static void enqueue(Session& session, const pb::Envelope& envelope) {
        auto wire = protocol::encode(envelope);
        if (wire.empty())
            throw std::runtime_error("edge envelope encode failed");
        session.outbound.emplace_back(std::move(wire));
    }

    static ruvia::Task<void> publishIngress(ruvia::Context& c, std::string_view wire,
                                            std::int64_t receivedAtMs) {
        std::vector<service::message::StreamField> fields;
        fields.push_back({"wire", std::string(wire)});
        fields.push_back({"received_at_ms", std::to_string(receivedAtMs)});
        (void)co_await service::message::redis::add(
            c.redis(), kEdgeIngressStream, fields, 100000);
    }

    static std::string sessionKey(std::string_view nodeId) {
        return session_state::key(nodeId);
    }

    // Runs beside the session read loop on the same worker, so it shares
    // session.outboundSequence and the local queue without a lock. It is the only
    // coroutine that writes to the node WebSocket after the hello acknowledgement;
    // concurrent writes can otherwise kill the pump while the read loop keeps the
    // node looking online.
    static ruvia::Task<void> pumpEgress(ruvia::Context& c, ruvia::WebSocket& socket,
                                        Session& session, ruvia::StopToken stopToken) {
        const std::string terminalKey = terminalInputKey(session.nodeId);
        const std::string configKey = "iot:edge:config:" + session.nodeId;
        const std::string egressKey = "iot:edge:egress:" + session.nodeId;
        const std::array<std::string_view, 3> blockingKeys{
            configKey, terminalKey, egressKey};
        const auto blockingRedis = c.redis().withOptions(
            ruvia::OperationOptions{.stopToken = stopToken});
        std::exception_ptr failure;
        try {
            while (!stopToken.stopRequested()) {
                int replies = 0;
                while (!session.outbound.empty() && replies < 64) {
                    auto wire = std::move(session.outbound.front());
                    session.outbound.pop_front();
                    co_await socket.binary(wire);
                    ++replies;
                }
                const auto configs = co_await drainKey(c, socket, session, configKey, 64);
                if (configs != 0)
                    session.configSentAtMs = protocol::nowMs();
                const auto keystrokes = co_await drainKey(c, socket, session, terminalKey, 64);
                const auto commands = co_await drainKey(c, socket, session, egressKey, 64);
                if (replies + configs + keystrokes + commands == 0) {
                    auto item = co_await blockingRedis.blpop(
                        blockingKeys, ruvia::RedisBlockWait::indefinitely());
                    if (item) {
                        const auto key = item->key();
                        const auto sent = co_await deliverQueuedItem(
                            c, socket, session, key, item->value());
                        if (sent && key == configKey)
                            session.configSentAtMs = protocol::nowMs();
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            try {
                co_await socket.close(
                    ruvia::WebSocketCloseOptions{.code = 1011, .reason = "edge egress failed"});
            } catch (...) {
            }
            std::rethrow_exception(failure);
        }
    }

    static ruvia::Task<bool> deliverQueuedItem(
        ruvia::Context& c, ruvia::WebSocket& socket, Session& session,
        std::string_view key, std::string_view item) {
        pb::Envelope envelope;
        if (!protocol::decode(item, envelope))
            co_return false;
        protocol::bindSession(
            envelope,
            protocol::bytes(session.platformBytes.data(), session.platformBytes.size()),
            protocol::bytes(session.nodeBytes.data(), session.nodeBytes.size()), session.epoch,
            ++session.outboundSequence, session.protocolVersion);
        std::exception_ptr failure;
        try {
            co_await send(socket, envelope);
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            // Popping transfers ownership to this session. Put the command back
            // before forcing a reconnect so a transient socket failure cannot
            // leave its database task pending forever.
            (void)co_await c.redis().lpush(key, item);
            std::rethrow_exception(failure);
        }
        co_return true;
    }

    static ruvia::Task<int> drainKey(ruvia::Context& c, ruvia::WebSocket& socket,
                                     Session& session, const std::string& key, int limit) {
        int sent = 0;
        for (int count = 0; count < limit; ++count) {
            auto item = co_await c.redis().lpop(key);
            if (!item)
                break;
            if (co_await deliverQueuedItem(c, socket, session, key, *item))
                ++sent;
        }
        co_return sent;
    }

    static ruvia::Task<void> queue(ruvia::Context& c, std::string_view nodeId,
                                   const pb::Envelope& envelope) {
        const auto wire = protocol::encode(envelope);
        if (wire.empty())
            throw std::runtime_error("edge terminal envelope encode failed");
        // Terminal input gets its own downstream key: the shared egress list is
        // head-trimmed by command dispatch, which would hand the node a terminal_data
        // whose terminal_open it never saw. Overflow here drops the newest envelope
        // instead, so whatever the node does receive stays a valid prefix.
        const auto key = terminalInputKey(nodeId);
        constexpr std::int64_t maxBacklog = 4096;
        const auto length = co_await c.redis().rpush(key, wire);
        if (length == 1)
            (void)co_await c.redis().expire(key, std::chrono::seconds(120));
        if (length > maxBacklog) {
            (void)co_await c.redis().rpop(key);
            throw std::runtime_error("edge terminal input backlog exceeded");
        }
    }

    static std::string terminalInputKey(std::string_view nodeId) {
        return "iot:edge:terminal:in:" + std::string(nodeId);
    }

    static std::string terminalSessionKey(std::string_view nodeId,
                                          std::string_view terminalId) {
        return "iot:edge:terminal:session:" + std::string(nodeId) + ":" +
               std::string(terminalId);
    }

    static std::string terminalOutputKey(std::string_view nodeId,
                                          std::string_view terminalId) {
        return "iot:edge:terminal:out:" + std::string(nodeId) + ":" +
               std::string(terminalId);
    }

    static std::string terminalInputAckKey(std::string_view nodeId,
                                           std::string_view terminalId) {
        return "iot:edge:terminal:in-ack:" + std::string(nodeId) + ":" +
               std::string(terminalId);
    }

    static std::string terminalOutputSequenceKey(std::string_view nodeId,
                                                  std::string_view terminalId) {
        return "iot:edge:terminal:out-seq:" + std::string(nodeId) + ":" +
               std::string(terminalId);
    }

    static ruvia::Task<bool> waitTerminalInputAck(
        ruvia::Context& c, std::string_view nodeId, std::string_view nodeSession,
        std::string_view terminalId, std::uint64_t sequence,
        const TerminalSession& terminalSession) {
        const auto key = terminalInputAckKey(nodeId, terminalId);
        const auto ownerKey = terminalSessionKey(nodeId, terminalId);
        const auto expected = std::to_string(sequence);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (terminalSession.nodeClosed)
                co_return false;
            const auto value = co_await c.redis().get(key);
            if (value && std::string_view(value->data(), value->size()) == expected)
                co_return true;
            const auto owner = co_await c.redis().get(ownerKey);
            if (!owner || std::string_view(owner->data(), owner->size()) != nodeSession)
                co_return false;
            (void)co_await ruvia::sleepFor(c.worker(), std::chrono::milliseconds(10));
        }
        co_return false;
    }

    static ruvia::Task<void> releaseTerminalSession(ruvia::Context& c,
                                                    std::string_view nodeId,
                                                    std::string_view terminalId,
                                                    std::string_view nodeSession) {
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1], KEYS[2], KEYS[3], KEYS[4])
return 1
)lua";
        const auto ownershipKey = terminalSessionKey(nodeId, terminalId);
        const auto outputKey = terminalOutputKey(nodeId, terminalId);
        const auto inputAckKey = terminalInputAckKey(nodeId, terminalId);
        const auto outputSequenceKey = terminalOutputSequenceKey(nodeId, terminalId);
        const std::string_view keys[]{ownershipKey, outputKey, inputAckKey,
                                      outputSequenceKey};
        const std::string_view arguments[]{nodeSession};
        const auto reply = co_await c.redis().eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
            service::message::redis::throwValue("release edge terminal session", reply);
    }

    static ruvia::Task<void> sendWebTerminal(ruvia::WebSocket& socket,
                                              const webpb::WebTerminalFrame& frame) {
        std::string wire;
        if (!frame.SerializeToString(&wire))
            throw std::runtime_error("web terminal protobuf encode failed");
        co_await socket.binary(wire);
    }

    static ruvia::Task<void> pumpTerminal(
        ruvia::Context& c, ruvia::WebSocket& socket, std::string nodeId,
        std::string nodeSession, std::string terminalId,
        std::array<std::uint8_t, 16> terminalBytes, ruvia::StopToken stopToken,
        TerminalSession& terminalSession) {
        const std::string key = terminalOutputKey(nodeId, terminalId);
        const std::string ownershipKey = terminalSessionKey(nodeId, terminalId);
        const auto redis = c.redis();
        const auto openDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(15);
        auto nextKeepalive =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!stopToken.stopRequested()) {
            auto item = co_await redis.lpop(key);
            if (item) {
                webpb::WebTerminalFrame frame;
                if (!frame.ParseFromArray(item->data(), static_cast<int>(item->size()))) {
                    webpb::WebTerminalFrame close;
                    close.mutable_close()->set_reason("terminal stream protocol error");
                    co_await sendWebTerminal(socket, close);
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1011,
                        .reason = "terminal stream protocol error",
                    });
                    co_return;
                }
                if (terminalSession.protocolVersion >= 5 &&
                    frame.payload_case() == webpb::WebTerminalFrame::kData &&
                    frame.data().sequence() == 0) {
                    webpb::WebTerminalFrame close;
                    close.mutable_close()->set_reason("terminal output sequence missing");
                    co_await sendWebTerminal(socket, close);
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1011,
                        .reason = "terminal output sequence missing",
                    });
                    co_return;
                }
                if (frame.payload_case() == webpb::WebTerminalFrame::kReady) {
                    terminalSession.opened = true;
                } else if (frame.payload_case() == webpb::WebTerminalFrame::kClose) {
                    terminalSession.nodeClosed = true;
                }
                co_await socket.binary(*item);
                if (terminalSession.protocolVersion >= 5 &&
                    frame.payload_case() == webpb::WebTerminalFrame::kData) {
                    auto ack = protocol::outbound(nodeId);
                    auto* terminalAck = ack.mutable_terminal_data_ack();
                    terminalAck->set_terminal_id(
                        protocol::bytes(terminalBytes.data(), terminalBytes.size()));
                    terminalAck->set_sequence(frame.data().sequence());
                    co_await queue(c, nodeId, ack);
                }
                if (frame.payload_case() == webpb::WebTerminalFrame::kClose) {
                    co_await socket.close(
                        ruvia::WebSocketCloseOptions{.code = 1000, .reason = "terminal closed"});
                    co_return;
                }
            }

            if (!terminalSession.opened &&
                std::chrono::steady_clock::now() >= openDeadline) {
                webpb::WebTerminalFrame close;
                close.mutable_close()->set_reason("terminal open timed out");
                co_await sendWebTerminal(socket, close);
                co_await socket.close(ruvia::WebSocketCloseOptions{
                    .code = 1013,
                    .reason = "terminal open timed out",
                });
                co_return;
            }

            if (terminalSession.opened &&
                std::chrono::steady_clock::now() >= nextKeepalive) {
                const auto active = co_await redis.get(sessionKey(nodeId));
                if (!active ||
                    std::string_view(active->data(), active->size()) !=
                        std::string_view(nodeSession)) {
                    webpb::WebTerminalFrame close;
                    close.mutable_close()->set_reason("edge node connection lost");
                    co_await sendWebTerminal(socket, close);
                    terminalSession.nodeClosed = true;
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1013,
                        .reason = "edge node connection lost",
                    });
                    co_return;
                }
                const auto owner = co_await redis.get(ownershipKey);
                if (!owner ||
                    std::string_view(owner->data(), owner->size()) !=
                        std::string_view(nodeSession)) {
                    webpb::WebTerminalFrame close;
                    close.mutable_close()->set_reason("terminal ownership lost");
                    co_await sendWebTerminal(socket, close);
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1013,
                        .reason = "terminal ownership lost",
                    });
                    co_return;
                }
                (void)co_await redis.expire(ownershipKey, std::chrono::seconds(120));

                // WebSocket ping/pong only keeps the browser connection alive. A resize is a
                // harmless application frame that also keeps the node-to-ttyd terminal path
                // active while the user is reading output or the browser tab is backgrounded.
                auto keepalive = protocol::outbound(nodeId);
                auto* resize = keepalive.mutable_terminal_resize();
                resize->set_terminal_id(
                    protocol::bytes(terminalBytes.data(), terminalBytes.size()));
                resize->set_columns(terminalSession.columns);
                resize->set_rows(terminalSession.rows);
                co_await queue(c, nodeId, keepalive);
                nextKeepalive =
                    std::chrono::steady_clock::now() + std::chrono::seconds(20);
            }

            if (!item)
                (void)co_await ruvia::sleepFor(c.worker(), std::chrono::milliseconds(10));
        }
    }

    static ruvia::Task<void> handle(ruvia::Context& c, Session& session,
                                    const pb::Envelope& input) {
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
            heartbeatAck->set_request_device_status(input.heartbeat().managed_endpoint_count() > 0);
            enqueue(session, reply);
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
            enqueue(session, reply);
            break;
        }
        case pb::Envelope::kRawPacket: {
            const auto& packet = input.raw_packet();
            if (packet.packet_id().size() != 16)
                break;
            auto reply = makeEnvelope(session);
            reply.mutable_raw_packet_ack()->set_packet_id(packet.packet_id());
            enqueue(session, reply);
            break;
        }
        case pb::Envelope::kCommandResult: {
            const auto& result = input.command_result();
            if (result.command_id().size() != 16)
                break;
            auto reply = makeEnvelope(session);
            reply.mutable_command_result_ack()->set_command_id(result.command_id());
            enqueue(session, reply);
            break;
        }
        case pb::Envelope::kPing: {
            // Older nodes send zero-nonce application pings while a remote
            // terminal is open. They still use the pong as their application
            // liveness signal, so ignoring nonce zero forces a reconnect after
            // the negotiated watchdog interval and strands the browser terminal.
            auto reply = makeEnvelope(session);
            reply.mutable_pong()->set_nonce(input.ping().nonce());
            enqueue(session, reply);
            break;
        }
        case pb::Envelope::kTerminalData:
            co_await saveTerminalData(c, session, input.terminal_data());
            break;
        case pb::Envelope::kTerminalDataAck:
            co_await saveTerminalDataAck(c, session, input.terminal_data_ack());
            break;
        case pb::Envelope::kTerminalOpened:
            co_await saveTerminalOpened(c, session, input.terminal_opened());
            break;
        case pb::Envelope::kTerminalClose:
            co_await saveTerminalClose(c, session, input.terminal_close());
            break;
        case pb::Envelope::kLogResult:
            co_await saveLogResult(c, input.log_result());
            break;
        case pb::Envelope::kLogLevelResult:
            co_await saveLogLevelResult(c, input.log_level_result());
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
        ruvia::RedisSetOptions options;
        options.expiration =
            ruvia::RedisSetExpiration::expiresAfter(std::chrono::seconds(60));
        co_await c.redis().set("iot:edge:logs:" + id, wire, std::move(options));
    }

    static ruvia::Task<void> saveLogLevelResult(ruvia::Context& c,
                                                const pb::LogLevelResult& result) {
        if (result.request_id().size() != 16)
            co_return;
        std::string wire;
        if (!result.SerializeToString(&wire))
            co_return;
        const auto id = protocol::uuidText(result.request_id());
        ruvia::RedisSetOptions options;
        options.expiration =
            ruvia::RedisSetExpiration::expiresAfter(std::chrono::seconds(60));
        co_await c.redis().set("iot:edge:logs:level:" + id, wire, std::move(options));
    }

    static ruvia::Task<void> saveTerminalFrame(ruvia::Context& c, const Session& session,
                                               std::string_view terminalId,
                                               const webpb::WebTerminalFrame& frame) {
        std::string wire;
        if (!frame.SerializeToString(&wire))
            co_return;
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('RPUSH', KEYS[2], ARGV[2])
redis.call('EXPIRE', KEYS[1], ARGV[3])
redis.call('EXPIRE', KEYS[2], ARGV[3])
return 1
)lua";
        const auto ownershipKey = terminalSessionKey(session.nodeId, terminalId);
        const auto outputKey = terminalOutputKey(session.nodeId, terminalId);
        const auto epoch =
            session_state::value(session.epoch, session.protocolVersion);
        const std::string ttl = "120";
        const std::string_view keys[]{ownershipKey, outputKey};
        const std::string_view arguments[]{epoch, wire, ttl};
        const auto reply = co_await c.redis().eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
            service::message::redis::throwValue("append edge terminal output", reply);
    }

    static ruvia::Task<void> saveTerminalData(ruvia::Context& c, const Session& session,
                                              const pb::TerminalData& data) {
        if (data.terminal_id().size() != 16 || data.data().empty())
            co_return;
        const auto id = protocol::uuidText(data.terminal_id());
        if (session.protocolVersion < 5) {
            webpb::WebTerminalFrame legacyFrame;
            legacyFrame.mutable_data()->set_data(data.data());
            co_await saveTerminalFrame(c, session, id, legacyFrame);
            co_return;
        }
        if (data.sequence() == 0)
            throw std::runtime_error("edge terminal output sequence missing");
        webpb::WebTerminalFrame frame;
        frame.mutable_data()->set_data(data.data());
        frame.mutable_data()->set_sequence(data.sequence());
        std::string wire;
        if (!frame.SerializeToString(&wire))
            co_return;
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
local previous = tonumber(redis.call('GET', KEYS[3]) or '0')
local sequence = tonumber(ARGV[3])
if sequence == previous then return 2 end
if sequence ~= previous + 1 then return -1 end
redis.call('RPUSH', KEYS[2], ARGV[2])
redis.call('SET', KEYS[3], ARGV[3], 'EX', ARGV[4])
redis.call('EXPIRE', KEYS[1], ARGV[4])
redis.call('EXPIRE', KEYS[2], ARGV[4])
return 1
)lua";
        const auto ownershipKey = terminalSessionKey(session.nodeId, id);
        const auto outputKey = terminalOutputKey(session.nodeId, id);
        const auto sequenceKey = terminalOutputSequenceKey(session.nodeId, id);
        const auto epoch =
            session_state::value(session.epoch, session.protocolVersion);
        const auto sequence = std::to_string(data.sequence());
        const std::string ttl = "120";
        const std::string_view keys[]{ownershipKey, outputKey, sequenceKey};
        const std::string_view arguments[]{epoch, wire, sequence, ttl};
        const auto reply = co_await c.redis().eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
            service::message::redis::throwValue("append sequenced terminal output", reply);
        if (reply.integer() < 0)
            throw std::runtime_error("edge terminal output sequence mismatch");
    }

    static ruvia::Task<void> saveTerminalDataAck(
        ruvia::Context& c, const Session& session, const pb::TerminalDataAck& ack) {
        if (session.protocolVersion < 5)
            co_return;
        if (ack.terminal_id().size() != 16 || ack.sequence() == 0)
            co_return;
        const auto id = protocol::uuidText(ack.terminal_id());
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
local previous = tonumber(redis.call('GET', KEYS[2]) or '0')
local sequence = tonumber(ARGV[2])
if sequence == previous then return 1 end
if sequence ~= previous + 1 then return -1 end
redis.call('SET', KEYS[2], ARGV[2], 'EX', ARGV[3])
redis.call('EXPIRE', KEYS[1], ARGV[3])
return 1
)lua";
        const auto ownershipKey = terminalSessionKey(session.nodeId, id);
        const auto ackKey = terminalInputAckKey(session.nodeId, id);
        const auto epoch =
            session_state::value(session.epoch, session.protocolVersion);
        const auto sequence = std::to_string(ack.sequence());
        const std::string ttl = "120";
        const std::string_view keys[]{ownershipKey, ackKey};
        const std::string_view arguments[]{epoch, sequence, ttl};
        const auto reply = co_await c.redis().eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
            service::message::redis::throwValue("advance terminal input ack", reply);
        if (reply.integer() < 0)
            throw std::runtime_error("edge terminal input acknowledgement mismatch");
    }

    static ruvia::Task<void> saveTerminalOpened(ruvia::Context& c,
                                                const Session& session,
                                                const pb::TerminalOpened& opened) {
        if (opened.terminal_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(opened.terminal_id());
        webpb::WebTerminalFrame frame;
        frame.mutable_ready();
        co_await saveTerminalFrame(c, session, id, frame);
    }

    static ruvia::Task<void> saveTerminalClose(ruvia::Context& c,
                                               const Session& session,
                                               const pb::TerminalClose& close) {
        if (close.terminal_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(close.terminal_id());
        webpb::WebTerminalFrame frame;
        auto* terminalClose = frame.mutable_close();
        terminalClose->set_exit_code(close.exit_code());
        terminalClose->set_reason(close.reason());
        co_await saveTerminalFrame(c, session, id, frame);
    }
};

} // namespace service::edge
