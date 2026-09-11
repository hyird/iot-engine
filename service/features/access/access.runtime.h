#pragma once

#include <algorithm>
#include <array>
#include <asio.hpp>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <ruvia/core/OneShot.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/features/access/access.service.h"
#include "service/features/access/access.transport.h"
#include "service/features/messaging/messaging.service.h"
#include "service/features/messaging/messaging.transport.h"
#include "service/features/messaging/stream_multiplexer/stream_multiplexer.runtime.h"
#include "service/utils/crypto.h"
#include "service/utils/json.h"
#include "service/utils/text.h"

namespace service::access {

class AccessOperationHandler final {
  public:
    static ruvia::Task<std::string> handle(ruvia::WebWorkerContext& context, std::string_view operation, std::string_view payload, ruvia::StopToken stop) {
        if (stop.stopRequested()) {
            service::common::fail(10004, "Access projection cancelled", 503);
        }
        if (operation == "refresh") {
            co_await session::refresh(context);
            co_return "{}";
        }
        if (operation == "audit") {
            const auto request = ruvia::JsonValue::parse(payload);
            if (!request || !request->isObject()) {
                service::common::fail(10002, "Invalid access audit payload", 400);
            }
            const auto text = [&request](std::string_view field) {
                const auto value = request->get<ruvia::String>(field);
                return value ? std::string(value->view()) : std::string{};
            };
            const auto json = [&request](std::string_view field) {
                const auto value = service::utils::jsonField(*request, field);
                return value ? std::string(value->view()) : std::string("{}");
            };
            std::int64_t status = 200;
            if (const auto value = request->get<ruvia::Int64>("httpStatus")) {
                status = static_cast<std::int64_t>(*value);
            }
            const auto action = text("action");
            const auto accessKeyId = text("accessKeyId");
            const auto method = text("method");
            const auto target = text("target");
            const auto requestIp = text("requestIp");
            const auto deviceId = text("deviceId");
            const auto requestPayload = json("requestPayload");
            const auto responsePayload = json("responsePayload");
            co_await audit::publish(context.redis(), action, accessKeyId, method, target, requestIp, status, deviceId, requestPayload, responsePayload);
            co_return "{}";
        }
        service::common::fail(10002, "Unknown access operation", 400);
    }
};

class WebhookRuntime final {
  public:
    WebhookRuntime() = default;
    WebhookRuntime(const WebhookRuntime&) = delete;
    WebhookRuntime& operator=(const WebhookRuntime&) = delete;

    ~WebhookRuntime() { stop(); }

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
            throw std::runtime_error("access webhook runtime requires valid Service Workers");
        }
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        auto readiness = ready->get_future();
        stopped_ = stopped->get_future().share();
        const auto posted = worker_.post(
            [this, ready, stopped](ruvia::WebWorkerContext& context) {
                return run(context, workerIndex_, ready, stopped);
            }
        );
        if (!posted.accepted()) {
            stopped->set_value();
            running_.store(false);
            worker_ = {};
            stopped_ = {};
            throw std::runtime_error("service worker rejected access webhook runtime");
        }
        try {
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
            const auto wakeAccepted = worker.post([](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
                                                service::message::workerStreamMultiplexer().signal(
                                                    service::message::WorkerStreamTask::Webhook
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
        worker_ = {};
    }

  private:
    static constexpr std::string_view kGroup = "iot-engine:open-webhook";
    static constexpr std::size_t kBatchSize = 100;
    static constexpr std::size_t kEventConcurrency = 4;
    static constexpr std::size_t kTargetConcurrency = 4;
    static constexpr std::int64_t kDeliveryProgressTtlSeconds = 7 * 24 * 60 * 60;

    using Target = webhook::Target;
    using Catalog = webhook::Catalog;
    using DeviceCatalog = webhook::DeviceCatalog;

    struct Delivery final {
        std::string id;
        std::string eventType;
        std::string deviceId;
        std::string deviceCode;
        std::string occurredAt;
        std::string body;
    };

    struct LatestPoint final {
        std::int64_t sort{ 0 };
        std::int64_t observedAt{ 0 };
        std::string id;
        std::string name;
        std::string value{ "null" };
        std::string unit;
        std::string dataType;
        std::string encode;
    };

    enum class StreamKind {
        Catalog,
        Session,
        DeliveryResult,
        Audit,
        Event
    };

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index, std::shared_ptr<std::promise<void>> ready, std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            std::map<std::string, StreamKind, std::less<>> streamKinds;
            streamKinds.emplace(
                service::message::webhookCatalogChangesStream(index),
                StreamKind::Catalog
            );
            streamKinds.emplace(stream::sessionChanges(), StreamKind::Session);
            streamKinds.emplace(stream::deliveryResult(), StreamKind::DeliveryResult);
            streamKinds.emplace(stream::audit(), StreamKind::Audit);
            streamKinds.emplace(stream::event(), StreamKind::Event);
            std::vector<std::string> streams;
            streams.reserve(streamKinds.size());
            for (const auto& [streamName, kind] : streamKinds) {
                (void)kind;
                co_await message::redis::ensureGroup(redis, streamName, kGroup);
                streams.push_back(streamName);
            }
            WebhookHttpClient http;
            co_await session::ensure(context);
            auto catalog = co_await webhook::loadCatalog(context);
            ready->set_value();
            bool recovering = true;
            const auto consumer = service::runtime::instanceId() + ":service-" + std::to_string(index);
            const auto catalogReceiptConsumer =
                std::string(kGroup) + ":catalog:" + std::to_string(index);
            const auto sessionReceiptConsumer = std::string(kGroup) + ":session";
            static constexpr auto kPendingRecoveryInterval =
                std::chrono::milliseconds(5000);
            auto nextRecoverySweep = std::chrono::steady_clock::now();
            while (running_.load() && !context.stopToken().stopRequested()) {
                std::vector<message::redis::StreamBatch> batches;
                bool readFailed = false;
                bool groupMissing = false;
                try {
                    if (recovering ||
                        std::chrono::steady_clock::now() >= nextRecoverySweep) {
                        batches = co_await message::redis::claimGroupMany(
                            redis,
                            streams,
                            kGroup,
                            consumer,
                            kBatchSize
                        );
                        nextRecoverySweep = std::chrono::steady_clock::now() +
                            kPendingRecoveryInterval;
                    } else {
                        batches = co_await message::redis::readGroupMany(
                            redis,
                            streams,
                            kGroup,
                            consumer,
                            ">",
                            kBatchSize
                        );
                    }
                } catch (const std::exception& error) {
                    if (!running_.load() || context.stopToken().stopRequested()) {
                        break;
                    }
                    std::cerr << "open webhook stream read failed: " << error.what() << '\n';
                    recovering = true;
                    readFailed = true;
                    groupMissing = std::string_view(error.what()).find("NOGROUP") !=
                        std::string_view::npos;
                }
                if (readFailed && running_.load() &&
                    !context.stopToken().stopRequested()) {
                    try {
                        if (groupMissing) {
                            for (const auto& streamName : streams) {
                                co_await message::redis::ensureGroup(
                                    redis,
                                    streamName,
                                    kGroup
                                );
                            }
                            readFailed = false;
                        }
                    } catch (const std::exception& ensureError) {
                        std::cerr << "open webhook stream group recovery failed: "
                                  << ensureError.what() << '\n';
                    }
                }
                if (!running_.load() || context.stopToken().stopRequested()) {
                    break;
                }
                if (readFailed) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::milliseconds(250));
                    continue;
                }
                if (recovering && batches.empty()) {
                    recovering = false;
                }
                if (batches.empty()) {
                    co_await service::message::workerStreamMultiplexer().wait(
                        index,
                        service::message::WorkerStreamTask::Webhook,
                        context.stopToken(),
                        kPendingRecoveryInterval
                    );
                    continue;
                }
                bool failed = false;
                try {
                    bool refreshSessions = false;
                    std::vector<message::StreamMessage> sessionMessages;
                    for (const auto& batch : batches) {
                        const auto kind = streamKinds.at(batch.stream);
                        if (kind != StreamKind::Session) {
                            continue;
                        }
                        sessionMessages.insert(sessionMessages.end(), batch.messages.begin(), batch.messages.end());
                    }
                    const auto sessionEventIds =
                        service::message::idempotency::eventIds(sessionMessages);
                    const auto pendingSessionIds =
                        co_await service::message::idempotency::pending(
                            context,
                            sessionReceiptConsumer,
                            sessionEventIds
                        );
                    for (const auto& message : sessionMessages) {
                        refreshSessions =
                            refreshSessions ||
                            (service::message::idempotency::shouldProcess(
                                 message,
                                 pendingSessionIds
                             ) &&
                             sessionChange(message));
                    }
                    if (refreshSessions) {
                        co_await session::refresh(context);
                    }
                    co_await service::message::idempotency::markProcessed(
                        context,
                        sessionReceiptConsumer,
                        sessionEventIds
                    );

                    bool reloadCatalog = true; // Each instance consumes shared data with current authorization.
                    std::vector<message::StreamMessage> catalogMessages;
                    for (const auto& batch : batches) {
                        const auto kind = streamKinds.at(batch.stream);
                        if (kind != StreamKind::Catalog) {
                            continue;
                        }
                        catalogMessages.insert(catalogMessages.end(), batch.messages.begin(), batch.messages.end());
                    }
                    const auto catalogEventIds =
                        service::message::idempotency::eventIds(catalogMessages);
                    const auto pendingCatalogIds =
                        co_await service::message::idempotency::pending(
                            context,
                            catalogReceiptConsumer,
                            catalogEventIds
                        );
                    for (const auto& message : catalogMessages) {
                        reloadCatalog = reloadCatalog ||
                            (service::message::idempotency::shouldProcess(
                                 message,
                                 pendingCatalogIds
                             ) &&
                             catalogChange(message));
                    }
                    if (reloadCatalog) {
                        catalog = co_await webhook::loadCatalog(context);
                    }
                    co_await service::message::idempotency::markProcessed(
                        context,
                        catalogReceiptConsumer,
                        catalogEventIds
                    );
                    for (const auto& batch : batches) {
                        const auto kind = streamKinds.at(batch.stream);
                        if (kind != StreamKind::Catalog &&
                            kind != StreamKind::Session) {
                            continue;
                        }
                        co_await message::redis::acknowledgeAndDeleteMany(
                            redis,
                            batch.stream,
                            kGroup,
                            batch.messages
                        );
                    }
                    for (const auto& batch : batches) {
                        if (streamKinds.at(batch.stream) != StreamKind::DeliveryResult) {
                            continue;
                        }
                        co_await webhook::persistResults(context, batch.messages);
                        co_await message::redis::acknowledgeAndDeleteMany(
                            redis,
                            batch.stream,
                            kGroup,
                            batch.messages
                        );
                    }
                    for (const auto& batch : batches) {
                        if (streamKinds.at(batch.stream) != StreamKind::Audit) {
                            continue;
                        }
                        co_await webhook::persistAudits(context, batch.messages);
                        co_await message::redis::acknowledgeAndDeleteMany(
                            redis,
                            batch.stream,
                            kGroup,
                            batch.messages
                        );
                    }
                    for (const auto& batch : batches) {
                        if (streamKinds.at(batch.stream) != StreamKind::Event) {
                            continue;
                        }
                        co_await deliverEvents(context, batch.stream, batch.messages, catalog, http);
                    }
                } catch (const std::exception& error) {
                    std::cerr << "open webhook dispatch failed: " << error.what() << '\n';
                    recovering = true;
                    failed = true;
                }
                if (failed) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::milliseconds(250));
                }
            }
        } catch (...) {
            try {
                ready->set_exception(std::current_exception());
            } catch (...) {
            }
        }
        try {
            stopped->set_value();
        } catch (...) {
        }
    }

    static bool catalogChange(const message::StreamMessage& message) {
        const auto aggregate = message.get("aggregate");
        return aggregate == "access_key" || aggregate == "webhook" ||
            aggregate == "device" || aggregate == "protocol";
    }

    static bool sessionChange(const message::StreamMessage& message) {
        const auto aggregate = message.get("aggregate");
        return aggregate == "access_key" || aggregate == "device";
    }

    struct DeliveryAttempt final {
        bool succeeded{ false };
        std::string error;
    };

    struct DeviceEventQueue final {
        std::vector<std::size_t> indexes;
        std::size_t next{ 0 };
        bool blocked{ false };
    };

    ruvia::Task<void> deliverCaptured(ruvia::WebWorkerContext& context, const message::StreamMessage& message, const Catalog& catalog, WebhookHttpClient& http, DeliveryAttempt& attempt) {
        try {
            co_await deliver(context, message, catalog, http);
            attempt.succeeded = true;
        } catch (const std::exception& error) {
            attempt.error = error.what();
        } catch (...) {
            attempt.error = "unknown webhook delivery failure";
        }
    }

    ruvia::Task<void> deliverEvents(
        ruvia::WebWorkerContext& context,
        std::string_view sourceStream,
        const std::vector<message::StreamMessage>& messages,
        const Catalog& catalog,
        WebhookHttpClient& http
    ) {
        if (messages.empty()) {
            co_return;
        }
        std::map<std::string, DeviceEventQueue, std::less<>> queues;
        for (std::size_t index = 0; index < messages.size(); ++index) {
            auto key = std::string(messages[index].get("device_id"));
            if (key.empty()) {
                key = "invalid:" + std::to_string(index);
            }
            queues[key].indexes.push_back(index);
        }

        std::vector<message::StreamMessage> completed;
        completed.reserve(messages.size());
        std::string firstError;
        while (true) {
            std::vector<DeviceEventQueue*> selected;
            selected.reserve(kEventConcurrency);
            for (auto& [deviceId, queue] : queues) {
                (void)deviceId;
                if (!queue.blocked && queue.next < queue.indexes.size()) {
                    selected.push_back(&queue);
                }
                if (selected.size() == kEventConcurrency) {
                    break;
                }
            }
            if (selected.empty()) {
                break;
            }

            std::vector<DeliveryAttempt> attempts(selected.size());
            ruvia::TaskScope scope(
                context.worker(),
                ruvia::TaskScopeOptions{ .resource = context.resource() }
            );
            for (std::size_t index = 0; index < selected.size(); ++index) {
                const auto messageIndex = selected[index]->indexes[selected[index]->next];
                scope.spawn(deliverCaptured(context, messages[messageIndex], catalog, http, attempts[index]));
            }
            co_await scope.join();
            for (std::size_t index = 0; index < selected.size(); ++index) {
                auto& queue = *selected[index];
                const auto messageIndex = queue.indexes[queue.next];
                if (attempts[index].succeeded) {
                    completed.push_back(messages[messageIndex]);
                    ++queue.next;
                } else {
                    queue.blocked = true;
                    if (firstError.empty()) {
                        firstError = std::move(attempts[index].error);
                    }
                }
            }
        }
        if (!completed.empty()) {
            co_await message::redis::acknowledgeAndDeleteMany(
                context.redis(),
                sourceStream,
                kGroup,
                completed
            );
        }
        if (!firstError.empty()) {
            throw std::runtime_error(firstError);
        }
    }

    ruvia::Task<void> deliver(ruvia::WebWorkerContext& context, const message::StreamMessage& message, const Catalog& catalog, WebhookHttpClient& http) {
        const auto eventType = message.get("event_type");
        const auto deviceId = message.get("device_id");
        if (!service::message::supportedEvent(eventType) || !service::common::isUuid(deviceId)) {
            co_return;
        }
        const auto device = catalog.find(deviceId);
        if (device == catalog.end()) {
            co_return;
        }
        const auto targetEntry = device->second.targets.find(eventType);
        if (targetEntry == device->second.targets.end()) {
            co_return;
        }
        const auto now = service::message::utcNowMilliseconds();
        std::vector<const Target*> targets;
        targets.reserve(targetEntry->second.size());
        for (const auto& target : targetEntry->second) {
            if (target.expiresAtMs == 0 || target.expiresAtMs > now) {
                targets.push_back(&target);
            }
        }
        if (targets.empty()) {
            co_return;
        }
        const auto delivery = co_await buildDelivery(context, message, device->second);
        const auto completed = co_await completedTargets(context.redis(), delivery);
        std::erase_if(targets, [&completed](const Target* target) {
            return completed.contains(target->id);
        });
        if (targets.empty()) {
            co_return;
        }
        for (std::size_t offset = 0; offset < targets.size(); offset += kTargetConcurrency) {
            const auto end = std::min(targets.size(), offset + kTargetConcurrency);
            ruvia::TaskScope scope(
                context.worker(),
                ruvia::TaskScopeOptions{ .resource = context.resource() }
            );
            for (auto index = offset; index < end; ++index) {
                scope.spawn(deliverTarget(context, http, *targets[index], delivery));
            }
            co_await scope.join();
        }
    }

    static std::string deliveryProgressKey(const Delivery& delivery) {
        return "iot:open-access:delivery-progress:" + delivery.eventType + ":" + delivery.id;
    }

    template <typename Redis>
    static ruvia::Task<std::set<std::string, std::less<>>>
    completedTargets(const Redis& redis, const Delivery& delivery) {
        const auto reply = co_await message::redis::command(
            redis,
            { "HKEYS", deliveryProgressKey(delivery) }
        );
        if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
            message::redis::throwValue("read webhook delivery progress", reply);
        }
        std::set<std::string, std::less<>> result;
        for (const auto& value : reply.array()) {
            if (value.kind() != ruvia::RedisValue::Kind::kString) {
                throw std::runtime_error("webhook delivery progress contains a non-string target");
            }
            result.emplace(value.string());
        }
        co_return result;
    }

    static std::string deviceReference(std::string_view id, std::string_view code, std::string_view name) {
        return "{\"id\":" + service::utils::jsonQuoted(id) + ",\"code\":" + service::utils::jsonQuoted(code) +
            ",\"name\":" + service::utils::jsonQuoted(name) + "}";
    }

    static std::string jsonFieldOr(const ruvia::JsonValue& object, std::string_view field, std::string_view fallback) {
        const auto value = service::utils::jsonField(object, field);
        return value ? std::string(value->view()) : std::string(fallback);
    }

    static std::string mergeEventData(std::string_view deviceJson, std::string_view rawData) {
        std::string result = "{\"device\":" + std::string(deviceJson);
        if (const auto parsed = ruvia::JsonValue::parse(rawData); parsed && parsed->isObject()) {
            (void)ruvia::detail::visitJsonObjectFields(
                ruvia::detail::ResolvedPmrResourceTag{},
                parsed->view(),
                std::pmr::get_default_resource(),
                [&](std::string_view name, std::string_view value) {
                    if (name != "device") {
                        result += "," + service::utils::jsonQuoted(name) + ":" + std::string(value);
                    }
                    return true;
                }
            );
        }
        result.push_back('}');
        return result;
    }

    static std::string imageEventData(std::string_view deviceJson, std::string_view rawData, std::string_view occurredAt) {
        const auto parsed = ruvia::JsonValue::parse(rawData);
        if (!parsed || !parsed->isObject()) {
            return mergeEventData(deviceJson, rawData);
        }
        const auto values = service::utils::jsonField(*parsed, "values");
        if (!values || !values->isObject()) {
            return mergeEventData(deviceJson, rawData);
        }

        std::string image;
        (void)ruvia::detail::visitJsonObjectFields(
            ruvia::detail::ResolvedPmrResourceTag{},
            values->view(),
            std::pmr::get_default_resource(),
            [&](std::string_view id, std::string_view raw) {
                if (!image.empty()) {
                    return true;
                }
                const auto item = ruvia::JsonValue::parse(raw);
                if (!item || !item->isObject()) {
                    return true;
                }
                const auto type = item->get<ruvia::String>("type");
                const auto value = service::utils::jsonField(*item, "value");
                const auto text = item->get<ruvia::String>("value");
                const bool jpeg = type && type->view() == "JPEG";
                const bool dataUrl = text && text->view().starts_with("data:image/");
                if (!value || (!jpeg && !dataUrl)) {
                    return true;
                }
                const auto name = item->get<ruvia::String>("name");
                image = "{\"id\":" + service::utils::jsonQuoted(id) + ",\"name\":" +
                    service::utils::jsonQuoted(name ? name->view() : std::string_view("image")) +
                    ",\"data\":" + std::string(value->view()) +
                    ",\"time\":" + service::utils::jsonQuoted(occurredAt) + "}";
                return true;
            }
        );
        if (image.empty()) {
            return mergeEventData(deviceJson, rawData);
        }
        return "{\"device\":" + std::string(deviceJson) + ",\"image\":" + image + "}";
    }

    static std::string commandEventData(std::string_view deviceJson, const ruvia::JsonValue& payload, bool dispatched) {
        (void)dispatched;
        return "{\"device\":" + std::string(deviceJson) + ",\"command\":{\"id\":" +
            jsonFieldOr(payload, "commandId", "null") + ",\"status\":" +
            jsonFieldOr(payload, "status", "null") + ",\"reason\":" +
            jsonFieldOr(payload, "reason", "null") + ",\"elements\":" +
            jsonFieldOr(payload, "elements", "[]") + ",\"actual_values\":" +
            jsonFieldOr(payload, "actualValues", "[]") + "}}";
    }

    static ruvia::Task<std::string> realtimeData(ruvia::WebWorkerContext& context, std::string_view deviceId, const DeviceCatalog& device) {
        const auto reply = co_await message::redis::command(
            context.redis(),
            std::vector<std::string>{ "HGETALL", telemetry::latest::latestKey(deviceId) }
        );
        std::set<std::string, std::less<>> configured;
        std::map<std::string, LatestPoint, std::less<>> latest;
        bool hasConfigured = false;
        if (reply.kind() == ruvia::RedisValue::Kind::kArray) {
            const auto& entries = reply.array();
            for (std::size_t index = 0; index + 1 < entries.size(); index += 2) {
                if (entries[index].kind() != ruvia::RedisValue::Kind::kString ||
                    entries[index + 1].kind() != ruvia::RedisValue::Kind::kString) {
                    continue;
                }
                const auto field = entries[index].string();
                const auto raw = entries[index + 1].string();
                if (field == "_element_ids") {
                    hasConfigured = true;
                    const auto parsed = ruvia::JsonValue::parse(raw);
                    if (parsed && parsed->isObject()) {
                        (void)ruvia::detail::visitJsonObjectFields(
                            ruvia::detail::ResolvedPmrResourceTag{},
                            parsed->view(),
                            std::pmr::get_default_resource(),
                            [&](std::string_view name, std::string_view) {
                                if (!name.empty()) {
                                    configured.emplace(name);
                                }
                                return true;
                            }
                        );
                    }
                    continue;
                }
                if (field.empty() || field.front() == '_') {
                    continue;
                }
                const auto parsed = ruvia::JsonValue::parse(raw);
                if (!parsed || !parsed->isObject()) {
                    continue;
                }
                LatestPoint point;
                point.id.assign(field);
                if (const auto value = parsed->get<ruvia::String>("id")) {
                    point.id.assign(value->view());
                }
                point.name = point.id;
                if (const auto value = parsed->get<ruvia::String>("name")) {
                    point.name.assign(value->view());
                }
                if (const auto value = parsed->get<ruvia::String>("unit")) {
                    point.unit.assign(value->view());
                }
                if (const auto value = parsed->get<ruvia::String>("dataType")) {
                    point.dataType.assign(value->view());
                }
                if (const auto value = parsed->get<ruvia::String>("encode")) {
                    point.encode.assign(value->view());
                }
                if (const auto value = parsed->get<ruvia::Int64>("sort")) {
                    point.sort = static_cast<std::int64_t>(*value);
                }
                if (const auto value = parsed->get<ruvia::Int64>("observedAt")) {
                    point.observedAt = static_cast<std::int64_t>(*value);
                }
                if (const auto value = service::utils::jsonField(*parsed, "value")) {
                    point.value = telemetry::latest::canonicalPointJson(
                        value->view(),
                        point.dataType
                    );
                }
                latest.insert_or_assign(point.id, std::move(point));
            }
        }
        std::vector<LatestPoint> points;
        points.reserve(latest.size());
        for (auto& [id, point] : latest) {
            if ((!hasConfigured || configured.contains(id)) && point.encode != "JPEG") {
                points.push_back(std::move(point));
            }
        }
        std::ranges::sort(points, [](const LatestPoint& left, const LatestPoint& right) {
            return left.sort == right.sort ? left.id < right.id : left.sort < right.sort;
        });
        std::string body = "{\"device\":" + deviceReference(deviceId, device.code, device.name) +
            ",\"points\":[";
        for (std::size_t index = 0; index < points.size(); ++index) {
            if (index != 0) {
                body.push_back(',');
            }
            const auto time = points[index].observedAt > 0
                ? service::utils::jsonQuoted(service::common::utcTimestampFromMilliseconds(points[index].observedAt))
                : std::string("null");
            body += "{\"id\":" + service::utils::jsonQuoted(points[index].id) +
                ",\"name\":" + service::utils::jsonQuoted(points[index].name) +
                ",\"value\":" + points[index].value +
                ",\"unit\":" + service::utils::jsonQuoted(points[index].unit) +
                ",\"time\":" + time + "}";
        }
        body += "]}";
        co_return body;
    }

    static ruvia::Task<Delivery> buildDelivery(ruvia::WebWorkerContext& context, const message::StreamMessage& message, const DeviceCatalog& catalog) {
        Delivery delivery;
        delivery.id = message.get("event_id").empty() ? service::common::nextUuidV7()
                                                      : std::string(message.get("event_id"));
        delivery.eventType = std::string(message.get("event_type"));
        delivery.deviceId = std::string(message.get("device_id"));
        delivery.deviceCode = std::string(message.get("device_code"));
        const auto occurredAt = service::common::parseInt64(
            std::optional<std::string_view>(message.get("occurred_at_ms"))
        );
        delivery.occurredAt = occurredAt ? service::common::utcTimestampFromMilliseconds(*occurredAt) : service::common::utcTimestampNow();

        const auto device =
            deviceReference(delivery.deviceId, delivery.deviceCode, catalog.name);
        const auto rawData = message.get("data_json");
        std::string data;
        if (delivery.eventType == "device.data.reported") {
            data = co_await realtimeData(context, delivery.deviceId, catalog);
        } else if (delivery.eventType == "device.image.reported") {
            data = imageEventData(device, rawData, delivery.occurredAt);
        } else if (delivery.eventType == "device.command.accepted" ||
                   delivery.eventType == "device.command.updated") {
            const auto payload = ruvia::JsonValue::parse(rawData);
            data = payload && payload->isObject()
                ? commandEventData(device, *payload, delivery.eventType == "device.command.accepted")
                : mergeEventData(device, rawData);
        } else {
            data = mergeEventData(device, rawData);
        }
        delivery.body =
            service::message::webhookEnvelope(delivery.eventType, delivery.occurredAt, delivery.id, data);
        co_return delivery;
    }

    ruvia::Task<void> deliverTarget(ruvia::WebWorkerContext& context, WebhookHttpClient& http, const Target& target, const Delivery& delivery) {
        const auto requestTimestamp = service::common::utcTimestampNow();
        auto headers = parseHeaders(target.headers);
        headers.emplace_back("X-IOT-Event", delivery.eventType);
        headers.emplace_back("X-IOT-Timestamp", requestTimestamp);
        headers.emplace_back("X-IOT-Delivery", delivery.id);
        if (!target.secret.empty()) {
            headers.emplace_back("X-IOT-Signature", "sha256=" + service::utils::hmacSha256(target.secret, delivery.body));
        }

        WebhookHttpResponse response;
        try {
            const auto url = parseWebhookUrl(target.url);
            auto [completion, receiver] = ruvia::makeOneShot<WebhookHttpResponse>(context.worker());
            auto shared = std::make_shared<ruvia::OneShotCompletion<WebhookHttpResponse>>(
                std::move(completion)
            );
            http.post(url, WebhookHttpClient::request(url, delivery.body, headers), std::chrono::seconds(target.timeout), target.skipTlsVerify, [shared](WebhookHttpResponse result) mutable {
                (void)shared->complete(std::move(result));
            });
            auto outcome = co_await receiver.wait();
            if (outcome.hasValue()) {
                response = std::move(outcome).takeValue();
            } else {
                response.error = "Webhook request was cancelled";
            }
        } catch (const std::exception& error) {
            response.error = error.what();
        }
        const bool success =
            response.error.empty() && response.status >= 200 && response.status < 300;
        if (!success && response.error.empty()) {
            response.error =
                "HTTP " + std::to_string(response.status) + " " + service::utils::sanitize(response.body, 500);
        }
        co_await enqueueResult(context, target, delivery, response, success);
    }

    static std::vector<std::pair<std::string, std::string>> parseHeaders(std::string_view json) {
        std::vector<std::pair<std::string, std::string>> result;
        (void)ruvia::detail::visitJsonObjectFields(
            ruvia::detail::ResolvedPmrResourceTag{},
            json,
            std::pmr::get_default_resource(),
            [&](std::string_view name, std::string_view raw) {
                auto input = raw;
                const auto value = ruvia::detail::parseJsonValue<ruvia::String>(
                    input,
                    std::pmr::get_default_resource()
                );
                if (value) {
                    validateWebhookHeader(name, value->view(), true);
                    result.emplace_back(name, value->view());
                }
                return true;
            }
        );
        return result;
    }

    static ruvia::Task<void> enqueueResult(ruvia::WebWorkerContext& context, const Target& target, const Delivery& delivery, const WebhookHttpResponse& response, bool success) {
        const auto error = service::utils::sanitize(response.error, 1000);
        const auto status = success ? "success" : "failed";
        const auto logId = service::common::nextUuidV7();
        const auto responseJson = "{\"httpStatus\":" + std::to_string(response.status) +
            ",\"body\":" + service::utils::jsonQuoted(service::utils::sanitize(response.body, 2000)) +
            (error.empty() ? "" : ",\"error\":" + service::utils::jsonQuoted(error)) + "}";
        static constexpr std::string_view script = R"lua(
if redis.call('HEXISTS', KEYS[2], ARGV[1]) ~= 0 then return false end
local arguments = {'MAXLEN', '~', ARGV[2], '*'}
for index = 6, #ARGV do arguments[#arguments + 1] = ARGV[index] end
local id = redis.call('XADD', KEYS[1], unpack(arguments))
redis.call('HSET', KEYS[2], ARGV[1], '1')
redis.call('EXPIRE', KEYS[2], ARGV[3])
redis.call('XADD', KEYS[3], 'MAXLEN', '~', ARGV[4], '*', 'task', ARGV[5])
return id
)lua";
        const std::vector<std::string> keyStore{
            stream::deliveryResult(),
            deliveryProgressKey(delivery),
            service::message::workerWakeStream(std::nullopt)
        };
        const std::vector<std::string> argumentStore{
            target.id,
            "100000",
            std::to_string(kDeliveryProgressTtlSeconds),
            std::to_string(service::message::kWorkerWakeCapacity),
            std::string(service::message::workerStreamTaskName(service::message::WorkerStreamTask::Webhook)),
            "log_id",
            logId,
            "access_key_id",
            target.accessKeyId,
            "webhook_id",
            target.id,
            "event_type",
            delivery.eventType,
            "status",
            status,
            "target",
            target.url,
            "http_status",
            std::to_string(response.status),
            "device_id",
            delivery.deviceId,
            "device_code",
            delivery.deviceCode,
            "message",
            error,
            "request_payload",
            delivery.body,
            "response_payload",
            responseJson,
            "completed_at_ms",
            std::to_string(service::message::utcNowMilliseconds()),
        };
        const std::vector<std::string_view> keys(keyStore.begin(), keyStore.end());
        const std::vector<std::string_view> arguments(argumentStore.begin(), argumentStore.end());
        const auto reply = co_await context.redis().eval(script, keys, arguments);
        if (!reply.null() && reply.kind() != ruvia::RedisValue::Kind::kString) {
            message::redis::throwValue("enqueue webhook result", reply);
        }
    }

    ruvia::WebWorkerHandle worker_;
    std::shared_future<void> stopped_;
    std::size_t workerIndex_ = 0;
    std::size_t serviceWorkerCount_ = 0;
    std::atomic_bool running_{ false };
};

} // namespace service::access
