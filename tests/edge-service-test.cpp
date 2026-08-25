#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/domains/edge/edge.service.h"

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
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

int main() {
    try {
        const auto nodeSelect = service::edge::EdgeService::nodeSelectForTest();
        const auto taskSelect = service::edge::EdgeService::tasksQueryForTest();

        requireMissing(nodeSelect, "COALESCE((status->'config'->>'activeVersion')::bigint",
                       "edge node select directly casts config activeVersion");
        requireMissing(nodeSelect, "COALESCE((status->'config'->>'desiredVersion')::bigint",
                       "edge node select directly casts config desiredVersion");
        requireMissing(nodeSelect, "COALESCE((status->'outbox'->>'records')::bigint",
                       "edge node select directly casts outbox records");
        requireMissing(nodeSelect, "COALESCE((status->'outbox'->>'bytes')::bigint",
                       "edge node select directly casts outbox bytes");
        requireMissing(nodeSelect, "COALESCE((capability->>'networkConfig')::boolean",
                       "edge node select directly casts networkConfig capability");
        requireMissing(nodeSelect, "COALESCE((capability->>'networkConfigVersion')::bigint",
                       "edge node select directly casts networkConfigVersion capability");
        requireMissing(nodeSelect, "COALESCE((mobile->'signal'->>'csq')::bigint",
                       "edge node select directly casts mobile csq");
        requireMissing(nodeSelect, "COALESCE((mobile->>'registered')::boolean",
                       "edge node select directly casts mobile registered");
        requireMissing(nodeSelect, "COALESCE((SELECT (task.result->>'progressPercent')::bigint",
                       "edge node select directly casts firmware task progress");

        requireMissing(taskSelect, "COALESCE((result->>'progressPercent')::bigint",
                       "edge task select directly casts progressPercent");
        requireMissing(taskSelect, "COALESCE((result->>'downloadedBytes')::bigint",
                       "edge task select directly casts downloadedBytes");
        requireMissing(taskSelect, "COALESCE((result->>'totalBytes')::bigint",
                       "edge task select directly casts totalBytes");
        requireContains(nodeSelect, "status->'config'->>'activeVersion' ~ '^-?[0-9]{1,18}$'",
                        "edge node select does not guard config activeVersion");
        requireContains(nodeSelect, "capability->>'networkConfigVersion' ~ '^-?[0-9]{1,18}$'",
                        "edge node select does not guard networkConfigVersion");
        requireContains(nodeSelect, "CASE lower(COALESCE(capability->>'networkConfig', ''))",
                        "edge node select does not guard networkConfig capability");
        requireContains(taskSelect, "result->>'progressPercent' ~ '^-?[0-9]{1,18}$'",
                        "edge task select does not guard progressPercent");

        const auto serviceSource = edgeSource("service/domains/edge/edge.service.h");
        const auto controllerSource = edgeSource("service/domains/edge/edge.controller.h");
        const auto gatewaySource = edgeSource("service/features/edge/gateway.h");
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
        requireMissing(serviceSource, "const std::string action(body.action()->view());",
                       "edge modem dereferences optional action without validation");
        requireContains(serviceSource, "action != \"apply_profile\" && action != \"redial\"",
                        "edge modem accepts unknown actions as redial");
        requireContains(serviceSource,
                        "pdpType != \"IP\" && pdpType != \"IPV6\" && pdpType != \"IPV4V6\"",
                        "edge modem accepts unknown PDP type as IPv4");
        requireContains(serviceSource,
                        "authType != \"none\" && authType != \"pap\" && authType != \"chap\"",
                        "edge modem accepts unknown auth type as none");
        requireMissing(serviceSource, "queuePlatform(",
                       "edge service still exposes remote platform configuration");
        requireMissing(serviceSource, "deletePlatform(",
                       "edge service still exposes remote platform deletion");
        requireMissing(controllerSource, "/:id/platforms",
                       "edge controller still exposes platform management routes");
        requireMissing(serviceSource, "std::clamp<std::int64_t>(*query.limit(), 1, 48)",
                       "edge log request clamps invalid limit instead of rejecting it");
        requireMissing(serviceSource, "const auto level = std::string(body.level()->view());",
                       "edge log level dereferences optional level without validation");
        requireContains(serviceSource, "status != \"approved\" && status != \"rejected\"",
                        "edge enrollment accepts invalid registration status");
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
