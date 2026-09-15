#pragma once
#include "service/common/uuid.h"

#include <string_view>
#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <terminal.pb.h>
#include "service/common/http.h"
#include "service/features/edge/edge.protocol.h"
#include "service/features/edge/session/session.service.h"
#include "service/features/edge/terminal/terminal.entity.h"
#include "service/features/edge/terminal/terminal.types.h"

namespace service::edge::terminal_state {

// Close only this terminal. Retain its final browser frame for the output pump.
inline constexpr std::string_view kFailScript = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1], KEYS[2], KEYS[3], KEYS[4])
redis.call('RPUSH', KEYS[2], ARGV[2])
redis.call('EXPIRE', KEYS[2], 120)
return 1
)lua";

// KEYS: node session, terminal owner, output queue, input ACK, output sequence.
// Sequence state must live as long as the idle terminal, not just its last I/O.
// Check both owners atomically so an old browser cannot extend a replaced session.
inline constexpr std::string_view kRefreshScript = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return -1 end
if redis.call('GET', KEYS[2]) ~= ARGV[1] then return 0 end
for i = 2, 5 do
    redis.call('EXPIRE', KEYS[i], ARGV[2])
end
return 1
)lua";

namespace webpb = ::iot::edge::terminal::v1;

class TerminalService final {
  public:
    static ruvia::Task<std::optional<std::string>> consumeTicket(ruvia::Context& c, std::string_view ticket) {
        const auto node = co_await c.redis().getDel(TerminalTicketRecord::ticketKey(ticket));
        if (!node) co_return std::nullopt;
        const TerminalTicketRecord record{std::string(node->data(), node->size())};
        co_return record.nodeId;
    }

    static ruvia::Task<std::optional<std::string>> findNodeSession(ruvia::Context& c, std::string_view nodeId) {
        const auto session = co_await c.redis().get(session_state::key(nodeId));
        if (!session) co_return std::nullopt;
        co_return std::string(session->data(), session->size());
    }

    static ruvia::Task<void> registerSession(ruvia::Context& c, std::string_view nodeId,
        std::string_view terminalId, std::string_view nodeSession) {
        ruvia::RedisSetOptions options;
        options.expiration = ruvia::RedisSetExpiration::expiresAfter(std::chrono::seconds(120));
        const TerminalSessionRecord record{std::string(nodeSession)};
        co_await c.redis().set(TerminalSessionRecord::terminalSessionKey(nodeId, terminalId), record.nodeSession, std::move(options));
    }

    static ruvia::Task<std::optional<std::string>> takeOutput(ruvia::Context& c,
        std::string_view nodeId, std::string_view terminalId) {
        const auto item = co_await c.redis().lpop(terminalOutputKey(nodeId, terminalId));
        if (!item) co_return std::nullopt;
        co_return std::string(item->data(), item->size());
    }

    static ruvia::Task<InputAckStatus> inputAckStatus(ruvia::Context& c,
        std::string_view nodeId, std::string_view terminalId,
        std::string_view nodeSession, std::uint64_t sequence) {
        const auto value = co_await c.redis().get(TerminalSessionRecord::terminalInputAckKey(nodeId, terminalId));
        if (value && std::string_view(value->data(), value->size()) == std::to_string(sequence))
            co_return InputAckStatus::Acknowledged;
        const auto owner = co_await c.redis().get(TerminalSessionRecord::terminalSessionKey(nodeId, terminalId));
        if (!owner) co_return InputAckStatus::OwnershipLost;
        const TerminalSessionRecord record{std::string(owner->data(), owner->size())};
        if (record.nodeSession != nodeSession) co_return InputAckStatus::OwnershipLost;
        co_return InputAckStatus::Pending;
    }

    static ruvia::Task<std::int64_t> refreshSession(ruvia::Context& c,
        std::string_view nodeId, std::string_view terminalId, std::string_view nodeSession) {
        const auto nodeKey = session_state::key(nodeId);
        const auto owner = TerminalSessionRecord::terminalSessionKey(nodeId, terminalId);
        const auto output = terminalOutputKey(nodeId, terminalId);
        const auto inputAck = TerminalSessionRecord::terminalInputAckKey(nodeId, terminalId);
        const auto outputSequence = TerminalSessionRecord::terminalOutputSequenceKey(nodeId, terminalId);
        const std::string_view keys[]{nodeKey, owner, output, inputAck, outputSequence};
        const std::string_view arguments[]{nodeSession, "120"};
        const auto result = co_await c.redis().eval(kRefreshScript, keys, arguments);
        if (result.kind() != ruvia::RedisValue::Kind::kInteger)
            service::message::redis::throwValue("refresh edge terminal state", result);
        co_return result.integer();
    }

    static ruvia::Task<void> enqueueInput(ruvia::Context& c, std::string_view nodeId, const pb::Envelope& envelope) {
        const auto wire = protocol::encode(envelope);
        if (wire.empty()) {
            throw std::runtime_error("edge terminal envelope encode failed");
        }
        // Terminal input gets its own downstream key: the shared egress list is
        // head-trimmed by command dispatch, which would hand the node a terminal_data
        // whose terminal_open it never saw. Overflow here drops the newest envelope
        // instead, so whatever the node does receive stays a valid prefix.
        const auto key = terminalInputKey(nodeId);
        static constexpr std::string_view script = R"lua(
if redis.call('LLEN', KEYS[1]) >= tonumber(ARGV[2]) then return 0 end
local length = redis.call('RPUSH', KEYS[1], ARGV[1])
if length == 1 then redis.call('EXPIRE', KEYS[1], ARGV[3]) end
return length
)lua";
        const std::string_view keys[]{key};
        const std::string_view arguments[]{wire, "4096", "120"};
        const auto reply = co_await c.redis().eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
            service::message::redis::throwValue("enqueue edge terminal input", reply);
        if (reply.integer() == 0) {
            throw std::runtime_error("edge terminal input backlog exceeded");
        }
        co_await dispatch::notifyNode(c.redis(), nodeId);
    }

    static ruvia::Task<void> releaseTerminalSession(ruvia::Context& c, std::string_view nodeId, std::string_view terminalId, std::string_view nodeSession) {
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1], KEYS[2], KEYS[3], KEYS[4])
return 1
)lua";
        const auto ownershipKey = TerminalSessionRecord::terminalSessionKey(nodeId, terminalId);
        const auto outputKey = terminalOutputKey(nodeId, terminalId);
        const auto inputAckKey = TerminalSessionRecord::terminalInputAckKey(nodeId, terminalId);
        const auto outputSequenceKey = TerminalSessionRecord::terminalOutputSequenceKey(nodeId, terminalId);
        const std::string_view keys[]{ ownershipKey, outputKey, inputAckKey, outputSequenceKey };
        const std::string_view arguments[]{ nodeSession };
        const auto reply = co_await c.redis().eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("release edge terminal session", reply);
        }
    }

    static ruvia::Task<void> saveTerminalFrame(ruvia::Context& c, const ConnectionIdentity& session, std::string_view terminalId, const webpb::WebTerminalFrame& frame) {
        std::string wire;
        if (!frame.SerializeToString(&wire)) {
            co_return;
        }
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('RPUSH', KEYS[2], ARGV[2])
redis.call('EXPIRE', KEYS[1], ARGV[3])
redis.call('EXPIRE', KEYS[2], ARGV[3])
return 1
)lua";
        const auto ownershipKey = TerminalSessionRecord::terminalSessionKey(session.nodeId, terminalId);
        const auto outputKey = terminalOutputKey(session.nodeId, terminalId);
        const auto epoch = session_state::value(
            session.epoch,
            session.protocolVersion,
            session.workerIndex
        );
        const std::string ttl = "120";
        const std::string_view keys[]{ ownershipKey, outputKey };
        const std::string_view arguments[]{ epoch, wire, ttl };
        const auto reply = co_await c.redis().eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("append edge terminal output", reply);
        }
    }

    static ruvia::Task<void> failTerminal(ruvia::Context& c, const ConnectionIdentity& session, std::string_view terminalId, std::string_view reason) {
        std::array<std::uint8_t, 16> terminalBytes{};
        if (!protocol::uuidBytes(terminalId, terminalBytes.data())) {
            co_return;
        }
        webpb::WebTerminalFrame frame;
        frame.mutable_close()->set_reason(std::string(reason));
        std::string wire;
        if (!frame.SerializeToString(&wire)) {
            co_return;
        }
        const auto owner = TerminalSessionRecord::terminalSessionKey(session.nodeId, terminalId);
        const auto output = terminalOutputKey(session.nodeId, terminalId);
        const auto inputAck = TerminalSessionRecord::terminalInputAckKey(session.nodeId, terminalId);
        const auto outputSequence = TerminalSessionRecord::terminalOutputSequenceKey(session.nodeId, terminalId);
        const auto epoch = session_state::value(session.epoch, session.protocolVersion, session.workerIndex);
        const std::string_view keys[]{ owner, output, inputAck, outputSequence };
        const std::string_view args[]{ epoch, wire };
        const auto result = co_await c.redis().eval(terminal_state::kFailScript, keys, args);
        if (result.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("close failed terminal", result);
        }
        if (result.integer() == 1) {
            auto close = service::edge::protocol::outbound(service::common::nextUuidV7(), service::message::utcNowMilliseconds(), service::edge::protocol::platformId(), session.nodeId);
            close.mutable_terminal_close()->set_terminal_id(
                protocol::bytes(terminalBytes.data(), terminalBytes.size())
            );
            close.mutable_terminal_close()->set_reason(std::string(reason));
            co_await enqueueInput(c, session.nodeId, close);
        }
    }

    static ruvia::Task<void> saveTerminalData(ruvia::Context& c, const ConnectionIdentity& session, const pb::TerminalData& data) {
        if (data.terminal_id().size() != 16 || data.data().empty()) {
            co_return;
        }
        const auto id = protocol::uuidText(data.terminal_id());
        if (session.protocolVersion < 5) {
            webpb::WebTerminalFrame legacyFrame;
            legacyFrame.mutable_data()->set_data(data.data());
            co_await saveTerminalFrame(c, session, id, legacyFrame);
            co_return;
        }
        if (data.sequence() == 0) {
            co_await failTerminal(c, session, id, "terminal output sequence missing");
            co_return;
        }
        webpb::WebTerminalFrame frame;
        frame.mutable_data()->set_data(data.data());
        frame.mutable_data()->set_sequence(data.sequence());
        std::string wire;
        if (!frame.SerializeToString(&wire)) {
            co_return;
        }
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
        const auto ownershipKey = TerminalSessionRecord::terminalSessionKey(session.nodeId, id);
        const auto outputKey = terminalOutputKey(session.nodeId, id);
        const auto sequenceKey = TerminalSessionRecord::terminalOutputSequenceKey(session.nodeId, id);
        const auto epoch = session_state::value(
            session.epoch,
            session.protocolVersion,
            session.workerIndex
        );
        const auto sequence = std::to_string(data.sequence());
        const std::string ttl = "120";
        const std::string_view keys[]{ ownershipKey, outputKey, sequenceKey };
        const std::string_view arguments[]{ epoch, wire, sequence, ttl };
        const auto reply = co_await c.redis().eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("append sequenced terminal output", reply);
        }
        if (reply.integer() < 0) {
            co_await failTerminal(c, session, id, "terminal output sequence mismatch");
        }
    }

    static ruvia::Task<void> saveTerminalDataAck(
        ruvia::Context& c,
        const ConnectionIdentity& session,
        const pb::TerminalDataAck& ack
    ) {
        if (session.protocolVersion < 5) {
            co_return;
        }
        if (ack.terminal_id().size() != 16) {
            co_return;
        }
        const auto id = protocol::uuidText(ack.terminal_id());
        if (ack.sequence() == 0) {
            co_await failTerminal(c, session, id, "terminal input sequence missing");
            co_return;
        }
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
        const auto ownershipKey = TerminalSessionRecord::terminalSessionKey(session.nodeId, id);
        const auto ackKey = TerminalSessionRecord::terminalInputAckKey(session.nodeId, id);
        const auto epoch = session_state::value(
            session.epoch,
            session.protocolVersion,
            session.workerIndex
        );
        const auto sequence = std::to_string(ack.sequence());
        const std::string ttl = "120";
        const std::string_view keys[]{ ownershipKey, ackKey };
        const std::string_view arguments[]{ epoch, sequence, ttl };
        const auto reply = co_await c.redis().eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("advance terminal input ack", reply);
        }
        if (reply.integer() < 0) {
            co_await failTerminal(c, session, id, "terminal input acknowledgement mismatch");
        }
    }

    static ruvia::Task<void> saveTerminalOpened(ruvia::Context& c, const ConnectionIdentity& session, const pb::TerminalOpened& opened) {
        if (opened.terminal_id().size() != 16) {
            co_return;
        }
        const auto id = protocol::uuidText(opened.terminal_id());
        webpb::WebTerminalFrame frame;
        frame.mutable_ready();
        co_await saveTerminalFrame(c, session, id, frame);
    }

    static ruvia::Task<void> saveTerminalClose(ruvia::Context& c, const ConnectionIdentity& session, const pb::TerminalClose& close) {
        if (close.terminal_id().size() != 16) {
            co_return;
        }
        const auto id = protocol::uuidText(close.terminal_id());
        webpb::WebTerminalFrame frame;
        auto* terminalClose = frame.mutable_close();
        terminalClose->set_exit_code(close.exit_code());
        terminalClose->set_reason(close.reason());
        co_await saveTerminalFrame(c, session, id, frame);
    }
};

} // namespace service::edge::terminal_state
