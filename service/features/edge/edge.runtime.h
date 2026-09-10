#pragma once

#include "service/common/observability.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/StopToken.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/features/edge/edge.transport.h"
#include "service/features/event/event.transport.h"
#include "service/features/event/stream_multiplexer/stream_multiplexer.runtime.h"

namespace service::edge {

// One instance is created by App::useWorkerState for every Service Worker.
// The session table and Redis notification Stream are worker-local: no wakeup,
// callback, or socket is forwarded to another worker.
class Dispatcher final {
  public:
    using SessionWake = std::function<void()>;
    using SessionFailure = std::function<void(std::string_view)>;

    Dispatcher() = default;
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Dispatcher(Dispatcher&&) noexcept = default;
    Dispatcher& operator=(Dispatcher&&) = delete;

    [[nodiscard]] std::size_t workerIndex() const {
        if (!workerIndex_) {
            throw std::runtime_error("edge dispatcher is not ready");
        }
        return *workerIndex_;
    }

    void registerSession(
        std::string nodeId,
        std::uint64_t epoch,
        SessionWake wake,
        SessionFailure failure = {}
    ) {
        if (!running_ || failed_ || !wake) {
            throw std::runtime_error("edge dispatcher is not ready");
        }
        sessions_.insert_or_assign(
            std::move(nodeId),
            SessionTarget{ epoch, std::move(wake), std::move(failure) }
        );
    }

    void unregisterSession(std::string_view nodeId, std::uint64_t epoch) noexcept {
        const auto current = sessions_.find(nodeId);
        if (current != sessions_.end() && current->second.epoch == epoch) {
            sessions_.erase(current);
        }
    }

    void requestStop() noexcept {
        if (stopSource_) {
            stopSource_->requestStop();
        }
        service::message::workerStreamMultiplexer().signal(
            service::message::WorkerStreamTask::EdgeDispatcher
        );
    }

    // Projector lease failure is a worker-local lifecycle event.  Every
    // session registered on this Dispatcher is closed on the same Worker so
    // no socket, callback, or mutable session state crosses workers.
    void failSessions(std::string_view reason) noexcept {
        failed_ = true;
        for (auto& [nodeId, target] : sessions_) {
            (void)nodeId;
            try {
                if (target.failure) {
                    target.failure(reason);
                }
            } catch (const std::exception& error) {
                std::cerr << "edge session failure notification failed: "
                          << error.what() << '\n';
            } catch (...) {
                std::cerr << "edge session failure notification failed: unknown error\n";
            }
        }
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t workerIndex, std::shared_ptr<std::promise<void>> ready, std::shared_ptr<std::promise<void>> stopped) {
        bool readySet = false;
        try {
            if (running_) {
                throw std::runtime_error("edge dispatcher is already running");
            }
            workerIndex_ = workerIndex;
            failed_ = false;
            stopSource_ = std::make_unique<ruvia::StopSource>();
            const auto redis = context.redis();
            const auto workerStream = dispatch::stream(workerIndex);
            const auto consumer = "service-edge-dispatcher-" +
                std::to_string(workerIndex);
            co_await service::message::redis::ensureGroup(
                redis,
                workerStream,
                dispatch::kGroup
            );
            running_ = true;
            ready->set_value();
            readySet = true;
            bool recovering = true;
            const auto stopToken = ruvia::combineStopTokens(
                context.stopToken(),
                stopSource_->token()
            );
            while (!stopToken.stopRequested()) {
                bool failed = false;
                try {
                    auto messages = recovering
                        ? co_await service::message::redis::readGroup(
                              redis,
                              workerStream,
                              dispatch::kGroup,
                              consumer,
                              "0",
                              std::chrono::milliseconds(0),
                              256
                          )
                        : co_await service::message::redis::readGroup(
                              redis,
                              workerStream,
                              dispatch::kGroup,
                              consumer,
                              ">",
                              std::chrono::milliseconds(0),
                              256
                          );
                    if (recovering && messages.empty()) {
                        recovering = false;
                        continue;
                    }
                    if (messages.empty()) {
                        co_await service::message::workerStreamMultiplexer().wait(
                            workerIndex,
                            service::message::WorkerStreamTask::EdgeDispatcher,
                            stopToken
                        );
                        continue;
                    }
                    for (const auto& message : messages) {
                        const auto event = dispatch::eventFrom(message);
                        if (event.kind == dispatch::kNodeKind &&
                            !event.nodeId.empty() && !deliver(event.nodeId)) {
                            recovering = true;
                            failed = true;
                            break;
                        }
                        co_await service::message::redis::acknowledgeAndDelete(
                            redis,
                            workerStream,
                            dispatch::kGroup,
                            message.id
                        );
                    }
                } catch (const std::exception& error) {
                    if (stopToken.stopRequested()) {
                        break;
                    }
                    std::cerr << "edge dispatch worker " << workerIndex
                              << " read failed: " << error.what() << '\n';
                    recovering = true;
                    failed = true;
                }
                if (failed) {
                    (void)co_await ruvia::sleepFor(
                        context.worker(),
                        std::chrono::milliseconds(250)
                    );
                }
            }
        } catch (...) {
            if (!readySet) {
                try {
                    ready->set_exception(std::current_exception());
                } catch (...) {
                }
            }
        }
        running_ = false;
        sessions_.clear();
        stopSource_.reset();
        try {
            stopped->set_value();
        } catch (...) {
        }
    }

  private:
    struct SessionTarget {
        std::uint64_t epoch{};
        SessionWake wake;
        SessionFailure failure;
    };

    bool deliver(std::string_view nodeId) {
        const auto current = sessions_.find(nodeId);
        if (current == sessions_.end()) {
            return true;
        }
        try {
            current->second.wake();
            return true;
        } catch (const std::exception& error) {
            std::cerr << "edge dispatch worker " << *workerIndex_
                      << " wake failed: " << error.what() << '\n';
            return false;
        } catch (...) {
            std::cerr << "edge dispatch worker " << *workerIndex_
                      << " wake failed: unknown error\n";
            return false;
        }
    }

    std::map<std::string, SessionTarget, std::less<>> sessions_;
    std::unique_ptr<ruvia::StopSource> stopSource_;
    std::optional<std::size_t> workerIndex_;
    bool running_{ false };
    bool failed_{ false };
};

// One supervisor is assembled for each Service Worker. Functional state remains
// in that worker's Dispatcher instance above.
class DispatcherRuntime final {
  public:
    DispatcherRuntime() = default;
    DispatcherRuntime(const DispatcherRuntime&) = delete;
    DispatcherRuntime& operator=(const DispatcherRuntime&) = delete;

    ~DispatcherRuntime() { stop(); }

    void start(ruvia::WebWorkerHandle worker, std::size_t workerIndex, std::size_t serviceWorkerCount) {
        if (running_.exchange(true)) {
            return;
        }
        worker_ = std::move(worker);
        workerIndex_ = workerIndex;
        serviceWorkerCount_ = serviceWorkerCount;
        if (!worker_.valid() || serviceWorkerCount_ == 0 ||
            workerIndex_ >= serviceWorkerCount_) {
            running_.store(false);
            worker_ = {};
            throw std::runtime_error("edge dispatcher requires service workers");
        }
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        auto readiness = ready->get_future();
        stopped_ = stopped->get_future().share();
        try {
            const auto posted = worker_.post(
                [workerIndex, ready, stopped](ruvia::WebWorkerContext& context) {
                    return context.workerState<Dispatcher>().run(
                        context,
                        workerIndex,
                        ready,
                        stopped
                    );
                }
            );
            if (!posted.accepted()) {
                stopped->set_value();
                throw std::runtime_error("service worker rejected edge dispatcher");
            }
            readiness.get();
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() noexcept {
        if (!running_.exchange(false)) {
            return;
        }
        if (worker_.valid()) {
            const auto worker = worker_;
            const auto wakeAccepted = worker.post([](ruvia::WebWorkerContext& context) -> ruvia::Task<void> {
                                                context.workerState<Dispatcher>().requestStop();
                                                co_return;
                                            })
                                          .accepted();
            if (!wakeAccepted) {
                // A closed worker already propagates its stop token to Dispatcher.
            }
        }
        if (stopped_.valid()) {
            stopped_.wait();
        }
        stopped_ = {};
        worker_ = {};
    }

  private:
    ruvia::WebWorkerHandle worker_;
    std::shared_future<void> stopped_;
    std::size_t workerIndex_ = 0;
    std::size_t serviceWorkerCount_ = 0;
    std::atomic_bool running_{ false };
};

} // namespace service::edge

#include "service/features/edge/edge.service.h"

namespace service::edge {

class EdgeControlRuntime final {
  public:
    static ruvia::Task<std::string> handle(ruvia::WebWorkerContext& context, std::string_view operation, std::string_view payload, ruvia::StopToken stop) {
        if (stop.stopRequested()) {
            service::common::fail(10004, "Edge operation cancelled", 503);
        }
        if (operation == "queue-snapshot") {
            const auto separator = payload.find('\n');
            if (separator == std::string_view::npos) {
                service::common::fail(10002, "Invalid edge snapshot payload", 400);
            }
            const auto nodeId = payload.substr(0, separator);
            const auto actorId = payload.substr(separator + 1);
            if (!service::common::isUuid(nodeId) || !service::common::isUuid(actorId)) {
                service::common::fail(10002, "Invalid edge snapshot identifiers", 400);
            }
            const auto revision = co_await configService().queueSnapshot(
                context,
                nodeId,
                actorId
            );
            co_return std::to_string(revision);
        }

        const auto separator = payload.find('\n');
        if (separator == std::string_view::npos || !service::common::isUuid(payload.substr(0, separator))) {
            service::common::fail(10002, "Invalid edge control payload", 400);
        }
        const auto nodeId = payload.substr(0, separator);
        const auto requestPayload = payload.substr(separator + 1);
        if (requestPayload.empty()) {
            service::common::fail(10002, "Empty edge control request", 400);
        }

        pb::Envelope envelope = protocol::outbound(nodeId);
        bool parsed = false;
        if (operation == "queue-network") {
            pb::NetworkConfigRequest request;
            parsed = request.ParseFromArray(requestPayload.data(), static_cast<int>(requestPayload.size()));
            if (parsed) {
                *envelope.mutable_network_config_request() = std::move(request);
            }
        } else if (operation == "queue-firmware") {
            pb::FirmwareUpdateRequest request;
            parsed = request.ParseFromArray(requestPayload.data(), static_cast<int>(requestPayload.size()));
            if (parsed) {
                *envelope.mutable_firmware_update_request() = std::move(request);
            }
        } else if (operation == "request-logs") {
            pb::LogRequest request;
            parsed = request.ParseFromArray(requestPayload.data(), static_cast<int>(requestPayload.size()));
            if (parsed) {
                *envelope.mutable_log_request() = std::move(request);
            }
        } else if (operation == "set-log-level") {
            pb::LogLevelRequest request;
            parsed = request.ParseFromArray(requestPayload.data(), static_cast<int>(requestPayload.size()));
            if (parsed) {
                *envelope.mutable_log_level_request() = std::move(request);
            }
        } else {
            service::common::fail(10002, "Unknown edge operation", 400);
        }
        if (!parsed) {
            service::common::fail(10002, "Invalid edge control request", 400);
        }
        const auto wire = protocol::encode(envelope);
        if (wire.empty()) {
            service::common::fail(10002, "Invalid edge control request", 400);
        }
        co_await dispatch::enqueue(context.redis(), nodeId, wire);
        co_return "{}";
    }
};

class Projector final : private EdgeProjectionService {
  public:
    Projector() = default;
    Projector(const Projector&) = delete;
    Projector& operator=(const Projector&) = delete;

    ~Projector() { stop(); }

    void start(ruvia::WebWorkerHandle worker, std::size_t workerIndex, std::size_t serviceWorkerCount) {
        if (running_.exchange(true)) {
            return;
        }
        failed_.store(false);
        worker_ = std::move(worker);
        workerIndex_ = workerIndex;
        serviceWorkerCount_ = serviceWorkerCount;
        if (!worker_.valid() || serviceWorkerCount_ == 0 ||
            workerIndex_ >= serviceWorkerCount_) {
            running_.store(false);
            worker_ = {};
            throw std::runtime_error("edge projector requires Service Workers");
        }
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        auto readiness = ready->get_future();
        stopped_ = stopped->get_future().share();
        const auto posted = worker_.post(
            [this, ready, stopped](ruvia::WebWorkerContext& context) {
                leaseScope_ = std::make_unique<ruvia::TaskScope>(context.worker());
                return run(context, workerIndex_, ready, stopped);
            }
        );
        if (!posted.accepted()) {
            stopped->set_value();
            running_.store(false);
            worker_ = {};
            stopped_ = {};
            leaseScope_.reset();
            throw std::runtime_error("service worker rejected edge projector");
        }
        try {
            readiness.get();
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() noexcept {
        const bool wasRunning = running_.exchange(false);
        if (!wasRunning && !stopped_.valid()) {
            return;
        }
        if (leaseScope_) {
            leaseScope_->requestStop();
        }
        if (worker_.valid()) {
            const auto worker = worker_;
            const auto wakeAccepted = worker.post([](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
                                                service::message::workerStreamMultiplexer().signal(
                                                    service::message::WorkerStreamTask::EdgeProjector
                                                );
                                                co_return;
                                            })
                                          .accepted();
            if (!wakeAccepted) {
                // A closed worker already propagates its stop token to the task.
            }
        }
        if (stopped_.valid()) {
            stopped_.wait();
        }
        stopped_ = {};
        leaseScope_.reset();
        worker_ = {};
    }

    [[nodiscard]] bool failed() const noexcept { return failed_.load(); }

  private:
    static constexpr std::size_t kBatchSize = 512;
    static constexpr auto kLeaseRenewInterval = std::chrono::seconds(5);
    static constexpr auto kRecoverySweepInterval =
        std::chrono::seconds(1);

    void markLeaseLost(ruvia::WebWorkerContext& context) noexcept {
        failed_.store(true);
        if (leaseLost_.exchange(true)) {
            return;
        }
        try {
            if (auto* registry = service::observability::processRegistry()) {
                registry->component("edge-projector", service::observability::ComponentState::Failed, "worker lease lost");
            }
        } catch (...) {
        }
        try {
            // This is deliberately resolved from the current worker context;
            // no session or callback is routed through another Worker.
            context.workerState<Dispatcher>().failSessions("worker lease lost");
        } catch (const std::exception& error) {
            std::cerr << "edge worker-local session shutdown failed: "
                      << error.what() << '\n';
        } catch (...) {
            std::cerr << "edge worker-local session shutdown failed: unknown error\n";
        }
        try {
            service::message::workerStreamMultiplexer().signal(
                service::message::WorkerStreamTask::EdgeProjector
            );
        } catch (...) {
        }
    }

    struct DeadStream final {
        std::string stream;
        std::string lease;
        std::string token;
    };

    template <typename Redis>
    static ruvia::Task<bool> acquireLease(const Redis& redis, std::size_t index) {
        static constexpr std::string_view script = R"lua(
if redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2], 'NX') then return 1 end
return 0
)lua";
        const auto key = projector_stream::leaseKey(index);
        const auto token = projector_stream::ownerToken(index);
        const auto ttl = std::to_string(projector_stream::kLeaseTtl.count());
        const std::string_view keys[]{ key };
        const std::string_view arguments[]{ token, ttl };
        const auto reply = co_await redis.eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("acquire edge projector lease", reply);
        }
        co_return reply.integer() == 1;
    }

    template <typename Redis>
    static ruvia::Task<bool> renewLeaseKey(const Redis& redis, std::string_view key, std::string_view token) {
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('PEXPIRE', KEYS[1], ARGV[2])
return 1
)lua";
        const auto ttl = std::to_string(projector_stream::kLeaseTtl.count());
        const std::string_view keys[]{ key };
        const std::string_view arguments[]{ token, ttl };
        const auto reply = co_await redis.eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("renew edge projector lease", reply);
        }
        co_return reply.integer() == 1;
    }

    template <typename Redis>
    static ruvia::Task<bool> leaseOwned(const Redis& redis, std::string_view key, std::string_view token) {
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) == ARGV[1] then return 1 end
return 0
)lua";
        const std::string_view keys[]{ key };
        const std::string_view arguments[]{ token };
        const auto reply = co_await redis.eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("check edge projector lease", reply);
        }
        co_return reply.integer() == 1;
    }

    template <typename Redis>
    static ruvia::Task<bool> renewLease(const Redis& redis, std::size_t index) {
        const auto key = projector_stream::leaseKey(index);
        const auto token = projector_stream::ownerToken(index);
        co_return co_await renewLeaseKey(redis, key, token);
    }

    template <typename Redis>
    static ruvia::Task<bool> releaseLeaseKey(const Redis& redis, std::string_view key, std::string_view token) {
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
return redis.call('DEL', KEYS[1])
)lua";
        const std::string_view keys[]{ key };
        const std::string_view arguments[]{ token };
        const auto reply = co_await redis.eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("release edge projector lease", reply);
        }
        co_return reply.integer() == 1;
    }

    template <typename Redis>
    static ruvia::Task<bool> releaseLease(const Redis& redis, std::size_t index) {
        const auto key = projector_stream::leaseKey(index);
        const auto token = projector_stream::ownerToken(index);
        co_return co_await releaseLeaseKey(redis, key, token);
    }

    template <typename Redis>
    static ruvia::Task<void> registerStream(const Redis& redis, std::string_view streamName) {
        const auto reply = co_await service::message::redis::command(
            redis,
            { "SADD", std::string(projector_stream::kStreamRegistry), std::string(streamName) }
        );
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("register edge projector stream", reply);
        }
    }

    static std::optional<std::pair<std::string, std::size_t>> ownerFromStream(
        std::string_view streamName
    ) {
        if (!streamName.starts_with(projector_stream::kStreamPrefix)) {
            return std::nullopt;
        }
        const auto suffix = streamName.substr(projector_stream::kStreamPrefix.size());
        const auto separator = suffix.rfind(':');
        if (separator == std::string_view::npos || separator == 0 ||
            separator + 1 >= suffix.size()) {
            return std::nullopt;
        }
        const auto instance = suffix.substr(0, separator);
        if (instance.find(':') != std::string_view::npos) {
            return std::nullopt;
        }
        std::uint64_t parsedIndex = 0;
        const auto indexText = suffix.substr(separator + 1);
        const auto [end, error] = std::from_chars(
            indexText.data(),
            indexText.data() + indexText.size(),
            parsedIndex
        );
        if (error != std::errc{} || end != indexText.data() + indexText.size() ||
            parsedIndex > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return std::nullopt;
        }
        return std::pair{ std::string(instance), static_cast<std::size_t>(parsedIndex) };
    }

    static std::string recoveryToken(std::size_t workerIndex, const std::pair<std::string, std::size_t>& owner) {
        return std::string(service::runtime::instanceId()) + ":recovery:" +
            std::to_string(workerIndex) + ":" + owner.first + ":" +
            std::to_string(owner.second);
    }

    template <typename Redis>
    static ruvia::Task<bool> claimRecoveryLease(const Redis& redis, std::string_view key, std::string_view token) {
        static constexpr std::string_view script = R"lua(
if redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2], 'NX') then return 1 end
return 0
)lua";
        const auto ttl = std::to_string(projector_stream::kLeaseTtl.count());
        const std::string_view keys[]{ key };
        const std::string_view arguments[]{ token, ttl };
        const auto reply = co_await redis.eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("claim dead edge projector lease", reply);
        }
        co_return reply.integer() == 1;
    }

    template <typename Redis>
    static ruvia::Task<std::vector<DeadStream>> discoverDeadStreams(
        const Redis& redis,
        std::string_view currentStream,
        std::size_t workerIndex
    ) {
        std::map<std::string, DeadStream, std::less<>> dead;
        const auto candidates = co_await service::message::redis::keysMatching(
            redis,
            std::string(projector_stream::kStreamPrefix) + "*"
        );
        for (const auto& candidate : candidates) {
            if (candidate == currentStream) {
                continue;
            }
            const auto owner = ownerFromStream(candidate);
            if (!owner) {
                continue;
            }
            const auto lease = projector_stream::leaseKey(owner->second, owner->first);
            const auto token = recoveryToken(workerIndex, *owner);
            const auto current = co_await redis.get(lease);
            if (current) {
                if (std::string_view(current->data(), current->size()) != token) {
                    // A non-expired owner or another recovery worker still owns
                    // this stream.  Neither case is safe to steal.
                    continue;
                }
                // Seeing our recovery token is not enough to keep reading: the
                // lease may expire between discovery and the first XREADGROUP.
                // Renewal is token-checked and never creates a missing lease.
                if (!co_await renewLeaseKey(redis, lease, token)) {
                    continue;
                }
            } else if (!co_await claimRecoveryLease(redis, lease, token)) {
                continue;
            }
            dead.emplace(candidate, DeadStream{ candidate, lease, token });
        }
        std::vector<DeadStream> result;
        result.reserve(dead.size());
        for (auto& [streamName, deadStream] : dead) {
            (void)streamName;
            result.push_back(std::move(deadStream));
        }
        co_return result;
    }

    template <typename Redis>
    static ruvia::Task<bool> eraseDeadStream(const Redis& redis, const DeadStream& deadStream) {
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
if redis.call('EXISTS', KEYS[2]) ~= 0 then
  if redis.call('XLEN', KEYS[2]) ~= 0 then return 0 end
  local groups = redis.call('XINFO', 'GROUPS', KEYS[2])
  for _, group in ipairs(groups) do
    for index = 1, #group, 2 do
      if group[index] == 'pending' and tonumber(group[index + 1]) > 0 then
        return 0
      end
    end
  end
  redis.call('DEL', KEYS[2])
end
redis.call('DEL', KEYS[1])
redis.call('SREM', KEYS[3], KEYS[2])
return 1
)lua";
        const std::string registry(projector_stream::kStreamRegistry);
        const std::string_view keys[]{ deadStream.lease, deadStream.stream, registry };
        const std::string_view arguments[]{ deadStream.token };
        const auto reply = co_await redis.eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("erase dead edge projector stream", reply);
        }
        co_return reply.integer() == 1;
    }

    template <typename Redis>
    static ruvia::Task<bool> acknowledgeAndDeleteFenced(
        const Redis& redis,
        std::string_view lease,
        std::string_view token,
        std::string_view stream,
        std::string_view group,
        const std::vector<service::message::StreamMessage>& messages
    ) {
        if (messages.empty()) {
            co_return true;
        }
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
local group = ARGV[2]
for index = 3, #ARGV do
  redis.call('XACK', KEYS[2], group, ARGV[index])
  redis.call('XDEL', KEYS[2], ARGV[index])
end
return 1
)lua";
        const std::string leaseKey(lease);
        const std::string streamKey(stream);
        const std::string_view keys[]{ leaseKey, streamKey };
        std::vector<std::string> scriptArguments;
        scriptArguments.reserve(messages.size() + 2);
        scriptArguments.emplace_back(token);
        scriptArguments.push_back(std::string(group));
        for (const auto& message : messages) {
            scriptArguments.push_back(message.id);
        }
        std::vector<std::string_view> scriptViews;
        scriptViews.reserve(scriptArguments.size());
        for (const auto& argument : scriptArguments) {
            scriptViews.emplace_back(argument);
        }
        const auto reply = co_await redis.eval(
            script,
            keys,
            std::span<const std::string_view>(scriptViews)
        );
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("fenced edge projector acknowledgement", reply);
        }
        co_return reply.integer() == 1;
    }

    std::vector<std::pair<std::string, std::string>> recoveryLeaseSnapshot() const {
        std::vector<std::pair<std::string, std::string>> leases;
        leases.reserve(recoveryLeases_.size());
        for (const auto& [key, token] : recoveryLeases_) {
            leases.emplace_back(key, token);
        }
        return leases;
    }

    template <typename Redis>
    ruvia::Task<void> maintainLeases(ruvia::WebWorkerContext& context, const Redis& redis, std::size_t index, ruvia::StopToken scopeStop) {
        const auto stop = ruvia::combineStopTokens(context.stopToken(), scopeStop);
        while (running_.load() && !stop.stopRequested()) {
            (void)co_await ruvia::sleepFor(
                context.worker(),
                kLeaseRenewInterval,
                stop
            );
            if (stop.stopRequested() || !running_.load()) {
                break;
            }
            try {
                if (!co_await renewLease(redis, index)) {
                    markLeaseLost(context);
                    co_return;
                }
                std::vector<std::string> lostRecoveryLeases;
                for (const auto& [key, token] : recoveryLeaseSnapshot()) {
                    if (!co_await renewLeaseKey(redis, key, token)) {
                        lostRecoveryLeases.push_back(key);
                    }
                }
                for (const auto& key : lostRecoveryLeases) {
                    recoveryLeases_.erase(key);
                    lostRecoveryLeases_.insert(key);
                }
                if (!lostRecoveryLeases.empty()) {
                    service::message::workerStreamMultiplexer().signal(
                        service::message::WorkerStreamTask::EdgeProjector
                    );
                }
            } catch (const std::exception& error) {
                markLeaseLost(context);
                std::cerr << "edge projector lease renewal failed: " << error.what() << '\n';
                co_return;
            }
        }
    }

    template <typename Redis>
    static ruvia::Task<void> cleanupRegistry(const Redis& redis) {
        static constexpr std::string_view script = R"lua(
if redis.call('EXISTS', KEYS[2]) == 0 then
  return redis.call('SREM', KEYS[1], KEYS[2])
end
return 0
)lua";
        const auto reply = co_await service::message::redis::command(
            redis,
            { "SMEMBERS", std::string(projector_stream::kStreamRegistry) }
        );
        if (reply.kind() == ruvia::RedisValue::Kind::kError &&
            reply.error().starts_with("WRONGTYPE")) {
            service::message::redis::throwValue("list edge projector streams", reply);
        }
        if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
            if (reply.kind() == ruvia::RedisValue::Kind::kNull) {
                co_return;
            }
            service::message::redis::throwValue("list edge projector streams", reply);
        }
        for (const auto& value : reply.array()) {
            if (value.kind() != ruvia::RedisValue::Kind::kString) {
                continue;
            }
            const auto streamName = std::string(value.string());
            const std::string registry(projector_stream::kStreamRegistry);
            const std::string_view keys[]{ registry, streamName };
            const std::span<const std::string_view> arguments;
            const auto removed = co_await redis.eval(
                script,
                keys,
                arguments
            );
            if (removed.kind() != ruvia::RedisValue::Kind::kInteger) {
                service::message::redis::throwValue("clean edge projector registry", removed);
            }
        }
    }

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index, std::shared_ptr<std::promise<void>> ready, std::shared_ptr<std::promise<void>> stopped) {
        bool readySet = false;
        bool leaseAcquired = false;
        bool heartbeatStarted = false;
        try {
            const auto redis = context.redis();
            const auto currentStream = projector_stream::stream(index);
            const auto currentLease = projector_stream::leaseKey(index);
            const auto currentToken = projector_stream::ownerToken(index);
            recoveryLeases_.clear();
            lostRecoveryLeases_.clear();
            leaseLost_.store(false);
            failed_.store(false);

            // Startup acquisition is deliberately separate from renewal.  NX
            // is the only operation allowed to establish this worker's lease;
            // all later heartbeats require the exact original token.
            if (!co_await acquireLease(redis, index)) {
                throw std::runtime_error(
                    "edge projector lease is owned by another worker"
                );
            }
            leaseAcquired = true;

            // The heartbeat has its own task on this same Worker.  Database
            // hydration and a large projection batch therefore cannot starve
            // the lease renewal loop.
            if (!leaseScope_) {
                throw std::runtime_error("edge projector lease scope is not ready");
            }
            leaseScope_->spawn(
                maintainLeases(context, redis, index, leaseScope_->stopToken())
            );
            heartbeatStarted = true;

            co_await cleanupRegistry(redis);
            co_await registerStream(redis, currentStream);

            std::vector<std::string> streams{ currentStream };
            std::map<std::string, DeadStream, std::less<>> deadStreams;

            // A dead v3 owner is recovered only after its lease has expired,
            // and the recovery lease is acquired before its consumer group is
            // created or its first entry is read.
            for (auto& dead : co_await discoverDeadStreams(
                     redis,
                     currentStream,
                     index
                 )) {
                if (!co_await service::message::redis::ensureGroupIfPresent(
                        redis,
                        dead.stream,
                        kEdgeIngressGroup
                    )) {
                    (void)co_await releaseLeaseKey(redis, dead.lease, dead.token);
                    continue;
                }
                if (!co_await renewLeaseKey(redis, dead.lease, dead.token)) {
                    continue;
                }
                streams.push_back(dead.stream);
                recoveryLeases_[dead.lease] = dead.token;
                deadStreams.emplace(dead.stream, std::move(dead));
                co_await registerStream(redis, streams.back());
            }

            co_await service::message::redis::ensureGroup(
                redis,
                currentStream,
                kEdgeIngressGroup
            );
            co_await hydrateAuth(context);
            auto catalog = co_await metadata::hydrate(context);
            ready->set_value();
            readySet = true;

            bool recovering = true;
            const auto consumer = service::runtime::instanceId() + ":service-" +
                std::to_string(index);
            auto nextDiscovery = std::chrono::steady_clock::now();
            auto nextRecoverySweep = nextDiscovery;

            while (running_.load() && !context.stopToken().stopRequested() &&
                   !leaseLost_.load()) {
                // A recovery heartbeat loss removes that stream from the read
                // set before another message can be acknowledged for it.
                if (!lostRecoveryLeases_.empty()) {
                    for (auto dead = deadStreams.begin();
                         dead != deadStreams.end();) {
                        if (!lostRecoveryLeases_.contains(dead->second.lease)) {
                            ++dead;
                            continue;
                        }
                        streams.erase(
                            std::remove(streams.begin(), streams.end(), dead->first),
                            streams.end()
                        );
                        recoveryLeases_.erase(dead->second.lease);
                        dead = deadStreams.erase(dead);
                        recovering = true;
                    }
                    lostRecoveryLeases_.clear();
                }

                bool maintenanceFailed = false;
                try {
                    if (std::chrono::steady_clock::now() >= nextDiscovery) {
                        const auto discovered = co_await discoverDeadStreams(
                            redis,
                            currentStream,
                            index
                        );
                        std::set<std::string, std::less<>> discoveredNames;
                        for (const auto& dead : discovered) {
                            discoveredNames.insert(dead.stream);
                        }
                        for (auto dead = deadStreams.begin();
                             dead != deadStreams.end();) {
                            if (discoveredNames.contains(dead->first)) {
                                ++dead;
                                continue;
                            }
                            streams.erase(
                                std::remove(streams.begin(), streams.end(), dead->first),
                                streams.end()
                            );
                            recoveryLeases_.erase(dead->second.lease);
                            dead = deadStreams.erase(dead);
                            recovering = true;
                        }
                        for (auto& dead : discovered) {
                            if (deadStreams.contains(dead.stream)) {
                                continue;
                            }
                            if (!co_await service::message::redis::ensureGroupIfPresent(
                                    redis,
                                    dead.stream,
                                    kEdgeIngressGroup
                                )) {
                                (void)co_await releaseLeaseKey(
                                    redis,
                                    dead.lease,
                                    dead.token
                                );
                                continue;
                            }
                            if (!co_await renewLeaseKey(redis, dead.lease, dead.token)) {
                                continue;
                            }
                            const auto streamName = dead.stream;
                            recoveryLeases_[dead.lease] = dead.token;
                            deadStreams.emplace(streamName, std::move(dead));
                            streams.push_back(streamName);
                            co_await registerStream(redis, streamName);
                            recovering = true;
                            nextRecoverySweep = std::chrono::steady_clock::now();
                        }
                        nextDiscovery = std::chrono::steady_clock::now() +
                            kLeaseRenewInterval;
                    }

                } catch (const std::exception& error) {
                    std::cerr << "edge stream discovery failed: " << error.what() << '\n';
                    maintenanceFailed = true;
                }
                if (!running_.load() || context.stopToken().stopRequested() ||
                    leaseLost_.load()) {
                    break;
                }
                if (maintenanceFailed) {
                    (void)co_await ruvia::sleepFor(
                        context.worker(), std::chrono::milliseconds(250)
                    );
                    continue;
                }

                std::vector<service::message::redis::StreamBatch> batches;
                bool readFailed = false;
                bool groupMissing = false;
                try {
                    if (!deadStreams.empty() &&
                        std::chrono::steady_clock::now() >= nextRecoverySweep) {
                        std::vector<std::string> recoveryStreams;
                        recoveryStreams.reserve(deadStreams.size());
                        for (const auto& [stream, dead] : deadStreams) {
                            (void)dead;
                            recoveryStreams.push_back(stream);
                        }
                        // A recovery lease fences the crashed owner, so its
                        // PEL is safe to sweep immediately.  The ordinary
                        // shared read set keeps its normal idle threshold.
                        batches = co_await service::message::redis::claimGroupMany(
                            redis,
                            recoveryStreams,
                            kEdgeIngressGroup,
                            consumer,
                            kBatchSize,
                            std::chrono::milliseconds(0)
                        );
                        nextRecoverySweep = std::chrono::steady_clock::now() +
                            kRecoverySweepInterval;
                    }
                    if (batches.empty()) {
                        batches = recovering
                            ? co_await service::message::redis::claimGroupMany(
                                  redis,
                                  streams,
                                  kEdgeIngressGroup,
                                  consumer,
                                  kBatchSize
                              )
                            : co_await service::message::redis::readGroupMany(
                                  redis,
                                  streams,
                                  kEdgeIngressGroup,
                                  consumer,
                                  ">",
                                  kBatchSize
                              );
                    }
                } catch (const std::exception& error) {
                    if (!running_.load() || context.stopToken().stopRequested() ||
                        leaseLost_.load()) {
                        break;
                    }
                    std::cerr << "edge stream read failed: " << error.what() << '\n';
                    recovering = true;
                    readFailed = true;
                    groupMissing = std::string_view(error.what()).find("NOGROUP") !=
                        std::string_view::npos;
                }
                if (groupMissing && running_.load() &&
                    !context.stopToken().stopRequested() && !leaseLost_.load()) {
                    try {
                        co_await service::message::redis::ensureGroup(
                            redis,
                            currentStream,
                            kEdgeIngressGroup
                        );
                        for (auto dead = deadStreams.begin();
                             dead != deadStreams.end();) {
                            if (co_await service::message::redis::ensureGroupIfPresent(
                                    redis,
                                    dead->first,
                                    kEdgeIngressGroup
                                )) {
                                ++dead;
                                continue;
                            }
                            streams.erase(
                                std::remove(streams.begin(), streams.end(), dead->first),
                                streams.end()
                            );
                            recoveryLeases_.erase(dead->second.lease);
                            dead = deadStreams.erase(dead);
                            nextRecoverySweep = std::chrono::steady_clock::now();
                        }
                        readFailed = false;
                    } catch (const std::exception& ensureError) {
                        std::cerr << "edge stream group recovery failed: "
                                  << ensureError.what() << '\n';
                    }
                }
                if (!running_.load() || context.stopToken().stopRequested() ||
                    leaseLost_.load()) {
                    break;
                }
                if (readFailed) {
                    (void)co_await ruvia::sleepFor(
                        context.worker(),
                        std::chrono::milliseconds(250)
                    );
                    continue;
                }
                if (recovering && batches.empty()) {
                    recovering = false;
                }
                if (batches.empty()) {
                    try {
                        for (auto dead = deadStreams.begin();
                             dead != deadStreams.end();) {
                            if (co_await eraseDeadStream(redis, dead->second)) {
                                streams.erase(
                                    std::remove(streams.begin(), streams.end(), dead->first),
                                    streams.end()
                                );
                                recoveryLeases_.erase(dead->second.lease);
                                dead = deadStreams.erase(dead);
                            } else {
                                ++dead;
                            }
                        }
                    } catch (const std::exception& error) {
                        std::cerr << "edge stream cleanup failed: " << error.what() << '\n';
                        maintenanceFailed = true;
                    }
                    if (!running_.load() || context.stopToken().stopRequested() ||
                        leaseLost_.load()) {
                        break;
                    }
                    if (maintenanceFailed) {
                        (void)co_await ruvia::sleepFor(
                            context.worker(),
                            std::chrono::milliseconds(250)
                        );
                        continue;
                    }
                    co_await service::message::workerStreamMultiplexer().wait(
                        index,
                        service::message::WorkerStreamTask::EdgeProjector,
                        context.stopToken(),
                        kLeaseRenewInterval
                    );
                    continue;
                }

                bool projectionFailed = false;
                try {
                    for (const auto& batch : batches) {
                        if (leaseLost_.load()) {
                            break;
                        }
                        const DeadStream* owner = nullptr;
                        DeadStream currentOwner{
                            currentStream,
                            currentLease,
                            currentToken
                        };
                        if (batch.stream == currentStream) {
                            owner = &currentOwner;
                        } else if (const auto dead = deadStreams.find(batch.stream);
                                   dead != deadStreams.end()) {
                            owner = &dead->second;
                        }
                        if (!owner) {
                            throw std::runtime_error(
                                "edge projector batch is outside the owned stream set"
                            );
                        }
                        if (!co_await leaseOwned(redis, owner->lease, owner->token)) {
                            if (batch.stream == currentStream) {
                                markLeaseLost(context);
                            } else {
                                lostRecoveryLeases_.insert(owner->lease);
                                recovering = true;
                            }
                            break;
                        }

                        std::vector<service::message::StreamMessage> telemetry;
                        telemetry.reserve(batch.messages.size());
                        for (const auto& message : batch.messages) {
                            if (leaseLost_.load()) {
                                break;
                            }
                            const bool metadataEvent =
                                message.get("kind") == projector_stream::kMetadataKind;
                            if (metadataEvent) {
                                const auto nodeId = message.get("node_id");
                                if (!nodeId.empty()) {
                                    auto snapshot = co_await metadata::loadNodeFromDatabase(
                                        context,
                                        nodeId
                                    );
                                    co_await metadata::storeNode(
                                        redis,
                                        nodeId,
                                        snapshot,
                                        false
                                    );
                                    catalog[std::string(nodeId)] = std::move(snapshot);
                                }
                                continue;
                            }
                            const bool ingressEvent =
                                message.get("kind") == projector_stream::kIngressKind;
                            if (ingressEvent) {
                                co_await project(
                                    context,
                                    catalog,
                                    message.get("wire"),
                                    message.get("received_at_ms"),
                                    telemetry
                                );
                            }
                        }
                        if (leaseLost_.load()) {
                            break;
                        }
                        co_await service::telemetry::TelemetryService::ingest(
                            context,
                            telemetry
                        );
                        service::message::workerStreamMultiplexer().signalTelemetryConsumers();
                        if (leaseLost_.load()) {
                            break;
                        }
                        if (!co_await acknowledgeAndDeleteFenced(
                                redis,
                                owner->lease,
                                owner->token,
                                batch.stream,
                                kEdgeIngressGroup,
                                batch.messages
                            )) {
                            if (batch.stream == currentStream) {
                                markLeaseLost(context);
                            } else {
                                lostRecoveryLeases_.insert(owner->lease);
                                recovering = true;
                            }
                            break;
                        }
                    }
                } catch (const std::exception& error) {
                    recovering = true;
                    projectionFailed = true;
                    std::cerr << "edge projection failed: " << error.what() << '\n';
                }
                if (leaseLost_.load()) {
                    break;
                }
                if (projectionFailed) {
                    (void)co_await ruvia::sleepFor(
                        context.worker(),
                        std::chrono::milliseconds(250)
                    );
                }
            }
        } catch (...) {
            if (!readySet) {
                try {
                    ready->set_exception(std::current_exception());
                } catch (...) {
                }
            }
        }

        if (heartbeatStarted && leaseScope_) {
            leaseScope_->requestStop();
            try {
                co_await leaseScope_->join();
            } catch (const std::exception& error) {
                std::cerr << "edge projector heartbeat stopped with error: "
                          << error.what() << '\n';
            } catch (...) {
                std::cerr << "edge projector heartbeat stopped with unknown error\n";
            }
        }
        if (leaseAcquired) {
            try {
                const auto redis = context.redis();
                for (const auto& [key, token] : recoveryLeaseSnapshot()) {
                    (void)co_await releaseLeaseKey(redis, key, token);
                }
                (void)co_await releaseLease(redis, index);
            } catch (const std::exception& error) {
                std::cerr << "edge projector lease release failed: " << error.what()
                          << '\n';
            } catch (...) {
                std::cerr << "edge projector lease release failed: unknown error\n";
            }
        }
        running_.store(false);
        try {
            stopped->set_value();
        } catch (...) {
        }
    }

    ruvia::WebWorkerHandle worker_;
    std::shared_future<void> stopped_;
    std::unique_ptr<ruvia::TaskScope> leaseScope_;
    std::size_t workerIndex_ = 0;
    std::size_t serviceWorkerCount_ = 0;
    std::atomic_bool running_{ false };
    std::atomic_bool leaseLost_{ false };
    std::atomic_bool failed_{ false };
    std::map<std::string, std::string, std::less<>> recoveryLeases_;
    std::set<std::string, std::less<>> lostRecoveryLeases_;
};

} // namespace service::edge
