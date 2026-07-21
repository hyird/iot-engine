#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "service/common/bridge/message.contract.h"
#include "service/modules/southbridge/network/tcp_link.manager.h"
#include "service/modules/southbridge/protocol/dtu_registry.h"
#include "service/modules/southbridge/protocol/protocol_dispatcher.h"
#include "service/modules/southbridge/protocol/session_manager.h"
#include "service/modules/southbridge/protocol/protocol_registry.h"
#include "service/modules/southbridge/protocol/raw_protocol_adapter.h"
#include "service/modules/southbridge/queue/protocol_task.queue.h"
#include "service/modules/southbridge/queue/redis_stream.h"
#include "service/modules/southbridge/runtime_config.repository.h"

namespace service::southbridge {

class SouthbridgeRuntime {
  public:
    explicit SouthbridgeRuntime(SouthbridgeConfig config)
        : config_(std::move(config)), producer_(config_.redis), taskQueue_(config_.redis),
          dispatcher_(dtus_, sessions_, protocols_, producer_),
          linkManager_(producer_, dispatcher_) {
        protocols_.registerAdapter(std::make_unique<RawProtocolAdapter>("Modbus"));
        protocols_.registerAdapter(std::make_unique<RawProtocolAdapter>("S7"));
        protocols_.registerAdapter(std::make_unique<RawProtocolAdapter>("SL651"));
    }

    SouthbridgeRuntime(const SouthbridgeRuntime&) = delete;
    SouthbridgeRuntime& operator=(const SouthbridgeRuntime&) = delete;

    ~SouthbridgeRuntime() { stop(); }

    void start() {
        if (running_.exchange(true))
            return;
        try {
            producer_.clearSessions();
            bridge::RedisStreamConnection ingress(config_.redis);
            ingress.connect();
            ingress.ensureGroup(bridge::kIngressStream, kIngressGroup);
            ingress.ensureGroup(bridge::kProtocolHighTaskStream, kProtocolTaskGroup);
            ingress.ensureGroup(bridge::kProtocolNormalTaskStream, kProtocolTaskGroup);
            bridge::RedisStreamConnection configEvents(config_.redis);
            configEvents.connect();
            configEvents.ensureGroup(bridge::kConfigStream, kConfigGroup);

            std::promise<RuntimeCounts> configStarted;
            auto configReady = configStarted.get_future();
            configThread_ = std::thread([this, started = std::move(configStarted)]() mutable {
                consumeConfig(std::move(started));
            });
            const auto counts = configReady.get();
            protocolThread_ = std::thread([this] { consumeProtocol(); });
            std::cout << "southbridge runtime started: links=" << counts.links
                      << " devices=" << counts.devices
                      << " link_workers=1 protocol_workers=1 config_workers=1\n";
        } catch (...) {
            running_.store(false);
            linkManager_.stop();
            if (configThread_.joinable())
                configThread_.join();
            throw;
        }
    }

    void stop() {
        if (!running_.exchange(false))
            return;
        linkManager_.stop();
        if (protocolThread_.joinable())
            protocolThread_.join();
        if (configThread_.joinable())
            configThread_.join();
        std::cout << "southbridge runtime stopped\n";
    }

  private:
    struct RuntimeCounts {
        std::size_t links = 0;
        std::size_t devices = 0;
    };

    struct InflightTask {
        std::string stream;
        std::string entryId;
        std::string messageId;
        std::string connectionId;
        std::string protocol;
        std::int64_t deadlineMs = 0;
        bool modbusTcp = false;
        std::uint16_t transactionId = 0;
        std::uint8_t unitId = 0;
        std::uint8_t functionCode = 0;
    };

    enum class TaskExecution { Completed, Inflight, Deferred, WaitingInflight };

    void consumeProtocol() {
        bridge::RedisStreamConnection redis(config_.redis);
        bool recoverIngress = true;
        bool recoverHigh = true;
        bool recoverNormal = true;
        while (running_.load()) {
            try {
                redis.ensureGroup(bridge::kIngressStream, kIngressGroup);
                redis.ensureGroup(bridge::kProtocolHighTaskStream, kProtocolTaskGroup);
                redis.ensureGroup(bridge::kProtocolNormalTaskStream, kProtocolTaskGroup);
                processInflightTimeouts(redis, recoverHigh, recoverNormal);
                const auto handledHigh =
                    consumeTaskBatch(redis, bridge::kProtocolHighTaskStream, recoverHigh,
                                     std::chrono::milliseconds(0));
                const auto handledIngress =
                    consumeIngressBatch(redis, recoverIngress, recoverHigh, recoverNormal,
                                        std::chrono::milliseconds(0));
                if (handledHigh || handledIngress)
                    continue;
                if (consumeTaskBatch(redis, bridge::kProtocolNormalTaskStream, recoverNormal,
                                     std::chrono::milliseconds(100)))
                    continue;
                (void)consumeIngressBatch(redis, recoverIngress, recoverHigh, recoverNormal,
                                          std::chrono::milliseconds(100));
            } catch (const std::exception& error) {
                if (running_.load())
                    std::cerr << "southbridge protocol worker failed: " << error.what() << '\n';
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }

    bool consumeIngressBatch(bridge::RedisStreamConnection& redis, bool& recoverPending,
                             bool& recoverHigh, bool& recoverNormal,
                             std::chrono::milliseconds block) {
        const auto messages = redis.readGroup(bridge::kIngressStream, kIngressGroup, kConsumer,
                                              recoverPending ? "0" : ">", block, 32);
        if (recoverPending && messages.empty()) {
            recoverPending = false;
            return false;
        }
        for (const auto& message : messages) {
            try {
                for (const auto& resolved : dispatcher_.process(bridge::ingressFrom(message)))
                    completeMatchingInflight(redis, resolved, recoverHigh, recoverNormal);
                redis.acknowledgeAndDelete(bridge::kIngressStream, kIngressGroup, message.id);
                clearRetry(redis, kIngressGroup, message.id);
            } catch (const std::exception& error) {
                if (!handleFailure(redis, bridge::kIngressStream, kIngressGroup, message,
                                   "ingress_consumer_error", error.what())) {
                    recoverPending = true;
                    std::this_thread::sleep_for(kRetryDelay);
                    break;
                }
            }
        }
        return !messages.empty();
    }

    bool consumeTaskBatch(bridge::RedisStreamConnection& redis, std::string_view stream,
                          bool& recoverPending, std::chrono::milliseconds block) {
        const auto recovering = recoverPending;
        const auto messages = redis.readGroup(stream, kProtocolTaskGroup, kConsumer,
                                              recovering ? "0" : ">", block, recovering ? 100 : 1);
        if (recoverPending && messages.empty()) {
            recoverPending = false;
            return false;
        }
        bool handled = false;
        for (const auto& message : messages) {
            try {
                const auto task = bridge::protocolTaskFrom(message);
                const auto execution = executeProtocolTask(redis, stream, message.id, task);
                if (execution == TaskExecution::Completed) {
                    redis.acknowledgeGroupedAndDelete(stream, kProtocolTaskGroup, message.id,
                                                      bridge::protocolTaskDepthKey(task.groupKey));
                    clearRetry(redis, kProtocolTaskGroup, message.id);
                    handled = true;
                } else if (execution == TaskExecution::Inflight) {
                    // Check ingress after starting a query so a continuous High queue cannot
                    // delay the response that completes this in-flight task.
                    handled = false;
                }
            } catch (const std::exception& error) {
                // Registration and responses use the same worker. Let ingress run before a
                // failed task is retried, otherwise the task can block its own connection setup.
                handled = false;
                if (!handleProtocolTaskFailure(redis, stream, message, error.what())) {
                    recoverPending = true;
                    std::this_thread::sleep_for(kRetryDelay);
                    break;
                }
            }
        }
        if (recovering)
            recoverPending = false;
        return handled;
    }

    TaskExecution executeProtocolTask(bridge::RedisStreamConnection& redis, std::string_view stream,
                                      std::string_view entryId, const bridge::ProtocolTask& task) {
        const auto discovery = task.kind == "discovery_broadcast";
        if (task.kind != "wire_send" && !discovery)
            throw std::runtime_error("Unsupported protocol task kind: " + task.kind);
        std::vector<std::string> connectionIds;
        if (discovery) {
            const auto expectedGroup = discoveryGroupKey(task.linkId);
            if (task.groupKey != expectedGroup)
                throw std::runtime_error("Protocol discovery group is invalid");
            for (const auto& session : sessions_.unboundByLink(task.linkId))
                connectionIds.push_back(session.connectionId);
            if (connectionIds.empty())
                return TaskExecution::Completed;
        } else {
            const auto dtu = dtus_.find(task.groupKey);
            if (!dtu || dtu->protocol != task.protocol)
                throw std::runtime_error("Protocol task DTU is unavailable");
            auto connectionId = task.connectionId;
            if (connectionId.empty()) {
                const auto session = sessions_.findByDtuKey(task.groupKey);
                if (session)
                    connectionId = session->connectionId;
            }
            if (connectionId.empty())
                throw std::runtime_error("Protocol task connection is unavailable");
            connectionIds.push_back(std::move(connectionId));
        }
        auto bytes = bridge::fromHex(task.payload);
        if (bytes.empty())
            throw std::runtime_error("Protocol task wire payload is invalid");
        std::optional<std::string> inflightToken;
        if (task.expectsResponse) {
            if (task.protocol != "Modbus")
                throw std::runtime_error(
                    "Query-response matching is currently available for Modbus only");
            const auto inflightKey = bridge::protocolInflightKey(task.groupKey);
            const auto existing = redis.hashEntries(inflightKey);
            if (!existing.empty()) {
                const auto inflight = parseInflight(existing);
                return inflight.entryId == entryId ? TaskExecution::WaitingInflight
                                                   : TaskExecution::Deferred;
            }
            const auto inflight = makeInflight(stream, entryId, task,
                                               discovery ? std::string_view("ANY_UNREGISTERED")
                                                         : std::string_view(connectionIds.front()),
                                               bytes);
            inflightToken = task.messageId;
            if (!redis.claimHash(
                    inflightKey, inflightFields(inflight, *inflightToken),
                    std::chrono::milliseconds(
                        std::clamp<std::int64_t>(task.responseTimeoutMs, 100, 60000) + 60000)))
                return TaskExecution::Deferred;
        }
        bool sentAny = false;
        for (const auto& connectionId : connectionIds) {
            auto sent = linkManager_.send(connectionId, bytes);
            if (sent.wait_for(kWriteTimeout) == std::future_status::ready && sent.get())
                sentAny = true;
        }
        if (!sentAny) {
            if (inflightToken)
                (void)redis.eraseHashIfFieldValue(bridge::protocolInflightKey(task.groupKey),
                                                  "token", *inflightToken);
            throw std::runtime_error("Protocol task wire send failed");
        }
        return task.expectsResponse ? TaskExecution::Inflight : TaskExecution::Completed;
    }

    static std::string discoveryGroupKey(std::string_view linkId) {
        return "link:" + std::string(linkId) + ":discovery";
    }

    static std::string depthKeyForInflight(std::string_view inflightKey) {
        if (!inflightKey.starts_with(bridge::kProtocolInflightPrefix))
            throw std::runtime_error("Invalid protocol in-flight key");
        return bridge::protocolTaskDepthKey(
            inflightKey.substr(bridge::kProtocolInflightPrefix.size()));
    }

    static InflightTask makeInflight(std::string_view stream, std::string_view entryId,
                                     const bridge::ProtocolTask& task,
                                     std::string_view connectionId,
                                     const std::vector<std::uint8_t>& bytes) {
        InflightTask inflight;
        inflight.stream = std::string(stream);
        inflight.entryId = std::string(entryId);
        inflight.messageId = task.messageId;
        inflight.connectionId = std::string(connectionId);
        inflight.protocol = task.protocol;
        inflight.deadlineMs = bridge::utcNowMilliseconds() +
                              std::clamp<std::int64_t>(task.responseTimeoutMs, 100, 60000);
        if (task.transport == "TCP") {
            if (bytes.size() < 8)
                throw std::runtime_error("Modbus TCP request is too short");
            inflight.modbusTcp = true;
            inflight.transactionId =
                static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
            inflight.unitId = bytes[6];
            inflight.functionCode = static_cast<std::uint8_t>(bytes[7] & 0x7FU);
        } else if (task.transport == "RTU") {
            if (bytes.size() < 2)
                throw std::runtime_error("Modbus RTU request is too short");
            inflight.unitId = bytes[0];
            inflight.functionCode = static_cast<std::uint8_t>(bytes[1] & 0x7FU);
        } else
            throw std::runtime_error("Modbus query transport must be TCP or RTU");
        if (inflight.functionCode == 0)
            throw std::runtime_error("Modbus request function code is invalid");
        return inflight;
    }

    static std::vector<bridge::StreamField> inflightFields(const InflightTask& inflight,
                                                           std::string_view token) {
        return {{"token", std::string(token)},
                {"stream", inflight.stream},
                {"entry_id", inflight.entryId},
                {"message_id", inflight.messageId},
                {"connection_id", inflight.connectionId},
                {"protocol", inflight.protocol},
                {"deadline_at_ms", std::to_string(inflight.deadlineMs)},
                {"transport", inflight.modbusTcp ? "TCP" : "RTU"},
                {"transaction_id", std::to_string(inflight.transactionId)},
                {"unit_id", std::to_string(inflight.unitId)},
                {"function_code", std::to_string(inflight.functionCode)}};
    }

    static InflightTask parseInflight(const std::vector<bridge::StreamField>& entries) {
        std::map<std::string_view, std::string_view> fields;
        for (const auto& entry : entries)
            fields.emplace(entry.name, entry.value);
        const auto required = [&fields](std::string_view name) {
            const auto field = fields.find(name);
            if (field == fields.end() || field->second.empty())
                throw std::runtime_error("Missing in-flight task field: " + std::string(name));
            return field->second;
        };
        const auto integer = [&required](std::string_view name, auto& result) {
            const auto field = required(name);
            const auto [end, error] =
                std::from_chars(field.data(), field.data() + field.size(), result);
            if (error != std::errc{} || end != field.data() + field.size())
                throw std::runtime_error("Invalid in-flight task integer");
        };
        InflightTask inflight;
        inflight.stream = required("stream");
        inflight.entryId = required("entry_id");
        inflight.messageId = required("message_id");
        inflight.connectionId = required("connection_id");
        inflight.protocol = required("protocol");
        integer("deadline_at_ms", inflight.deadlineMs);
        const auto transport = required("transport");
        if (transport != "TCP" && transport != "RTU")
            throw std::runtime_error("Invalid in-flight task transport");
        inflight.modbusTcp = transport == "TCP";
        integer("transaction_id", inflight.transactionId);
        integer("unit_id", inflight.unitId);
        integer("function_code", inflight.functionCode);
        return inflight;
    }

    static bool matchesInflight(const InflightTask& inflight,
                                const ResolvedIngress& resolved) noexcept {
        const auto& bytes = resolved.packet.payload;
        if (inflight.protocol != "Modbus" ||
            (inflight.connectionId != "ANY_UNREGISTERED" &&
             resolved.packet.connectionId != inflight.connectionId))
            return false;
        if (inflight.modbusTcp) {
            if (bytes.size() < 8 || bytes[2] != 0 || bytes[3] != 0)
                return false;
            const auto transactionId =
                static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
            return transactionId == inflight.transactionId && bytes[6] == inflight.unitId &&
                   (bytes[7] & 0x7FU) == inflight.functionCode;
        }
        return bytes.size() >= 2 && bytes[0] == inflight.unitId &&
               (bytes[1] & 0x7FU) == inflight.functionCode;
    }

    void completeMatchingInflight(bridge::RedisStreamConnection& redis,
                                  const ResolvedIngress& resolved, bool& recoverHigh,
                                  bool& recoverNormal) {
        const std::array groupKeys{resolved.dtu.key, discoveryGroupKey(resolved.packet.linkId)};
        for (const auto& groupKey : groupKeys) {
            const auto inflightKey = bridge::protocolInflightKey(groupKey);
            const auto fields = redis.hashEntries(inflightKey);
            if (fields.empty())
                continue;
            const auto inflight = parseInflight(fields);
            if (!matchesInflight(inflight, resolved))
                continue;
            if (redis.completeInflightTask(inflight.stream, kProtocolTaskGroup, inflight.entryId,
                                           bridge::protocolTaskDepthKey(groupKey), inflightKey,
                                           inflight.messageId)) {
                clearRetry(redis, kProtocolTaskGroup, inflight.entryId);
                if (inflight.stream == bridge::kProtocolHighTaskStream)
                    recoverHigh = true;
                else if (inflight.stream == bridge::kProtocolNormalTaskStream)
                    recoverNormal = true;
            }
            return;
        }
    }

    void processInflightTimeouts(bridge::RedisStreamConnection& redis, bool& recoverHigh,
                                 bool& recoverNormal) {
        const auto now = bridge::utcNowMilliseconds();
        if (now - lastInflightSweepAtMs_ < kInflightSweepInterval.count())
            return;
        lastInflightSweepAtMs_ = now;
        for (const auto& inflightKey :
             redis.keysMatching(std::string(bridge::kProtocolInflightPrefix) + '*')) {
            InflightTask inflight;
            try {
                const auto fields = redis.hashEntries(inflightKey);
                if (fields.empty())
                    continue;
                inflight = parseInflight(fields);
            } catch (const std::exception& error) {
                redis.eraseHash(inflightKey);
                recoverHigh = true;
                recoverNormal = true;
                std::cerr << "southbridge removed invalid in-flight task: key=" << inflightKey
                          << " error=" << error.what() << '\n';
                continue;
            }
            if (inflight.deadlineMs > now)
                continue;
            const auto attempt = redis.incrementWithExpiry(
                retryKey(kProtocolTaskGroup, inflight.entryId), kRetryTtl);
            if (attempt < kMaxDeliveryAttempts) {
                if (redis.eraseHashIfFieldValue(inflightKey, "token", inflight.messageId)) {
                    if (inflight.stream == bridge::kProtocolHighTaskStream)
                        recoverHigh = true;
                    else if (inflight.stream == bridge::kProtocolNormalTaskStream)
                        recoverNormal = true;
                    std::cerr << "southbridge protocol response timeout retry: stream="
                              << inflight.stream << " id=" << inflight.entryId
                              << " attempt=" << attempt << '\n';
                } else {
                    clearRetry(redis, kProtocolTaskGroup, inflight.entryId);
                }
                continue;
            }
            if (!redis.completeInflightTask(inflight.stream, kProtocolTaskGroup, inflight.entryId,
                                            depthKeyForInflight(inflightKey), inflightKey,
                                            inflight.messageId)) {
                clearRetry(redis, kProtocolTaskGroup, inflight.entryId);
                continue;
            }
            producer_.publish(
                bridge::kDeadLetterStream,
                {{"message_id", bridge::nextMessageId()},
                 {"causation_id", inflight.messageId},
                 {"source_stream", inflight.stream},
                 {"source_group", std::string(kProtocolTaskGroup)},
                 {"source_entry_id", inflight.entryId},
                 {"group_key", inflightKey.substr(bridge::kProtocolInflightPrefix.size())},
                 {"connection_id", inflight.connectionId},
                 {"transport", inflight.modbusTcp ? "TCP" : "RTU"},
                 {"transaction_id", std::to_string(inflight.transactionId)},
                 {"unit_id", std::to_string(inflight.unitId)},
                 {"function_code", std::to_string(inflight.functionCode)},
                 {"delivery_attempts", std::to_string(attempt)},
                 {"reason", "protocol_response_timeout"},
                 {"error", "Timed out waiting for protocol response"},
                 {"occurred_at_ms", std::to_string(now)}});
            clearRetry(redis, kProtocolTaskGroup, inflight.entryId);
        }
    }

    bool handleProtocolTaskFailure(bridge::RedisStreamConnection& redis, std::string_view stream,
                                   const bridge::StreamMessage& message, std::string_view error) {
        const auto attempt =
            redis.incrementWithExpiry(retryKey(kProtocolTaskGroup, message.id), kRetryTtl);
        if (attempt < kMaxDeliveryAttempts) {
            std::cerr << "southbridge protocol task retry: stream=" << stream
                      << " id=" << message.id << " attempt=" << attempt << " error=" << error
                      << '\n';
            return false;
        }
        producer_.publish(bridge::kDeadLetterStream,
                          {{"message_id", bridge::nextMessageId()},
                           {"causation_id", std::string(message.get("message_id"))},
                           {"source_stream", std::string(stream)},
                           {"source_group", std::string(kProtocolTaskGroup)},
                           {"source_entry_id", message.id},
                           {"group_key", std::string(message.get("group_key"))},
                           {"protocol", std::string(message.get("protocol"))},
                           {"link_id", std::string(message.get("link_id"))},
                           {"device_id", std::string(message.get("device_id"))},
                           {"delivery_attempts", std::to_string(attempt)},
                           {"reason", "protocol_task_error"},
                           {"error", std::string(error)},
                           {"occurred_at_ms", std::to_string(bridge::utcNowMilliseconds())}});
        const auto groupKey = message.get("group_key");
        if (groupKey.empty())
            redis.acknowledgeAndDelete(stream, kProtocolTaskGroup, message.id);
        else
            redis.acknowledgeGroupedAndDelete(stream, kProtocolTaskGroup, message.id,
                                              bridge::protocolTaskDepthKey(groupKey));
        clearRetry(redis, kProtocolTaskGroup, message.id);
        return true;
    }

    void consumeConfig(std::promise<RuntimeCounts> started) {
        RuntimeConfigRepository repository(config_.postgres);
        bridge::RedisStreamConnection redis(config_.redis);
        try {
            redis.ensureGroup(bridge::kConfigStream, kConfigGroup);
            auto snapshot = repository.load();
            const RuntimeCounts counts{snapshot.links.size(), snapshot.devices.size()};
            dtus_.reload(snapshot);
            linkManager_.start(snapshot);
            started.set_value(counts);
        } catch (...) {
            started.set_exception(std::current_exception());
            return;
        }
        bool recoverPending = true;
        while (running_.load()) {
            try {
                redis.ensureGroup(bridge::kConfigStream, kConfigGroup);
                const auto messages =
                    redis.readGroup(bridge::kConfigStream, kConfigGroup, kConsumer,
                                    recoverPending ? "0" : ">", std::chrono::milliseconds(1000));
                if (recoverPending && messages.empty()) {
                    recoverPending = false;
                    continue;
                }
                if (messages.empty())
                    continue;
                try {
                    auto snapshot = repository.load();
                    dtus_.reload(snapshot);
                    linkManager_.reload(std::move(snapshot));
                    for (const auto& message : messages) {
                        redis.acknowledgeAndDelete(bridge::kConfigStream, kConfigGroup, message.id);
                        clearRetry(redis, kConfigGroup, message.id);
                    }
                } catch (const std::exception& error) {
                    recoverPending = false;
                    for (const auto& message : messages) {
                        if (!handleFailure(redis, bridge::kConfigStream, kConfigGroup, message,
                                           "config_consumer_error", error.what()))
                            recoverPending = true;
                    }
                    if (recoverPending)
                        std::this_thread::sleep_for(kRetryDelay);
                }
            } catch (const std::exception& error) {
                if (running_.load())
                    std::cerr << "southbridge config consumer failed: " << error.what() << '\n';
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }

    bool handleFailure(bridge::RedisStreamConnection& redis, std::string_view stream,
                       std::string_view group, const bridge::StreamMessage& message,
                       std::string_view reason, std::string_view error) {
        const auto attempt = redis.incrementWithExpiry(retryKey(group, message.id), kRetryTtl);
        if (attempt < kMaxDeliveryAttempts) {
            std::cerr << "southbridge message retry: stream=" << stream << " id=" << message.id
                      << " attempt=" << attempt << " error=" << error << '\n';
            return false;
        }
        producer_.publish(bridge::kDeadLetterStream,
                          {{"message_id", bridge::nextMessageId()},
                           {"causation_id", std::string(message.get("message_id"))},
                           {"source_stream", std::string(stream)},
                           {"source_group", std::string(group)},
                           {"source_entry_id", message.id},
                           {"delivery_attempts", std::to_string(attempt)},
                           {"reason", std::string(reason)},
                           {"error", std::string(error)},
                           {"occurred_at_ms", std::to_string(bridge::utcNowMilliseconds())}});
        redis.acknowledgeAndDelete(stream, group, message.id);
        clearRetry(redis, group, message.id);
        return true;
    }

    static std::string retryKey(std::string_view group, std::string_view id) {
        return "iot:retry:" + std::string(group) + ':' + std::string(id);
    }

    static void clearRetry(bridge::RedisStreamConnection& redis, std::string_view group,
                           std::string_view id) {
        redis.erase(retryKey(group, id));
    }

    static constexpr std::string_view kIngressGroup = "iot-engine:packet";
    static constexpr std::string_view kProtocolTaskGroup = "iot-engine:protocol-task";
    static constexpr std::string_view kConfigGroup = "iot-engine:config";
    static constexpr std::string_view kConsumer = "worker-1";
    static constexpr std::int64_t kMaxDeliveryAttempts = 3;
    static constexpr auto kRetryDelay = std::chrono::milliseconds(250);
    static constexpr auto kRetryTtl = std::chrono::hours(24);
    static constexpr auto kWriteTimeout = std::chrono::seconds(2);
    static constexpr auto kInflightSweepInterval = std::chrono::milliseconds(100);

    SouthbridgeConfig config_;
    bridge::RedisStreamProducer producer_;
    RedisProtocolTaskQueue taskQueue_;
    DtuRegistry dtus_;
    SessionRegistry sessions_;
    ProtocolRegistry protocols_;
    ProtocolDispatcher dispatcher_;
    TcpLinkManager linkManager_;
    std::thread protocolThread_;
    std::thread configThread_;
    std::int64_t lastInflightSweepAtMs_ = 0;
    std::atomic_bool running_{false};
};

} // namespace service::southbridge
