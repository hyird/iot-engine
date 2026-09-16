#pragma once
#include "service/features/edge/serial_debug/serial_debug.entity.h"
#include "service/features/edge/serial_debug/serial_debug.protocol.h"
#include "service/features/edge/session/session.service.h"
#include "service/features/edge/edge.protocol.h"

namespace service::edge::serial_debug {
class Service final {
  public:
    static ruvia::Task<std::optional<TicketRecord>> consumeTicket(ruvia::Context& c, std::string_view ticket) {
        const auto value = co_await c.redis().getDel(service::message::serial_debug::ticketKey(ticket));
        if (!value) co_return std::nullopt;
        co_return TicketRecord::decode(*value);
    }
    static ruvia::Task<bool> registerSession(ruvia::Context& c, const TicketRecord& ticket, std::string_view id) {
        const auto parsed = session_state::parse(ticket.nodeSession);
        if (!parsed || parsed->protocolVersion < 5) co_return false;
        const SessionRecord record{ticket.nodeSession};
        const auto currentKey = session_state::key(ticket.nodeId);
        const auto ownerKey = SessionRecord::key(ticket.nodeId, id);
        const std::string_view keys[]{currentKey, ownerKey};
        const std::string_view args[]{record.nodeSession};
        const auto reply = co_await c.redis().eval(
            "if redis.call('GET',KEYS[1]) ~= ARGV[1] then return 0 end "
            "redis.call('SET',KEYS[2],ARGV[1],'EX',60); return 1", keys, args);
        co_return reply.kind() == ruvia::RedisValue::Kind::kInteger && reply.integer() == 1;
    }
    static ruvia::Task<bool> enqueue(ruvia::Context& c, const TicketRecord& ticket,
                                    std::string_view id, wire::SerialDebugRequest request) {
        const auto parsed = session_state::parse(ticket.nodeSession);
        if (!parsed) co_return false;
        std::array<std::uint8_t, 16> bytes{};
        if (!service::common::uuidBytes(id, bytes.data())) co_return false;
        request.set_session_id(bytes.data(), bytes.size());
        auto envelope = protocol::outbound(service::common::nextUuidV7(),
            service::message::utcNowMilliseconds(), protocol::platformId(), ticket.nodeId);
        envelope.set_session_epoch(parsed->epoch);
        envelope.set_protocol_version(parsed->protocolVersion);
        *envelope.mutable_serial_debug_request() = std::move(request);
        const auto encoded = protocol::encode(envelope);
        const auto currentKey = session_state::key(ticket.nodeId);
        const auto ownerKey = SessionRecord::key(ticket.nodeId, id);
        const auto inputKey = service::message::serial_debug::inputKey(ticket.nodeId);
        const std::string_view keys[]{currentKey, ownerKey, inputKey};
        const std::string_view args[]{ticket.nodeSession, encoded};
        const auto reply = co_await c.redis().eval(R"lua(
if redis.call('GET',KEYS[1]) ~= ARGV[1] or redis.call('GET',KEYS[2]) ~= ARGV[1] then return 0 end
if redis.call('LLEN',KEYS[3]) >= 64 then return 0 end
redis.call('EXPIRE',KEYS[2],60)
redis.call('RPUSH',KEYS[3],ARGV[2])
redis.call('EXPIRE',KEYS[3],15)
return 1
)lua", keys, args);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger || reply.integer() != 1) co_return false;
        co_await dispatch::notifyNode(c.redis(), ticket.nodeId);
        co_return true;
    }
    static ruvia::Task<std::optional<std::string>> takeOutput(ruvia::Context& c,
        const TicketRecord& ticket, std::string_view id) {
        const auto item = co_await c.redis().lpop(service::message::serial_debug::outputKey(ticket.nodeId, id));
        if (!item) co_return std::nullopt;
        co_return std::string(*item);
    }
    static ruvia::Task<bool> connected(ruvia::Context& c, const TicketRecord& ticket) {
        const auto current = co_await c.redis().get(session_state::key(ticket.nodeId));
        co_return current && std::string_view(current->data(), current->size()) == std::string_view(ticket.nodeSession);
    }
    static ruvia::Task<void> saveEvent(ruvia::Context& c, std::string_view nodeId,
        std::string_view nodeSession, const wire::SerialDebugEvent& event) {
        if (event.session_id().size() != 16 || event.data().size() > 1024 ||
            event.sequence() == 0) co_return;
        const auto id = protocol::uuidText(event.session_id());
        const auto ownerKey = SessionRecord::key(nodeId, id);
        const auto outputKey = service::message::serial_debug::outputKey(nodeId, id);
        const auto currentKey = session_state::key(nodeId);
        const auto json = eventJson(event);
        const std::string_view keys[]{ownerKey, outputKey, currentKey};
        const std::string_view args[]{nodeSession, json};
        (void)co_await c.redis().eval(R"lua(
if redis.call('GET',KEYS[1]) ~= ARGV[1] or redis.call('GET',KEYS[3]) ~= ARGV[1] then return 0 end
redis.call('RPUSH',KEYS[2],ARGV[2])
redis.call('LTRIM',KEYS[2],-256,-1)
redis.call('EXPIRE',KEYS[2],60)
return 1
)lua", keys, args);
    }
    static ruvia::Task<void> release(ruvia::Context& c, const TicketRecord& ticket, std::string_view id) {
        const auto ownerKey = SessionRecord::key(ticket.nodeId, id);
        const auto outputKey = service::message::serial_debug::outputKey(ticket.nodeId, id);
        const std::string_view keys[]{ownerKey, outputKey};
        const std::string_view args[]{ticket.nodeSession};
        (void)co_await c.redis().eval(
            "if redis.call('GET',KEYS[1]) == ARGV[1] then redis.call('DEL',KEYS[1],KEYS[2]) end; return 1",
            keys, args);
    }
};
} // namespace service::edge::serial_debug
