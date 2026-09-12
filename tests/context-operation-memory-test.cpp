#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/Db.h"
#include "ruvia/web/detail/db/DbConfigStorage.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/Redis.h"
#include "ruvia/web/detail/redis/RedisConfigStorage.h"
#include "ruvia/web/detail/redis/RedisTypesAccess.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>

namespace {

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

void check(bool condition, const char* message) {
    if (!condition) {
        fail(message);
    }
}

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t live() const noexcept { return live_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++live_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        --live_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t live_{0};
};

#if defined(RUVIA_ENABLE_DATABASE) || defined(RUVIA_ENABLE_REDIS)
struct Fixture final {
    Fixture()
        : attachment(ruvia::attachEventLoop(io)),
          workerHandle(attachment.loop().handle()),
          requestMemory(workerMemory, std::span<std::byte>(requestBuffer)),
          request(ruvia::detail::HttpRequestAccess::make())
#ifdef RUVIA_ENABLE_DATABASE
          , dbDefinitions{ruvia::detail::DbDefinition{
                std::pmr::string("default", workerMemory.resource()),
                ruvia::detail::DbConfigStorage(
#ifdef RUVIA_ENABLE_MARIADB
                    ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb}, workerMemory.resource())}}
#else
                    ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql}, workerMemory.resource())}}
#endif
#endif
#ifdef RUVIA_ENABLE_REDIS
          , redisDefinitions{ruvia::detail::RedisDefinition{
                std::pmr::string("default", workerMemory.resource()),
                ruvia::detail::RedisConfigStorage({}, workerMemory.resource())}}
#endif
          , capabilities(io, workerHandle, workerMemory.resource(), definitions(), {}) {
        ruvia::detail::HttpRequestAccess::setResource(request, requestMemory.resource());
    }

    ruvia::detail::WorkerCapabilityDefinitions definitions() {
        return {
#ifdef RUVIA_ENABLE_DATABASE
            .databases = dbDefinitions,
#endif
#ifdef RUVIA_ENABLE_REDIS
            .redis = redisDefinitions,
#endif
        };
    }

    asio::io_context io;
    ruvia::EventLoopAttachment attachment;
    ruvia::WorkerHandle workerHandle;
    ruvia::WorkerMemory workerMemory;
    std::array<std::byte, 64 * 1024> requestBuffer{};
    ruvia::RequestMemory requestMemory;
    ruvia::HttpRequest request;
    ruvia::StopToken stopToken;
#ifdef RUVIA_ENABLE_DATABASE
    std::array<ruvia::detail::DbDefinition, 1> dbDefinitions;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::array<ruvia::detail::RedisDefinition, 1> redisDefinitions;
#endif
    ruvia::detail::WorkerCapabilities capabilities;
};
#endif

void resultStorageIsReclaimed() {
#ifdef RUVIA_ENABLE_DATABASE
    {
    CountingResource resource;
    {
        auto retained = ruvia::detail::DbResultAccess::makeResult(&resource);
        auto& rows = ruvia::detail::DbResultAccess::rows(retained);
        auto row = ruvia::detail::DbResultAccess::ownedRow(&resource);
        ruvia::detail::DbResultAccess::ownedColumnNames(row).emplace_back("value");
        ruvia::detail::DbResultAccess::ownedFields(row).push_back(
            ruvia::detail::DbResultAccess::ownedField(std::string(2048, 'r'), &resource));
        rows.push_back(std::move(row));
        const auto baseline = resource.live();
        check(retained.front()["value"].value() == std::string(2048, 'r'),
            "retained DB result changed");
        for (int index = 0; index != 100; ++index) {
            {
                auto transient = ruvia::detail::DbResultAccess::makeResult(&resource);
                auto transientRow = ruvia::detail::DbResultAccess::ownedRow(&resource);
                ruvia::detail::DbResultAccess::ownedFields(transientRow).push_back(
                    ruvia::detail::DbResultAccess::ownedField(
                        std::string(2048, char('a' + index % 26)), &resource));
                ruvia::detail::DbResultAccess::rows(transient).push_back(std::move(transientRow));
                check(resource.live() > baseline, "DB result did not allocate");
            }
            check(resource.live() == baseline, "DB result storage accumulated");
        }
        check(resource.live() == baseline, "DB result storage accumulated");
    }
    check(resource.live() == 0, "DB result storage was not released");
    }
#endif
#ifdef RUVIA_ENABLE_REDIS
    {
    CountingResource resource;
    {
        auto retained = ruvia::detail::RedisTypesAccess::stringValue("retained", &resource);
        const auto baseline = resource.live();
        for (int index = 0; index != 100; ++index) {
            {
                auto transient = ruvia::detail::RedisTypesAccess::stringValue(
                    std::string(2048, char('a' + index % 26)), &resource);
                check(transient.string().size() == 2048, "Redis result value mismatch");
            }
            check(resource.live() == baseline, "Redis result storage accumulated");
        }
        check(retained.string() == "retained", "retained Redis result changed");
    }
    check(resource.live() == 0, "Redis result storage was not released");
    }
#endif
}

}  // namespace

int main() {
#if defined(RUVIA_ENABLE_DATABASE) || defined(RUVIA_ENABLE_REDIS)
    Fixture fixture;
    auto context = ruvia::detail::ContextAccess::make(fixture.requestMemory, fixture.request,
        fixture.capabilities.contextServices(fixture.stopToken));
    check(context.pool() == fixture.workerMemory.resource(),
        "Context operation resource is not worker owned");
#ifdef RUVIA_ENABLE_DATABASE
    auto database = context.db();
#endif
#ifdef RUVIA_ENABLE_REDIS
    auto redis = context.redis();
#endif
    const auto adjacentProbes = [&](const void* first, const void* second) {
        const auto begin = reinterpret_cast<std::uintptr_t>(fixture.requestBuffer.data());
        const auto end = begin + fixture.requestBuffer.size();
        const auto left = reinterpret_cast<std::uintptr_t>(first);
        const auto right = reinterpret_cast<std::uintptr_t>(second);
        return left >= begin && left < end && right >= begin && right < end &&
               (left + 1 == right || right + 1 == left);
    };
    for (int index = 0; index != 2000; ++index) {
        auto* before = static_cast<std::byte*>(fixture.requestMemory.resource()->allocate(1, 1));
#ifdef RUVIA_ENABLE_DATABASE
        {
            auto operation = database.query(
                "SELECT ?", std::string(2048, char('a' + index % 26)));
            (void)operation;
        }
#endif
#ifdef RUVIA_ENABLE_REDIS
        {
            auto operation = redis.command(
                "SET", "key", std::string(2048, char('a' + index % 26)));
            (void)operation;
        }
#endif
        auto* after = static_cast<std::byte*>(fixture.requestMemory.resource()->allocate(1, 1));
        // A monotonic resource may consume its buffer from either end (libc++
        // allocates downwards). The probes must remain adjacent either way.
        check(adjacentProbes(before, after),
            "long lived operation advanced request arena");
    }
    const auto* before = fixture.requestMemory.resource()->allocate(1, 1);
    (void)fixture.requestMemory.resource()->allocate(2048, 1);
    const auto* after = fixture.requestMemory.resource()->allocate(1, 1);
    check(!adjacentProbes(before, after), "arena probe missed an injected request allocation");
#endif
    resultStorageIsReclaimed();
    return 0;
}
