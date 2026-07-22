#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <vector>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/TaskScope.h>

#include "service/common/bridge/message.contract.h"
#include "service/common/packet_log.h"
#include "service/modules/southbridge/network/worker_tcp.runtime.h"
#include "service/modules/southbridge/protocol/modbus/modbus.runtime.h"
#include "service/modules/southbridge/protocol/protocol_engine.h"
#include "service/modules/southbridge/protocol/s7/s7.runtime.h"
#include "service/modules/southbridge/protocol/sl651/sl651.runtime.h"
#include "service/modules/southbridge/queue/redis_stream_async.h"
#include "service/modules/southbridge/queue/worker_redis.client.h"
#include "service/modules/southbridge/runtime_config.redis.h"

namespace service::southbridge {

class SouthbridgeWorker final {
  public:
    using ServerSocketRouter = WorkerTcpRuntime::ServerAcceptHandler;

    SouthbridgeWorker(ruvia::EventLoop loop, ruvia::RedisConfig redisConfig,
                      std::size_t workerIndex, std::size_t workerCount,
                      ServerSocketRouter serverSocketRouter)
        : loop_(std::move(loop)), resource_(), scope_(loop_.handle(), &resource_),
          scheduler_(loop_.ioContext()),
          redis_(loop_.ioContext(), std::move(redisConfig), scheduler_), engine_(protocols()),
          tcp_(
              loop_.ioContext(), scheduler_, workerIndex, workerCount,
              [this](ProtocolConnectionInfo info) { enqueueConnected(std::move(info)); },
              [this](bridge::IngressPacket packet) { enqueueIngress(std::move(packet)); },
              [this](std::string connectionId, std::string reason) {
                  enqueueDisconnect(std::move(connectionId), std::move(reason));
              },
              [this](WorkerLinkState state) {
                  if (!stopping_)
                      scope_.spawn(publishLinkEvent(std::move(state)));
              },
              workerIndex == 0, std::move(serverSocketRouter)),
          workerIndex_(workerIndex), workerCount_(workerCount),
          consumer_("south-" + std::to_string(workerIndex)) {}

    SouthbridgeWorker(const SouthbridgeWorker&) = delete;
    SouthbridgeWorker& operator=(const SouthbridgeWorker&) = delete;

    void start(std::shared_ptr<std::promise<void>> ready) {
        if (!loop_.post([this, ready = std::move(ready)] { scope_.spawn(initialize(ready)); })
                 .accepted())
            throw std::runtime_error("south worker rejected startup");
    }

    [[nodiscard]] ruvia::Task<void> shutdown() {
        if (stopping_)
            co_return;
        stopping_ = true;
        if (tickToken_ != 0)
            scheduler_.cancel(tickToken_);
        tcp_.stop();
        scope_.requestStop();
        try {
            co_await scope_.join();
        } catch (...) {
        }
        for (const auto& [connectionId, deviceCodes] : routes_)
            for (const auto& deviceCode : deviceCodes)
                co_await markDeviceOffline(deviceCode, connectionId, "local_closed");
        routes_.clear();
        co_await bridge::redis_async::eraseMatching(redis_, "iot:state:link:*:worker:" +
                                                                std::to_string(workerIndex_));
        co_await bridge::redis_async::eraseHash(redis_, "iot:state:runtime-config:worker:" +
                                                            std::to_string(workerIndex_));
        redis_.close();
        scheduler_.stop();
    }

    [[nodiscard]] std::size_t index() const noexcept { return workerIndex_; }

    void adoptServerSocket(std::string linkId, WorkerTcpRuntime::NativeSocket handle,
                           std::string remote) {
        const auto posted = loop_.post(
            [this, linkId = std::move(linkId), handle, remote = std::move(remote)]() mutable {
                tcp_.adoptServerSocket(std::move(linkId), handle, std::move(remote));
            });
        if (!posted.accepted())
            WorkerTcpRuntime::closeNative(handle);
    }

  private:
    struct PendingCommand {
        std::string stream;
        std::string entryId;
        bridge::ProtocolTask task;
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
        std::optional<bridge::IngressPacket> packet;
        std::optional<bridge::ConnectionEvent> connectionEvent;
        std::string connectionId;
    };

    static ProtocolRuntimeRegistry protocols() {
        ProtocolRuntimeRegistry result;
        result.add(std::make_unique<modbus::Runtime>());
        result.add(std::make_unique<s7::Runtime>());
        result.add(std::make_unique<sl651::Runtime>());
        return result;
    }

    [[nodiscard]] std::string configGroup() const { return "iot-engine:config"; }

    [[nodiscard]] std::string ingressGroup() const { return "iot-engine:protocol-ingress"; }

    [[nodiscard]] std::string egressGroup() const { return "iot-engine:socket-egress"; }

    [[nodiscard]] std::string linkEventGroup() const { return "iot-engine:link-state"; }

    [[nodiscard]] std::string configStream() const { return bridge::configStream(workerIndex_); }

    [[nodiscard]] std::string commandStream(bool high) const {
        return bridge::commandStream(workerIndex_, high);
    }

    [[nodiscard]] std::string commandGroup() const { return "iot-engine:command"; }

    [[nodiscard]] std::string controlStream() const { return bridge::controlStream(workerIndex_); }

    [[nodiscard]] std::string ingressStream() const { return bridge::ingressStream(workerIndex_); }

    [[nodiscard]] std::string parsedStream() const { return bridge::parsedStream(workerIndex_); }

    [[nodiscard]] std::string egressStream() const { return bridge::egressStream(workerIndex_); }

    [[nodiscard]] std::string linkEventStream() const {
        return bridge::linkEventStream(workerIndex_);
    }

    [[nodiscard]] std::string commandResultStream() const {
        return bridge::commandResultStream(workerIndex_);
    }

    [[nodiscard]] std::string deadLetterStream() const {
        return bridge::deadLetterStream(workerIndex_);
    }

    [[nodiscard]] std::string_view protocolForLink(std::string_view linkId) const noexcept {
        const auto current = std::find_if(
            loadedSnapshot_.links.begin(), loadedSnapshot_.links.end(),
            [linkId](const auto& link) { return link.id == linkId; });
        return current == loadedSnapshot_.links.end() ? std::string_view{} : current->protocol;
    }

    [[nodiscard]] std::string deviceCodesForConnection(std::string_view connectionId) const {
        const auto current = routes_.find(connectionId);
        if (current == routes_.end())
            return {};
        std::string result;
        for (const auto& deviceCode : current->second) {
            if (!result.empty())
                result.push_back(',');
            result += deviceCode;
        }
        return result;
    }

    [[nodiscard]] const bridge::ProtocolTask*
    taskForCausation(std::string_view causationId) const noexcept {
        auto current = pendingCommands_.find(causationId);
        if (current != pendingCommands_.end())
            return &current->second.task;
        const auto child = broadcastParents_.find(causationId);
        if (child == broadcastParents_.end())
            return nullptr;
        current = pendingCommands_.find(child->second);
        return current == pendingCommands_.end() ? nullptr : &current->second.task;
    }

    [[nodiscard]] service::common::packet_log::Context
    ingressLogContext(const bridge::IngressPacket& packet,
                      std::string_view deviceCodes = {}) const noexcept {
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
    connectionLogContext(const bridge::ConnectionEvent& event) const noexcept {
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
    taskLogContext(const bridge::ProtocolTask& task,
                   std::string_view connectionId = {}) const noexcept {
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
        if (network != networkConnections_.end())
            context.remoteAddress = network->second.remoteAddress;
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
    parsedLogContext(const bridge::ParsedDeviceMessage& parsed) const noexcept {
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

    [[nodiscard]] static bool hasMarker(std::string_view value,
                                        std::string_view marker) noexcept {
        return value.find(marker) != std::string_view::npos;
    }

    ruvia::Task<void> initialize(std::shared_ptr<std::promise<void>> ready) {
        try {
            co_await redis_.connect();
            co_await bridge::redis_async::ensureGroup(redis_, configStream(), configGroup());
            co_await bridge::redis_async::ensureGroup(redis_, ingressStream(), ingressGroup());
            co_await bridge::redis_async::ensureGroup(redis_, egressStream(), egressGroup());
            co_await bridge::redis_async::ensureGroup(redis_, linkEventStream(), linkEventGroup());
            co_await bridge::redis_async::ensureGroup(redis_, commandStream(true), commandGroup());
            co_await bridge::redis_async::ensureGroup(redis_, commandStream(false), commandGroup());
            co_await bridge::redis_async::ensureGroup(redis_, controlStream(),
                                                      "iot-engine:control");
            // Runtime state is a projection owned by this worker. Remove leftovers from an
            // unclean previous process before publishing the freshly loaded snapshot.
            co_await bridge::redis_async::eraseMatching(redis_, "iot:state:link:*:worker:" +
                                                                    std::to_string(workerIndex_));
            co_await bridge::redis_async::eraseMatchingIfFieldValue(
                redis_, "iot:state:device:*", "worker_id", std::to_string(workerIndex_));
            loadedConfigVersion_ = co_await config_redis::activeVersion(redis_);
            auto snapshot = co_await config_redis::load(redis_, loadedConfigVersion_);
            engine_.reload(snapshot);
            tcp_.reload(snapshot);
            loadedSnapshot_ = std::move(snapshot);
            co_await publishRuntimeConfigState();
            ready->set_value();
            scheduleTick(std::chrono::milliseconds(0));
        } catch (...) {
            try {
                ready->set_exception(std::current_exception());
            } catch (...) {
            }
            stopping_ = true;
            redis_.close();
        }
    }

    void scheduleTick(std::chrono::milliseconds delay = kTickInterval) {
        if (stopping_)
            return;
        tickToken_ = scheduler_.scheduleAfter(delay, [this] {
            tickToken_ = 0;
            if (!stopping_)
                scope_.spawn(tick());
        });
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
            scheduleTick(handled ? std::chrono::milliseconds(0) : kTickInterval);
        } catch (const std::exception& error) {
            lastCoordinatorError_ = error.what();
            scheduleTick(kFailureDelay);
        }
    }

    ruvia::Task<bool> consumeConfig() {
        bool changed = false;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastConfigCheck_ >= kConfigCheckInterval) {
            lastConfigCheck_ = now;
            const auto version = co_await config_redis::activeVersion(redis_);
            if (version != loadedConfigVersion_) {
                auto snapshot = co_await config_redis::load(redis_, version);
                const auto affected = affectedLinks(loadedSnapshot_, snapshot);
                tcp_.stopLinks(affected);
                engine_.reload(snapshot, affected);
                tcp_.reconcile(snapshot, affected);
                for (const auto& link : loadedSnapshot_.links) {
                    if (std::none_of(
                            snapshot.links.begin(), snapshot.links.end(),
                            [&link](const auto& current) { return current.id == link.id; }))
                        co_await bridge::redis_async::eraseHash(
                            redis_, "iot:state:link:" + link.id +
                                        ":worker:" + std::to_string(workerIndex_));
                }
                loadedSnapshot_ = std::move(snapshot);
                loadedConfigVersion_ = version;
                co_await publishRuntimeConfigState();
                changed = true;
            }
        }
        const auto stream = configStream();
        const auto group = configGroup();
        const auto messages = co_await bridge::redis_async::readGroup(
            redis_, stream, group, consumer_, configRecovering_ ? "0" : ">",
            std::chrono::milliseconds(0), 16);
        if (configRecovering_ && messages.empty())
            configRecovering_ = false;
        if (messages.empty())
            co_return changed;
        for (const auto& message : messages)
            co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group, message.id);
        co_return true;
    }

    ruvia::Task<bool> consumeCommand(bool high) {
        const auto stream = commandStream(high);
        auto& recovering = high ? highRecovering_ : normalRecovering_;
        const auto messages = co_await bridge::redis_async::readGroup(
            redis_, stream, commandGroup(), consumer_, recovering ? "0" : ">",
            std::chrono::milliseconds(0), kCommandConsumeBatch);
        if (recovering && messages.empty())
            recovering = false;
        if (messages.empty())
            co_return false;
        for (const auto& message : messages) {
            bridge::ProtocolTask task;
            std::string parseError;
            try {
                task = bridge::protocolTaskFrom(message);
            } catch (const std::exception& error) {
                parseError = error.what();
            }
            if (!parseError.empty()) {
                co_await deadLetterMessage(message, "protocol_task_invalid", parseError);
                co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, commandGroup(),
                                                                   message.id);
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
                    .payload = bridge::fromHex(task.payload),
                    .readbackPayload = bridge::fromHex(task.readbackPayload),
                    .expectedReadbackData = bridge::fromHex(task.expectedReadbackData),
                    .expectedValue = task.expectedValue,
                    .elements =
                        [&task] {
                            std::vector<CommandElementValue> elements;
                            elements.reserve(task.elements.size());
                            for (const auto& [elementId, value] : task.elements)
                                elements.push_back({elementId, value});
                            return elements;
                        }(),
                    .highPriority = high,
                    .expectsResponse = task.expectsResponse,
                    .timeout = std::chrono::milliseconds(
                        std::clamp<std::int64_t>(task.responseTimeoutMs, 100, 60000))};
            };
            if (task.kind == "discovery") {
                const auto payload = bridge::fromHex(task.payload);
                service::common::packet_log::write(
                    service::common::packet_log::Level::Info, "DISCOVERY_REQUEST",
                    taskLogContext(task), payload);
            }
            if (task.kind == "discovery" && task.connectionId.empty()) {
                const auto connections = tcp_.connectionIds(task.linkId);
                if (connections.empty()) {
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Warn, "DISCOVERY_FAILED",
                        taskLogContext(task), {}, "discovery_no_connections");
                    co_await failUndeliverable(stream, message.id, task.messageId, task,
                                               "discovery_no_connections");
                    continue;
                }
                pendingCommands_.insert_or_assign(task.messageId,
                                                  PendingCommand{stream, message.id, task});
                broadcasts_[task.messageId] = BroadcastCommand{.remaining = connections.size()};
                const auto targetCount = "targets=" + std::to_string(connections.size());
                service::common::packet_log::write(
                    service::common::packet_log::Level::Info, "BROADCAST_START",
                    taskLogContext(task), {}, targetCount);
                for (const auto& connectionId : connections) {
                    const auto childId = bridge::nextMessageId();
                    broadcastParents_[childId] = task.messageId;
                    auto context = taskLogContext(task, connectionId);
                    context.causationId = task.messageId;
                    context.messageId = childId;
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Debug, "BROADCAST_TARGET", context);
                    co_await applyActions(connectionId,
                                          engine_.execute(connectionId, makeCommand(childId)));
                }
                continue;
            }
            if (task.connectionId.empty()) {
                co_await failUndeliverable(stream, message.id, task.messageId, task,
                                           "connection_route_missing");
                continue;
            }
            const auto epoch = connectionEpochs_.find(task.connectionId);
            if (task.sessionEpoch != 0 &&
                (epoch == connectionEpochs_.end() || epoch->second != task.sessionEpoch)) {
                co_await failUndeliverable(stream, message.id, task.messageId, task,
                                           "stale_session_epoch");
                continue;
            }
            pendingCommands_.insert_or_assign(task.messageId,
                                              PendingCommand{stream, message.id, task});
            auto command = makeCommand(task.messageId);
            if (task.kind == "discovery")
                service::common::packet_log::write(
                    service::common::packet_log::Level::Info, "DISCOVERY_START",
                    taskLogContext(task, task.connectionId));
            co_await applyActions(task.connectionId,
                                  engine_.execute(task.connectionId, std::move(command)));
        }
        co_return true;
    }

    [[nodiscard]] static std::set<std::string, std::less<>>
    affectedLinks(const RuntimeSnapshot& previous, const RuntimeSnapshot& next) {
        std::map<std::string, LinkDefinition, std::less<>> previousLinks;
        std::map<std::string, LinkDefinition, std::less<>> nextLinks;
        std::map<std::string, std::vector<DeviceDefinition>, std::less<>> previousDevices;
        std::map<std::string, std::vector<DeviceDefinition>, std::less<>> nextDevices;
        for (const auto& link : previous.links)
            previousLinks.emplace(link.id, link);
        for (const auto& link : next.links)
            nextLinks.emplace(link.id, link);
        for (const auto& device : previous.devices)
            previousDevices[device.linkId].push_back(device);
        for (const auto& device : next.devices)
            nextDevices[device.linkId].push_back(device);
        const auto sortDevices = [](auto& grouped) {
            for (auto& [linkId, devices] : grouped) {
                (void)linkId;
                std::ranges::sort(devices, {}, &DeviceDefinition::id);
            }
        };
        sortDevices(previousDevices);
        sortDevices(nextDevices);

        std::set<std::string, std::less<>> result;
        for (const auto& [id, link] : previousLinks) {
            const auto current = nextLinks.find(id);
            if (current == nextLinks.end() || current->second != link ||
                previousDevices[id] != nextDevices[id])
                result.insert(id);
        }
        for (const auto& [id, link] : nextLinks) {
            const auto old = previousLinks.find(id);
            if (old == previousLinks.end() || old->second != link ||
                previousDevices[id] != nextDevices[id])
                result.insert(id);
        }
        return result;
    }

    ruvia::Task<bool> consumeControl() {
        const auto stream = controlStream();
        const auto messages = co_await bridge::redis_async::readGroup(
            redis_, stream, "iot-engine:control", consumer_, controlRecovering_ ? "0" : ">",
            std::chrono::milliseconds(0), 32);
        if (controlRecovering_ && messages.empty())
            controlRecovering_ = false;
        for (const auto& message : messages) {
            const auto connectionId = message.get("connection_id");
            if (!connectionId.empty())
                tcp_.close(connectionId, message.get("reason").empty()
                                             ? "device_re_registered"
                                             : std::string(message.get("reason")));
            co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, "iot-engine:control",
                                                               message.id);
        }
        co_return !messages.empty();
    }

    void enqueueIngress(bridge::IngressPacket packet) {
        if (stopping_)
            return;
        packet.workerInstanceId = workerInstanceId_;
        const auto deviceCodes = deviceCodesForConnection(packet.connectionId);
        const auto logContext = ingressLogContext(packet, deviceCodes);
        service::common::packet_log::write(service::common::packet_log::Level::Debug, "RX_BYTES",
                                           logContext, packet.payload);
        if (ingressWork_.size() >= kRawIngressCapacity) {
            service::common::packet_log::write(service::common::packet_log::Level::Error,
                                               "RX_DROPPED", logContext, packet.payload,
                                               "raw_ingress_backpressure");
            tcp_.close(packet.connectionId, "raw_ingress_backpressure");
            return;
        }
        const auto connectionId = packet.connectionId;
        ingressWork_.push_back({.packet = std::move(packet),
                                .connectionEvent = std::nullopt,
                                .connectionId = connectionId});
        startIngressDrain();
    }

    void enqueueConnected(ProtocolConnectionInfo info) {
        if (stopping_)
            return;
        const auto connectionId = info.connectionId;
        networkConnections_.insert_or_assign(connectionId, info);
        bridge::ConnectionEvent event{.messageId = bridge::nextMessageId(),
                                      .workerInstanceId = workerInstanceId_,
                                      .eventType = "connected",
                                      .linkId = info.linkId,
                                      .connectionId = info.connectionId,
                                      .remoteAddress = info.remoteAddress,
                                      .targetId = info.targetId,
                                      .reason = {},
                                      .sessionEpoch = info.sessionEpoch,
                                      .occurredAtMs = bridge::utcNowMilliseconds()};
        service::common::packet_log::write(service::common::packet_log::Level::Info,
                                           "CONNECTED", connectionLogContext(event));
        ingressWork_.push_back({.packet = std::nullopt,
                                .connectionEvent = std::move(event),
                                .connectionId = connectionId});
        startIngressDrain();
    }

    void enqueueDisconnect(std::string connectionId, std::string reason) {
        if (stopping_)
            return;
        const auto current = networkConnections_.find(connectionId);
        if (current == networkConnections_.end())
            return;
        const auto info = current->second;
        networkConnections_.erase(current);
        bridge::ConnectionEvent event{.messageId = bridge::nextMessageId(),
                                      .workerInstanceId = workerInstanceId_,
                                      .eventType = "disconnected",
                                      .linkId = info.linkId,
                                      .connectionId = info.connectionId,
                                      .remoteAddress = info.remoteAddress,
                                      .targetId = info.targetId,
                                      .reason = std::move(reason),
                                      .sessionEpoch = info.sessionEpoch,
                                      .occurredAtMs = bridge::utcNowMilliseconds()};
        service::common::packet_log::write(service::common::packet_log::Level::Warn,
                                           "DISCONNECTED", connectionLogContext(event), {},
                                           event.reason);
        ingressWork_.push_back({.packet = std::nullopt,
                                .connectionEvent = std::move(event),
                                .connectionId = std::move(connectionId)});
        startIngressDrain();
    }

    void startIngressDrain() {
        if (ingressDraining_ || stopping_)
            return;
        ingressDraining_ = true;
        scope_.spawn(drainIngress());
    }

    ruvia::Task<void> drainIngress() {
        while (!stopping_ && !ingressWork_.empty()) {
            auto work = std::move(ingressWork_.front());
            ingressWork_.pop_front();
            try {
                const auto fields = work.packet
                                        ? bridge::ingressFields(*work.packet)
                                        : bridge::connectionEventFields(*work.connectionEvent);
                (void)co_await bridge::redis_async::add(redis_, ingressStream(), fields,
                                                        kRawIngressCapacity);
            } catch (const std::exception& error) {
                lastCoordinatorError_ = std::string("raw_ingress_publish_failed: ") + error.what();
                if (work.packet) {
                    const auto deviceCodes = deviceCodesForConnection(work.packet->connectionId);
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Error, "RX_PUBLISH_FAILED",
                        ingressLogContext(*work.packet, deviceCodes), work.packet->payload,
                        error.what());
                }
                tcp_.close(work.connectionId, "raw_ingress_publish_failed");
            }
        }
        ingressDraining_ = false;
    }

    ruvia::Task<bool> consumeIngress() {
        if (ingressConsuming_)
            co_return false;
        ingressConsuming_ = true;
        try {
            const auto stream = ingressStream();
            const auto group = ingressGroup();
            const auto messages = co_await bridge::redis_async::readGroup(
                redis_, stream, group, consumer_, ingressRecovering_ ? "0" : ">",
                std::chrono::milliseconds(0), 32);
            if (ingressRecovering_ && messages.empty())
                ingressRecovering_ = false;
            for (const auto& message : messages) {
                const auto eventType = message.get("event_type");
                if (eventType == "connected" || eventType == "disconnected") {
                    bridge::ConnectionEvent event;
                    std::string parseError;
                    try {
                        event = bridge::connectionEventFrom(message);
                    } catch (const std::exception& error) {
                        parseError = error.what();
                    }
                    if (!parseError.empty()) {
                        co_await deadLetterMessage(message, "connection_event_invalid", parseError);
                        co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group,
                                                                           message.id);
                        continue;
                    }
                    if (event.workerInstanceId != workerInstanceId_) {
                        co_await deadLetterMessage(message, "stale_worker_instance");
                        co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group,
                                                                           message.id);
                        continue;
                    }
                    if (event.eventType == "connected") {
                        const auto current = connectionEpochs_.find(event.connectionId);
                        if (current != connectionEpochs_.end() &&
                            current->second >= event.sessionEpoch) {
                            co_await deadLetterMessage(message, "stale_session_epoch");
                        } else {
                            connectionEpochs_[event.connectionId] = event.sessionEpoch;
                            co_await applyActions(
                                event.connectionId,
                                engine_.connected({.connectionId = event.connectionId,
                                                   .linkId = event.linkId,
                                                   .remoteAddress = event.remoteAddress,
                                                   .targetId = event.targetId,
                                                   .sessionEpoch = event.sessionEpoch}));
                        }
                    } else {
                        const auto current = connectionEpochs_.find(event.connectionId);
                        if (current != connectionEpochs_.end() &&
                            current->second == event.sessionEpoch) {
                            co_await applyActions(
                                event.connectionId,
                                engine_.disconnected(event.connectionId, event.reason), true,
                                event.reason);
                        }
                    }
                    co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group,
                                                                       message.id);
                    continue;
                }
                bridge::IngressPacket packet;
                std::string parseError;
                try {
                    packet = bridge::ingressFrom(message);
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
                    const auto raw = bridge::fromHex(message.get("payload_hex"));
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Error, "PARSE_ERROR", logContext, raw,
                        parseError);
                    co_await deadLetterMessage(message, "raw_ingress_invalid", parseError);
                    co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group,
                                                                       message.id);
                    continue;
                }
                if (packet.workerInstanceId != workerInstanceId_) {
                    const auto deviceCodes = deviceCodesForConnection(packet.connectionId);
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Warn, "RX_REJECTED",
                        ingressLogContext(packet, deviceCodes), packet.payload,
                        "stale_worker_instance");
                    co_await deadLetterMessage(message, "stale_worker_instance");
                    co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group,
                                                                       message.id);
                    continue;
                }
                const auto epoch = connectionEpochs_.find(packet.connectionId);
                if (epoch == connectionEpochs_.end() || epoch->second != packet.sessionEpoch ||
                    !engine_.contains(packet.connectionId)) {
                    const auto deviceCodes = deviceCodesForConnection(packet.connectionId);
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Warn, "RX_REJECTED",
                        ingressLogContext(packet, deviceCodes), packet.payload,
                        "stale_session_epoch");
                    co_await deadLetterMessage(message, "stale_session_epoch");
                    co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group,
                                                                       message.id);
                    continue;
                }
                try {
                    auto actions = engine_.consume(packet);
                    co_await applyActions(packet.connectionId, std::move(actions));
                } catch (const std::exception& error) {
                    const auto deviceCodes = deviceCodesForConnection(packet.connectionId);
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Error, "PARSE_ERROR",
                        ingressLogContext(packet, deviceCodes), packet.payload, error.what());
                    ingressConsuming_ = false;
                    throw;
                } catch (...) {
                    const auto deviceCodes = deviceCodesForConnection(packet.connectionId);
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Error, "PARSE_ERROR",
                        ingressLogContext(packet, deviceCodes), packet.payload,
                        "unknown_protocol_exception");
                    ingressConsuming_ = false;
                    throw;
                }
                co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group,
                                                                   message.id);
            }
            ingressConsuming_ = false;
            co_return !messages.empty();
        } catch (...) {
            ingressConsuming_ = false;
            throw;
        }
    }

    ruvia::Task<bool> consumeEgress() {
        if (egressWritePending_)
            co_return false;
        const auto stream = egressStream();
        const auto group = egressGroup();
        const auto messages = co_await bridge::redis_async::readGroup(
            redis_, stream, group, consumer_, egressRecovering_ ? "0" : ">",
            std::chrono::milliseconds(0), 1);
        if (egressRecovering_ && messages.empty())
            egressRecovering_ = false;
        if (messages.empty())
            co_return false;
        const auto& message = messages.front();
        bridge::EgressPacket packet;
        std::string parseError;
        try {
            packet = bridge::egressFrom(message);
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
            const auto raw = bridge::fromHex(message.get("payload_hex"));
            service::common::packet_log::write(
                service::common::packet_log::Level::Error, "TX_REJECTED", logContext, raw,
                parseError);
            co_await deadLetterMessage(message, "socket_egress_invalid", parseError);
            co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group, message.id);
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
                service::common::packet_log::Level::Warn, "TX_REJECTED", logContext,
                packet.payload, "stale_worker_instance");
            co_await deadLetterMessage(message, "stale_worker_instance");
            co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group, message.id);
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
                service::common::packet_log::Level::Warn, "TX_REJECTED", logContext,
                packet.payload, "stale_session_epoch");
            co_await deadLetterMessage(message, "stale_session_epoch");
            co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group, message.id);
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
            if (egressLog.linkId.empty())
                egressLog.linkId = network->second.linkId;
            if (egressLog.protocol.empty())
                egressLog.protocol = protocolForLink(network->second.linkId);
            egressLog.remoteAddress = network->second.remoteAddress;
        }
        service::common::packet_log::write(service::common::packet_log::Level::Debug,
                                           "TX_BYTES", egressLogContext(egressLog),
                                           packet.payload);
        tcp_.send(packet.connectionId, std::move(packet.payload),
                  [this, entryId, egressLog = std::move(egressLog)](bool success) mutable {
                      if (!stopping_)
                          scope_.spawn(
                              completeEgress(entryId, success, std::move(egressLog)));
                  });
        co_return true;
    }

    ruvia::Task<void> completeEgress(std::string entryId, bool success,
                                     EgressLogContext egressLog) {
        service::common::packet_log::write(
            success ? service::common::packet_log::Level::Debug
                    : service::common::packet_log::Level::Error,
            success ? "TX_SUCCESS" : "TX_FAILED", egressLogContext(egressLog), {},
            success ? std::string_view{} : std::string_view("socket_write_failed"));
        try {
            if (!success) {
                bridge::StreamMessage message;
                message.id = entryId;
                co_await deadLetterMessage(message, "socket_write_failed");
            }
            co_await bridge::redis_async::acknowledgeAndDelete(redis_, egressStream(),
                                                               egressGroup(), entryId);
        } catch (const std::exception& error) {
            lastCoordinatorError_ = std::string("socket_egress_complete_failed: ") + error.what();
            egressRecovering_ = true;
        }
        egressWritePending_ = false;
    }

    void spawnActions(std::string connectionId, std::vector<ProtocolAction> actions,
                      bool disconnected = false, std::string reason = {}) {
        if (stopping_)
            return;
        scope_.spawn(applyActions(std::move(connectionId), std::move(actions), disconnected,
                                  std::move(reason)));
    }

    ruvia::Task<void> applyActions(std::string connectionId, std::vector<ProtocolAction> actions,
                                   bool disconnected = false, std::string reason = {}) {
        // A closed socket must stop being routable before potentially slow command-result
        // publication. Otherwise one Redis failure can leave an offline device advertised as
        // online indefinitely.
        if (disconnected)
            co_await cleanupConnection(connectionId, reason);
        for (auto& action : actions) {
            if (action.connectionId.empty())
                action.connectionId = connectionId;
            switch (action.kind) {
            case ProtocolActionKind::Send:
                if (const auto epoch = connectionEpochs_.find(action.connectionId);
                    epoch != connectionEpochs_.end()) {
                    bridge::EgressPacket packet{.messageId = bridge::nextMessageId(),
                                                .workerInstanceId = workerInstanceId_,
                                                .causationId = action.commandId,
                                                .connectionId = action.connectionId,
                                                .sessionEpoch = epoch->second,
                                                .createdAtMs = bridge::utcNowMilliseconds(),
                                                .payload = std::move(action.bytes)};
                    (void)co_await bridge::redis_async::publish(redis_, egressStream(),
                                                                bridge::egressFields(packet),
                                                                kEgressStreamCapacity);
                } else if (!action.commandId.empty()) {
                    co_await finishCommand(action.commandId, false, "stale_session_epoch");
                }
                break;
            case ProtocolActionKind::Close: {
                const auto event = hasMarker(action.reason, "timeout") ? "TIMEOUT"
                                                                       : "PROTOCOL_CLOSE";
                service::common::packet_log::write(
                    service::common::packet_log::Level::Warn, event,
                    actionLogContext(action, "protocol"), {}, action.reason);
                tcp_.close(action.connectionId,
                           action.reason.empty() ? "protocol_closed" : action.reason);
                break;
            }
            case ProtocolActionKind::BindDevice:
                service::common::packet_log::write(
                    service::common::packet_log::Level::Info, "DEVICE_BOUND",
                    actionLogContext(action, "bind"));
                co_await bindRouteIfConnected(action);
                break;
            case ProtocolActionKind::PublishParsed:
                service::common::packet_log::write(
                    service::common::packet_log::Level::Debug, "PARSE_SUCCESS",
                    parsedLogContext(action.parsed), {}, action.parsed.source);
                (void)co_await bridge::redis_async::publish(redis_, parsedStream(),
                                                            bridge::parsedFields(action.parsed));
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
                    context, {}, action.reason);
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
                    service::common::packet_log::Level::Warn, event, context, {}, action.reason);
                co_await finishCommand(action.commandId, false, action.reason);
                break;
            }
            case ProtocolActionKind::ScheduleDeadline:
                scheduleProtocolDeadline(action.connectionId, action.deadlineToken,
                                         action.deadlineAfter);
                break;
            case ProtocolActionKind::CancelDeadline:
                cancelProtocolDeadline(action.connectionId, action.deadlineToken);
                break;
            }
        }
        if (disconnected)
            co_await failPendingForConnection(connectionId, reason);
    }

    void scheduleProtocolDeadline(std::string connectionId, std::uint64_t protocolToken,
                                  std::chrono::milliseconds delay) {
        const auto key = std::pair{connectionId, protocolToken};
        cancelProtocolDeadline(connectionId, protocolToken);
        protocolDeadlines_[key] = scheduler_.scheduleAfter(
            delay, [this, connectionId = std::move(connectionId), protocolToken, key] {
                protocolDeadlines_.erase(key);
                if (stopping_)
                    return;
                ProtocolAction diagnostic;
                diagnostic.connectionId = connectionId;
                const auto context = actionLogContext(diagnostic, "deadline");
                const auto token = std::to_string(protocolToken);
                service::common::packet_log::write(
                    service::common::packet_log::Level::Debug, "DEADLINE_FIRED", context, {},
                    token);
                auto actions = engine_.deadline(connectionId, protocolToken);
                if (actions.empty())
                    service::common::packet_log::write(
                        service::common::packet_log::Level::Warn, "TIMEOUT", context, {},
                        "deadline_expired_without_protocol_action");
                spawnActions(connectionId, std::move(actions));
            });
    }

    void cancelProtocolDeadline(std::string_view connectionId, std::uint64_t protocolToken) {
        const auto current = protocolDeadlines_.find({std::string(connectionId), protocolToken});
        if (current == protocolDeadlines_.end())
            return;
        scheduler_.cancel(current->second);
        protocolDeadlines_.erase(current);
    }

    ruvia::Task<void> cleanupConnection(std::string_view connectionId, std::string_view reason) {
        for (auto current = protocolDeadlines_.begin(); current != protocolDeadlines_.end();) {
            if (current->first.first == connectionId) {
                scheduler_.cancel(current->second);
                current = protocolDeadlines_.erase(current);
            } else
                ++current;
        }
        const auto bound = routes_.find(connectionId);
        if (bound != routes_.end()) {
            for (const auto& deviceCode : bound->second) {
                co_await markDeviceOffline(deviceCode, connectionId, reason);
            }
            routes_.erase(bound);
        }
        co_await bridge::redis_async::eraseHash(redis_, "iot:state:connection:" +
                                                            std::string(connectionId));
        connectionEpochs_.erase(std::string(connectionId));
        (void)reason;
    }

    ruvia::Task<void> failPendingForConnection(std::string_view connectionId,
                                               std::string_view reason) {
        std::vector<std::string> commandIds;
        for (const auto& [commandId, pending] : pendingCommands_)
            if (pending.task.connectionId == connectionId)
                commandIds.push_back(commandId);
        const auto failureReason = reason.empty() ? std::string_view("device_offline") : reason;
        for (const auto& commandId : commandIds)
            co_await finishCommand(commandId, false, failureReason);
    }

    ruvia::Task<void> bindRouteIfConnected(const ProtocolAction& action) {
        const auto connected = connectionEpochs_.find(action.connectionId);
        if (connected == connectionEpochs_.end())
            co_return;
        const auto epoch = connected->second;
        co_await bindRoute(action, epoch);
        // Redis I/O yields to the worker loop. EOF may have removed this connection while the
        // HSET was in flight; compensate before making it locally routable.
        const auto current = connectionEpochs_.find(action.connectionId);
        if (current == connectionEpochs_.end() || current->second != epoch) {
            co_await markDeviceOffline(action.deviceCode, action.connectionId,
                                       "connection_closed_during_registration");
            co_return;
        }
        routes_[action.connectionId].insert(action.deviceCode);
    }

    ruvia::Task<void> bindRoute(const ProtocolAction& action, std::uint64_t sessionEpoch) {
        static constexpr std::string_view script = R"lua(
local previous_worker = redis.call('HGET', KEYS[1], 'worker_id') or ''
local previous_connection = redis.call('HGET', KEYS[1], 'connection_id') or ''
redis.call('HSET', KEYS[1],
  'device_id', ARGV[1], 'device_code', ARGV[2], 'worker_id', ARGV[3],
  'connection_id', ARGV[4], 'session_epoch', ARGV[5], 'updated_at_ms', ARGV[6])
return {previous_worker, previous_connection}
)lua";
        const auto key = "iot:state:device:" + action.deviceCode;
        const auto worker = std::to_string(workerIndex_);
        const auto epoch = std::to_string(sessionEpoch);
        const auto now = std::to_string(bridge::utcNowMilliseconds());
        const std::string_view keys[]{key};
        const std::string_view args[]{
            action.deviceId, action.deviceCode, worker, action.connectionId, epoch, now};
        const auto reply = co_await redis_.eval(script, keys, args);
        if (reply.kind() != ruvia::RedisValue::Kind::kArray || reply.array().size() != 2)
            bridge::redis_async::throwValue("bind device route", reply);
        const auto oldWorker = reply.array()[0].kind() == ruvia::RedisValue::Kind::kString
                                   ? std::string(reply.array()[0].string())
                                   : std::string{};
        const auto oldConnection = reply.array()[1].kind() == ruvia::RedisValue::Kind::kString
                                       ? std::string(reply.array()[1].string())
                                       : std::string{};
        if (oldConnection.empty() || oldConnection == action.connectionId || oldWorker.empty())
            co_return;
        if (oldWorker == worker) {
            tcp_.close(oldConnection, "device_re_registered");
            co_return;
        }
        (void)co_await bridge::redis_async::publish(
            redis_, std::string(bridge::kControlStreamPrefix) + oldWorker,
            {{"message_id", bridge::nextMessageId()},
             {"connection_id", oldConnection},
             {"device_code", action.deviceCode},
             {"reason", "device_re_registered"},
             {"created_at_ms", std::to_string(bridge::utcNowMilliseconds())}},
            1000);
    }

    ruvia::Task<void> markDeviceOffline(std::string_view deviceCode, std::string_view connectionId,
                                        std::string_view reason) {
        static constexpr std::string_view script = R"lua(
if redis.call('HGET', KEYS[1], 'connection_id') ~= ARGV[1] then return 0 end
redis.call('HDEL', KEYS[1], 'worker_id', 'connection_id', 'session_epoch')
if not redis.call('HGET', KEYS[1], 'last_report_at_ms') then
  redis.call('HSET', KEYS[1], 'state', 'offline', 'state_reason', ARGV[2])
end
redis.call('HSET', KEYS[1], 'updated_at_ms', ARGV[3])
return 1
)lua";
        const auto key = "iot:state:device:" + std::string(deviceCode);
        const auto now = std::to_string(bridge::utcNowMilliseconds());
        const std::string_view keys[]{key};
        const std::string_view args[]{connectionId, reason, now};
        (void)co_await redis_.eval(script, keys, args);
    }

    ruvia::Task<void> finishCommand(std::string_view commandId, bool success,
                                    std::string_view reason) {
        if (commandId.empty())
            co_return;
        const auto child = broadcastParents_.find(commandId);
        if (child != broadcastParents_.end()) {
            const auto parentId = child->second;
            broadcastParents_.erase(child);
            const auto broadcast = broadcasts_.find(parentId);
            if (broadcast == broadcasts_.end())
                co_return;
            service::common::packet_log::Context childContext;
            childContext.workerIndex = workerIndex_;
            childContext.operation = "broadcast";
            childContext.messageId = commandId;
            childContext.causationId = parentId;
            service::common::packet_log::write(
                success ? service::common::packet_log::Level::Debug
                        : service::common::packet_log::Level::Warn,
                success ? "BROADCAST_TARGET_SUCCESS" : "BROADCAST_TARGET_FAILED", childContext,
                {}, reason);
            broadcast->second.anySuccess = broadcast->second.anySuccess || success;
            if (!reason.empty())
                broadcast->second.reason = reason;
            if (--broadcast->second.remaining != 0)
                co_return;
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
                aggregateSuccess ? "BROADCAST_COMPLETED" : "BROADCAST_FAILED", aggregateContext,
                {}, aggregateReason);
            co_await finishCommand(parentId, aggregateSuccess, aggregateReason);
            co_return;
        }
        const auto current = pendingCommands_.find(commandId);
        if (current == pendingCommands_.end())
            co_return;
        auto pending = std::move(current->second);
        pendingCommands_.erase(current);
        if (!success &&
            co_await retryOrDeadLetter(pending.stream, pending.entryId, pending.task, reason))
            co_return;
        service::common::packet_log::write(
            success ? service::common::packet_log::Level::Info
                    : service::common::packet_log::Level::Warn,
            pending.task.kind == "discovery"
                ? (success ? "DISCOVERY_RESULT" : "DISCOVERY_FAILED")
                : (success ? "COMMAND_RESULT" : "COMMAND_FAILED"),
            taskLogContext(pending.task), {}, reason);
        (void)co_await bridge::redis_async::publish(
            redis_, commandResultStream(),
            {{"message_id", bridge::nextMessageId()},
             {"causation_id", std::string(commandId)},
             {"command_id", std::string(commandId)},
             {"device_id", pending.task.deviceId},
             {"device_code", pending.task.deviceCode},
             {"protocol", pending.task.protocol},
             {"attempt", std::to_string(pending.task.attempt)},
             {"success", success ? "1" : "0"},
             {"reason", std::string(reason)},
             {"worker_id", std::to_string(workerIndex_)},
             {"created_at_ms", std::to_string(bridge::utcNowMilliseconds())},
             {"completed_at_ms", std::to_string(bridge::utcNowMilliseconds())}},
            10000);
        co_await bridge::redis_async::acknowledgeAndDelete(redis_, pending.stream, commandGroup(),
                                                           pending.entryId);
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
            "stale_session_epoch"};
        const auto contains = [reason](std::string_view marker) {
            return reason.find(marker) != std::string_view::npos;
        };
        if (std::any_of(permanentMarkers.begin(), permanentMarkers.end(), contains))
            return false;
        return contains("timeout") || contains("temporarily_unavailable") || contains("redis");
    }

    ruvia::Task<bool> retryOrDeadLetter(std::string_view stream, std::string_view entryId,
                                        bridge::ProtocolTask task, std::string_view reason) {
        if (retryableFailure(reason) && task.attempt < task.maxAttempts) {
            ++task.attempt;
            const auto retryReason = "attempt=" + std::to_string(task.attempt) + " reason=" +
                                     std::string(reason);
            service::common::packet_log::write(
                service::common::packet_log::Level::Warn,
                task.kind == "discovery" ? "DISCOVERY_RETRY" : "COMMAND_RETRY",
                taskLogContext(task), {}, retryReason);
            (void)co_await bridge::redis_async::publish(
                redis_, stream, bridge::protocolTaskFields(task), kCommandStreamCapacity);
            co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, commandGroup(),
                                                               entryId);
            co_return true;
        }
        auto fields = bridge::protocolTaskFields(task);
        fields.push_back({"failure_reason", std::string(reason)});
        fields.push_back({"source_entry_id", std::string(entryId)});
        fields.push_back({"worker_id", std::to_string(workerIndex_)});
        fields.push_back({"failed_at_ms", std::to_string(bridge::utcNowMilliseconds())});
        service::common::packet_log::write(
            service::common::packet_log::Level::Error, "DEAD_LETTER", taskLogContext(task), {},
            reason);
        (void)co_await bridge::redis_async::publish(redis_, deadLetterStream(), fields,
                                                    kDeadLetterCapacity);
        co_return false;
    }

    ruvia::Task<void> failUndeliverable(std::string_view stream, std::string_view entryId,
                                        std::string_view commandId,
                                        const bridge::ProtocolTask& task, std::string_view reason) {
        if (co_await retryOrDeadLetter(stream, entryId, task, reason))
            co_return;
        (void)co_await bridge::redis_async::publish(
            redis_, commandResultStream(),
            {{"message_id", bridge::nextMessageId()},
             {"causation_id", std::string(commandId)},
             {"command_id", std::string(commandId)},
             {"device_id", task.deviceId},
             {"device_code", task.deviceCode},
             {"protocol", task.protocol},
             {"attempt", std::to_string(task.attempt)},
             {"success", "0"},
             {"reason", std::string(reason)},
             {"worker_id", std::to_string(workerIndex_)},
             {"created_at_ms", std::to_string(bridge::utcNowMilliseconds())},
             {"completed_at_ms", std::to_string(bridge::utcNowMilliseconds())}},
            10000);
        co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, commandGroup(), entryId);
    }

    [[nodiscard]] std::vector<bridge::StreamField> linkStateFields(const WorkerLinkState& state) {
        std::string endpoints;
        for (const auto& endpoint : state.remoteEndpoints) {
            if (!endpoints.empty())
                endpoints.push_back(',');
            endpoints += endpoint;
        }
        std::vector<bridge::StreamField> fields{
            {"message_id", bridge::nextMessageId()},
            {"worker_instance_id", workerInstanceId_},
            {"link_id", state.linkId},
            {"worker_id", std::to_string(state.workerIndex)},
            {"state", state.state},
            {"state_reason", state.reason},
            {"error", state.error},
            {"connection_count", std::to_string(state.remoteEndpoints.size())},
            {"remote_endpoints", endpoints},
            {"last_activity_at_ms", std::to_string(state.lastActivityAtMs)},
            {"created_at_ms", std::to_string(bridge::utcNowMilliseconds())}};
        for (const auto& target : state.targets) {
            const auto prefix = "target:" + target.id + ':';
            fields.push_back({prefix + "state", target.state});
            fields.push_back({prefix + "reason", target.reason});
            fields.push_back({prefix + "error", target.error});
            fields.push_back(
                {prefix + "last_activity_at_ms", std::to_string(target.lastActivityAtMs)});
        }
        return fields;
    }

    ruvia::Task<void> publishLinkEvent(WorkerLinkState state) {
        (void)co_await bridge::redis_async::publish(
            redis_, linkEventStream(), linkStateFields(state), kLinkEventStreamCapacity);
    }

    ruvia::Task<bool> consumeLinkEvent() {
        const auto stream = linkEventStream();
        const auto group = linkEventGroup();
        const auto messages = co_await bridge::redis_async::readGroup(
            redis_, stream, group, consumer_, linkEventRecovering_ ? "0" : ">",
            std::chrono::milliseconds(0), 32);
        if (linkEventRecovering_ && messages.empty())
            linkEventRecovering_ = false;
        for (const auto& message : messages) {
            const auto linkId = message.get("link_id");
            const auto workerId = message.get("worker_id");
            const auto workerInstanceId = message.get("worker_instance_id");
            if (linkId.empty() || workerId != std::to_string(workerIndex_) ||
                workerInstanceId != workerInstanceId_) {
                co_await deadLetterMessage(message, "link_event_invalid");
                co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group,
                                                                   message.id);
                continue;
            }
            std::vector<bridge::StreamField> fields;
            fields.reserve(message.fields.size());
            for (const auto& field : message.fields) {
                if (field.name == "message_id" || field.name == "created_at_ms")
                    continue;
                fields.push_back(field);
            }
            fields.push_back({"updated_at_ms", std::to_string(bridge::utcNowMilliseconds())});
            const auto key =
                "iot:state:link:" + std::string(linkId) + ":worker:" + std::to_string(workerIndex_);
            co_await bridge::redis_async::eraseHash(redis_, key);
            co_await bridge::redis_async::setHash(redis_, key, fields);
            co_await bridge::redis_async::acknowledgeAndDelete(redis_, stream, group, message.id);
        }
        co_return !messages.empty();
    }

    ruvia::Task<void> deadLetterMessage(const bridge::StreamMessage& message,
                                        std::string_view reason, std::string_view detail = {}) {
        auto fields = message.fields;
        fields.push_back({"source_entry_id", message.id});
        fields.push_back({"failure_reason", std::string(reason)});
        fields.push_back({"failure_detail", std::string(detail)});
        fields.push_back({"worker_id", std::to_string(workerIndex_)});
        fields.push_back({"failed_at_ms", std::to_string(bridge::utcNowMilliseconds())});
        (void)co_await bridge::redis_async::publish(redis_, deadLetterStream(), fields,
                                                    kDeadLetterCapacity);
    }

    ruvia::Task<void> publishRuntimeConfigState() {
        const auto now = std::to_string(bridge::utcNowMilliseconds());
        co_await bridge::redis_async::setHash(
            redis_, "iot:state:runtime-config:worker:" + std::to_string(workerIndex_),
            {{"worker_id", std::to_string(workerIndex_)},
             {"version", loadedConfigVersion_},
             {"state", "applied"},
             {"applied_at_ms", now}});
    }

    static constexpr auto kTickInterval = std::chrono::milliseconds(10);
    static constexpr std::size_t kRawIngressCapacity = 100000;
    static constexpr auto kFailureDelay = std::chrono::milliseconds(250);
    static constexpr auto kConfigCheckInterval = std::chrono::milliseconds(250);
    static constexpr std::size_t kCommandStreamCapacity = 10000;
    static constexpr std::size_t kCommandConsumeBatch = 16;
    static constexpr std::size_t kEgressStreamCapacity = 10000;
    static constexpr std::size_t kLinkEventStreamCapacity = 1000;
    static constexpr std::size_t kDeadLetterCapacity = 1000;

    ruvia::EventLoop loop_;
    std::pmr::unsynchronized_pool_resource resource_;
    ruvia::TaskScope scope_;
    WorkerTimerScheduler scheduler_;
    WorkerRedisClient redis_;
    ProtocolEngine engine_;
    WorkerTcpRuntime tcp_;
    std::size_t workerIndex_ = 0;
    std::size_t workerCount_ = 1;
    std::string workerInstanceId_ = bridge::nextMessageId();
    std::string consumer_;
    std::map<std::pair<std::string, std::uint64_t>, WorkerTimerScheduler::Token> protocolDeadlines_;
    std::map<std::string, std::set<std::string>, std::less<>> routes_;
    std::map<std::string, ProtocolConnectionInfo, std::less<>> networkConnections_;
    std::map<std::string, std::uint64_t, std::less<>> connectionEpochs_;
    std::map<std::string, PendingCommand, std::less<>> pendingCommands_;
    std::map<std::string, BroadcastCommand, std::less<>> broadcasts_;
    std::map<std::string, std::string, std::less<>> broadcastParents_;
    WorkerTimerScheduler::Token tickToken_ = 0;
    std::string lastCoordinatorError_;
    std::string loadedConfigVersion_;
    RuntimeSnapshot loadedSnapshot_;
    std::chrono::steady_clock::time_point lastConfigCheck_{};
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
};

} // namespace service::southbridge
