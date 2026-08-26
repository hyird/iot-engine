#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/application/runtime.h"
#include "service/common/message/contract.h"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::string source(std::string_view relative) {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      std::filesystem::path(relative);
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot read architecture source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void testLifecycleOrder() {
    service::observability::Registry metrics;
    service::application::Runtime runtime(metrics);
    std::vector<std::string> calls;
    runtime.add({.name = "database",
                 .start = [&] { calls.emplace_back("start-database"); },
                 .stop = [&] { calls.emplace_back("stop-database"); }});
    runtime.add({.name = "consumer",
                 .dependencies = {"database"},
                 .start = [&] { calls.emplace_back("start-consumer"); },
                 .stop = [&] { calls.emplace_back("stop-consumer"); }});
    runtime.start();
    require(metrics.ready(), "runtime did not become ready");
    runtime.stop();
    const std::vector<std::string> expected{"start-database", "start-consumer",
                                             "stop-consumer", "stop-database"};
    require(calls == expected, "runtime lifecycle order is incorrect");
}

void testLifecycleRollback() {
    service::observability::Registry metrics;
    service::application::Runtime runtime(metrics);
    bool stopped = false;
    runtime.add({.name = "first", .start = [] {}, .stop = [&] { stopped = true; }});
    runtime.add({.name = "broken",
                 .dependencies = {"first"},
                 .start = [] { throw std::runtime_error("expected"); },
                 .stop = [] {}});
    try {
        runtime.start();
        require(false, "runtime accepted a failed component");
    } catch (const std::runtime_error&) {
    }
    require(stopped, "runtime did not roll back started components");
    require(!metrics.ready(), "failed runtime reported ready");
}

void testMessageEnvelope() {
    service::message::IngressPacket packet;
    packet.messageId = "event-1";
    packet.linkId = "link-1";
    packet.connectionId = "connection-1";
    const auto fields = service::message::ingressFields(packet);
    const auto field = [&](std::string_view name) {
        for (const auto& item : fields)
            if (item.name == name)
                return item.value;
        return std::string{};
    };
    require(field("event_id") == packet.messageId, "message envelope has no event id");
    require(field("schema_version") == "1", "message envelope has no schema version");
    require(field("event_type") == "packet", "message envelope has no event type");
}

void testExplicitOutbox() {
    const auto schema = source("service/config/schema.h");
    const auto dispatcher = source("service/features/event/outbox.h");
    require(schema.find("0023_transactional_outbox") != std::string::npos,
            "transactional outbox migration is missing");
    require(schema.find("CREATE TRIGGER") == std::string::npos,
            "outbox must not use database triggers");
    require(dispatcher.find("FOR UPDATE SKIP LOCKED") != std::string::npos,
            "outbox dispatcher is not safe for concurrent instances");
    for (const auto path : {"service/domains/device/device.service.h",
                            "service/domains/link/link.service.h",
                            "service/domains/protocol/protocol.service.h",
                            "service/domains/access/access.service.h"}) {
        const auto content = source(path);
        require(content.find("enqueueConfigEvent(transaction") != std::string::npos,
                "domain write does not enqueue an outbox event in its transaction");
        require(content.find("publishConfigEvent") == std::string::npos,
                "domain still directly publishes config events");
    }
}

void testInjectedSharedState() {
    const auto auth = source("service/domains/auth/auth.service.h");
    require(auth.find("explicit AuthService(LoginRateLimiter& limiter)") != std::string::npos,
            "auth service does not inject its rate limiter");
    require(auth.find("iot:auth:login-failures:") != std::string::npos,
            "login rate limiting is not shared through Redis");
    require(auth.find("unordered_map") == std::string::npos,
            "login rate limiting still uses process-local state");
    const auto command = source("service/features/command/service.h");
    require(command.find("explicit CommandService(service::device::DeviceAccessService&") !=
                std::string::npos,
            "command service does not inject device authorization");
}

} // namespace

int main() {
    try {
        testLifecycleOrder();
        testLifecycleRollback();
        testMessageEnvelope();
        testExplicitOutbox();
        testInjectedSharedState();
        std::cout << "architecture tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "architecture test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
