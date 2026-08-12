#pragma once

#include <array>
#include <chrono>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/detail/redis/RedisRegistry.h>
#include <ruvia/web/redis/Redis.h>

namespace service::collector {

// Collector's protocol actor already owns its EventLoop and keeps Redis operations
// inside the same structured TaskScope. This worker-affine adapter provides one
// connection for business commands and one dedicated connection for blocking reads.
// Ruvia's worker-local registry owns one logical alias with an ordinary pool and
// a lazy blocking pool. Passing the owning WorkerHandle lets both pools arm exact
// deadline and cancellation timers without a periodic scan.
class Client final {
  public:
    Client(asio::io_context& ioContext, ruvia::RedisConfig config,
           const ruvia::WorkerHandle& worker)
        : resource_(), operationScope_(),
          definitions_{makeDefinition(std::move(config), &resource_)},
          registry_(ioContext, &resource_, definitions_, &worker) {}

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ~Client() { close(); }

    [[nodiscard]] ruvia::Task<void> connect() {
        co_await registry_.connect();
    }

    [[nodiscard]] auto withOptions(ruvia::OperationOptions options) const {
        return registry_.get(&resource_, operationScope_).withOptions(std::move(options));
    }

    void close() noexcept {
        if (closed_)
            return;
        closed_ = true;
        registry_.closeNow();
    }

    [[nodiscard]] ruvia::Task<ruvia::RedisValue>
    command(std::span<const std::string_view> args) const {
        auto redis = withOptions(ruvia::OperationOptions{.timeout = kCommandTimeout});
        co_return co_await redis.command(args);
    }

    [[nodiscard]] ruvia::Task<ruvia::RedisValue>
    eval(std::string_view script, std::span<const std::string_view> keys = {},
         std::span<const std::string_view> args = {}) const {
        auto redis = withOptions(ruvia::OperationOptions{.timeout = kCommandTimeout});
        co_return co_await redis.eval(script, keys, args);
    }

  private:
    static ruvia::detail::RedisDefinition
    makeDefinition(ruvia::RedisConfig config, std::pmr::memory_resource* resource) {
        config.poolSizePerWorker = 1;
        config.blockingPoolSizePerWorker = 1;
        // Ordinary commands carry an exact per-operation timeout below. The blocking
        // pool stays on BLOCK 0 until its StopToken fires, with no timer wakeups.
        config.commandTimeout = std::nullopt;
        return {
            std::pmr::string(ruvia::detail::kDefaultRedisAlias, resource),
            ruvia::detail::RedisConfigStorage(config, resource),
        };
    }

    static constexpr auto kCommandTimeout = std::chrono::seconds(30);

    mutable std::pmr::unsynchronized_pool_resource resource_;
    mutable ruvia::detail::ScopedOperationScope operationScope_;
    std::array<ruvia::detail::RedisDefinition, 1> definitions_;
    ruvia::detail::RedisRegistry registry_;
    bool closed_ = false;
};

} // namespace service::collector
