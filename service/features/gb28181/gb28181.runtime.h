#pragma once

#include <asio/steady_timer.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/OneShot.h>
#include <ruvia/core/StopToken.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/redis/Redis.h>

#include "service/common/message.h"
#include "service/features/gb28181/device/device.runtime.h"
#include "service/features/gb28181/gb28181.config.h"
#include "service/features/gb28181/gb28181.service.h"
#include "service/features/gb28181/media/media.runtime.h"
#include "service/features/gb28181/media/media.transport.h"
#include "service/features/gb28181/sip/sip.transport.h"

namespace service::gb28181 {

// A GB28181 protocol actor is owned by exactly one Collector Worker.  The
// actor borrows that worker's already-driven EventLoop and RedisHandle; it does
// not create a private thread pool or route SIP packets through another
// worker.  `index` is an ownership label for diagnostics and SDK callback
// routing only, never a device/stream partition key.
class CollectorRuntime final {
  public:
    using OwnerIndex = std::size_t;

    CollectorRuntime(AppConfig config, ruvia::EventLoop loop, ruvia::RedisHandle redis, OwnerIndex index, OwnerIndex count);
    CollectorRuntime(const CollectorRuntime&) = delete;
    CollectorRuntime& operator=(const CollectorRuntime&) = delete;
    ~CollectorRuntime();

    [[nodiscard]] ruvia::Task<void> initialize();
    [[nodiscard]] ruvia::Task<void> shutdown();

    [[nodiscard]] bool enabled() const noexcept { return config_.enabled; }

    [[nodiscard]] bool started() const noexcept { return started_; }

    [[nodiscard]] OwnerIndex index() const noexcept { return index_; }

    [[nodiscard]] OwnerIndex count() const noexcept { return count_; }

    [[nodiscard]] ruvia::EventLoop loop() const noexcept { return loop_; }

    [[nodiscard]] ruvia::WorkerHandle worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] DeviceRegistry& devices() noexcept { return devices_; }

    [[nodiscard]] StreamRegistry& streams() noexcept { return streams_; }

    [[nodiscard]] SipServer* sip() noexcept { return sip_.get(); }

    // Called by the Collector command consumer on this same EventLoop.  The
    // operation is deliberately local and synchronous with respect to the SIP
    // actor; the returned JSON is sent back through the Redis command reply
    // stream by the caller.
    [[nodiscard]] ruvia::Task<std::string>
    execute(std::string operation, std::string payload, ruvia::StopToken stop = {});

  private:
    struct DeviceProjection final {
        Device device;
        DeviceChange change{ DeviceChange::Status };
        std::string ownerToken;
        std::string projectionId;
        std::uint64_t sequence{ 0 };
    };

    struct StreamProjection final {
        StreamStatus stream;
        std::string ownerToken;
        std::string projectionId;
        std::uint64_t sequence{ 0 };
    };

    using ProjectionCompletion = std::function<void(bool)>;

    struct ProjectionBarrier {
        ProjectionCompletion complete;
        std::uint64_t after{ 0 };
        std::uint64_t through{ 0 };
    };

    using ProjectionEvent = std::variant<DeviceProjection, StreamProjection, ProjectionBarrier>;

    AppConfig config_;
    ruvia::EventLoop loop_;
    ruvia::WorkerHandle worker_;
    ruvia::RedisHandle redis_;
    OwnerIndex index_{ 0 };
    OwnerIndex count_{ 0 };
    ruvia::TaskScope scope_;
    ruvia::TaskScope projectionScope_;
    DeviceRegistry devices_;
    StreamRegistry streams_;
    std::shared_ptr<SipServer> sip_;
    std::atomic_bool started_{ false };
    std::atomic_bool stopping_{ false };
    bool sdkRegistered_{ false };
    bool scopeJoined_{ false };
    std::string ownerToken_;
    std::unordered_map<std::string, std::uint64_t> streamGenerations_;
    std::unordered_map<std::string, bool> streamOnline_;
    std::unordered_map<std::string, std::string> previewOwners_;
    std::deque<ProjectionEvent> projectionQueue_;
    bool projectionDrainRunning_{ false };
    bool acceptingProjection_{ true };
    bool currentProjectionSucceeded_{ true };
    std::uint64_t projectionSequence_{ 0 };
    std::uint64_t lastSipBarrierSequence_{ 0 };
    std::deque<std::uint64_t> projectionFailures_;
    std::optional<std::uint64_t> activeControlProjection_;

    [[nodiscard]] ruvia::Task<void> publishDevice(const Device& device, DeviceChange change, std::string_view ownerToken, std::string_view projectionId);
    [[nodiscard]] ruvia::Task<void>
    publishStream(const StreamStatus& stream, std::string_view ownerToken, std::string_view projectionId);
    [[nodiscard]] ruvia::Task<bool> persistProjection(
        std::vector<service::message::StreamField> fields,
        std::string projectionId
    );
    void enqueueBarrier(ProjectionCompletion completion, std::uint64_t after);
    [[nodiscard]] ruvia::Task<bool> waitForProjection(ruvia::StopToken stop, std::uint64_t after);
    [[nodiscard]] ruvia::Task<void> retainPreview(const SipServer::PreviewStartResult& preview);
    [[nodiscard]] ruvia::Task<void> releasePreview(std::string sessionId);
    [[nodiscard]] ruvia::Task<void> publishConfig();
    [[nodiscard]] ruvia::Task<void> drainProjection();
    [[nodiscard]] ruvia::Task<void> controlLoop();
    [[nodiscard]] ruvia::Task<void>
    handleControl(const service::message::StreamMessage& message);
    [[nodiscard]] ruvia::Task<void> refreshOwnerLeases();
    [[nodiscard]] ruvia::Task<void> clearOwnerLeases();
    void enqueueDeviceProjection(const Device& device, DeviceChange change);
    void enqueueStreamProjection(const StreamStatus& stream);
    [[nodiscard]] std::string deviceOwnerToken(const Device& device) const;
    [[nodiscard]] std::string streamOwnerToken(const StreamStatus& stream);
    void registerSdkRoute();
    void unregisterSdkRoute() noexcept;
    void requireCurrentLoop() const;

    struct OwnerLease final {
        std::string token;
        std::string deviceId;
        std::string streamId;
        std::chrono::steady_clock::time_point expiresAt{};
        std::unique_ptr<asio::steady_timer> timer;
        bool renewing{ false };
    };

    std::unordered_map<std::string, OwnerLease> ownerLeases_;
    std::unordered_map<std::string, std::string> retiredOwnerTokens_;

    [[nodiscard]] ruvia::Task<bool>
    retainOwner(std::string key, std::string token, std::string deviceId = {}, std::string streamId = {});
    [[nodiscard]] bool ownsLocal(std::string_view key, std::string_view token) const;
    [[nodiscard]] ruvia::Task<void> releaseOwner(std::string key, std::string token);
    [[nodiscard]] ruvia::Task<void> renewOwnerLease(std::string key, std::string token);
    void armOwnerLeaseTimer(std::string key);
    void expireOwnerLease(std::string key, std::string token);
    void invalidateOwnerTarget(const std::string& key, const std::string& token, const std::string& deviceId, const std::string& streamId);
};

class Projector final : public GbProjectionService {
  public:
    using OwnerIndex = std::size_t;
    static constexpr OwnerIndex kUnassignedOwner =
        std::numeric_limits<OwnerIndex>::max();

    Projector() = default;
    Projector(const Projector&) = delete;
    Projector& operator=(const Projector&) = delete;

    ~Projector() { stop(); }

    [[nodiscard]] OwnerIndex workerIndex() const noexcept {
        return workerIndex_;
    }

    [[nodiscard]] ruvia::WebWorkerHandle worker() const noexcept {
        std::lock_guard lock(lifecycleMutex_);
        return worker_;
    }

    // Worker ids are the only stable link between a request context and the
    // projector that was prepared for that worker.  The index is a startup
    // label; it is never used to partition a device or stream.
    [[nodiscard]] ruvia::WorkerId workerId() const noexcept {
        std::lock_guard lock(lifecycleMutex_);
        return worker_.id();
    }

    void start(ruvia::WebWorkerHandle worker, OwnerIndex workerIndex, OwnerIndex serviceWorkerCount);
    void stop() noexcept;

  private:
    struct State final {
        ruvia::StopSource stop;
    };

    [[nodiscard]] ruvia::Task<void>
    consume(ruvia::WebWorkerContext& context, const std::shared_ptr<State>& state);

    std::atomic_bool running_{ false };
    mutable std::mutex lifecycleMutex_;
    ruvia::WebWorkerHandle worker_;
    OwnerIndex workerIndex_{ 0 };
    OwnerIndex serviceWorkerCount_{ 0 };
    std::shared_ptr<State> state_;
    std::shared_future<void> stopped_;
};

// Registered by the process-level Redis RPC consumer. The handler is the
// only feature entry point for northbound GB28181 management operations.
class GbControlRuntime final {
  public:
    static ruvia::Task<std::string> handle(ruvia::WebWorkerContext& context, std::string_view operation, std::string_view payload, ruvia::StopToken stop);
};

} // namespace service::gb28181
