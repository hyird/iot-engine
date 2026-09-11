#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/TaskScope.h>

#include "service/common/log.h"
#include "service/common/message.h"
#include "service/features/collector/collector.service.h"
#include "service/features/collector/engine/engine.runtime.h"
#include "service/features/collector/modbus/modbus.protocol.h"
#include "service/features/collector/redis/redis.transport.h"
#include "service/features/collector/s7/s7.protocol.h"
#include "service/features/collector/sl651/sl651.protocol.h"
#include "service/features/collector/tcp/tcp.transport.h"
#include "service/features/messaging/messaging.transport.h"
#include "service/features/gb28181/gb28181.runtime.h"

namespace service::collector {

class CollectorWorker final {
  public:
    CollectorWorker(ruvia::EventLoop loop, ruvia::RedisConfig redisConfig, std::size_t workerIndex, std::size_t workerCount, AppConfig gb28181)
        : loop_(std::move(loop)), workerHandle_(loop_.handle()), resource_(),
          scope_(workerHandle_, ruvia::TaskScopeOptions{ .resource = &resource_ }),
          scheduler_(loop_.ioContext()),
          redis_(loop_.ioContext(), std::move(redisConfig), workerHandle_),
          engine_(protocols()),
          tcp_(
              loop_.ioContext(),
              scheduler_,
              workerIndex,
              workerCount,
              [this](ProtocolConnectionInfo info) {
                  enqueueConnected(std::move(info));
              },
              [this](message::IngressPacket packet) {
                  enqueueIngress(std::move(packet));
              },
              [this](std::string connectionId, std::string reason) {
                  enqueueDisconnect(std::move(connectionId), std::move(reason));
              },
              [this](LinkState state) {
                  if (!stopping_) {
                      scope_.spawn(publishLinkEvent(std::move(state)));
                  }
              },
              [this](std::string linkId, std::string targetId, std::string connectionId, std::function<void(bool)> completed) {
                  if (stopping_) {
                      completed(false);
                      return;
                  }
                  scope_.spawn(claimTarget(std::move(linkId), std::move(targetId), std::move(connectionId), std::move(completed)));
              },
              [this](std::string linkId, std::string targetId, std::string connectionId) {
                  releaseTargetLease(std::move(linkId), std::move(targetId), std::move(connectionId));
              }
          ),
          gb28181_(std::move(gb28181), loop_, redis_.withOptions({}), workerIndex, workerCount),
          workerIndex_(workerIndex), workerCount_(workerCount),
          consumer_("collector-" + std::to_string(workerIndex)) {}

    CollectorWorker(const CollectorWorker&) = delete;
    CollectorWorker& operator=(const CollectorWorker&) = delete;

    void start(std::shared_ptr<std::promise<void>> ready) {
        if (!loop_.post([this, ready = std::move(ready)] {
                      scope_.spawn(initialize(ready));
                  })
                 .accepted()) {
            throw std::runtime_error("collector worker rejected startup");
        }
    }

    [[nodiscard]] ruvia::Task<void> shutdown() {
        if (shutdownStarted_) {
            co_return;
        }
        shutdownStarted_ = true;
        stopping_ = true;
        if (tickToken_ != 0) {
            scheduler_.cancel(tickToken_);
        }
        if (targetRenewalToken_ != 0) {
            scheduler_.cancel(targetRenewalToken_);
        }
        for (const auto& [connectionId, deadline] : targetLeaseDeadlines_) {
            scheduler_.cancel(deadline.token);
        }
        targetLeaseDeadlines_.clear();
        tcp_.stop();
        try {
            co_await gb28181_.shutdown();
        } catch (...) {
        }
        scope_.requestStop();
        try {
            co_await scope_.join();
        } catch (...) {
        }
        for (const auto& [connectionId, deviceCodes] : routes_) {
            for (const auto& deviceCode : deviceCodes) {
                try {
                    co_await markDeviceOffline(deviceCode, connectionId, "local_closed");
                } catch (...) {
                }
            }
        }
        routes_.clear();
        const auto leases = targetLeases_;
        targetLeases_.clear();
        for (const auto& [target, connectionId] : leases) {
            try {
                co_await ownership::releaseTarget(redis_, target.first, target.second, targetLeaseOwner(connectionId));
            } catch (...) {
            }
        }
        try {
            co_await message::redis::eraseMatching(redis_, "iot:runtime:link:*:worker:" + service::runtime::instanceId() + ":" + std::to_string(workerIndex_));
            co_await message::redis::eraseHash(redis_, "iot:runtime:collector:" + service::runtime::instanceId() + ":" + std::to_string(workerIndex_));
        } catch (...) {
        }
        redis_.close();
        scheduler_.stop();
    }

    [[nodiscard]] std::size_t index() const noexcept { return workerIndex_; }

  private:
    struct TargetLeaseDeadline {
        DeadlineScheduler::Token token;
        std::chrono::steady_clock::time_point expiresAt;
    };

    struct PendingCommand {
        std::string stream;
        std::string entryId;
        message::ProtocolTask task;
    };

    struct BroadcastCommand {
        std::size_t remaining = 0;
        bool anySuccess = false;
        std::string reason;
    };

    struct EgressLogContext {
        std::string operation;
        std::string protocol;
        std::string linkId;
        std::string deviceId;
        std::string deviceCode;
        std::string connectionId;
        std::string remoteAddress;
        std::string messageId;
        std::string causationId;
        std::uint64_t sessionEpoch = 0;
    };

    struct IngressWork {
        std::optional<message::IngressPacket> packet;
        std::optional<message::ConnectionEvent> connectionEvent;
        std::string connectionId;
    };

    static ProtocolRuntimeRegistry protocols() {
        ProtocolRuntimeRegistry result;
        result.add(std::make_unique<modbus::Runtime>());
        result.add(std::make_unique<s7::Runtime>());
        result.add(std::make_unique<sl651::Runtime>());
        return result;
    }

    [[nodiscard]] std::string collectorGroup() const { return "iot-engine:collector"; }

    [[nodiscard]] std::string configGroup() const { return collectorGroup(); }

    [[nodiscard]] std::string ingressGroup() const { return collectorGroup(); }

    [[nodiscard]] std::string egressGroup() const { return collectorGroup(); }

    [[nodiscard]] std::string linkEventGroup() const { return collectorGroup(); }

    [[nodiscard]] std::string configStream() const { return message::configStream(workerIndex_); }

    [[nodiscard]] std::string commandStream(bool high) const {
        return message::commandStream(workerIndex_, high);
    }

    [[nodiscard]] std::string commandGroup() const { return collectorGroup(); }

    [[nodiscard]] std::string controlStream() const { return message::controlStream(workerIndex_); }

    [[nodiscard]] std::string ingressStream() const { return message::ingressStream(workerIndex_); }

    [[nodiscard]] std::string parsedStream() const { return message::parsedStream(); }

    [[nodiscard]] std::string egressStream() const { return message::egressStream(workerIndex_); }

    [[nodiscard]] std::string linkEventStream() const {
        return message::linkEventStream(workerIndex_);
    }

    [[nodiscard]] std::string commandResultStream() const { return message::commandResultStream(); }

    [[nodiscard]] std::string deadLetterStream() const {
        return message::deadLetterStream(workerIndex_);
    }

    [[nodiscard]] std::string_view protocolForLink(std::string_view linkId) const noexcept {
        const auto current = std::find_if(
            loadedSnapshot_.links.begin(),
            loadedSnapshot_.links.end(),
            [linkId](const auto& link) {
                return link.id == linkId;
            }
        );
        return current == loadedSnapshot_.links.end() ? std::string_view{} : current->protocol;
    }

    [[nodiscard]] std::string deviceCodesForConnection(std::string_view connectionId) const {
        const auto current = routes_.find(connectionId);
        if (current == routes_.end()) {
            return {};
        }
        std::string result;
        for (const auto& deviceCode : current->second) {
            if (!result.empty()) {
                result.push_back(',');
            }
            result += deviceCode;
        }
        return result;
    }

    [[nodiscard]] const message::ProtocolTask*
    taskForCausation(std::string_view causationId) const noexcept {
        auto current = pendingCommands_.find(causationId);
        if (current != pendingCommands_.end()) {
            return &current->second.task;
        }
        const auto child = broadcastParents_.find(causationId);
        if (child == broadcastParents_.end()) {
            return nullptr;
        }
        current = pendingCommands_.find(child->second);
        return current == pendingCommands_.end() ? nullptr : &current->second.task;
    }

    [[nodiscard]] service::common::packet_log::Context
    ingressLogContext(const message::IngressPacket& packet, std::string_view deviceCodes = {}) const noexcept {
        service::common::packet_log::Context context;
        context.workerIndex = workerIndex_;
        context.direction = "RX";
        context.operation = "transport";
        context.protocol = protocolForLink(packet.linkId);
        context.linkId = packet.linkId;
        context.deviceCode = deviceCodes;
        context.connectionId = packet.connectionId;
        context.remoteAddress = packet.remoteAddress;
        context.messageId = packet.messageId;
        context.sessionEpoch = packet.sessionEpoch;
        return context;
    }

    [[nodiscard]] service::common::packet_log::Context
    connectionLogContext(const message::ConnectionEvent& event) const noexcept {
        service::common::packet_log::Context context;
        context.workerIndex = workerIndex_;
        context.operation = "connection";
        context.protocol = protocolForLink(event.linkId);
        context.linkId = event.linkId;
        context.connectionId = event.connectionId;
        context.remoteAddress = event.remoteAddress;
        context.messageId = event.messageId;
        context.sessionEpoch = event.sessionEpoch;
        return context;
    }

    [[nodiscard]] service::common::packet_log::Context
    taskLogContext(const message::ProtocolTask& task, std::string_view connectionId = {}) const noexcept {
        service::common::packet_log::Context context;
        context.workerIndex = workerIndex_;
        context.operation = task.kind;
        context.protocol = task.protocol;
        context.linkId = task.linkId;
        context.deviceId = task.deviceId;
        context.deviceCode = task.deviceCode;
        context.connectionId = connectionId.empty() ? task.connectionId : connectionId;
        context.messageId = task.messageId;
        context.causationId = task.causationId;
        context.sessionEpoch = task.sessionEpoch;
        const auto network = networkConnections_.find(context.connectionId);
        if (network != networkConnections_.end()) {
            context.remoteAddress = network->second.remoteAddress;
        }
        return context;
    }

    [[nodiscard]] service::common::packet_log::Context
    actionLogContext(const ProtocolAction& action, std::string_view operation = {}) const noexcept {
        service::common::packet_log::Context context;
        context.workerIndex = workerIndex_;
        context.operation = operation;
        context.deviceId = action.deviceId;
        context.deviceCode = action.deviceCode;
        context.connectionId = action.connectionId;
        context.causationId = action.commandId;
        const auto network = networkConnections_.find(action.connectionId);
        if (network != networkConnections_.end()) {
            context.linkId = network->second.linkId;
            context.remoteAddress = network->second.remoteAddress;
            context.sessionEpoch = network->second.sessionEpoch;
            context.protocol = protocolForLink(network->second.linkId);
        }
        return context;
    }

    [[nodiscard]] service::common::packet_log::Context
    parsedLogContext(const message::ParsedDeviceMessage& parsed) const noexcept {
        service::common::packet_log::Context context;
        context.workerIndex = workerIndex_;
        context.direction = "RX";
        context.operation = "parse";
        context.protocol = parsed.protocol;
        context.linkId = parsed.linkId;
        context.deviceId = parsed.deviceId;
        context.deviceCode = parsed.deviceCode;
        context.connectionId = parsed.connectionId;
        context.messageId = parsed.messageId;
        context.causationId = parsed.causationId;
        const auto network = networkConnections_.find(parsed.connectionId);
        if (network != networkConnections_.end()) {
            context.remoteAddress = network->second.remoteAddress;
            context.sessionEpoch = network->second.sessionEpoch;
        }
        return context;
    }

    [[nodiscard]] service::common::packet_log::Context
    egressLogContext(const EgressLogContext& egress) const noexcept {
        service::common::packet_log::Context context;
        context.workerIndex = workerIndex_;
        context.direction = "TX";
        context.operation = egress.operation;
        context.protocol = egress.protocol;
        context.linkId = egress.linkId;
        context.deviceId = egress.deviceId;
        context.deviceCode = egress.deviceCode;
        context.connectionId = egress.connectionId;
        context.remoteAddress = egress.remoteAddress;
        context.messageId = egress.messageId;
        context.causationId = egress.causationId;
        context.sessionEpoch = egress.sessionEpoch;
        return context;
    }

    [[nodiscard]] static bool hasMarker(std::string_view value, std::string_view marker) noexcept {
        return value.find(marker) != std::string_view::npos;
    }

    [[nodiscard]] std::string targetLeaseOwner(std::string_view connectionId) const {
        return service::runtime::instanceId() + ":worker:" + std::to_string(workerIndex_) + ":" + std::string(connectionId);
    }

    ruvia::Task<void> claimTarget(std::string linkId, std::string targetId, std::string connectionId, std::function<void(bool)> completed) {
        bool claimed = false;
        const auto began = std::chrono::steady_clock::now();
        try {
            claimed = co_await ownership::retainTarget(redis_.withOptions({ .timeout = std::chrono::seconds(3) }), linkId, targetId, targetLeaseOwner(connectionId));
            if (claimed && !stopping_ && std::chrono::steady_clock::now() - began < kTargetLeaseValidity &&
                !targetLeases_.contains({ linkId, targetId })) {
                targetLeases_.insert_or_assign({ linkId, targetId }, connectionId);
                armTargetLeaseDeadline({ linkId, targetId }, connectionId, began);
            } else if (claimed) {
                claimed = false;
                co_await ownership::releaseTarget(redis_, linkId, targetId, targetLeaseOwner(connectionId));
            }
        } catch (const std::exception& error) {
            lastCoordinatorError_ = std::string("target_claim_failed: ") + error.what();
        } catch (...) {
            lastCoordinatorError_ = "target_claim_failed: unknown error";
        }
        if (stopping_ && claimed) {
            const auto current = targetLeases_.find({ linkId, targetId });
            if (current != targetLeases_.end() && current->second == connectionId) {
                targetLeases_.erase(current);
            }
            try {
                co_await ownership::releaseTarget(redis_, linkId, targetId, targetLeaseOwner(connectionId));
            } catch (...) {
            }
            claimed = false;
        }
        if (completed) {
            completed(claimed && !stopping_);
        }
    }

    void releaseTargetLease(std::string linkId, std::string targetId, std::string connectionId) {
        const auto current = targetLeases_.find({ linkId, targetId });
        if (current == targetLeases_.end() || current->second != connectionId || stopping_) {
            return;
        }
        targetLeases_.erase(current);
        if (const auto deadline = targetLeaseDeadlines_.find(connectionId); deadline != targetLeaseDeadlines_.end()) {
            scheduler_.cancel(deadline->second.token);
            targetLeaseDeadlines_.erase(deadline);
        }
        scope_.spawn(releaseTargetLeaseAsync(std::move(linkId), std::move(targetId), std::move(connectionId)));
    }

    ruvia::Task<void> releaseTargetLeaseAsync(std::string linkId, std::string targetId, std::string connectionId) {
        try {
            co_await ownership::releaseTarget(redis_, linkId, targetId, targetLeaseOwner(connectionId));
        } catch (const std::exception& error) {
            lastCoordinatorError_ = std::string("target_release_failed: ") + error.what();
        } catch (...) {
            lastCoordinatorError_ = "target_release_failed: unknown error";
        }
    }

    ruvia::Task<void> renewTargetLease(ClientTargetKey target, std::string connectionId) {
        bool retained = false;
        const auto began = std::chrono::steady_clock::now();
        try {
            retained = co_await ownership::renewTarget(redis_.withOptions({ .timeout = std::chrono::seconds(3) }), target.first, target.second, targetLeaseOwner(connectionId));
        } catch (const std::exception& error) {
            lastCoordinatorError_ = std::string("target_renew_failed: ") + error.what();
        } catch (...) {
            lastCoordinatorError_ = "target_renew_failed: unknown error";
        }
        if (stopping_) {
            co_return;
        }
        const auto current = targetLeases_.find(target);
        if (current != targetLeases_.end() && current->second == connectionId) {
            const auto deadline = targetLeaseDeadlines_.find(connectionId);
            const auto now = std::chrono::steady_clock::now();
            if (retained && now - began < kTargetLeaseValidity &&
                deadline != targetLeaseDeadlines_.end() && now < deadline->second.expiresAt) {
                armTargetLeaseDeadline(target, connectionId, began);
                co_return;
            }
            targetLeases_.erase(current);
            tcp_.revokeTarget(connectionId, "target_ownership_lost");
        }
    }

    void scheduleTargetRenewal() {
        if (stopping_) {
            return;
        }
        targetRenewalToken_ = scheduler_.scheduleAfter(std::chrono::seconds(5), [this] {
            targetRenewalToken_ = 0;
            if (!stopping_) {
                for (const auto& [target, connectionId] : targetLeases_) {
                    scope_.spawn(renewTargetLease(target, connectionId));
                }
                scheduleTargetRenewal();
            }
        });
    }

    void armTargetLeaseDeadline(const ClientTargetKey& target, const std::string& connectionId, std::chrono::steady_clock::time_point began) {
        if (const auto old = targetLeaseDeadlines_.find(connectionId); old != targetLeaseDeadlines_.end()) {
            scheduler_.cancel(old->second.token);
        }
        const auto delay = std::max(std::chrono::milliseconds(1), std::chrono::duration_cast<std::chrono::milliseconds>(began + kTargetLeaseValidity - std::chrono::steady_clock::now()));
        const auto token = scheduler_.scheduleAfter(delay, [this, target, connectionId] {
            targetLeaseDeadlines_.erase(connectionId);
            const auto current = targetLeases_.find(target);
            if (current != targetLeases_.end() && current->second == connectionId) {
                targetLeases_.erase(current);
                tcp_.revokeTarget(connectionId, "target_ownership_expired");
            }
        });
        targetLeaseDeadlines_.insert_or_assign(connectionId, TargetLeaseDeadline{ token, began + kTargetLeaseValidity });
    }

    ruvia::Task<void> initialize(std::shared_ptr<std::promise<void>> ready) {
        try {
            co_await redis_.connect();
            co_await gb28181_.initialize();
            co_await message::redis::ensureGroup(redis_, configStream(), configGroup());
            co_await message::redis::ensureGroup(redis_, ingressStream(), ingressGroup());
            co_await message::redis::ensureGroup(redis_, egressStream(), egressGroup());
            co_await message::redis::ensureGroup(redis_, linkEventStream(), linkEventGroup());
            co_await message::redis::ensureGroup(redis_, commandStream(true), commandGroup());
            co_await message::redis::ensureGroup(redis_, commandStream(false), commandGroup());
            co_await message::redis::ensureGroup(redis_, controlStream(), collectorGroup());
            loadedConfigVersion_ = co_await config::activeVersion(redis_);
            desiredConfigVersion_ = loadedConfigVersion_;
            desiredSnapshot_ = co_await config::load(redis_, desiredConfigVersion_);
            auto snapshot = co_await ownership::retain(redis_, desiredSnapshot_);
            engine_.reload(snapshot);
            tcp_.reload(snapshot);
            loadedSnapshot_ = std::move(snapshot);
            co_await publishRuntimeState();
            ready->set_value();
            scheduleTargetRenewal();
            nextOwnershipRefresh_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            nextConfigRecovery_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            scheduleTick(std::chrono::milliseconds(0));
        } catch (...) {
            try {
                ready->set_exception(std::current_exception());
            } catch (...) {
            }
            stopping_ = true;
        }
    }

    void scheduleTick(std::chrono::milliseconds delay = std::chrono::milliseconds(0)) {
        if (stopping_) {
            return;
        }
        tickToken_ = scheduler_.scheduleAfter(delay, [this] {
            tickToken_ = 0;
            if (!stopping_) {
                scope_.spawn(tick());
            }
        });
    }

    void recoverClaimedMessages(std::string error) {
        lastCoordinatorError_ = std::move(error);
        // A successful XREADGROUP can assign messages to this consumer before a broken
        // connection prevents the reply from reaching us. Every local stream must therefore
        // drain its own PEL after either a blocking or non-blocking read fails.
        configRecovering_ = true;
        highRecovering_ = true;
        normalRecovering_ = true;
        controlRecovering_ = true;
        ingressRecovering_ = true;
        egressRecovering_ = true;
        linkEventRecovering_ = true;
        scheduleTick(kFailureDelay);
    }

    ruvia::Task<void> tick() {
        try {
            bool handled = co_await consumeConfig();
            handled = co_await consumeControl() || handled;
            handled = co_await consumeLinkEvent() || handled;
            handled = co_await consumeIngress() || handled;
            handled = co_await consumeCommand(true) || handled;
            // Raw ingress is always given another chance before normal work so registration and
            // command responses cannot be starved by polling.
            handled = co_await consumeIngress() || handled;
            handled = co_await consumeCommand(true) || handled;
            handled = co_await consumeCommand(false) || handled;
            handled = co_await consumeEgress() || handled;
            if (handled) {
                scheduleTick();
            } else {
                scope_.spawn(waitForWork());
            }
        } catch (const std::exception& error) {
            // XREADGROUP assigns a whole batch to this worker before the individual messages
            // are processed. If one item fails, the remaining items are already in the PEL and
            // a subsequent `>` read cannot see them. Re-enter recovery for every local stream;
            // each stream is partitioned by worker, so this only replays messages already owned
            // by this worker and does not introduce cross-worker claiming or polling.
            recoverClaimedMessages(error.what());
        } catch (...) {
            recoverClaimedMessages("unknown_collector_exception");
        }
    }

    ruvia::Task<bool> consumeConfig() {
        const auto stream = configStream();
        const auto group = configGroup();
        const auto messages = co_await readAvailable(stream, group, configRecovering_, 16);
        bool changed = false;
        const auto now = std::chrono::steady_clock::now();
        if (!messages.empty() || now >= nextOwnershipRefresh_) {
            if (now >= nextOwnershipRefresh_) {
                nextOwnershipRefresh_ = now + std::chrono::seconds(5);
            }
            // Normal changes arrive through the shared invalidation stream.
            // Its retention is bounded, so a slow connected reader can miss a
            // trimmed hint without a disconnect. Check the version once per
            // recovery window, not on every five-second lease renewal.
            const bool recoverConfig = now >= nextConfigRecovery_;
            const auto version = messages.empty() && !recoverConfig
                ? desiredConfigVersion_
                : co_await config::activeVersion(redis_);
            if (version != desiredConfigVersion_) {
                auto desired = co_await config::load(redis_, version);
                desiredSnapshot_ = std::move(desired);
                desiredConfigVersion_ = version;
            }
            if (!messages.empty() || recoverConfig) {
                nextConfigRecovery_ = now + std::chrono::seconds(60);
            }
            auto snapshot = co_await ownership::retain(redis_, desiredSnapshot_);
            if (version != loadedConfigVersion_ || config::signature(snapshot) != config::signature(loadedSnapshot_)) {
                const auto plan = planRuntimeReconcile(loadedSnapshot_, snapshot);
                tcp_.reconcile(snapshot, plan);
                engine_.reload(snapshot, plan.affectedLinks);
                auto sessionRefreshes =
                    engine_.refreshClientSessions(plan.refreshClientSessions);
                std::erase_if(sessionRefreshes, [this](const auto& refresh) {
                    const auto sessionEpoch = tcp_.advanceSessionEpoch(refresh.connectionId);
                    if (sessionEpoch == 0) {
                        return true;
                    }
                    connectionEpochs_[refresh.connectionId] = sessionEpoch;
                    if (const auto network = networkConnections_.find(refresh.connectionId);
                        network != networkConnections_.end()) {
                        network->second.sessionEpoch = sessionEpoch;
                    }
                    cancelProtocolDeadlinesForConnection(refresh.connectionId);
                    return false;
                });
                for (auto& refresh : sessionRefreshes) {
                    co_await applySessionRefresh(std::move(refresh));
                }
                for (const auto& link : loadedSnapshot_.links) {
                    if (std::none_of(
                            snapshot.links.begin(),
                            snapshot.links.end(),
                            [&link](const auto& current) {
                                return current.id == link.id;
                            }
                        )) {
                        co_await message::redis::eraseHash(
                            redis_,
                            "iot:runtime:link:" + link.id +
                                ":worker:" + service::runtime::instanceId() + ":" + std::to_string(workerIndex_)
                        );
                    }
                }
                loadedSnapshot_ = std::move(snapshot);
                loadedConfigVersion_ = desiredConfigVersion_;
                co_await publishRuntimeState();
                changed = true;
            }
        }
        for (const auto& message : messages) {
            co_await message::redis::acknowledgeAndDelete(redis_, stream, group, message.id);
        }
        co_return changed || !messages.empty();
    }

    ruvia::Task<bool> consumeCommand(bool high) {
        const auto stream = commandStream(high);
        auto& recovering = high ? highRecovering_ : normalRecovering_;
        const auto messages =
            co_await readAvailable(stream, commandGroup(), recovering, kCommandConsumeBatch);
        if (messages.empty()) {
            co_return false;
        }
        for (const auto& message : messages) {
            message::ProtocolTask task;
            std::string parseError;
            try {
                task = message::protocolTaskFrom(message);
            } catch (const std::exception& error) {
                parseError = error.what();
            }
            if (!parseError.empty()) {
                co_await deadLetterAndAcknowledge(message, "protocol_task_invalid", parseError, stream, commandGroup());
                continue;
            }
            const auto makeCommand = [&](std::string id) {
                return ProtocolCommand{
                    .id = std::move(id),
                    .deviceId = task.deviceId,
                    .deviceCode = task.deviceCode,
                    .transport = task.transport,
                    .kind = task.kind,
                    .protocol = task.protocol,
                    .payload = message::fromHex(task.payload),
                    .readbackPayload = message::fromHex(task.readbackPayload),
                    .expectedReadbackData = message::fromHex(task.expectedReadbackData),
                    .expectedValue = task.expectedValue,
                    .elements =
                        [&task] {
                            std::vector<CommandElementValue> elements;
                            elements.reserve(task.elements.size());
                            for (const auto& [elementId, value] : task.elements) {
                                elements.push_back({ elementId, value });
                            }
                            return elements;
                        }(),
                    .highPriority = high,
                    .expectsResponse = task.expectsResponse,
                    .timeout = std::chrono::milliseconds(
                        std::clamp<std::int64_t>(task.responseTimeoutMs, 100, 60000)
                    )
                };
            };
            if (task.kind == "discovery") {
                const auto payload = message::fromHex(task.payload);
                service::common::packet_log::write(
                    service::common::packet_log::Level::Info,
                    "DISCOVERY_REQUEST",
                    taskLogContext(task),
                    payload
                );
            }
            if (task.kind == "discovery" && task.connectionId.empty()) {
                const auto connections = tcp_.connectionIds(task.linkId);
                if (connections.empty()) {
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Warn,
                        "DISCOVERY_FAILED",
                        taskLogContext(task),
                        {},
                        "discovery_no_connections"
                    );
                    co_await failUndeliverable(stream, message.id, task.messageId, task, "discovery_no_connections");
                    continue;
                }
                pendingCommands_.insert_or_assign(task.messageId, PendingCommand{ stream, message.id, task });
                broadcasts_[task.messageId] = BroadcastCommand{ .remaining = connections.size() };
                const auto targetCount = "targets=" + std::to_string(connections.size());
                service::common::packet_log::write(
                    service::common::packet_log::Level::Info,
                    "BROADCAST_START",
                    taskLogContext(task),
                    {},
                    targetCount
                );
                for (const auto& connectionId : connections) {
                    const auto childId = message::nextMessageId();
                    broadcastParents_[childId] = task.messageId;
                    auto context = taskLogContext(task, connectionId);
                    context.causationId = task.messageId;
                    context.messageId = childId;
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Debug,
                        "BROADCAST_TARGET",
                        context
                    );
                    co_await applyActions(connectionId, engine_.execute(connectionId, makeCommand(childId)));
                }
                continue;
            }
            if (task.connectionId.empty()) {
                co_await failUndeliverable(stream, message.id, task.messageId, task, "connection_route_missing");
                continue;
            }
            const auto epoch = connectionEpochs_.find(task.connectionId);
            if (task.sessionEpoch != 0 &&
                (epoch == connectionEpochs_.end() || epoch->second != task.sessionEpoch)) {
                co_await failUndeliverable(stream, message.id, task.messageId, task, "stale_session_epoch");
                continue;
            }
            if (task.kind == "command") {
                if (message::utcNowMilliseconds() - task.createdAtMs >= 60000) {
                    co_await failUndeliverable(stream, message.id, task.messageId, task, "dispatch_deadline_expired");
                    continue;
                }
                const auto receipt = "iot:v2:command:sent:" + task.messageId;
                const auto claimed = co_await message::redis::command(redis_, { "SET", receipt, "1", "NX", "EX", "86400" });
                if (claimed.kind() == ruvia::RedisValue::Kind::kNull) {
                    co_await message::redis::acknowledgeAndDelete(redis_, stream, commandGroup(), message.id);
                    continue;
                }
                if (claimed.kind() == ruvia::RedisValue::Kind::kError) {
                    message::redis::throwValue("claim command transmission", claimed);
                }
            }
            pendingCommands_.insert_or_assign(task.messageId, PendingCommand{ stream, message.id, task });
            auto command = makeCommand(task.messageId);
            if (task.kind == "discovery") {
                service::common::packet_log::write(
                    service::common::packet_log::Level::Info,
                    "DISCOVERY_START",
                    taskLogContext(task, task.connectionId)
                );
            }
            co_await applyActions(task.connectionId, engine_.execute(task.connectionId, std::move(command)));
        }
        co_return true;
    }

    ruvia::Task<bool> consumeControl() {
        const auto stream = controlStream();
        const auto messages =
            co_await readAvailable(stream, collectorGroup(), controlRecovering_, 32);
        for (const auto& message : messages) {
            const auto connectionId = message.get("connection_id");
            if (!connectionId.empty()) {
                tcp_.close(connectionId, message.get("reason").empty() ? "device_re_registered" : std::string(message.get("reason")));
            }
            co_await message::redis::acknowledgeAndDelete(redis_, stream, collectorGroup(), message.id);
        }
        co_return !messages.empty();
    }

    void enqueueIngress(message::IngressPacket packet) {
        if (stopping_) {
            return;
        }
        packet.workerInstanceId = workerInstanceId_;
        const auto deviceCodes = deviceCodesForConnection(packet.connectionId);
        const auto logContext = ingressLogContext(packet, deviceCodes);
        service::common::packet_log::write(service::common::packet_log::Level::Debug, "RX_BYTES", logContext, packet.payload);
        if (ingressWork_.size() >= kRawIngressCapacity) {
            service::common::packet_log::write(service::common::packet_log::Level::Error, "RX_DROPPED", logContext, packet.payload, "raw_ingress_backpressure");
            tcp_.close(packet.connectionId, "raw_ingress_backpressure");
            return;
        }
        const auto connectionId = packet.connectionId;
        ingressWork_.push_back({ .packet = std::move(packet), .connectionEvent = std::nullopt, .connectionId = connectionId });
        startIngressDrain();
    }

    void enqueueConnected(ProtocolConnectionInfo info) {
        if (stopping_) {
            return;
        }
        const auto connectionId = info.connectionId;
        networkConnections_.insert_or_assign(connectionId, info);
        message::ConnectionEvent event{ .messageId = message::nextMessageId(),
                                        .workerInstanceId = workerInstanceId_,
                                        .eventType = "connected",
                                        .linkId = info.linkId,
                                        .connectionId = info.connectionId,
                                        .remoteAddress = info.remoteAddress,
                                        .targetId = info.targetId,
                                        .reason = {},
                                        .sessionEpoch = info.sessionEpoch,
                                        .occurredAtMs = message::utcNowMilliseconds() };
        service::common::packet_log::write(service::common::packet_log::Level::Info, "CONNECTED", connectionLogContext(event));
        ingressWork_.push_back({ .packet = std::nullopt, .connectionEvent = std::move(event), .connectionId = connectionId });
        startIngressDrain();
    }

    void enqueueDisconnect(std::string connectionId, std::string reason) {
        if (stopping_) {
            return;
        }
        const auto current = networkConnections_.find(connectionId);
        if (current == networkConnections_.end()) {
            return;
        }
        const auto info = current->second;
        networkConnections_.erase(current);
        if (!info.targetId.empty()) {
            releaseTargetLease(info.linkId, info.targetId, connectionId);
        }
        message::ConnectionEvent event{ .messageId = message::nextMessageId(),
                                        .workerInstanceId = workerInstanceId_,
                                        .eventType = "disconnected",
                                        .linkId = info.linkId,
                                        .connectionId = info.connectionId,
                                        .remoteAddress = info.remoteAddress,
                                        .targetId = info.targetId,
                                        .reason = std::move(reason),
                                        .sessionEpoch = info.sessionEpoch,
                                        .occurredAtMs = message::utcNowMilliseconds() };
        service::common::packet_log::write(service::common::packet_log::Level::Warn, "DISCONNECTED", connectionLogContext(event), {}, event.reason);
        ingressWork_.push_back({ .packet = std::nullopt, .connectionEvent = std::move(event), .connectionId = std::move(connectionId) });
        startIngressDrain();
    }

    void startIngressDrain() {
        if (ingressDraining_ || stopping_) {
            return;
        }
        ingressDraining_ = true;
        scope_.spawn(drainIngress());
    }

    ruvia::Task<void> drainIngress() {
        while (!stopping_ && !ingressWork_.empty()) {
            auto work = std::move(ingressWork_.front());
            ingressWork_.pop_front();
            try {
                const auto fields = work.packet
                    ? message::ingressFields(*work.packet)
                    : message::connectionEventFields(*work.connectionEvent);
                (void)co_await message::redis::add(redis_, ingressStream(), fields, kRawIngressCapacity);
            } catch (const std::exception& error) {
                lastCoordinatorError_ = std::string("raw_ingress_publish_failed: ") + error.what();
                if (work.packet) {
                    const auto deviceCodes = deviceCodesForConnection(work.packet->connectionId);
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Error,
                        "RX_PUBLISH_FAILED",
                        ingressLogContext(*work.packet, deviceCodes),
                        work.packet->payload,
                        error.what()
                    );
                }
                tcp_.close(work.connectionId, "raw_ingress_publish_failed");
            }
        }
        ingressDraining_ = false;
    }

    ruvia::Task<bool> consumeIngress() {
        if (ingressConsuming_) {
            co_return false;
        }
        ingressConsuming_ = true;
        try {
            const auto stream = ingressStream();
            const auto group = ingressGroup();
            const auto messages =
                co_await readAvailable(stream, group, ingressRecovering_, 32);
            for (const auto& message : messages) {
                const auto eventType = message.get("event_type");
                if (eventType == "connected" || eventType == "disconnected") {
                    message::ConnectionEvent event;
                    std::string parseError;
                    try {
                        event = message::connectionEventFrom(message);
                    } catch (const std::exception& error) {
                        parseError = error.what();
                    }
                    if (!parseError.empty()) {
                        co_await deadLetterAndAcknowledge(
                            message,
                            "connection_event_invalid",
                            parseError,
                            stream,
                            group
                        );
                        continue;
                    }
                    if (event.workerInstanceId != workerInstanceId_) {
                        co_await deadLetterAndAcknowledge(
                            message,
                            "stale_worker_instance",
                            {},
                            stream,
                            group
                        );
                        continue;
                    }
                    if (event.eventType == "connected") {
                        const auto current = connectionEpochs_.find(event.connectionId);
                        if (current != connectionEpochs_.end() &&
                            current->second >= event.sessionEpoch) {
                            co_await deadLetterAndAcknowledge(
                                message,
                                "stale_session_epoch",
                                {},
                                stream,
                                group
                            );
                            continue;
                        } else {
                            connectionEpochs_[event.connectionId] = event.sessionEpoch;
                            co_await applyActions(
                                event.connectionId,
                                engine_.connected({ .connectionId = event.connectionId, .linkId = event.linkId, .remoteAddress = event.remoteAddress, .targetId = event.targetId, .sessionEpoch = event.sessionEpoch })
                            );
                        }
                    } else {
                        const auto current = connectionEpochs_.find(event.connectionId);
                        if (current != connectionEpochs_.end() &&
                            current->second == event.sessionEpoch) {
                            co_await applyActions(
                                event.connectionId,
                                engine_.disconnected(event.connectionId, event.reason),
                                true,
                                event.reason
                            );
                        }
                    }
                    co_await message::redis::acknowledgeAndDelete(redis_, stream, group, message.id);
                    continue;
                }
                message::IngressPacket packet;
                std::string parseError;
                try {
                    packet = message::ingressFrom(message);
                } catch (const std::exception& error) {
                    parseError = error.what();
                }
                if (!parseError.empty()) {
                    service::common::packet_log::Context logContext;
                    logContext.workerIndex = workerIndex_;
                    logContext.direction = "RX";
                    logContext.operation = "decode_ingress";
                    logContext.protocol = protocolForLink(message.get("link_id"));
                    logContext.linkId = message.get("link_id");
                    logContext.connectionId = message.get("connection_id");
                    logContext.remoteAddress = message.get("remote_address");
                    logContext.messageId = message.get("message_id");
                    const auto raw = message::fromHex(message.get("payload_hex"));
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Error,
                        "PARSE_ERROR",
                        logContext,
                        raw,
                        parseError
                    );
                    co_await deadLetterAndAcknowledge(message, "raw_ingress_invalid", parseError, stream, group);
                    continue;
                }
                if (packet.workerInstanceId != workerInstanceId_) {
                    const auto deviceCodes = deviceCodesForConnection(packet.connectionId);
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Warn,
                        "RX_REJECTED",
                        ingressLogContext(packet, deviceCodes),
                        packet.payload,
                        "stale_worker_instance"
                    );
                    co_await deadLetterAndAcknowledge(
                        message,
                        "stale_worker_instance",
                        {},
                        stream,
                        group
                    );
                    continue;
                }
                const auto epoch = connectionEpochs_.find(packet.connectionId);
                if (epoch == connectionEpochs_.end() || epoch->second != packet.sessionEpoch ||
                    !engine_.contains(packet.connectionId)) {
                    const auto deviceCodes = deviceCodesForConnection(packet.connectionId);
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Warn,
                        "RX_REJECTED",
                        ingressLogContext(packet, deviceCodes),
                        packet.payload,
                        "stale_session_epoch"
                    );
                    co_await deadLetterAndAcknowledge(
                        message,
                        "stale_session_epoch",
                        {},
                        stream,
                        group
                    );
                    continue;
                }
                try {
                    auto actions = engine_.consume(packet);
                    co_await applyActions(packet.connectionId, std::move(actions));
                } catch (const std::exception& error) {
                    const auto deviceCodes = deviceCodesForConnection(packet.connectionId);
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Error,
                        "PARSE_ERROR",
                        ingressLogContext(packet, deviceCodes),
                        packet.payload,
                        error.what()
                    );
                    ingressConsuming_ = false;
                    throw;
                } catch (...) {
                    const auto deviceCodes = deviceCodesForConnection(packet.connectionId);
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Error,
                        "PARSE_ERROR",
                        ingressLogContext(packet, deviceCodes),
                        packet.payload,
                        "unknown_protocol_exception"
                    );
                    ingressConsuming_ = false;
                    throw;
                }
                co_await message::redis::acknowledgeAndDelete(redis_, stream, group, message.id);
            }
            ingressConsuming_ = false;
            co_return !messages.empty();
        } catch (...) {
            ingressConsuming_ = false;
            throw;
        }
    }

    ruvia::Task<bool> consumeEgress() {
        if (egressWritePending_) {
            co_return false;
        }
        const auto stream = egressStream();
        const auto group = egressGroup();
        const auto messages =
            co_await readAvailable(stream, group, egressRecovering_, 1);
        if (messages.empty()) {
            co_return false;
        }
        const auto& message = messages.front();
        message::EgressPacket packet;
        std::string parseError;
        try {
            packet = message::egressFrom(message);
        } catch (const std::exception& error) {
            parseError = error.what();
        }
        if (!parseError.empty()) {
            service::common::packet_log::Context logContext;
            logContext.workerIndex = workerIndex_;
            logContext.direction = "TX";
            logContext.operation = "decode_egress";
            logContext.connectionId = message.get("connection_id");
            logContext.messageId = message.get("message_id");
            logContext.causationId = message.get("causation_id");
            const auto raw = message::fromHex(message.get("payload_hex"));
            service::common::packet_log::write(
                service::common::packet_log::Level::Error,
                "TX_REJECTED",
                logContext,
                raw,
                parseError
            );
            co_await deadLetterAndAcknowledge(message, "socket_egress_invalid", parseError, stream, group);
            co_return true;
        }
        if (packet.workerInstanceId != workerInstanceId_) {
            service::common::packet_log::Context logContext;
            logContext.workerIndex = workerIndex_;
            logContext.direction = "TX";
            logContext.operation = "transport";
            logContext.connectionId = packet.connectionId;
            logContext.messageId = packet.messageId;
            logContext.causationId = packet.causationId;
            logContext.sessionEpoch = packet.sessionEpoch;
            service::common::packet_log::write(
                service::common::packet_log::Level::Warn,
                "TX_REJECTED",
                logContext,
                packet.payload,
                "stale_worker_instance"
            );
            co_await deadLetterAndAcknowledge(message, "stale_worker_instance", {}, stream, group);
            co_return true;
        }
        const auto connection = networkConnections_.find(packet.connectionId);
        const auto owner = co_await message::redis::command(redis_, { "GET", ownership::key(connection == networkConnections_.end() ? std::string_view{} : connection->second.linkId) });
        if (owner.kind() != ruvia::RedisValue::Kind::kString || owner.string() != service::runtime::instanceId()) {
            tcp_.close(packet.connectionId, "link_ownership_lost");
            co_await message::redis::acknowledgeAndDelete(redis_, stream, group, message.id);
            co_return true;
        }
        const auto epoch = connectionEpochs_.find(packet.connectionId);
        if (epoch == connectionEpochs_.end() || epoch->second != packet.sessionEpoch) {
            service::common::packet_log::Context logContext;
            logContext.workerIndex = workerIndex_;
            logContext.direction = "TX";
            logContext.operation = "transport";
            logContext.connectionId = packet.connectionId;
            logContext.messageId = packet.messageId;
            logContext.causationId = packet.causationId;
            logContext.sessionEpoch = packet.sessionEpoch;
            service::common::packet_log::write(
                service::common::packet_log::Level::Warn,
                "TX_REJECTED",
                logContext,
                packet.payload,
                "stale_session_epoch"
            );
            co_await deadLetterAndAcknowledge(message, "stale_session_epoch", {}, stream, group);
            co_return true;
        }
        egressWritePending_ = true;
        const auto entryId = message.id;
        EgressLogContext egressLog;
        egressLog.connectionId = packet.connectionId;
        egressLog.messageId = packet.messageId;
        egressLog.causationId = packet.causationId;
        egressLog.sessionEpoch = packet.sessionEpoch;
        if (const auto* task = taskForCausation(packet.causationId)) {
            egressLog.operation = task->kind;
            egressLog.protocol = task->protocol;
            egressLog.linkId = task->linkId;
            egressLog.deviceId = task->deviceId;
            egressLog.deviceCode = task->deviceCode;
        } else {
            egressLog.operation = "protocol";
            egressLog.deviceCode = deviceCodesForConnection(packet.connectionId);
        }
        const auto network = networkConnections_.find(packet.connectionId);
        if (network != networkConnections_.end()) {
            if (egressLog.linkId.empty()) {
                egressLog.linkId = network->second.linkId;
            }
            if (egressLog.protocol.empty()) {
                egressLog.protocol = protocolForLink(network->second.linkId);
            }
            egressLog.remoteAddress = network->second.remoteAddress;
        }
        service::common::packet_log::write(service::common::packet_log::Level::Debug, "TX_BYTES", egressLogContext(egressLog), packet.payload);
        tcp_.send(packet.connectionId, std::move(packet.payload), [this, entryId, egressLog = std::move(egressLog)](bool success) mutable {
            if (!stopping_) {
                scope_.spawn(
                    completeEgress(entryId, success, std::move(egressLog))
                );
            }
        });
        co_return true;
    }

    ruvia::Task<void> completeEgress(std::string entryId, bool success, EgressLogContext egressLog) {
        service::common::packet_log::write(
            success ? service::common::packet_log::Level::Debug
                    : service::common::packet_log::Level::Error,
            success ? "TX_SUCCESS" : "TX_FAILED",
            egressLogContext(egressLog),
            {},
            success ? std::string_view{} : std::string_view("socket_write_failed")
        );
        try {
            const auto stream = egressStream();
            const auto group = egressGroup();
            if (!success) {
                message::StreamMessage message;
                message.id = entryId;
                co_await deadLetterAndAcknowledge(message, "socket_write_failed", {}, stream, group);
            } else {
                co_await message::redis::acknowledgeAndDelete(redis_, stream, group, entryId);
            }
        } catch (const std::exception& error) {
            lastCoordinatorError_ = std::string("socket_egress_complete_failed: ") + error.what();
            egressRecovering_ = true;
        }
        egressWritePending_ = false;
    }

    void spawnActions(std::string connectionId, std::vector<ProtocolAction> actions, bool disconnected = false, std::string reason = {}) {
        if (stopping_) {
            return;
        }
        scope_.spawn(applyActions(std::move(connectionId), std::move(actions), disconnected, std::move(reason)));
    }

    ruvia::Task<void> applyActions(std::string connectionId, std::vector<ProtocolAction> actions, bool disconnected = false, std::string reason = {}) {
        // A closed socket must stop being routable before potentially slow command-result
        // publication. Otherwise one Redis failure can leave an offline device advertised as
        // online indefinitely.
        if (disconnected) {
            co_await cleanupConnection(connectionId, reason);
        }
        for (auto& action : actions) {
            if (action.connectionId.empty()) {
                action.connectionId = connectionId;
            }
            switch (action.kind) {
                case ProtocolActionKind::Send:
                    if (const auto epoch = connectionEpochs_.find(action.connectionId);
                        epoch != connectionEpochs_.end()) {
                        message::EgressPacket packet{ .messageId = message::nextMessageId(),
                                                      .workerInstanceId = workerInstanceId_,
                                                      .causationId = action.commandId,
                                                      .connectionId = action.connectionId,
                                                      .sessionEpoch = epoch->second,
                                                      .createdAtMs = message::utcNowMilliseconds(),
                                                      .payload = std::move(action.bytes) };
                        (void)co_await message::redis::publish(redis_, egressStream(), message::egressFields(packet), kEgressStreamCapacity);
                    } else if (!action.commandId.empty()) {
                        co_await finishCommand(action.commandId, false, "stale_session_epoch");
                    }
                    break;
                case ProtocolActionKind::Close: {
                    const auto event = hasMarker(action.reason, "timeout") ? "TIMEOUT"
                                                                           : "PROTOCOL_CLOSE";
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Warn,
                        event,
                        actionLogContext(action, "protocol"),
                        {},
                        action.reason
                    );
                    tcp_.close(action.connectionId, action.reason.empty() ? "protocol_closed" : action.reason);
                    break;
                }
                case ProtocolActionKind::BindDevice:
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Info,
                        "DEVICE_BOUND",
                        actionLogContext(action, "bind")
                    );
                    co_await bindRouteIfConnected(action);
                    break;
                case ProtocolActionKind::PublishParsed:
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Debug,
                        "PARSE_SUCCESS",
                        parsedLogContext(action.parsed),
                        {},
                        action.parsed.source
                    );
                    (void)co_await message::redis::publishAndWake(
                        redis_,
                        parsedStream(),
                        message::parsedFields(action.parsed),
                        std::nullopt,
                        message::WorkerStreamTask::Telemetry
                    );
                    break;
                case ProtocolActionKind::CompleteCommand: {
                    const auto* task = taskForCausation(action.commandId);
                    auto context = task ? taskLogContext(*task, action.connectionId)
                                        : actionLogContext(action, "command");
                    context.causationId = action.commandId;
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Info,
                        task && task->kind == "discovery" ? "DISCOVERY_COMPLETED"
                                                          : "COMMAND_COMPLETED",
                        context,
                        {},
                        action.reason
                    );
                    co_await finishCommand(action.commandId, true, action.reason);
                    break;
                }
                case ProtocolActionKind::FailCommand: {
                    const auto* task = taskForCausation(action.commandId);
                    auto context = task ? taskLogContext(*task, action.connectionId)
                                        : actionLogContext(action, "command");
                    context.causationId = action.commandId;
                    const auto event = hasMarker(action.reason, "timeout")
                        ? "TIMEOUT"
                        : (task && task->kind == "discovery" ? "DISCOVERY_FAILED"
                                                             : "COMMAND_FAILED");
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Warn,
                        event,
                        context,
                        {},
                        action.reason
                    );
                    co_await finishCommand(action.commandId, false, action.reason);
                    break;
                }
                case ProtocolActionKind::ScheduleDeadline:
                    scheduleProtocolDeadline(action.connectionId, action.deadlineToken, action.deadlineAfter);
                    break;
                case ProtocolActionKind::CancelDeadline:
                    cancelProtocolDeadline(action.connectionId, action.deadlineToken);
                    break;
            }
        }
        if (disconnected) {
            co_await failPendingForConnection(connectionId, reason);
        }
    }

    void scheduleProtocolDeadline(std::string connectionId, std::uint64_t protocolToken, std::chrono::milliseconds delay) {
        const auto key = std::pair{ connectionId, protocolToken };
        cancelProtocolDeadline(connectionId, protocolToken);
        protocolDeadlines_[key] = scheduler_.scheduleAfter(
            delay,
            [this, connectionId = std::move(connectionId), protocolToken, key] {
                protocolDeadlines_.erase(key);
                if (stopping_) {
                    return;
                }
                ProtocolAction diagnostic;
                diagnostic.connectionId = connectionId;
                const auto context = actionLogContext(diagnostic, "deadline");
                const auto token = std::to_string(protocolToken);
                service::common::packet_log::write(
                    service::common::packet_log::Level::Debug,
                    "DEADLINE_FIRED",
                    context,
                    {},
                    token
                );
                auto actions = engine_.deadline(connectionId, protocolToken);
                if (actions.empty()) {
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Warn,
                        "TIMEOUT",
                        context,
                        {},
                        "deadline_expired_without_protocol_action"
                    );
                }
                spawnActions(connectionId, std::move(actions));
            }
        );
    }

    void cancelProtocolDeadline(std::string_view connectionId, std::uint64_t protocolToken) {
        const auto current = protocolDeadlines_.find({ std::string(connectionId), protocolToken });
        if (current == protocolDeadlines_.end()) {
            return;
        }
        scheduler_.cancel(current->second);
        protocolDeadlines_.erase(current);
    }

    ruvia::Task<void> waitForWork() {
        try {
            const std::vector<std::string> streams{
                configStream(),
                controlStream(),
                commandStream(true),
                commandStream(false),
                ingressStream(),
                egressStream(),
                linkEventStream()
            };
            const auto until = std::max(
                std::chrono::milliseconds(1),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    nextOwnershipRefresh_ - std::chrono::steady_clock::now()
                )
            );
            auto batches = co_await message::redis::readGroupManyBlockingUntil(
                redis_,
                streams,
                collectorGroup(),
                consumer_,
                scope_.stopToken(),
                until,
                1
            );
            for (auto& batch : batches) {
                auto& ready = readyMessages_[batch.stream];
                ready.insert(ready.end(), std::make_move_iterator(batch.messages.begin()), std::make_move_iterator(batch.messages.end()));
            }
            if (!stopping_) {
                scheduleTick();
            }
        } catch (const std::exception& error) {
            if (!stopping_) {
                recoverClaimedMessages(error.what());
            }
        } catch (...) {
            if (!stopping_) {
                recoverClaimedMessages("unknown_blocking_read_exception");
            }
        }
    }

    ruvia::Task<std::vector<message::StreamMessage>>
    readAvailable(std::string_view stream, std::string_view group, bool& recovering, std::size_t count) {
        if (auto ready = readyMessages_.find(stream); ready != readyMessages_.end()) {
            auto messages = std::move(ready->second);
            readyMessages_.erase(ready);
            co_return messages;
        }
        auto messages = co_await message::redis::readGroup(
            redis_,
            stream,
            group,
            consumer_,
            recovering ? "0" : ">",
            std::chrono::milliseconds(0),
            count
        );
        if (recovering && messages.empty()) {
            recovering = false;
        }
        co_return messages;
    }

    void cancelProtocolDeadlinesForConnection(std::string_view connectionId) {
        for (auto current = protocolDeadlines_.begin(); current != protocolDeadlines_.end();) {
            if (current->first.first == connectionId) {
                scheduler_.cancel(current->second);
                current = protocolDeadlines_.erase(current);
            } else {
                ++current;
            }
        }
    }

    ruvia::Task<void> applySessionRefresh(ProtocolSessionRefresh refresh) {
        std::set<std::string, std::less<>> nextDeviceCodes;
        for (const auto& action : refresh.startedActions) {
            if (action.kind == ProtocolActionKind::BindDevice) {
                nextDeviceCodes.insert(action.deviceId);
            }
        }

        const auto bound = routes_.find(refresh.connectionId);
        if (bound != routes_.end()) {
            for (const auto& deviceCode : bound->second) {
                if (!nextDeviceCodes.contains(deviceCode)) {
                    co_await markDeviceOffline(deviceCode, refresh.connectionId, "configuration_reloaded");
                }
            }
            routes_.erase(bound);
        }

        co_await applyActions(refresh.connectionId, std::move(refresh.retiredActions));
        co_await failPendingForConnection(refresh.connectionId, "configuration_reloaded");
        co_await applyActions(refresh.connectionId, std::move(refresh.startedActions));
    }

    ruvia::Task<void> cleanupConnection(std::string_view connectionId, std::string_view reason) {
        cancelProtocolDeadlinesForConnection(connectionId);
        const auto bound = routes_.find(connectionId);
        if (bound != routes_.end()) {
            for (const auto& deviceCode : bound->second) {
                co_await markDeviceOffline(deviceCode, connectionId, reason);
            }
            routes_.erase(bound);
        }
        co_await message::redis::eraseHash(redis_, "iot:runtime:connection:" + std::string(connectionId));
        connectionEpochs_.erase(std::string(connectionId));
        (void)reason;
    }

    ruvia::Task<void> failPendingForConnection(std::string_view connectionId, std::string_view reason) {
        std::vector<std::string> commandIds;
        for (const auto& [commandId, pending] : pendingCommands_) {
            if (pending.task.connectionId == connectionId) {
                commandIds.push_back(commandId);
            }
        }
        const auto failureReason = reason.empty() ? std::string_view("device_offline") : reason;
        for (const auto& commandId : commandIds) {
            co_await finishCommand(commandId, false, failureReason);
        }
    }

    ruvia::Task<void> bindRouteIfConnected(const ProtocolAction& action) {
        const auto connected = connectionEpochs_.find(action.connectionId);
        if (connected == connectionEpochs_.end()) {
            co_return;
        }
        const auto epoch = connected->second;
        co_await bindRoute(action, epoch);
        // Redis I/O yields to the worker loop. EOF may have removed this connection while the
        // HSET was in flight; compensate before making it locally routable.
        const auto current = connectionEpochs_.find(action.connectionId);
        if (current == connectionEpochs_.end() || current->second != epoch) {
            co_await markDeviceOffline(action.deviceId, action.connectionId, "connection_closed_during_registration");
            co_return;
        }
        routes_[action.connectionId].insert(action.deviceId);
    }

    ruvia::Task<void> bindRoute(const ProtocolAction& action, std::uint64_t sessionEpoch) {
        static constexpr std::string_view script = R"lua(
local previous_worker = redis.call('HGET', KEYS[1], 'worker_id') or ''
local previous_instance = redis.call('HGET', KEYS[1], 'instance_id') or ''
local previous_connection = redis.call('HGET', KEYS[1], 'connection_id') or ''
redis.call('HSET', KEYS[1],
  'device_id', ARGV[1], 'device_code', ARGV[2], 'worker_id', ARGV[3],
  'connection_id', ARGV[4], 'session_epoch', ARGV[5], 'updated_at_ms', ARGV[6],
  'instance_id', ARGV[7], 'link_id', ARGV[8])
if previous_connection ~= ARGV[4] then redis.call('XADD', KEYS[2], 'MAXLEN', '~', '100000', '*', 'topic', 'device') end
return {previous_worker, previous_connection, previous_instance}
)lua";
        const auto connection = networkConnections_.find(action.connectionId);
        if (connection == networkConnections_.end()) {
            co_return;
        }
        const auto key = service::telemetry::latest::runtimeKey(action.deviceId);
        const auto revisionKey = std::string(service::message::live::kChanges);
        const auto worker = std::to_string(workerIndex_);
        const auto epoch = std::to_string(sessionEpoch);
        const auto now = std::to_string(message::utcNowMilliseconds());
        const std::string_view keys[]{ key, revisionKey };
        const std::string_view args[]{
            action.deviceId,
            action.deviceCode,
            worker,
            action.connectionId,
            epoch,
            now,
            service::runtime::instanceId(),
            connection->second.linkId
        };
        const auto reply = co_await redis_.eval(script, keys, args);
        if (reply.kind() != ruvia::RedisValue::Kind::kArray || reply.array().size() != 3) {
            message::redis::throwValue("bind device route", reply);
        }
        const auto oldWorker = reply.array()[0].kind() == ruvia::RedisValue::Kind::kString
            ? std::string(reply.array()[0].string())
            : std::string{};
        const auto oldConnection = reply.array()[1].kind() == ruvia::RedisValue::Kind::kString
            ? std::string(reply.array()[1].string())
            : std::string{};
        if (oldConnection.empty() || oldConnection == action.connectionId || oldWorker.empty()) {
            co_return;
        }
        const auto oldInstance = reply.array()[2].string();
        if (oldWorker == worker && oldInstance == service::runtime::instanceId()) {
            tcp_.close(oldConnection, "device_re_registered");
            co_return;
        }
        (void)co_await message::redis::publish(
            redis_,
            std::string(message::kControlStreamPrefix) + std::string(oldInstance) + ":" + oldWorker,
            { { "message_id", message::nextMessageId() },
              { "connection_id", oldConnection },
              { "device_code", action.deviceCode },
              { "reason", "device_re_registered" },
              { "created_at_ms", std::to_string(message::utcNowMilliseconds()) } },
            1000
        );
    }

    ruvia::Task<void> markDeviceOffline(std::string_view deviceId, std::string_view connectionId, std::string_view reason) {
        static constexpr std::string_view script = R"lua(
if redis.call('HGET', KEYS[1], 'connection_id') ~= ARGV[1] then return 0 end
redis.call('HDEL', KEYS[1], 'instance_id', 'worker_id', 'connection_id', 'session_epoch')
if not redis.call('HGET', KEYS[1], 'last_report_at_ms') then
  redis.call('HSET', KEYS[1], 'state', 'offline', 'state_reason', ARGV[2])
end
redis.call('HSET', KEYS[1], 'updated_at_ms', ARGV[3])
redis.call('HSET', KEYS[2], '_state', cjson.encode({
  state = 'offline',
  reason = ARGV[2],
  lastReportAt = tonumber(redis.call('HGET', KEYS[1], 'last_report_at_ms') or '0') or 0,
  onlineUntil = tonumber(redis.call('HGET', KEYS[1], 'online_until_ms') or '0') or 0,
  updatedAt = tonumber(ARGV[3]) or 0
}), '_updated_at_ms', ARGV[3])
redis.call('XADD', KEYS[3], 'MAXLEN', '~', '100000', '*', 'topic', 'device')
return 1
)lua";
        const auto key = service::telemetry::latest::runtimeKey(deviceId);
        const auto latestKey = service::telemetry::latest::latestKey(deviceId);
        const auto revisionKey = std::string(service::message::live::kChanges);
        const auto now = std::to_string(message::utcNowMilliseconds());
        const std::string_view keys[]{ key, latestKey, revisionKey };
        const std::string_view args[]{ connectionId, reason, now };
        (void)co_await redis_.eval(script, keys, args);
    }

    ruvia::Task<void> finishCommand(std::string_view commandId, bool success, std::string_view reason) {
        if (commandId.empty()) {
            co_return;
        }
        const auto child = broadcastParents_.find(commandId);
        if (child != broadcastParents_.end()) {
            const auto parentId = child->second;
            broadcastParents_.erase(child);
            const auto broadcast = broadcasts_.find(parentId);
            if (broadcast == broadcasts_.end()) {
                co_return;
            }
            service::common::packet_log::Context childContext;
            childContext.workerIndex = workerIndex_;
            childContext.operation = "broadcast";
            childContext.messageId = commandId;
            childContext.causationId = parentId;
            service::common::packet_log::write(
                success ? service::common::packet_log::Level::Debug
                        : service::common::packet_log::Level::Warn,
                success ? "BROADCAST_TARGET_SUCCESS" : "BROADCAST_TARGET_FAILED",
                childContext,
                {},
                reason
            );
            broadcast->second.anySuccess = broadcast->second.anySuccess || success;
            if (!reason.empty()) {
                broadcast->second.reason = reason;
            }
            if (--broadcast->second.remaining != 0) {
                co_return;
            }
            const auto aggregateSuccess = broadcast->second.anySuccess;
            const auto aggregateReason = broadcast->second.reason.empty()
                ? std::string("discovery_window_closed")
                : broadcast->second.reason;
            broadcasts_.erase(broadcast);
            service::common::packet_log::Context aggregateContext;
            aggregateContext.workerIndex = workerIndex_;
            aggregateContext.operation = "broadcast";
            aggregateContext.messageId = parentId;
            service::common::packet_log::write(
                aggregateSuccess ? service::common::packet_log::Level::Info
                                 : service::common::packet_log::Level::Warn,
                aggregateSuccess ? "BROADCAST_COMPLETED" : "BROADCAST_FAILED",
                aggregateContext,
                {},
                aggregateReason
            );
            co_await finishCommand(parentId, aggregateSuccess, aggregateReason);
            co_return;
        }
        const auto current = pendingCommands_.find(commandId);
        if (current == pendingCommands_.end()) {
            co_return;
        }
        auto pending = std::move(current->second);
        pendingCommands_.erase(current);
        if (!success &&
            co_await retryCommand(pending.stream, pending.entryId, pending.task, reason)) {
            co_return;
        }
        service::common::packet_log::write(
            success ? service::common::packet_log::Level::Info
                    : service::common::packet_log::Level::Warn,
            pending.task.kind == "discovery"
                ? (success ? "DISCOVERY_RESULT" : "DISCOVERY_FAILED")
                : (success ? "COMMAND_RESULT" : "COMMAND_FAILED"),
            taskLogContext(pending.task),
            {},
            reason
        );
        co_await finalizeCommand(pending.stream, pending.entryId, commandId, pending.task, success, reason);
    }

    [[nodiscard]] static bool retryableFailure(std::string_view reason) {
        static constexpr std::array<std::string_view, 10> permanentMarkers{
            "invalid",
            "required",
            "not_supported",
            "mismatch",
            "conflict",
            "queue_full",
            "busy",
            "negative_ack",
            "protocol_route_mismatch",
            "stale_session_epoch"
        };
        const auto contains = [reason](std::string_view marker) {
            return reason.find(marker) != std::string_view::npos;
        };
        if (std::any_of(permanentMarkers.begin(), permanentMarkers.end(), contains)) {
            return false;
        }
        return contains("timeout") || contains("temporarily_unavailable") || contains("redis");
    }

    ruvia::Task<bool> retryCommand(std::string_view stream, std::string_view entryId, message::ProtocolTask task, std::string_view reason) {
        if (retryableFailure(reason) && task.attempt < task.maxAttempts) {
            ++task.attempt;
            const auto retryReason = "attempt=" + std::to_string(task.attempt) + " reason=" +
                std::string(reason);
            service::common::packet_log::write(
                service::common::packet_log::Level::Warn,
                task.kind == "discovery" ? "DISCOVERY_RETRY" : "COMMAND_RETRY",
                taskLogContext(task),
                {},
                retryReason
            );
            (void)co_await message::redis::publishAndAcknowledge(
                redis_,
                stream,
                message::protocolTaskFields(task),
                kCommandStreamCapacity,
                stream,
                commandGroup(),
                consumer_,
                entryId
            );
            co_return true;
        }
        co_return false;
    }

    ruvia::Task<void> failUndeliverable(std::string_view stream, std::string_view entryId, std::string_view commandId, const message::ProtocolTask& task, std::string_view reason) {
        if (co_await retryCommand(stream, entryId, task, reason)) {
            co_return;
        }
        co_await finalizeCommand(stream, entryId, commandId, task, false, reason);
    }

    [[nodiscard]] std::vector<message::StreamField> linkStateFields(const LinkState& state) {
        std::string endpoints;
        for (const auto& endpoint : state.remoteEndpoints) {
            if (!endpoints.empty()) {
                endpoints.push_back(',');
            }
            endpoints += endpoint;
        }
        std::vector<message::StreamField> fields{
            { "message_id", message::nextMessageId() },
            { "worker_instance_id", workerInstanceId_ },
            { "link_id", state.linkId },
            { "worker_id", std::to_string(state.workerIndex) },
            { "state", state.state },
            { "state_reason", state.reason },
            { "error", state.error },
            { "connection_count", std::to_string(state.remoteEndpoints.size()) },
            { "remote_endpoints", endpoints },
            { "last_activity_at_ms", std::to_string(state.lastActivityAtMs) },
            { "created_at_ms", std::to_string(message::utcNowMilliseconds()) }
        };
        for (const auto& target : state.targets) {
            const auto prefix = "target:" + target.id + ':';
            fields.push_back({ prefix + "state", target.state });
            fields.push_back({ prefix + "reason", target.reason });
            fields.push_back({ prefix + "error", target.error });
            fields.push_back(
                { prefix + "last_activity_at_ms", std::to_string(target.lastActivityAtMs) }
            );
        }
        return fields;
    }

    ruvia::Task<void> publishLinkEvent(LinkState state) {
        (void)co_await message::redis::publish(
            redis_,
            linkEventStream(),
            linkStateFields(state),
            kLinkEventStreamCapacity
        );
    }

    ruvia::Task<bool> consumeLinkEvent() {
        const auto stream = linkEventStream();
        const auto group = linkEventGroup();
        const auto messages =
            co_await readAvailable(stream, group, linkEventRecovering_, 32);
        for (const auto& message : messages) {
            const auto linkId = message.get("link_id");
            const auto workerId = message.get("worker_id");
            const auto workerInstanceId = message.get("worker_instance_id");
            if (linkId.empty() || workerId != std::to_string(workerIndex_) ||
                workerInstanceId != workerInstanceId_) {
                co_await deadLetterAndAcknowledge(message, "link_event_invalid", {}, stream, group);
                continue;
            }
            std::vector<message::StreamField> fields;
            fields.reserve(message.fields.size());
            for (const auto& field : message.fields) {
                if (field.name == "message_id" || field.name == "created_at_ms") {
                    continue;
                }
                fields.push_back(field);
            }
            fields.push_back({ "updated_at_ms", std::to_string(message::utcNowMilliseconds()) });
            const auto key =
                "iot:runtime:link:" + std::string(linkId) +
                ":worker:" + service::runtime::instanceId() + ":" + std::to_string(workerIndex_);
            co_await message::redis::eraseHash(redis_, key);
            co_await message::redis::setHash(redis_, key, fields);
            co_await message::redis::acknowledgeAndDelete(redis_, stream, group, message.id);
        }
        co_return !messages.empty();
    }

    [[nodiscard]] std::vector<message::StreamField>
    deadLetterFields(std::vector<message::StreamField> fields, std::string_view sourceEntryId, std::string_view reason, std::string_view detail = {}) const {
        fields.push_back({ "source_entry_id", std::string(sourceEntryId) });
        fields.push_back({ "failure_reason", std::string(reason) });
        fields.push_back({ "failure_detail", std::string(detail) });
        fields.push_back({ "worker_id", std::to_string(workerIndex_) });
        fields.push_back({ "failed_at_ms", std::to_string(message::utcNowMilliseconds()) });
        return fields;
    }

    [[nodiscard]] std::vector<message::StreamField>
    commandResultFields(std::string_view commandId, const message::ProtocolTask& task, bool success, std::string_view reason) const {
        const auto completedAt = std::to_string(message::utcNowMilliseconds());
        return { { "message_id", message::nextMessageId() },
                 { "causation_id", std::string(commandId) },
                 { "command_id", std::string(commandId) },
                 { "device_id", task.deviceId },
                 { "device_code", task.deviceCode },
                 { "protocol", task.protocol },
                 { "attempt", std::to_string(task.attempt) },
                 { "success", success ? "1" : "0" },
                 { "reason", std::string(reason) },
                 { "worker_id", std::to_string(workerIndex_) },
                 { "created_at_ms", completedAt },
                 { "completed_at_ms", completedAt } };
    }

    ruvia::Task<void> deadLetterAndAcknowledge(
        const message::StreamMessage& streamMessage,
        std::string_view reason,
        std::string_view detail,
        std::string_view inputStream,
        std::string_view inputGroup
    ) {
        auto fields = deadLetterFields(streamMessage.fields, streamMessage.id, reason, detail);
        const auto outputStream = deadLetterStream();
        (void)co_await message::redis::publishAndAcknowledge(
            redis_,
            outputStream,
            fields,
            kDeadLetterCapacity,
            inputStream,
            inputGroup,
            consumer_,
            streamMessage.id
        );
    }

    ruvia::Task<void> finalizeCommand(std::string_view inputStream, std::string_view inputEntryId, std::string_view commandId, const message::ProtocolTask& task, bool success, std::string_view reason) {
        auto resultFields = commandResultFields(commandId, task, success, reason);
        const auto resultStream = commandResultStream();
        if (success) {
            (void)co_await message::redis::publishAndAcknowledge(
                redis_,
                resultStream,
                resultFields,
                10000,
                inputStream,
                commandGroup(),
                consumer_,
                inputEntryId,
                message::redis::StreamWake{ std::nullopt,
                                            message::WorkerStreamTask::CommandResult }
            );
            co_return;
        }

        service::common::packet_log::write(
            service::common::packet_log::Level::Error,
            "DEAD_LETTER",
            taskLogContext(task),
            {},
            reason
        );
        auto deadFields = deadLetterFields(message::protocolTaskFields(task), inputEntryId, reason);
        const auto deadStream = deadLetterStream();
        const std::array publications{
            message::redis::StreamPublication{ deadStream, deadFields, kDeadLetterCapacity },
            message::redis::StreamPublication{
                resultStream,
                resultFields,
                10000,
                message::redis::StreamWake{ std::nullopt,
                                            message::WorkerStreamTask::CommandResult } }
        };
        (void)co_await message::redis::publishAllAndAcknowledge(
            redis_,
            publications,
            inputStream,
            commandGroup(),
            consumer_,
            inputEntryId
        );
    }

    ruvia::Task<void> publishRuntimeState() {
        const auto now = std::to_string(message::utcNowMilliseconds());
        co_await message::redis::setHash(
            redis_,
            "iot:runtime:collector:" + service::runtime::instanceId() + ":" + std::to_string(workerIndex_),
            { { "worker_id", std::to_string(workerIndex_) },
              { "version", loadedConfigVersion_ },
              { "state", "applied" },
              { "applied_at_ms", now } }
        );
    }

    static constexpr std::size_t kRawIngressCapacity = 100000;
    // Redis retains ownership for 15 seconds. Stop local I/O before that bound,
    // even when a slow Redis command delays renewal on this worker.
    static constexpr auto kTargetLeaseValidity = std::chrono::seconds(12);
    static constexpr auto kFailureDelay = std::chrono::milliseconds(250);
    static constexpr std::size_t kCommandStreamCapacity = 10000;
    static constexpr std::size_t kCommandConsumeBatch = 16;
    static constexpr std::size_t kEgressStreamCapacity = 10000;
    static constexpr std::size_t kLinkEventStreamCapacity = 1000;
    static constexpr std::size_t kDeadLetterCapacity = 1000;

    ruvia::EventLoop loop_;
    ruvia::WorkerHandle workerHandle_;
    std::pmr::unsynchronized_pool_resource resource_;
    ruvia::TaskScope scope_;
    DeadlineScheduler scheduler_;
    CollectorRedisClient redis_;
    ProtocolEngine engine_;
    TcpTransport tcp_;
    service::gb28181::CollectorRuntime gb28181_;
    std::size_t workerIndex_ = 0;
    std::size_t workerCount_ = 1;
    std::string workerInstanceId_ = message::nextMessageId();
    std::string consumer_;
    std::map<std::pair<std::string, std::uint64_t>, DeadlineScheduler::Token> protocolDeadlines_;
    std::map<std::string, std::set<std::string>, std::less<>> routes_;
    std::map<std::string, ProtocolConnectionInfo, std::less<>> networkConnections_;
    std::map<std::string, std::uint64_t, std::less<>> connectionEpochs_;
    std::map<ClientTargetKey, std::string> targetLeases_;
    std::map<std::string, TargetLeaseDeadline, std::less<>> targetLeaseDeadlines_;
    DeadlineScheduler::Token targetRenewalToken_ = 0;
    std::map<std::string, PendingCommand, std::less<>> pendingCommands_;
    std::map<std::string, BroadcastCommand, std::less<>> broadcasts_;
    std::map<std::string, std::string, std::less<>> broadcastParents_;
    std::map<std::string, std::vector<message::StreamMessage>, std::less<>> readyMessages_;
    DeadlineScheduler::Token tickToken_ = 0;
    std::string lastCoordinatorError_;
    std::string loadedConfigVersion_;
    std::string desiredConfigVersion_;
    std::chrono::steady_clock::time_point nextOwnershipRefresh_{};
    std::chrono::steady_clock::time_point nextConfigRecovery_{};
    RuntimeSnapshot desiredSnapshot_;
    RuntimeSnapshot loadedSnapshot_;
    bool configRecovering_ = true;
    bool highRecovering_ = true;
    bool normalRecovering_ = true;
    bool controlRecovering_ = true;
    bool ingressRecovering_ = true;
    bool egressRecovering_ = true;
    bool linkEventRecovering_ = true;
    bool ingressConsuming_ = false;
    bool egressWritePending_ = false;
    bool ingressDraining_ = false;
    std::deque<IngressWork> ingressWork_;
    bool stopping_ = false;
    bool shutdownStarted_ = false;
};

} // namespace service::collector

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <atomic>
#include <thread>

#include <ruvia/core/detail/io/AsioAwait.h>
#include <ruvia/web/redis/Redis.h>

namespace service::collector {

class CollectorWorkerPool final {
  public:
    CollectorWorkerPool() = default;
    CollectorWorkerPool(const CollectorWorkerPool&) = delete;
    CollectorWorkerPool& operator=(const CollectorWorkerPool&) = delete;

    ~CollectorWorkerPool() { stop(); }

    void start(ruvia::RedisConfig redisConfig, std::size_t workerCount, AppConfig gb28181) {
        if (running_.exchange(true)) {
            return;
        }
        workerCount = std::max<std::size_t>(1, workerCount);
        try {
            pool_ = std::make_unique<ruvia::EventLoopPool>(
                ruvia::EventLoopPoolOptions{ .loopCount = workerCount, .mailboxCapacity = 8192 }
            );
            workers_.reserve(workerCount);
            for (std::size_t index = 0; index < workerCount; ++index) {
                workers_.push_back(std::make_unique<CollectorWorker>(pool_->loop(index), redisConfig, index, workerCount, gb28181));
            }
            pool_->start();

            std::vector<std::future<void>> readiness;
            readiness.reserve(workerCount);
            for (auto& worker : workers_) {
                auto ready = std::make_shared<std::promise<void>>();
                readiness.push_back(ready->get_future());
                worker->start(std::move(ready));
            }
            for (auto& ready : readiness) {
                ready.get();
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() noexcept {
        if (!running_.exchange(false)) {
            return;
        }
        if (!pool_) {
            workers_.clear();
            return;
        }
        std::vector<std::future<void>> stopped;
        stopped.reserve(workers_.size());
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            auto completion = std::make_shared<std::promise<void>>();
            stopped.push_back(completion->get_future());
            auto* worker = workers_[index].get();
            auto loop = pool_->loop(index);
            asio::co_spawn(
                loop.ioContext(),
                ruvia::detail::taskAsAwaitable(worker->shutdown()),
                [completion](std::exception_ptr error) {
                    try {
                        if (error) {
                            completion->set_exception(std::move(error));
                        } else {
                            completion->set_value();
                        }
                    } catch (...) {
                    }
                }
            );
        }
        for (auto& completion : stopped) {
            try {
                completion.get();
            } catch (...) {
            }
        }
        pool_->stop();
        try {
            pool_->join();
        } catch (...) {
        }
        workers_.clear();
        pool_.reset();
    }

    [[nodiscard]] static std::size_t defaultWorkerCount() noexcept {
        const auto cpu = std::max(2U, std::thread::hardware_concurrency());
        return std::max<std::size_t>(1, cpu / 2U);
    }

  private:
    std::unique_ptr<ruvia::EventLoopPool> pool_;
    std::vector<std::unique_ptr<CollectorWorker>> workers_;
    std::atomic_bool running_{ false };
};

} // namespace service::collector
