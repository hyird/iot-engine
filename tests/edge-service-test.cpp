#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/modules/edge_node/edge_node.service.h"

namespace {

void requireMissing(std::string_view haystack, std::string_view needle,
                    const char* message) {
    if (haystack.find(needle) != std::string_view::npos)
        throw std::runtime_error(message);
}

void requireContains(std::string_view haystack, std::string_view needle,
                     const char* message) {
    if (haystack.find(needle) == std::string_view::npos)
        throw std::runtime_error(message);
}

void requireBefore(std::string_view haystack, std::string_view before,
                   std::string_view after, const char* message) {
    const auto beforePosition = haystack.find(before);
    const auto afterPosition = beforePosition == std::string_view::npos
                                   ? std::string_view::npos
                                   : haystack.find(after, beforePosition + before.size());
    if (beforePosition == std::string_view::npos || afterPosition == std::string_view::npos)
        throw std::runtime_error(message);
}

std::string edgeSource(const char* relativePath) {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                relativePath;
    std::ifstream input(path, std::ios::binary);
    if (!input.good())
        throw std::runtime_error("cannot open edge source");
    std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::erase(source, '\r');
    return source;
}

} // namespace

int main() {
    try {
        const auto serviceSource = edgeSource("service/modules/edge_node/edge_node.service.h");
        requireMissing(serviceSource, "R\"sql", "edge service still embeds raw SQL");
        requireMissing(serviceSource, "c.db().query(\"", "edge service still calls raw query overload");
        requireMissing(serviceSource, "c.db().execute(\"", "edge service still calls raw execute overload");
        requireContains(serviceSource, "ruvia::DbQuery", "edge service does not use DbQuery");
        requireContains(serviceSource, "EdgeNodeEntity::tableName()",
                        "edge service does not use the edge node entity");
        requireContains(serviceSource, "EdgeNodeGroupEntity::tableName()",
                        "edge service does not use the edge group entity");
        requireContains(serviceSource, "c.pool()", "edge service does not build queries from Context.pool()");
        requireContains(serviceSource, "safeBigInt", "edge service does not guard numeric JSON fields");
        requireContains(serviceSource, "booleanText", "edge service does not normalize JSON booleans");
        requireContains(serviceSource, "jsonb_array_elements_text",
                        "edge service does not preserve bridge port JSON arrays");
        requireContains(serviceSource, "joinFunction", "edge service does not use public lateral joins");
        requireContains(serviceSource, "with(\"selected_group\"",
                        "edge service does not express recursive group traversal through DbQuery");
        requireContains(serviceSource, "roots.cast(roots.value(groupId), Type::kUuid)",
                        "group cycle validation does not start at the group being moved");
        requireContains(serviceSource, "cycle.cast(cycle.value(parentId), Type::kUuid)",
                        "group cycle validation does not reject moving into a descendant");
        requireContains(serviceSource, "nullableUuid", "edge service does not preserve nullable UUID writes");
        requireContains(serviceSource, "query.call(\n            \"jsonb_build_object\"",
                        "edge task request does not use typed JSON construction");
        const auto controllerSource = edgeSource("service/modules/edge_node/edge_node.controller.h");
        const auto gatewaySource = edgeSource("service/features/edge/gateway/gateway.transport.h");
        const auto dispatchSource = edgeSource("service/features/edge/edge.transport.h");
        const auto dispatcherSource = edgeSource("service/features/edge/edge.runtime.h");
        const auto multiplexerSource =
            edgeSource("service/features/messaging/stream_multiplexer/stream_multiplexer.runtime.h");
        const auto projectorRuntimeSource =
            edgeSource("service/features/edge/edge.runtime.h");
        const auto projectorServiceSource =
            edgeSource("service/features/edge/edge.service.h");
        const auto projectorStreamSource =
            edgeSource("service/features/edge/edge.transport.h");
        const auto metadataSource = edgeSource("service/features/edge/edge.service.h");
        const auto vpnEdgeConfigSource =
            edgeSource("service/features/vpn/vpn.service.h");
        const auto vpnServiceSource =
            edgeSource("service/modules/vpn/vpn.service.h");
        requireMissing(serviceSource, "const std::string status(body.status()->view());",
                       "edge enrollment dereferences optional status without validation");
        requireMissing(serviceSource, "const std::string name(body.name()->view());",
                       "edge service dereferences optional name without validation");
        requireMissing(serviceSource, "const auto& configs = *body.interfaces();",
                       "edge network config dereferences optional interfaces without validation");
        requireMissing(serviceSource, "static_cast<std::uint32_t>(*body.rollbackTimeoutSec())",
                       "edge network casts optional rollback timeout without validation");
        requireContains(serviceSource, "operation != \"upsert\" && operation != \"delete\"",
                        "edge network config accepts unknown operations as upsert");
        requireMissing(controllerSource, "/:id/modem",
                       "edge controller still exposes modem mutation route");
        requireMissing(serviceSource, "queueModem(",
                       "edge service still exposes modem mutation commands");
        requireMissing(serviceSource, "queuePlatform(",
                       "edge service still exposes remote platform configuration");
        requireMissing(serviceSource, "deletePlatform(",
                       "edge service still exposes remote platform deletion");
        requireMissing(controllerSource, "/:id/platforms",
                       "edge controller still exposes platform management routes");
        requireContains(controllerSource, "/:id/download",
                        "legacy firmware download route is missing");
        requireContains(serviceSource, "download_token",
                        "legacy firmware download tokens are missing");
        requireContains(serviceSource, "EDGE_PUBLIC_BASE_URL",
                        "legacy firmware download URL is missing");
        requireContains(serviceSource, "baseUrl.ends_with('/')",
                        "legacy firmware URL does not normalize a trailing slash");
        requireContains(serviceSource, "/download?token=",
                        "legacy firmware download URL does not include its token");
        requireContains(serviceSource, "firmwareStream",
                        "firmware transport does not select by node capability");
        requireContains(gatewaySource, "case pb::Envelope::kFirmwareChunkRequest",
                        "edge gateway does not serve firmware over the node WebSocket");
        requireContains(gatewaySource, "firmware::readChunk(",
                        "edge gateway does not bound firmware WS chunks");
        requireMissing(serviceSource, "std::clamp<std::int64_t>(*query.limit(), 1, 48)",
                       "edge log request clamps invalid limit instead of rejecting it");
        requireMissing(serviceSource, "const auto level = std::string(body.level()->view());",
                       "edge log level dereferences optional level without validation");
        requireContains(serviceSource, "status != \"approved\"",
                        "edge enrollment accepts invalid registration status");
        requireContains(controllerSource, "RUVIA_DELETE(\"/:id\", removeEnrollment",
                        "edge enrollment deletion route is missing");
        requireContains(serviceSource, "removal.value(\"pending\")",
                        "edge enrollment deletion is not limited to pending registrations");
        requireContains(serviceSource, "module_wire::authKey(imei)",
                        "edge enrollment deletion leaves stale authorization state");
        requireContains(serviceSource, "\"iot:edge:session:\" + std::string(id)",
                        "edge enrollment deletion does not disconnect the pending session");
        requireContains(serviceSource,
                        "level != \"debug\" && level != \"info\" && level != \"warn\"",
                        "edge log level accepts invalid values");
        requireContains(serviceSource, "sourceValue.size() > 16",
                        "edge log source does not enforce local length limit");
        requireMissing(gatewaySource, "if (input.ping().nonce() != 0)",
                       "edge gateway drops zero-nonce terminal liveness pings");
        requireContains(gatewaySource,
                        "reply.mutable_pong()->set_nonce(input.ping().nonce())",
                        "edge gateway does not echo terminal liveness pings");
        requireContains(gatewaySource, "if (*sessionProtocolVersion <= 3)",
                        "edge gateway does not preserve legacy terminal ticket compatibility");
        requireContains(gatewaySource, "terminalOpen->set_ticket(ticket)",
                        "edge gateway does not satisfy legacy terminal-open validation");
        requireContains(gatewaySource, "if (terminalSession.protocolVersion <= 3)",
                        "edge gateway does not isolate legacy immediate-ready behavior");
        requireContains(gatewaySource, "case pb::Envelope::kTerminalOpened",
                        "edge gateway does not consume terminal-open acknowledgement");
        requireContains(gatewaySource, "terminalSession.opened = true",
                        "edge gateway does not gate terminal input on node acknowledgement");
        requireBefore(gatewaySource, "terminalSession.opened = true;",
                      "co_await socket.binary(*item);",
                      "edge gateway exposes Ready before enabling terminal input");
        requireContains(gatewaySource, "terminal open timed out",
                        "edge gateway can wait forever for terminal-open acknowledgement");
        requireContains(gatewaySource, "terminalData->set_sequence",
                        "edge gateway does not sequence v5 terminal input");
        requireContains(gatewaySource, "waitTerminalInputAck",
                        "edge gateway does not apply terminal input backpressure");
        requireContains(gatewaySource, "mutable_terminal_data_ack",
                        "edge gateway does not acknowledge delivered terminal output");
        requireContains(gatewaySource, "std::deque<std::string> outbound",
                        "edge gateway does not serialize established-session replies");
        requireContains(gatewaySource, "enqueue(session, reply)",
                        "edge gateway still writes node acknowledgements outside the flush path");
        requireMissing(gatewaySource, "co_await drain(c, socket, session)",
                       "edge gateway still sends config frames from the session read loop");
        requireContains(gatewaySource, "co_await c.redis().lpush(key, item)",
                        "edge gateway drops a popped command when its socket send fails");
        requireMissing(gatewaySource, ".blpop(",
                       "edge gateway still allocates one blocking Redis connection per node");
        requireMissing(gatewaySource, "RedisBlockWait::indefinitely()",
                       "edge gateway still owns a per-session blocking Redis read");
        requireContains(gatewaySource, "c.workerState<SessionDispatcher>()",
                        "edge gateway does not resolve its worker-local dispatcher");
        requireContains(gatewaySource, "dispatcher.registerSession(",
                        "edge gateway does not register sessions with its worker dispatcher");
        requireContains(gatewaySource, "requestFlush(live)",
                        "edge gateway does not drain queued work on session establishment");
        requireMissing(dispatcherSource, "readGroupBlocking(",
                       "edge dispatcher still owns a dedicated blocking Redis connection");
        requireContains(multiplexerSource, "readGroupManyBlockingUntil(",
                        "Service Worker wake bus has no blocking Stream consumer");
        requireContains(dispatcherSource, "context.workerState<SessionDispatcher>().run(",
                        "edge dispatcher does not start the same local state on every worker");
        requireContains(dispatchSource, "iot:v2:edge:dispatch:",
                        "edge dispatch notifications do not use worker-isolated Redis keys");
        requireContains(dispatchSource, "session_state::parse(",
                        "edge dispatch notifications are not routed by session ownership");
        requireContains(projectorStreamSource, "iot:v3:edge:projector:",
                        "edge projection does not use worker-isolated Redis keys");
        requireContains(gatewaySource, "publishIngress(c, workerIndex",
                        "edge ingress is not routed by the accepting Worker");
        requireContains(gatewaySource, "projector_stream::publishMetadata(",
                        "edge reconnect does not refresh accepting-Worker metadata");
        requireContains(metadataSource, "session_state::parse(",
                        "edge metadata updates are not routed by session ownership");
        requireContains(projectorRuntimeSource, "projector_stream::stream(index)",
                        "edge projector does not preserve the accepting Worker");
        requireContains(projectorServiceSource,
                        "jsonKey(\"firmwareUpdate\"), boolean(hello.supports_firmware_update())",
                        "edge projector does not retain legacy firmware capability");
        requireContains(projectorServiceSource,
                        "jsonKey(\"firmwareStream\"), boolean(hello.supports_firmware_stream())",
                        "edge projector does not record WS firmware capability separately");
        requireContains(projectorServiceSource,
                        "query.nullIf(config::detail::jsonText(query, excludedMobile, \"apn\")",
                        "edge hello projection clears the last known mobile APN");
        requireContains(projectorServiceSource,
                        "query.nullIf(config::detail::jsonText(query, excludedMobile, \"operator\")",
                        "edge hello projection clears the last known mobile operator");
        requireContains(projectorServiceSource,
                        "query.nullIf(text(heartbeat.apn()), query.value(std::string_view{}))",
                        "edge heartbeat projection clears the last known mobile APN");
        requireContains(projectorServiceSource,
                        "query.nullIf(text(heartbeat.mobile_operator()), query.value(std::string_view{}))",
                        "edge heartbeat projection clears the last known mobile operator");
        requireContains(vpnEdgeConfigSource, "task.cast(task.value(peerId), ruvia::DbDataType::kText)",
                        "VPN task peer id has no explicit PostgreSQL type");
        requireContains(vpnEdgeConfigSource, "task.cast(task.value(request->enabled()), ruvia::DbDataType::kBoolean)",
                        "VPN task enabled flag has no explicit PostgreSQL type");
        requireContains(gatewaySource, "case pb::Envelope::kVpnConfigResult:",
                        "edge gateway does not project VPN configuration results");
        requireContains(projectorServiceSource, "transitioned, transitioned.column(\"request\"), \"enabled\")",
                        "VPN result transition does not return the requested enabled state");
        requireContains(projectorServiceSource, "\"config_version\")",
                        "VPN result transition does not return the requested config version");
        requireContains(projectorServiceSource, "update.cast(update.column(\"enabled\", \"task\"), ruvia::DbDataType::kBoolean)",
                        "VPN result projection reads a column outside the transition CTE");
        requireContains(projectorServiceSource, "update.column(\"config_version\", \"task\")",
                        "VPN result projection does not compare the returned config version");
        requireContains(vpnEdgeConfigSource, "superseded.cast(superseded.value(\"superseded\"), ruvia::DbDataType::kText)",
                        "new VPN tasks leave older tasks pending forever");
        requireContains(vpnServiceSource,
                        "reusableEdgePeerId.empty() ? service::common::nextUuidV7()",
                        "revoked Edge VPN peers are not reused when VPN is enabled again");
        requireContains(vpnServiceSource, ".set(\"revoked_at\", reactivation.nullValue())",
                        "reactivating an Edge VPN peer does not clear its revoked state");
        requireContains(vpnServiceSource, "\"vpn.peer.reactivate\"",
                        "Edge VPN peer reactivation is not audited separately");
        requireMissing(dispatcherSource, "workers_.back().post(",
                       "edge dispatcher still gives one worker a special role");
        requireMissing(dispatcherSource, "target.worker.post(",
                       "edge dispatcher still forwards wakeups between workers");
        requireContains(gatewaySource,
                        ".reason = \"edge egress failed\"",
                        "edge gateway leaves a half-open session online after its flush fails");
        requireContains(gatewaySource, "session.protocolVersion < 5",
                        "edge gateway does not isolate legacy terminal data handling");
        requireContains(gatewaySource,
                        "terminalSessionKey(nodeId, terminalId), nodeSession",
                        "edge gateway does not register terminal ownership before opening");
        requireContains(gatewaySource,
                        "return \"iot:edge:terminal:out:\" + std::string(nodeId)",
                        "edge terminal output keys are not isolated by node");
        requireContains(gatewaySource,
                        "if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end",
                        "edge gateway does not atomically verify terminal ownership");
        requireContains(gatewaySource,
                        "co_await saveTerminalData(c, session, input.terminal_data())",
                        "edge gateway does not bind terminal output to the authenticated session");
        requireContains(gatewaySource,
                        "redis.call('DEL', KEYS[1], KEYS[2], KEYS[3], KEYS[4])",
                        "edge gateway does not atomically release terminal state");
        requireMissing(gatewaySource, "\"iot:edge:terminal:out:\" + terminalId",
                       "edge gateway still routes terminal output by unscoped identifier");

        std::cout << "edge service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "edge service test failed: " << error.what() << '\n';
        return 1;
    }
}
