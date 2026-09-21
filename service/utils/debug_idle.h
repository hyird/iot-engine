#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/redis/RedisTypes.h>

namespace service::debug_idle {

inline constexpr auto kIdle = std::chrono::minutes(5);

inline std::string leaseKey(std::string_view kind, std::string_view id) {
    std::string key = "iot:debug:lease:";
    key.append(kind);
    key.push_back(':');
    key.append(id);
    return key;
}

inline std::string watchKey(std::string_view kind, std::string_view id) {
    std::string key = "iot:debug:watch:";
    key.append(kind);
    key.push_back(':');
    key.append(id);
    return key;
}

template <typename Redis>
ruvia::Task<void> touch(const Redis& redis, std::string_view kind, std::string_view id) {
    ruvia::RedisSetOptions options;
    options.expiration = ruvia::RedisSetExpiration::expiresAfter(kIdle);
    (void)co_await redis.set(leaseKey(kind, id), "1", options);
}

template <typename Redis>
ruvia::Task<void> clear(const Redis& redis, std::string_view kind, std::string_view id) {
    (void)co_await redis.del(leaseKey(kind, id));
    (void)co_await redis.del(watchKey(kind, id));
}

template <typename Redis>
ruvia::Task<bool> leased(const Redis& redis, std::string_view kind, std::string_view id) {
    co_return co_await redis.exists(leaseKey(kind, id));
}

template <typename Disable>
void watch(const ruvia::WorkerHandle& worker, std::string kind, std::string id, Disable disable) {
    (void)worker.post([kind = std::move(kind), id = std::move(id), disable = std::move(disable)](
                          ruvia::WebWorkerContext& ctx) mutable -> ruvia::Task<void> {
        const auto watchId = watchKey(kind, id);
        ruvia::RedisSetOptions claim;
        claim.condition = ruvia::RedisSetCondition::kIfAbsent;
        claim.expiration = ruvia::RedisSetExpiration::expiresAfter(kIdle + std::chrono::seconds(30));
        const auto claimed = co_await ctx.redis().set(watchId, "1", claim);
        if (!claimed.applied()) {
            co_return;
        }
        while (!ctx.stopToken().stopRequested()) {
            const auto ttl = co_await ctx.redis().ttl(leaseKey(kind, id));
            if (ttl.state() == ruvia::RedisTtlState::kExpiring && ttl.remaining() &&
                *ttl.remaining() > std::chrono::milliseconds::zero()) {
                ruvia::RedisSetOptions keep;
                keep.expiration =
                    ruvia::RedisSetExpiration::expiresAfter(*ttl.remaining() + std::chrono::seconds(30));
                (void)co_await ctx.redis().set(watchId, "1", keep);
                (void)co_await ruvia::sleepFor(
                    ctx.worker(), *ttl.remaining() + std::chrono::seconds(1), ctx.stopToken());
                continue;
            }
            if (co_await leased(ctx.redis(), kind, id)) {
                (void)co_await ruvia::sleepFor(ctx.worker(), kIdle, ctx.stopToken());
                continue;
            }
            try {
                co_await disable(ctx, id);
            } catch (...) {
            }
            (void)co_await ctx.redis().del(watchId);
            co_return;
        }
    });
}

} // namespace service::debug_idle
