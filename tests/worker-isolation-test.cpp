#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/web/detail/redis/RedisTypesAccess.h>

#include "service/common/observability.h"
#include "service/features/edge/edge.transport.h"
#include "service/middleware/live.h"

namespace {
struct RecordingRedis {
    mutable std::vector<std::string> keys;
    mutable std::vector<std::string> arguments;

    ruvia::Task<ruvia::RedisValue> eval(std::string_view, std::span<const std::string_view> inputKeys, std::span<const std::string_view> inputArguments) const {
        keys.assign(inputKeys.begin(), inputKeys.end());
        arguments.assign(inputArguments.begin(), inputArguments.end());
        co_return ruvia::detail::RedisTypesAccess::stringValue("1-0", std::pmr::get_default_resource());
    }
};

ruvia::Task<void> verifyMetadataOwner() {
    RecordingRedis redis;
    const auto published = co_await service::edge::projector_stream::publishMetadata(redis, 7, "node", "remote-instance");
    if (!published || redis.keys != std::vector<std::string>{ "iot:v3:edge:projector:lease:remote-instance:7", "iot:v3:edge:projector:remote-instance:7", "iot:v3:edge:projector:streams", "iot:service:worker:remote-instance:7:wake" } ||
        redis.arguments.empty() || redis.arguments[0] != "remote-instance:7") {
        throw std::runtime_error("metadata payload and wake did not retain the connection owner");
    }
}

struct Observation {
    const void* bus;
    std::string requests;
    bool received;
};

ruvia::Task<Observation> observe(ruvia::EventLoop loop, ruvia::WorkerHandle foreign, std::size_t index, std::promise<void>* ready) {
    auto& bus = service::live::bus();
    bus.setWorkerIndex(index);
    service::observability::Registry registry;
    service::observability::configureProcessRegistry(registry);
    bool rejected = false;
    try {
        (void)bus.subscribe(foreign, "device");
    } catch (const std::logic_error&) {
        rejected = true;
    }
    if (!rejected) {
        throw std::runtime_error("foreign worker subscription was accepted");
    }
    auto subscription = bus.subscribe(loop.handle(), "device");
    ready->set_value();
    const auto message = co_await subscription->receiver.receiveFor(std::chrono::milliseconds(500));
    if (bus.workerIndex() != index || service::observability::processRegistry() != &registry) {
        throw std::runtime_error("another worker replaced local state");
    }
    co_return Observation{ &bus, service::rpc::Contract::requests("test", bus.workerIndex()), message.hasValue() };
}
} // namespace

int main() {
    try {
        ruvia::EventLoopPool pool({ .loopCount = 2, .mailboxCapacity = 64 });
        pool.start();
        auto first = pool.loop(0);
        auto second = pool.loop(1);
        first.start(verifyMetadataOwner()).get();
        std::promise<void> firstReady, secondReady;
        auto firstPrepared = firstReady.get_future();
        auto secondPrepared = secondReady.get_future();
        auto firstTask = first.start(observe(first, second.handle(), 0, &firstReady));
        auto secondTask = second.start(observe(second, first.handle(), 1, &secondReady));
        if (firstPrepared.wait_for(std::chrono::seconds(5)) != std::future_status::ready ||
            secondPrepared.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("worker preparation failed");
        }
        firstPrepared.get();
        secondPrepared.get();
        if (!first.post([] {
                      service::live::bus().publish("device");
                  })
                 .accepted()) {
            throw std::runtime_error("local notification rejected");
        }
        const auto a = firstTask.get();
        const auto b = secondTask.get();
        pool.stop();
        pool.join();
        if (a.bus == b.bus || a.requests == b.requests || !a.received || b.received) {
            throw std::runtime_error("notification or request routing crossed workers");
        }
        std::cout << "worker isolation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "worker isolation test failed: " << error.what() << '\n';
        return 1;
    }
}
