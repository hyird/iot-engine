#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <terminal.pb.h>

#include <ruvia/core/TaskScope.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/edge/edge.protocol.h"
#include "service/features/edge/edge.runtime.h"
#include "service/features/edge/edge.service.h"
#include "service/features/edge/firmware/firmware.service.h"
#include "service/features/edge/gateway/gateway.service.h"
#include "service/features/edge/gateway/gateway.types.h"
#include "service/features/edge/session/session.service.h"
#include "service/features/edge/terminal/terminal.service.h"
#include "service/features/live/live.service.h"

namespace service::edge {

class GatewayTicketValidator final : public ruvia::Middleware<GatewayTicketValidator> {
  public:
    RUVIA_VALIDATE_QUERY(gateway::TerminalTicketQuery, RUVIA_RULE(ticket, RUVIA_REQUIRED("终端票据不能为空"), RUVIA_CUSTOM("终端票据无效", service::common::isUuidField)));
};

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
        "/terminal",
        terminal,
        webSocketOptions,
        GatewayTicketValidator
    );
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
        std::uint32_t protocolVersion{ protocol::kProtocolVersion };
        std::size_t workerIndex{};
        std::uint64_t epoch{};
        std::uint64_t inboundSequence{};
        std::uint64_t outboundSequence{};
        std::uint64_t configSentAtMs{};
        std::deque<std::string> outbound;
        std::array<TelemetryReceipt, 64> telemetryReceipts{};
        std::size_t nextTelemetryReceipt{};
        std::array<std::uint8_t, 16> firmwareRequestId{};
        std::filesystem::path firmwarePath;
        std::uint64_t firmwareSize{};
        bool firmwareSourceLoaded{};
        bool capabilitySeen{};
        std::chrono::steady_clock::time_point lastInbound{ std::chrono::steady_clock::now() };
    };

    struct LiveSession {
        ruvia::Context* context{};
        ruvia::WebSocket* socket{};
        Session* session{};
        ruvia::TaskScope* scope{};
        bool active{ true };
        bool flushPending{};
        bool flushing{};
    };

    struct TerminalSession {
        std::uint32_t columns{ 120 };
        std::uint32_t rows{ 30 };
        std::uint32_t protocolVersion{ protocol::kProtocolVersion };
        std::uint64_t inputSequence{};
        bool opened{};
        bool nodeClosed{};
    };

    ruvia::Task<void> connect(ruvia::Context& c) {
        auto& socket = c.webSocket();
        auto& dispatcher = c.workerState<SessionDispatcher>();
        const auto workerIndex = dispatcher.workerIndex();
        auto first = co_await socket.read();
        if (!first || !first->binary()) {
            co_await socket.close(
                ruvia::WebSocketCloseOptions{ .code = 1002, .reason = "binary hello required" }
            );
            co_return;
        }
        pb::Envelope input;
        if (!protocol::decode(first->payload(), input) ||
            !protocol::supportsProtocolVersion(input.protocol_version()) ||
            input.payload_case() != pb::Envelope::kHello ||
            !protocol::validSessionPlatformId(input.platform_id()) ||
            !protocol::validImei(input.hello().imei())) {
            co_await socket.close(
                ruvia::WebSocketCloseOptions{ .code = 1002, .reason = "invalid hello" }
            );
            co_return;
        }
        if (!co_await publishIngress(c, workerIndex, first->payload(), service::message::utcNowMilliseconds())) {
            co_await socket.close(ruvia::WebSocketCloseOptions{
                .code = 1008,
                .reason = "worker lease unavailable",
            });
            co_return;
        }
        const std::string imei(input.hello().imei());
        const auto enrollment = co_await gateway::GatewayService::loadEnrollment(c, imei);
        std::string nodeId = enrollment.nodeId;
        std::string status = enrollment.status;
        auto session = makeSession(
            nodeId,
            input.protocol_version(),
            input.platform_id(),
            workerIndex
        );
        if (status != "approved") {
            co_await sendEnrollment(socket, session);

            session.inboundSequence = input.sequence();
            while (auto message = co_await socket.read()) {
                if (!message->binary() || !protocol::decode(message->payload(), input) ||
                    input.protocol_version() != session.protocolVersion ||
                    input.payload_case() != pb::Envelope::kHeartbeat ||
                    !input.node_id().empty() || input.session_epoch() != 0 ||
                    input.sequence() <= session.inboundSequence ||
                    input.platform_id() !=
                        protocol::bytes(session.platformBytes.data(), session.platformBytes.size())) {
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1002,
                        .reason = "invalid pending heartbeat",
                    });
                    co_return;
                }
                session.inboundSequence = input.sequence();
                session.lastInbound = std::chrono::steady_clock::now();

                const auto refreshed = co_await gateway::GatewayService::loadEnrollment(c, imei, nodeId);
                nodeId = refreshed.nodeId;
                status = refreshed.status;
                if (status == "approved") {
                    break;
                }

                auto heartbeat = makeEnvelope(session);
                heartbeat.mutable_heartbeat_ack()->set_platform_time_ms(service::message::utcNowMilliseconds());
                co_await send(socket, heartbeat);
            }
            if (status != "approved") {
                co_return;
            }
            session = makeSession(
                nodeId,
                session.protocolVersion,
                protocol::bytes(session.platformBytes.data(), session.platformBytes.size()),
                workerIndex
            );
        }

        if (!co_await session_state::claim(
                c.redis(),
                session.nodeId,
                session.epoch,
                session.protocolVersion,
                session.workerIndex
            )) {
            co_await socket.close(ruvia::WebSocketCloseOptions{
                .code = 1008,
                .reason = "session replaced",
            });
            co_return;
        }
        // Rebuild this Worker's local metadata before any subsequent node payload.
        // Both events use the same worker-isolated Stream, preserving their order.
        if (!co_await projector_stream::publishMetadata(
                c.redis(),
                session.workerIndex,
                session.nodeId
            )) {
            (void)co_await session_state::release(
                c.redis(),
                session.nodeId,
                session.epoch,
                session.protocolVersion,
                session.workerIndex
            );
            co_await socket.close(ruvia::WebSocketCloseOptions{
                .code = 1008,
                .reason = "worker lease lost",
            });
            co_return;
        }
        auto ack = makeEnvelope(session);
        auto* helloAck = ack.mutable_hello_ack();
        helloAck->set_assigned_node_id(
            protocol::bytes(session.nodeBytes.data(), session.nodeBytes.size())
        );
        helloAck->set_session_epoch(session.epoch);
        helloAck->set_negotiated_protocol_version(session.protocolVersion);
        helloAck->set_heartbeat_interval_sec(300);
        helloAck->set_max_message_size(static_cast<std::uint32_t>(protocol::kMaxMessageSize));
        helloAck->set_platform_time_ms(service::message::utcNowMilliseconds());
        co_await send(socket, ack);
        std::exception_ptr sessionFailure;
        // Egress must not wait for the node to speak first. This worker's own
        // dispatcher wakes only sessions accepted by this worker.
        ruvia::TaskScope egressScope(
            c.worker(),
            ruvia::TaskScopeOptions{ .resource = c.pool() }
        );
        auto live = std::make_shared<LiveSession>(LiveSession{
            .context = &c,
            .socket = &socket,
            .session = &session,
            .scope = &egressScope,
        });
        bool registered = false;
        try {
            std::weak_ptr<LiveSession> weak = live;
            dispatcher.registerSession(
                session.nodeId,
                session.epoch,
                [weak] {
                    if (const auto current = weak.lock()) {
                        requestFlush(current);
                    }
                },
                [weak](std::string_view) {
                    if (const auto current = weak.lock()) {
                        if (!current->active) {
                            return;
                        }
                        current->active = false;
                        current->scope->requestStop();
                        // SessionDispatcher::failSessions is invoked by this same
                        // Service Worker. Abort wakes the connection's read
                        // loop without moving the socket across workers.
                        current->socket->abort();
                    }
                }
            );
            registered = true;
            egressScope.spawn(maintainSession(live));
            // Reliable per-node queues may already contain work from while the
            // node was offline; connection establishment is itself a wakeup.
            requestFlush(live);
            while (auto message = co_await socket.read()) {
                if (!message->binary() || !protocol::decode(message->payload(), input) ||
                    !validInbound(input, session)) {
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1002,
                        .reason = "invalid envelope",
                    });
                    break;
                }
                if (!co_await session_state::refresh(c.redis(), session.nodeId, session.epoch, session.protocolVersion, session.workerIndex)) {
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1008,
                        .reason = "session replaced",
                    });
                    break;
                }
                session.inboundSequence = input.sequence();
                const auto telemetry = telemetryDecision(session, input);
                session.lastInbound = std::chrono::steady_clock::now();
                if (shouldProject(input) && telemetry.publish &&
                    !co_await publishIngress(c, session.workerIndex, message->payload(), service::message::utcNowMilliseconds())) {
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1008,
                        .reason = "worker lease lost",
                    });
                    break;
                }
                if (telemetry.acknowledge) {
                    co_await handle(c, session, input);
                }
                requestFlush(live);
            }
        } catch (...) {
            sessionFailure = std::current_exception();
        }
        live->active = false;
        if (registered) {
            dispatcher.unregisterSession(session.nodeId, session.epoch);
        }
        egressScope.requestStop();
        try {
            co_await egressScope.join();
        } catch (...) {
            if (!sessionFailure) {
                sessionFailure = std::current_exception();
            }
        }
        try {
            (void)co_await session_state::release(c.redis(), session.nodeId, session.epoch, session.protocolVersion, session.workerIndex);
        } catch (...) {
            if (!sessionFailure) {
                sessionFailure = std::current_exception();
            }
        }
        if (sessionFailure) {
            std::rethrow_exception(sessionFailure);
        }
    }

    ruvia::Task<void> terminal(ruvia::Context& c) {
        auto& socket = c.webSocket();
        const auto& query = c.req().validated<gateway::TerminalTicketQuery>();
        const std::string ticket(query.get<"ticket">()->view());
        const auto node = co_await terminal_state::TerminalService::consumeTicket(c, ticket);
        if (!node) {
            co_await socket.close(ruvia::WebSocketCloseOptions{
                .code = 1008,
                .reason = "invalid terminal ticket",
            });
            co_return;
        }
        const std::string nodeId(*node);
        const auto active = co_await terminal_state::TerminalService::findNodeSession(c, nodeId);
        if (!active) {
            co_await socket.close(
                ruvia::WebSocketCloseOptions{ .code = 1013, .reason = "edge node offline" }
            );
            co_return;
        }
        const auto terminalId = service::common::nextUuidV7();
        std::array<std::uint8_t, 16> terminalBytes{};
        (void)service::common::uuidBytes(terminalId, terminalBytes.data());
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
        co_await terminal_state::TerminalService::registerSession(c, nodeId, terminalId, nodeSession);
        auto open = service::edge::protocol::outbound(service::common::nextUuidV7(), service::message::utcNowMilliseconds(), service::edge::protocol::platformId(), nodeId);
        auto* terminalOpen = open.mutable_terminal_open();
        terminalOpen->set_terminal_id(
            protocol::bytes(terminalBytes.data(), terminalBytes.size())
        );
        if (*sessionProtocolVersion <= 3) {
            terminalOpen->set_ticket(ticket);
        }
        terminalOpen->set_columns(120);
        terminalOpen->set_rows(30);
        co_await terminal_state::TerminalService::enqueueInput(c, nodeId, open);

        TerminalSession terminalSession;
        terminalSession.protocolVersion = *sessionProtocolVersion;
        if (terminalSession.protocolVersion <= 3) {
            webpb::WebTerminalFrame ready;
            ready.mutable_ready();
            co_await sendWebTerminal(socket, ready);
            terminalSession.opened = true;
        }
        ruvia::TaskScope outputScope(
            c.worker(),
            ruvia::TaskScopeOptions{ .resource = c.pool() }
        );
        outputScope.spawn(pumpTerminal(c, socket, nodeId, nodeSession, terminalId, terminalBytes, outputScope.stopToken(), terminalSession));
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
                if (!frame.ParseFromArray(message->payload().data(), static_cast<int>(message->payload().size()))) {
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
                    auto resize = service::edge::protocol::outbound(service::common::nextUuidV7(), service::message::utcNowMilliseconds(), service::edge::protocol::platformId(), nodeId);
                    auto* terminalResize = resize.mutable_terminal_resize();
                    terminalResize->set_terminal_id(
                        protocol::bytes(terminalBytes.data(), terminalBytes.size())
                    );
                    terminalResize->set_columns(size.columns());
                    terminalResize->set_rows(size.rows());
                    terminalSession.columns = size.columns();
                    terminalSession.rows = size.rows();
                    co_await terminal_state::TerminalService::enqueueInput(c, nodeId, resize);
                } else if (frame.payload_case() == webpb::WebTerminalFrame::kData) {
                    std::string_view remaining = frame.data().data();
                    while (!remaining.empty()) {
                        const auto size = std::min<std::size_t>(remaining.size(), 4096);
                        auto data = service::edge::protocol::outbound(service::common::nextUuidV7(), service::message::utcNowMilliseconds(), service::edge::protocol::platformId(), nodeId);
                        auto* terminalData = data.mutable_terminal_data();
                        terminalData->set_terminal_id(
                            protocol::bytes(terminalBytes.data(), terminalBytes.size())
                        );
                        terminalData->set_data(remaining.data(), size);
                        if (terminalSession.protocolVersion >= 5) {
                            terminalData->set_sequence(++terminalSession.inputSequence);
                        }
                        co_await terminal_state::TerminalService::enqueueInput(c, nodeId, data);
                        if (terminalSession.protocolVersion >= 5 &&
                            !co_await waitTerminalInputAck(
                                c,
                                nodeId,
                                nodeSession,
                                terminalId,
                                terminalSession.inputSequence,
                                terminalSession
                            )) {
                            closeCode = 1013;
                            closeReason = terminalSession.nodeClosed
                                ? "edge node closed terminal"
                                : "terminal input acknowledgement timed out";
                            break;
                        }
                        remaining.remove_prefix(size);
                    }
                    if (!closeReason.empty()) {
                        break;
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
            if (!failure) {
                failure = std::current_exception();
            }
        }
        if (!terminalSession.nodeClosed) {
            auto close = service::edge::protocol::outbound(service::common::nextUuidV7(), service::message::utcNowMilliseconds(), service::edge::protocol::platformId(), nodeId);
            auto* terminalClose = close.mutable_terminal_close();
            terminalClose->set_terminal_id(
                protocol::bytes(terminalBytes.data(), terminalBytes.size())
            );
            terminalClose->set_reason("browser closed");
            co_await terminal_state::TerminalService::enqueueInput(c, nodeId, close);
        }
        co_await terminal_state::TerminalService::releaseTerminalSession(c, nodeId, terminalId, nodeSession);
        if (!terminalSession.nodeClosed && !closeReason.empty()) {
            co_await socket.close(
                ruvia::WebSocketCloseOptions{ .code = closeCode, .reason = closeReason }
            );
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    static Session makeSession(std::string nodeId, std::uint32_t protocolVersion, std::string_view platformId, std::size_t workerIndex) {
        Session result;
        result.nodeId = std::move(nodeId);
        result.protocolVersion = protocolVersion;
        result.workerIndex = workerIndex;
        protocol::uuidBytes(result.nodeId, result.nodeBytes.data());
        std::memcpy(result.platformBytes.data(), platformId.data(), result.platformBytes.size());
        result.epoch = randomEpoch();
        return result;
    }

    static ruvia::Task<void> sendEnrollment(ruvia::WebSocket& socket, Session& session) {
        auto reply = makeEnvelope(session);
        auto* enrollment = reply.mutable_enrollment_pending();
        enrollment->set_code("pending");
        enrollment->set_message("registration pending approval");
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
            case pb::Envelope::kVpnConfigResult:
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
        bool publish{ true };
        bool acknowledge{ true };
    };

    static TelemetryDecision telemetryDecision(Session& session, const pb::Envelope& input) {
        if (input.payload_case() != pb::Envelope::kTelemetryBatch ||
            input.telemetry_batch().records().empty() ||
            input.telemetry_batch().records(0).record_id().size() != 16) {
            return {};
        }
        constexpr std::uint64_t retryAckIntervalMs = 1000;
        const auto recordId = input.telemetry_batch().records(0).record_id();
        const auto now = service::message::utcNowMilliseconds();
        for (auto& receipt : session.telemetryReceipts) {
            if (!receipt.occupied ||
                std::memcmp(receipt.recordId.data(), recordId.data(), 16) != 0) {
                continue;
            }
            if (now - receipt.acknowledgedAtMs < retryAckIntervalMs) {
                return { .publish = false, .acknowledge = false };
            }
            receipt.acknowledgedAtMs = now;
            return { .publish = false, .acknowledge = true };
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
        auto result = service::edge::protocol::outbound(service::common::nextUuidV7(), service::message::utcNowMilliseconds(), service::edge::protocol::platformId(), session.nodeId, session.epoch, ++session.outboundSequence, session.protocolVersion);
        result.set_platform_id(
            protocol::bytes(session.platformBytes.data(), session.platformBytes.size())
        );
        return result;
    }

    static ruvia::Task<void> send(ruvia::WebSocket& socket, const pb::Envelope& envelope) {
        const auto wire = protocol::encode(envelope);
        if (wire.empty()) {
            throw std::runtime_error("edge envelope encode failed");
        }
        co_await socket.binary(wire);
    }

    static void enqueue(Session& session, const pb::Envelope& envelope) {
        auto wire = protocol::encode(envelope);
        if (wire.empty()) {
            throw std::runtime_error("edge envelope encode failed");
        }
        session.outbound.emplace_back(std::move(wire));
    }

    static ruvia::Task<bool> publishIngress(ruvia::Context& c, std::size_t workerIndex, std::string_view wire, std::int64_t receivedAtMs) {
        co_return co_await projector_stream::publishIngress(
            c.redis(),
            workerIndex,
            wire,
            receivedAtMs
        );
    }

    static std::string sessionKey(std::string_view nodeId) {
        return session_state::key(nodeId);
    }

    // This worker's SessionDispatcher wakes the worker-local session when one of its
    // reliable queues changes. Redis reads are non-blocking and reuse this
    // worker's ordinary pool; no callback or socket crosses worker boundaries.
    static void requestFlush(const std::shared_ptr<LiveSession>& live) {
        if (!live->active || live->scope->stopRequested()) {
            return;
        }
        live->flushPending = true;
        if (live->flushing) {
            return;
        }
        live->flushing = true;
        try {
            live->scope->spawn(flushEgress(live));
        } catch (...) {
            live->flushing = false;
            throw;
        }
    }

    static ruvia::Task<void> maintainSession(std::shared_ptr<LiveSession> live) {
        // A live socket object is not proof of a responsive device. Deployed
        // 0.3.38 firmware already answers application Ping. Only actual inbound
        // messages renew the routing lease in the read loop.
        try {
            while (live->active && !live->scope->stopRequested()) {
                (void)co_await ruvia::sleepFor(live->context->worker(), std::chrono::seconds(20), live->scope->stopToken());
                if (!live->active || live->scope->stopRequested()) {
                    break;
                }
                const auto& session = *live->session;
                if (std::chrono::steady_clock::now() - session.lastInbound >=
                    std::chrono::seconds(60)) {
                    live->socket->abort();
                    break;
                }
                // This also services the old firmware's application watchdog
                // without requesting extra business telemetry.
                auto keepalive = makeEnvelope(*live->session);
                keepalive.mutable_ping()->set_nonce(keepalive.sequence());
                enqueue(*live->session, keepalive);
                requestFlush(live);
            }
        } catch (...) {
            live->socket->abort();
        }
    }

    static ruvia::Task<void> flushEgress(std::shared_ptr<LiveSession> live) {
        auto& c = *live->context;
        auto& socket = *live->socket;
        auto& session = *live->session;
        const std::string terminalKey = terminal_state::terminalInputKey(session.nodeId);
        const std::string configKey = "iot:edge:config:" + session.nodeId;
        const std::string egressKey = "iot:edge:egress:" + session.nodeId;
        const std::string commandKey = "iot:v2:edge:commands:" + session.nodeId;
        std::exception_ptr failure;
        try {
            while (live->active && !live->scope->stopRequested() &&
                   live->flushPending) {
                live->flushPending = false;
                for (;;) {
                    int replies = 0;
                    while (!session.outbound.empty() && replies < 64) {
                        auto wire = std::move(session.outbound.front());
                        session.outbound.pop_front();
                        co_await socket.binary(wire);
                        ++replies;
                    }
                    const auto configs =
                        co_await drainKey(c, socket, session, configKey, 64);
                    if (configs != 0) {
                        session.configSentAtMs = service::message::utcNowMilliseconds();
                    }
                    const auto keystrokes =
                        co_await drainKey(c, socket, session, terminalKey, 64);
                    const auto commands =
                        co_await drainKey(c, socket, session, commandKey, 64);
                    const auto tasks = co_await drainKey(c, socket, session, egressKey, 64);
                    if (replies + configs + keystrokes + commands + tasks == 0) {
                        break;
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        live->flushing = false;
        if (failure) {
            live->active = false;
            try {
                co_await socket.close(
                    ruvia::WebSocketCloseOptions{ .code = 1011,
                                                  .reason = "edge egress failed" }
                );
            } catch (...) {
                socket.abort();
            }
            std::rethrow_exception(failure);
        }
    }

    static ruvia::Task<bool> deliverQueuedItem(
        ruvia::Context& c,
        ruvia::WebSocket& socket,
        Session& session,
        std::string_view key,
        std::string_view item
    ) {
        pb::Envelope envelope;
        if (!protocol::decode(item, envelope)) {
            co_return false;
        }
        if (envelope.has_command_request()) {
            const auto id = protocol::uuidText(envelope.command_request().command_id());
            // Claim physical transmission once, including across reconnects. Other Edge tasks
            // retain their existing retry contract; device control cannot safely be replayed.
            if (!co_await gateway::GatewayService::claimCommand(c, id, session.nodeId)) {
                co_return false;
            }
        }
        protocol::bindSession(
            envelope,
            protocol::bytes(session.platformBytes.data(), session.platformBytes.size()),
            protocol::bytes(session.nodeBytes.data(), session.nodeBytes.size()),
            session.epoch,
            ++session.outboundSequence,
            session.protocolVersion
        );
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
            if (!envelope.has_command_request()) {
                (void)co_await c.redis().lpush(key, item);
            }
            std::rethrow_exception(failure);
        }
        co_return true;
    }

    static ruvia::Task<int> drainKey(ruvia::Context& c, ruvia::WebSocket& socket, Session& session, const std::string& key, int limit) {
        int sent = 0;
        for (int count = 0; count < limit; ++count) {
            auto item = co_await c.redis().lpop(key);
            if (!item) {
                break;
            }
            if (co_await deliverQueuedItem(c, socket, session, key, *item)) {
                ++sent;
            }
        }
        co_return sent;
    }

    static terminal_state::ConnectionIdentity terminalIdentity(const Session& session) {
        return {session.nodeId, session.epoch, session.protocolVersion, session.workerIndex};
    }
    static ruvia::Task<bool> waitTerminalInputAck(
        ruvia::Context& c,
        std::string_view nodeId,
        std::string_view nodeSession,
        std::string_view terminalId,
        std::uint64_t sequence,
        const TerminalSession& terminalSession
    ) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (terminalSession.nodeClosed) {
                co_return false;
            }
            const auto status = co_await terminal_state::TerminalService::inputAckStatus(
                c, nodeId, terminalId, nodeSession, sequence);
            if (status == terminal_state::InputAckStatus::Acknowledged) co_return true;
            if (status == terminal_state::InputAckStatus::OwnershipLost) co_return false;
            (void)co_await ruvia::sleepFor(c.worker(), std::chrono::milliseconds(10));
        }
        co_return false;
    }

    static ruvia::Task<void> sendWebTerminal(ruvia::WebSocket& socket, const webpb::WebTerminalFrame& frame) {
        std::string wire;
        if (!frame.SerializeToString(&wire)) {
            throw std::runtime_error("web terminal protobuf encode failed");
        }
        co_await socket.binary(wire);
    }

    static ruvia::Task<void> pumpTerminal(
        ruvia::Context& c,
        ruvia::WebSocket& socket,
        std::string nodeId,
        std::string nodeSession,
        std::string terminalId,
        std::array<std::uint8_t, 16> terminalBytes,
        ruvia::StopToken stopToken,
        TerminalSession& terminalSession
    ) {
        const auto openDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(15);
        auto nextKeepalive =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!stopToken.stopRequested()) {
            auto item = co_await terminal_state::TerminalService::takeOutput(c, nodeId, terminalId);
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
                    auto ack = service::edge::protocol::outbound(service::common::nextUuidV7(), service::message::utcNowMilliseconds(), service::edge::protocol::platformId(), nodeId);
                    auto* terminalAck = ack.mutable_terminal_data_ack();
                    terminalAck->set_terminal_id(
                        protocol::bytes(terminalBytes.data(), terminalBytes.size())
                    );
                    terminalAck->set_sequence(frame.data().sequence());
                    co_await terminal_state::TerminalService::enqueueInput(c, nodeId, ack);
                }
                if (frame.payload_case() == webpb::WebTerminalFrame::kClose) {
                    co_await socket.close(
                        ruvia::WebSocketCloseOptions{ .code = 1000, .reason = "terminal closed" }
                    );
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
                const auto refreshed = co_await terminal_state::TerminalService::refreshSession(c, nodeId, terminalId, nodeSession);
                if (refreshed < 0) {
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
                if (refreshed == 0) {
                    webpb::WebTerminalFrame close;
                    close.mutable_close()->set_reason("terminal ownership lost");
                    co_await sendWebTerminal(socket, close);
                    co_await socket.close(ruvia::WebSocketCloseOptions{
                        .code = 1013,
                        .reason = "terminal ownership lost",
                    });
                    co_return;
                }
                // WebSocket ping/pong only keeps the browser connection alive. A resize is a
                // harmless application frame that also keeps the node-to-ttyd terminal path
                // active while the user is reading output or the browser tab is backgrounded.
                auto keepalive = service::edge::protocol::outbound(service::common::nextUuidV7(), service::message::utcNowMilliseconds(), service::edge::protocol::platformId(), nodeId);
                auto* resize = keepalive.mutable_terminal_resize();
                resize->set_terminal_id(
                    protocol::bytes(terminalBytes.data(), terminalBytes.size())
                );
                resize->set_columns(terminalSession.columns);
                resize->set_rows(terminalSession.rows);
                co_await terminal_state::TerminalService::enqueueInput(c, nodeId, keepalive);
                nextKeepalive =
                    std::chrono::steady_clock::now() + std::chrono::seconds(20);
            }

            if (!item) {
                (void)co_await ruvia::sleepFor(c.worker(), std::chrono::milliseconds(10));
            }
        }
    }

    static ruvia::Task<void> handle(ruvia::Context& c, Session& session, const pb::Envelope& input) {
        switch (input.payload_case()) {
            case pb::Envelope::kHeartbeat: {
                constexpr std::uint64_t retryIntervalMs = 30000;
                const auto now = service::message::utcNowMilliseconds();
                if (session.configSentAtMs == 0 || now - session.configSentAtMs >= retryIntervalMs) {
                    (void)co_await configService().requeueIfStale(
                        c,
                        session.nodeId,
                        input.heartbeat().active_config_version()
                    );
                }
                auto reply = makeEnvelope(session);
                auto* heartbeatAck = reply.mutable_heartbeat_ack();
                heartbeatAck->set_platform_time_ms(service::message::utcNowMilliseconds());
                heartbeatAck->set_request_capability_report(!session.capabilitySeen);
                heartbeatAck->set_request_device_status(false);
                enqueue(session, reply);
                break;
            }
            case pb::Envelope::kCapabilityReport:
                session.capabilitySeen = true;
                break;
            case pb::Envelope::kTelemetryBatch: {
                // Legacy firmware cannot attach status to telemetry. Request it at
                // report time using the existing acknowledgement it understands.
                if (std::any_of(input.telemetry_batch().records().begin(), input.telemetry_batch().records().end(), [](const auto& record) {
                        return !record.has_device_status();
                    })) {
                    auto statusRequest = makeEnvelope(session);
                    statusRequest.mutable_heartbeat_ack()->set_request_device_status(true);
                    statusRequest.mutable_heartbeat_ack()->set_platform_time_ms(service::message::utcNowMilliseconds());
                    enqueue(session, statusRequest);
                }
                auto reply = makeEnvelope(session);
                auto* telemetryAck = reply.mutable_telemetry_ack();
                for (const auto& record : input.telemetry_batch().records()) {
                    if (record.record_id().size() != 16) {
                        continue;
                    }
                    telemetryAck->add_accepted_record_ids(record.record_id());
                    break;
                }
                enqueue(session, reply);
                break;
            }
            case pb::Envelope::kRawPacket: {
                const auto& packet = input.raw_packet();
                if (packet.packet_id().size() != 16) {
                    break;
                }
                auto reply = makeEnvelope(session);
                co_await ConfigService::storeDebugPacket(c, session.nodeId, packet);
                reply.mutable_raw_packet_ack()->set_packet_id(packet.packet_id());
                enqueue(session, reply);
                break;
            }
            case pb::Envelope::kCommandResult: {
                const auto& result = input.command_result();
                if (result.command_id().size() != 16) {
                    break;
                }
                auto reply = makeEnvelope(session);
                reply.mutable_command_result_ack()->set_command_id(result.command_id());
                enqueue(session, reply);
                break;
            }
            case pb::Envelope::kFirmwareChunkRequest:
                co_await sendFirmwareChunk(c, session, input.firmware_chunk_request());
                break;
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
                co_await terminal_state::TerminalService::saveTerminalData(c, terminalIdentity(session), input.terminal_data());
                break;
            case pb::Envelope::kTerminalDataAck:
                co_await terminal_state::TerminalService::saveTerminalDataAck(c, terminalIdentity(session), input.terminal_data_ack());
                break;
            case pb::Envelope::kTerminalOpened:
                co_await terminal_state::TerminalService::saveTerminalOpened(c, terminalIdentity(session), input.terminal_opened());
                break;
            case pb::Envelope::kTerminalClose:
                co_await terminal_state::TerminalService::saveTerminalClose(c, terminalIdentity(session), input.terminal_close());
                break;
            case pb::Envelope::kLogResult:
                co_await gateway::GatewayService::saveLogResult(c, session.nodeId, input.log_result());
                break;
            case pb::Envelope::kLogLevelResult:
                co_await gateway::GatewayService::saveLogLevelResult(c, input.log_level_result());
                break;
            default:
                break;
        }
    }

    static ruvia::Task<void> sendFirmwareChunk(
        ruvia::Context& c,
        Session& session,
        const pb::FirmwareChunkRequest& request
    ) {
        if (session.protocolVersion < 6 || request.request_id().size() != 16) {
            co_return;
        }
        const bool sameRequest =
            session.firmwareSourceLoaded &&
            std::memcmp(session.firmwareRequestId.data(), request.request_id().data(), 16) == 0;
        if (!sameRequest) {
            const auto requestId = protocol::uuidText(request.request_id());
            session.firmwareSourceLoaded = false;
            session.firmwarePath.clear();
            session.firmwareSize = 0;
            if (const auto source = co_await gateway::GatewayService::loadFirmwareSource(
                    c,
                    requestId,
                    session.nodeId
                )) {
                std::memcpy(session.firmwareRequestId.data(), request.request_id().data(), 16);
                session.firmwarePath = std::filesystem::path(source->storagePath);
                session.firmwareSize = source->sizeBytes;
                session.firmwareSourceLoaded = true;
            }
        }

        auto reply = makeEnvelope(session);
        auto* chunk = reply.mutable_firmware_chunk();
        chunk->set_request_id(request.request_id());
        chunk->set_offset(request.offset());
        if (!session.firmwareSourceLoaded) {
            chunk->set_error("firmware transfer source is unavailable");
            enqueue(session, reply);
            co_return;
        }
        auto value = firmware::readChunk(
            session.firmwarePath,
            session.firmwareSize,
            request.offset()
        );
        if (!value.error.empty()) {
            chunk->set_error(value.error);
        } else {
            chunk->set_data(std::move(value.data));
            chunk->set_eof(value.eof);
        }
        enqueue(session, reply);
    }

};

} // namespace service::edge
