#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <functional>
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

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>
#include <ruvia/core/OneShot.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/access/contract.h"
#include "service/features/access/audit.h"
#include "service/features/access/event.h"
#include "service/features/access/session.h"
#include "service/features/collector/stream.h"
#include "service/features/event/config.h"
#include "service/features/telemetry/latest.h"

namespace service::access {

struct WebhookHttpResponse final {
    std::int64_t status{0};
    std::string body;
    std::string error;
};

struct WebhookUrl final {
    bool tls{false};
    std::string host;
    std::string port;
    std::string target;
};

inline WebhookUrl parseWebhookUrl(std::string_view value) {
    WebhookUrl result;
    if (value.starts_with("https://")) {
        result.tls = true;
        value.remove_prefix(8);
        result.port = "443";
    } else if (value.starts_with("http://")) {
        value.remove_prefix(7);
        result.port = "80";
    } else {
        throw std::invalid_argument("Webhook URL scheme is invalid");
    }
    const auto path = value.find_first_of("/?");
    auto authority = value.substr(0, path);
    result.target = path == std::string_view::npos ? "/" : std::string(value.substr(path));
    if (authority.empty() || authority.find('@') != std::string_view::npos)
        throw std::invalid_argument("Webhook URL authority is invalid");
    if (authority.front() == '[') {
        const auto closing = authority.find(']');
        if (closing == std::string_view::npos)
            throw std::invalid_argument("Webhook IPv6 host is invalid");
        result.host = std::string(authority.substr(1, closing - 1));
        if (closing + 1 < authority.size()) {
            if (authority[closing + 1] != ':')
                throw std::invalid_argument("Webhook URL port is invalid");
            result.port = std::string(authority.substr(closing + 2));
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        result.host = std::string(authority.substr(0, colon));
        result.port = std::string(authority.substr(colon + 1));
    } else {
        result.host = std::string(authority);
    }
    if (result.host.empty() || result.port.empty())
        throw std::invalid_argument("Webhook URL host or port is invalid");
    return result;
}

class WebhookHttpClient final {
  public:
    WebhookHttpClient()
        : tls_(asio::ssl::context::tls_client), work_(asio::make_work_guard(io_)),
          thread_([this] { io_.run(); }) {
        tls_.set_default_verify_paths();
        tls_.set_verify_mode(asio::ssl::verify_peer);
    }

    ~WebhookHttpClient() {
        work_.reset();
        io_.stop();
        if (thread_.joinable())
            thread_.join();
    }

    WebhookHttpClient(const WebhookHttpClient&) = delete;
    WebhookHttpClient& operator=(const WebhookHttpClient&) = delete;

    void post(WebhookUrl url, std::string request, std::chrono::seconds timeout,
              std::function<void(WebhookHttpResponse)> done) {
        asio::co_spawn(
            io_,
            [this, url = std::move(url), request = std::move(request),
             timeout]() -> asio::awaitable<WebhookHttpResponse> {
                if (url.tls)
                    co_return co_await exchangeTls(url, request, timeout);
                co_return co_await exchangePlain(url, request, timeout);
            },
            [done = std::move(done)](std::exception_ptr error, WebhookHttpResponse response) {
                if (error) {
                    try {
                        std::rethrow_exception(error);
                    } catch (const std::exception& exception) {
                        response.error = exception.what();
                    } catch (...) {
                        response.error = "unknown outbound HTTP error";
                    }
                }
                done(std::move(response));
            });
    }

  private:
    template <typename Stream> static asio::awaitable<std::string> readResponse(Stream& stream) {
        std::string response;
        std::array<char, 8192> buffer{};
        while (response.size() < 65536) {
            std::error_code error;
            const auto size = co_await stream.async_read_some(
                asio::buffer(buffer), asio::redirect_error(asio::use_awaitable, error));
            if (error == asio::error::eof || error == asio::ssl::error::stream_truncated ||
                size == 0)
                break;
            if (error)
                throw std::system_error(error);
            response.append(buffer.data(), std::min<std::size_t>(size, 65536 - response.size()));
        }
        co_return response;
    }

    static WebhookHttpResponse parseResponse(std::string response) {
        WebhookHttpResponse result;
        const auto lineEnd = response.find("\r\n");
        if (lineEnd == std::string::npos)
            throw std::runtime_error("Webhook returned an invalid HTTP response");
        const auto line = std::string_view(response).substr(0, lineEnd);
        const auto firstSpace = line.find(' ');
        if (firstSpace == std::string_view::npos || firstSpace + 4 > line.size())
            throw std::runtime_error("Webhook returned an invalid HTTP status");
        const auto status = service::common::parseInt64(
            std::optional<std::string_view>(line.substr(firstSpace + 1, 3)));
        if (!status)
            throw std::runtime_error("Webhook returned an invalid HTTP status");
        result.status = *status;
        const auto body = response.find("\r\n\r\n");
        if (body != std::string::npos)
            result.body = response.substr(body + 4, 8192);
        return result;
    }

    static std::string hostHeader(const WebhookUrl& url) {
        const bool defaultPort = (url.tls && url.port == "443") || (!url.tls && url.port == "80");
        return url.host + (defaultPort ? "" : ":" + url.port);
    }

    asio::awaitable<WebhookHttpResponse>
    exchangePlain(const WebhookUrl& url, const std::string& request, std::chrono::seconds timeout) {
        auto executor = co_await asio::this_coro::executor;
        auto socket = std::make_shared<asio::ip::tcp::socket>(executor);
        auto resolver = std::make_shared<asio::ip::tcp::resolver>(executor);
        auto timer = std::make_shared<asio::steady_timer>(executor, timeout);
        timer->async_wait([resolver, socket](const std::error_code& error) {
            if (!error) {
                resolver->cancel();
                std::error_code ignored;
                socket->cancel(ignored);
            }
        });
        const auto endpoints =
            co_await resolver->async_resolve(url.host, url.port, asio::use_awaitable);
        co_await asio::async_connect(*socket, endpoints, asio::use_awaitable);
        co_await asio::async_write(*socket, asio::buffer(request), asio::use_awaitable);
        auto response = co_await readResponse(*socket);
        timer->cancel();
        co_return parseResponse(std::move(response));
    }

    asio::awaitable<WebhookHttpResponse>
    exchangeTls(const WebhookUrl& url, const std::string& request, std::chrono::seconds timeout) {
        auto executor = co_await asio::this_coro::executor;
        using Stream = asio::ssl::stream<asio::ip::tcp::socket>;
        auto stream = std::make_shared<Stream>(executor, tls_);
        auto resolver = std::make_shared<asio::ip::tcp::resolver>(executor);
        if (SSL_set_tlsext_host_name(stream->native_handle(), url.host.c_str()) != 1)
            throw std::runtime_error("Webhook TLS SNI setup failed");
        stream->set_verify_callback(asio::ssl::host_name_verification(url.host));
        auto timer = std::make_shared<asio::steady_timer>(executor, timeout);
        timer->async_wait([resolver, stream](const std::error_code& error) {
            if (!error) {
                resolver->cancel();
                std::error_code ignored;
                stream->next_layer().cancel(ignored);
            }
        });
        const auto endpoints =
            co_await resolver->async_resolve(url.host, url.port, asio::use_awaitable);
        co_await asio::async_connect(stream->next_layer(), endpoints, asio::use_awaitable);
        co_await stream->async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
        co_await asio::async_write(*stream, asio::buffer(request), asio::use_awaitable);
        auto response = co_await readResponse(*stream);
        timer->cancel();
        co_return parseResponse(std::move(response));
    }

  public:
    static std::string request(const WebhookUrl& url, std::string_view body,
                               const std::vector<std::pair<std::string, std::string>>& headers) {
        std::string result = "POST " + url.target + " HTTP/1.1\r\nHost: " + hostHeader(url) +
                             "\r\nUser-Agent: iot-engine-webhook/1.0\r\n";
        for (const auto& [name, value] : headers)
            result += name + ": " + value + "\r\n";
        result +=
            "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n";
        result.append(body);
        return result;
    }

  private:
    asio::io_context io_;
    asio::ssl::context tls_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::thread thread_;
};

class WebhookRuntime final {
public:
    WebhookRuntime() = default;
    WebhookRuntime(const WebhookRuntime&) = delete;
    WebhookRuntime& operator=(const WebhookRuntime&) = delete;
    ~WebhookRuntime() { stop(); }

    void start(const std::vector<ruvia::WebWorkerHandle>& workers) {
        if (running_.exchange(true))
            return;
        if (workers.empty()) {
            running_.store(false);
            throw std::runtime_error("access webhook runtime requires a service worker");
        }
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        auto readiness = ready->get_future();
        stopped_ = stopped->get_future().share();
        // Keep event ordering global. Target HTTP requests inside one event are
        // bounded-concurrent, while events themselves stay on this single consumer.
        const auto posted =
            workers.back().post([this, ready, stopped](ruvia::WebWorkerContext& context) {
                return run(context, ready, stopped);
            });
        if (!posted.accepted()) {
            running_.store(false);
            throw std::runtime_error("service worker rejected access webhook runtime");
        }
        readiness.get();
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        if (stopped_.valid())
            (void)stopped_.wait_for(std::chrono::seconds(3));
        stopped_ = {};
    }

  private:
    static constexpr std::string_view kGroup = "iot-engine:open-webhook";
    static constexpr std::string_view kDeliveryResultStream =
        "iot:channel:open-access:delivery-result";
    static constexpr std::size_t kBatchSize = 100;
    static constexpr std::size_t kEventConcurrency = 4;
    static constexpr std::size_t kTargetConcurrency = 4;
    static constexpr std::int64_t kDeliveryProgressTtlSeconds = 7 * 24 * 60 * 60;

    struct Target final {
        std::string id;
        std::string accessKeyId;
        std::string url;
        std::string secret;
        std::string headers;
        std::int64_t timeout{5};
        std::int64_t expiresAtMs{0};
    };

    struct DeviceCatalog final {
        std::string name;
        std::string code;
        std::map<std::string, std::vector<Target>, std::less<>> targets;
    };

    using Catalog = std::map<std::string, DeviceCatalog, std::less<>>;

    struct Delivery final {
        std::string id;
        std::string eventType;
        std::string deviceId;
        std::string deviceCode;
        std::string occurredAt;
        std::string body;
    };

    struct LatestPoint final {
        std::int64_t sort{0};
        std::int64_t observedAt{0};
        std::string id;
        std::string name;
        std::string value{"null"};
        std::string unit;
        std::string encode;
    };

    ruvia::Task<void> run(ruvia::WebWorkerContext& context,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            const std::vector<std::string> streams{
                std::string(service::message::kWebhookCatalogChangesStream),
                std::string(kDeliveryResultStream), std::string(audit::kStream),
                std::string(event::kStream)};
            for (const auto& stream : streams)
                co_await message::redis::ensureGroup(redis, stream, kGroup);
            co_await session::refresh(context);
            catalog_ = co_await loadCatalog(context);
            ready->set_value();
            bool recovering = true;
            while (running_.load() && !context.stopToken().stopRequested()) {
                std::vector<message::redis::StreamBatch> batches;
                bool readFailed = false;
                try {
                    batches = recovering
                        ? co_await message::redis::readGroupMany(
                              redis, streams, kGroup, "service-0", "0", kBatchSize)
                        : co_await message::redis::readGroupManyBlocking(
                              redis, streams, kGroup, "service-0", context.stopToken(),
                              kBatchSize);
                } catch (const std::exception& error) {
                    if (context.stopToken().stopRequested())
                        break;
                    std::cerr << "open webhook stream read failed: " << error.what() << '\n';
                    recovering = true;
                    readFailed = true;
                }
                if (readFailed) {
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
                    continue;
                }
                if (recovering && batches.empty())
                    recovering = false;
                if (batches.empty())
                    continue;
                bool failed = false;
                try {
                    bool reloadCatalog = false;
                    for (const auto& batch : batches) {
                        if (batch.stream != service::message::kWebhookCatalogChangesStream)
                            continue;
                        for (const auto& message : batch.messages)
                            reloadCatalog = reloadCatalog || catalogChange(message);
                    }
                    if (reloadCatalog) {
                        co_await session::refresh(context);
                        catalog_ = co_await loadCatalog(context);
                    }
                    for (const auto& batch : batches) {
                        if (batch.stream != service::message::kWebhookCatalogChangesStream)
                            continue;
                        co_await message::redis::acknowledgeAndDeleteMany(
                            redis, batch.stream, kGroup, batch.messages);
                    }
                    for (const auto& batch : batches) {
                        if (batch.stream != kDeliveryResultStream)
                            continue;
                        co_await persistResults(context, batch.messages);
                        co_await message::redis::acknowledgeAndDeleteMany(
                            redis, batch.stream, kGroup, batch.messages);
                    }
                    for (const auto& batch : batches) {
                        if (batch.stream != audit::kStream)
                            continue;
                        co_await persistAudits(context, batch.messages);
                        co_await message::redis::acknowledgeAndDeleteMany(
                            redis, batch.stream, kGroup, batch.messages);
                    }
                    for (const auto& batch : batches) {
                        if (batch.stream != event::kStream)
                            continue;
                        co_await deliverEvents(context, batch.messages);
                    }
                } catch (const std::exception& error) {
                    std::cerr << "open webhook dispatch failed: " << error.what() << '\n';
                    recovering = true;
                    failed = true;
                }
                if (failed)
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
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

    struct DeliveryAttempt final {
        bool succeeded{false};
        std::string error;
    };

    struct DeviceEventQueue final {
        std::vector<std::size_t> indexes;
        std::size_t next{0};
        bool blocked{false};
    };

    ruvia::Task<void> deliverCaptured(ruvia::WebWorkerContext& context,
                                      const message::StreamMessage& message,
                                      DeliveryAttempt& attempt) {
        try {
            co_await deliver(context, message);
            attempt.succeeded = true;
        } catch (const std::exception& error) {
            attempt.error = error.what();
        } catch (...) {
            attempt.error = "unknown webhook delivery failure";
        }
    }

    ruvia::Task<void> deliverEvents(
        ruvia::WebWorkerContext& context,
        const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        std::map<std::string, DeviceEventQueue, std::less<>> queues;
        for (std::size_t index = 0; index < messages.size(); ++index) {
            auto key = std::string(messages[index].get("device_id"));
            if (key.empty())
                key = "invalid:" + std::to_string(index);
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
                if (!queue.blocked && queue.next < queue.indexes.size())
                    selected.push_back(&queue);
                if (selected.size() == kEventConcurrency)
                    break;
            }
            if (selected.empty())
                break;

            std::vector<DeliveryAttempt> attempts(selected.size());
            ruvia::TaskScope scope(context.worker(), context.resource());
            for (std::size_t index = 0; index < selected.size(); ++index) {
                const auto messageIndex = selected[index]->indexes[selected[index]->next];
                scope.spawn(deliverCaptured(context, messages[messageIndex], attempts[index]));
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
                    if (firstError.empty())
                        firstError = std::move(attempts[index].error);
                }
            }
        }
        if (!completed.empty())
            co_await message::redis::acknowledgeAndDeleteMany(
                context.redis(), event::kStream, kGroup, completed);
        if (!firstError.empty())
            throw std::runtime_error(firstError);
    }

    static ruvia::Task<Catalog> loadCatalog(ruvia::WebWorkerContext& context) {
        const auto rows = co_await context.db().query(R"sql(
SELECT binding.device_id::text, device.name,
       COALESCE(device.protocol_params->>'device_code', ''),
       webhook.id::text, webhook.access_key_id::text, webhook.url,
       COALESCE(webhook.secret, ''), webhook.headers::text,
       webhook.timeout_seconds::text,
       COALESCE((EXTRACT(EPOCH FROM key.expires_at) * 1000)::bigint, 0)::text,
       event_type.value
FROM open_webhook webhook
JOIN open_access_key key ON key.id = webhook.access_key_id
JOIN open_access_key_device binding ON binding.access_key_id = key.id
JOIN device ON device.id = binding.device_id
CROSS JOIN LATERAL jsonb_array_elements_text(
  CASE WHEN jsonb_typeof(webhook.event_types) = 'array'
       THEN webhook.event_types ELSE '[]'::jsonb END) event_type(value)
WHERE webhook.deleted_at IS NULL AND webhook.status = 'enabled'
  AND key.deleted_at IS NULL AND key.status = 'enabled'
  AND (key.expires_at IS NULL OR key.expires_at > NOW())
  AND device.deleted_at IS NULL
ORDER BY binding.device_id, event_type.value, webhook.id)sql");
        Catalog result;
        for (const auto& row : rows.rows()) {
            const std::string deviceId(row[0].text());
            auto& device = result[deviceId];
            device.name.assign(row[1].text());
            device.code.assign(row[2].text());
            device.targets[std::string(row[10].text())].push_back(
                {std::string(row[3].text()), std::string(row[4].text()),
                 std::string(row[5].text()), std::string(row[6].text()),
                 std::string(row[7].text()), std::stoll(std::string(row[8].text())),
                 std::stoll(std::string(row[9].text()))});
        }
        co_return result;
    }

    ruvia::Task<void> deliver(ruvia::WebWorkerContext& context,
                              const message::StreamMessage& message) {
        const auto eventType = message.get("event_type");
        const auto deviceId = message.get("device_id");
        if (!supportedEvent(eventType) || !service::common::isUuid(deviceId))
            co_return;
        const auto device = catalog_.find(deviceId);
        if (device == catalog_.end())
            co_return;
        const auto targetEntry = device->second.targets.find(eventType);
        if (targetEntry == device->second.targets.end())
            co_return;
        const auto now = service::message::utcNowMilliseconds();
        std::vector<const Target*> targets;
        targets.reserve(targetEntry->second.size());
        for (const auto& target : targetEntry->second)
            if (target.expiresAtMs == 0 || target.expiresAtMs > now)
                targets.push_back(&target);
        if (targets.empty())
            co_return;
        const auto delivery = co_await buildDelivery(context, message, device->second);
        const auto completed = co_await completedTargets(context.redis(), delivery);
        std::erase_if(targets, [&completed](const Target* target) {
            return completed.contains(target->id);
        });
        if (targets.empty())
            co_return;
        for (std::size_t offset = 0; offset < targets.size(); offset += kTargetConcurrency) {
            const auto end = std::min(targets.size(), offset + kTargetConcurrency);
            ruvia::TaskScope scope(context.worker(), context.resource());
            for (auto index = offset; index < end; ++index)
                scope.spawn(deliverTarget(context, *targets[index], delivery));
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
            redis, {"HKEYS", deliveryProgressKey(delivery)});
        if (reply.kind() != ruvia::RedisValue::Kind::kArray)
            message::redis::throwValue("read webhook delivery progress", reply);
        std::set<std::string, std::less<>> result;
        for (const auto& value : reply.array()) {
            if (value.kind() != ruvia::RedisValue::Kind::kString)
                throw std::runtime_error("webhook delivery progress contains a non-string target");
            result.emplace(value.string());
        }
        co_return result;
    }

    static std::string deviceReference(std::string_view id, std::string_view code,
                                       std::string_view name) {
        return "{\"id\":" + jsonQuoted(id) + ",\"code\":" + jsonQuoted(code) +
               ",\"name\":" + jsonQuoted(name) + "}";
    }

    static std::string jsonFieldOr(const ruvia::JsonValue& object, std::string_view field,
                                   std::string_view fallback) {
        const auto value = jsonField(object, field);
        return value ? std::string(value->view()) : std::string(fallback);
    }

    static std::string mergeEventData(std::string_view deviceJson, std::string_view rawData) {
        std::string result = "{\"device\":" + std::string(deviceJson);
        if (const auto parsed = ruvia::JsonValue::parse(rawData); parsed && parsed->isObject()) {
            (void)ruvia::detail::visitJsonObjectFields(
                ruvia::detail::ResolvedPmrResourceTag{}, parsed->view(),
                std::pmr::get_default_resource(),
                [&](std::string_view name, std::string_view value) {
                    if (name != "device")
                        result += "," + jsonQuoted(name) + ":" + std::string(value);
                    return true;
                });
        }
        result.push_back('}');
        return result;
    }

    static std::string imageEventData(std::string_view deviceJson, std::string_view rawData,
                                      std::string_view occurredAt) {
        const auto parsed = ruvia::JsonValue::parse(rawData);
        if (!parsed || !parsed->isObject())
            return mergeEventData(deviceJson, rawData);
        const auto values = jsonField(*parsed, "values");
        if (!values || !values->isObject())
            return mergeEventData(deviceJson, rawData);

        std::string image;
        (void)ruvia::detail::visitJsonObjectFields(
            ruvia::detail::ResolvedPmrResourceTag{}, values->view(),
            std::pmr::get_default_resource(),
            [&](std::string_view id, std::string_view raw) {
                if (!image.empty())
                    return true;
                const auto item = ruvia::JsonValue::parse(raw);
                if (!item || !item->isObject())
                    return true;
                const auto type = item->get<ruvia::String>("type");
                const auto value = jsonField(*item, "value");
                const auto text = item->get<ruvia::String>("value");
                const bool jpeg = type && type->view() == "JPEG";
                const bool dataUrl = text && text->view().starts_with("data:image/");
                if (!value || (!jpeg && !dataUrl))
                    return true;
                const auto name = item->get<ruvia::String>("name");
                image = "{\"id\":" + jsonQuoted(id) + ",\"name\":" +
                        jsonQuoted(name ? name->view() : std::string_view("image")) +
                        ",\"data\":" + std::string(value->view()) +
                        ",\"time\":" + jsonQuoted(occurredAt) + "}";
                return true;
            });
        if (image.empty())
            return mergeEventData(deviceJson, rawData);
        return "{\"device\":" + std::string(deviceJson) + ",\"image\":" + image + "}";
    }

    static std::string commandEventData(std::string_view deviceJson,
                                        const ruvia::JsonValue& payload, bool dispatched) {
        const auto commandId = jsonFieldOr(payload, "commandId", "null");
        if (dispatched) {
            const auto elements = jsonFieldOr(payload, "elements", "{}");
            return "{\"accepted\":true,\"device\":" + std::string(deviceJson) +
                   ",\"command\":{\"key\":" + commandId + ",\"elements\":" + elements + "}}";
        }
        const auto status = payload.get<ruvia::String>("status");
        const auto success = status && status->view() == "SUCCESS";
        return "{\"device\":" + std::string(deviceJson) + ",\"command\":{\"key\":" +
               commandId + ",\"success\":" + (success ? "true" : "false") +
               ",\"status\":" + jsonFieldOr(payload, "status", "null") +
               ",\"reason\":" + jsonFieldOr(payload, "reason", "null") + "},\"points\":[]}";
    }

    static ruvia::Task<std::string> realtimeData(ruvia::WebWorkerContext& context,
                                                 std::string_view deviceId,
                                                 const DeviceCatalog& device) {
        const auto reply = co_await message::redis::command(
            context.redis(),
            std::vector<std::string>{"HGETALL", telemetry::latest::latestKey(device.code)});
        std::set<std::string, std::less<>> configured;
        std::map<std::string, LatestPoint, std::less<>> latest;
        bool hasConfigured = false;
        if (reply.kind() == ruvia::RedisValue::Kind::kArray) {
            const auto& entries = reply.array();
            for (std::size_t index = 0; index + 1 < entries.size(); index += 2) {
                if (entries[index].kind() != ruvia::RedisValue::Kind::kString ||
                    entries[index + 1].kind() != ruvia::RedisValue::Kind::kString)
                    continue;
                const auto field = entries[index].string();
                const auto raw = entries[index + 1].string();
                if (field == "_element_ids") {
                    hasConfigured = true;
                    const auto parsed = ruvia::JsonValue::parse(raw);
                    if (parsed && parsed->isObject())
                        (void)ruvia::detail::visitJsonObjectFields(
                            ruvia::detail::ResolvedPmrResourceTag{}, parsed->view(),
                            std::pmr::get_default_resource(),
                            [&](std::string_view name, std::string_view) {
                                if (!name.empty())
                                    configured.emplace(name);
                                return true;
                            });
                    continue;
                }
                if (field.empty() || field.front() == '_')
                    continue;
                const auto parsed = ruvia::JsonValue::parse(raw);
                if (!parsed || !parsed->isObject())
                    continue;
                LatestPoint point;
                point.id.assign(field);
                if (const auto value = parsed->get<ruvia::String>("id"))
                    point.id.assign(value->view());
                point.name = point.id;
                if (const auto value = parsed->get<ruvia::String>("name"))
                    point.name.assign(value->view());
                if (const auto value = parsed->get<ruvia::String>("unit"))
                    point.unit.assign(value->view());
                if (const auto value = parsed->get<ruvia::String>("encode"))
                    point.encode.assign(value->view());
                if (const auto value = parsed->get<ruvia::Int64>("sort"))
                    point.sort = static_cast<std::int64_t>(*value);
                if (const auto value = parsed->get<ruvia::Int64>("observedAt"))
                    point.observedAt = static_cast<std::int64_t>(*value);
                if (const auto value = jsonField(*parsed, "value"))
                    point.value.assign(value->view());
                latest.insert_or_assign(point.id, std::move(point));
            }
        }
        std::vector<LatestPoint> points;
        points.reserve(latest.size());
        for (auto& [id, point] : latest) {
            if ((!hasConfigured || configured.contains(id)) && point.encode != "JPEG")
                points.push_back(std::move(point));
        }
        std::ranges::sort(points, [](const LatestPoint& left, const LatestPoint& right) {
            return left.sort == right.sort ? left.id < right.id : left.sort < right.sort;
        });
        std::string body = "{\"device\":" + deviceReference(deviceId, device.code, device.name) +
                           ",\"points\":[";
        for (std::size_t index = 0; index < points.size(); ++index) {
            if (index != 0)
                body.push_back(',');
            const auto time = points[index].observedAt > 0
                                  ? jsonQuoted(iso8601(points[index].observedAt))
                                  : std::string("null");
            body += "{\"id\":" + jsonQuoted(points[index].id) +
                    ",\"name\":" + jsonQuoted(points[index].name) +
                    ",\"value\":" + points[index].value +
                    ",\"unit\":" + jsonQuoted(points[index].unit) +
                    ",\"time\":" + time + "}";
        }
        body += "]}";
        co_return body;
    }

    static ruvia::Task<Delivery> buildDelivery(ruvia::WebWorkerContext& context,
                                                const message::StreamMessage& message,
                                                const DeviceCatalog& catalog) {
        Delivery delivery;
        delivery.id = message.get("event_id").empty() ? service::common::nextUuidV7()
                                                       : std::string(message.get("event_id"));
        delivery.eventType = std::string(message.get("event_type"));
        delivery.deviceId = std::string(message.get("device_id"));
        delivery.deviceCode = std::string(message.get("device_code"));
        const auto occurredAt = service::common::parseInt64(
            std::optional<std::string_view>(message.get("occurred_at_ms")));
        delivery.occurredAt = occurredAt ? iso8601(*occurredAt) : nowIso8601();

        const auto device =
            deviceReference(delivery.deviceId, delivery.deviceCode, catalog.name);
        const auto rawData = message.get("data_json");
        std::string data;
        if (delivery.eventType == "device.data.reported") {
            data = co_await realtimeData(context, delivery.deviceId, catalog);
        } else if (delivery.eventType == "device.image.reported") {
            data = imageEventData(device, rawData, delivery.occurredAt);
        } else if (delivery.eventType == "device.command.dispatched" ||
                   delivery.eventType == "device.command.responded") {
            const auto payload = ruvia::JsonValue::parse(rawData);
            data = payload && payload->isObject()
                       ? commandEventData(device, *payload,
                                          delivery.eventType == "device.command.dispatched")
                       : mergeEventData(device, rawData);
        } else {
            data = mergeEventData(device, rawData);
        }
        delivery.body =
            webhookEnvelope(delivery.eventType, delivery.occurredAt, delivery.id, data);
        co_return delivery;
    }

    ruvia::Task<void> deliverTarget(ruvia::WebWorkerContext& context, const Target& target,
                                    const Delivery& delivery) {
        const auto requestTimestamp = nowIso8601();
        auto headers = parseHeaders(target.headers);
        headers.emplace_back("X-IOT-Event", delivery.eventType);
        headers.emplace_back("X-IOT-Timestamp", requestTimestamp);
        headers.emplace_back("X-IOT-Delivery", delivery.id);
        if (!target.secret.empty())
            headers.emplace_back("X-IOT-Signature",
                                 "sha256=" + hmacSha256(target.secret, delivery.body));

        WebhookHttpResponse response;
        try {
            const auto url = parseWebhookUrl(target.url);
            auto [completion, receiver] = ruvia::makeOneShot<WebhookHttpResponse>(context.worker());
            auto shared = std::make_shared<ruvia::OneShotCompletion<WebhookHttpResponse>>(
                std::move(completion));
            http_.post(url, WebhookHttpClient::request(url, delivery.body, headers),
                       std::chrono::seconds(target.timeout),
                       [shared](WebhookHttpResponse result) mutable {
                           (void)shared->complete(std::move(result));
                       });
            const auto outcome = co_await receiver.wait();
            if (outcome.value())
                response = *outcome.value();
            else
                response.error = "Webhook request was cancelled";
        } catch (const std::exception& error) {
            response.error = error.what();
        }
        const bool success =
            response.error.empty() && response.status >= 200 && response.status < 300;
        if (!success && response.error.empty())
            response.error =
                "HTTP " + std::to_string(response.status) + " " + sanitize(response.body, 500);
        co_await enqueueResult(context, target, delivery, response, success);
    }

    static std::vector<std::pair<std::string, std::string>> parseHeaders(std::string_view json) {
        std::vector<std::pair<std::string, std::string>> result;
        (void)ruvia::detail::visitJsonObjectFields(
            ruvia::detail::ResolvedPmrResourceTag{}, json, std::pmr::get_default_resource(),
            [&](std::string_view name, std::string_view raw) {
                auto input = raw;
                const auto value = ruvia::detail::parseJsonValue<ruvia::String>(
                    input, std::pmr::get_default_resource());
                if (value)
                    result.emplace_back(name, value->view());
                return true;
            });
        return result;
    }

    static ruvia::Task<void> enqueueResult(ruvia::WebWorkerContext& context,
                                           const Target& target,
                                           const Delivery& delivery,
                                           const WebhookHttpResponse& response, bool success) {
        const auto error = sanitize(response.error, 1000);
        const auto status = success ? "success" : "failed";
        const auto logId = service::common::nextUuidV7();
        const auto responseJson = "{\"httpStatus\":" + std::to_string(response.status) +
                                  ",\"body\":" + jsonQuoted(sanitize(response.body, 2000)) +
                                  (error.empty() ? "" : ",\"error\":" + jsonQuoted(error)) + "}";
        static constexpr std::string_view script = R"lua(
if redis.call('HEXISTS', KEYS[2], ARGV[1]) ~= 0 then return false end
local arguments = {'MAXLEN', '~', ARGV[2], '*'}
for index = 4, #ARGV do arguments[#arguments + 1] = ARGV[index] end
local id = redis.call('XADD', KEYS[1], unpack(arguments))
redis.call('HSET', KEYS[2], ARGV[1], '1')
redis.call('EXPIRE', KEYS[2], ARGV[3])
return id
)lua";
        const std::vector<std::string> keyStore{std::string(kDeliveryResultStream),
                                                deliveryProgressKey(delivery)};
        const std::vector<std::string> argumentStore{
            target.id,
            "100000",
            std::to_string(kDeliveryProgressTtlSeconds),
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
        const std::vector<std::string_view> arguments(argumentStore.begin(),
                                                       argumentStore.end());
        const auto reply = co_await context.redis().eval(script, keys, arguments);
        if (!reply.null() && reply.kind() != ruvia::RedisValue::Kind::kString)
            message::redis::throwValue("enqueue webhook result", reply);
    }

    static ruvia::Task<void> persistResults(
        ruvia::WebWorkerContext& context,
        const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        std::string sql = R"sql(
WITH incoming(
  sequence, log_id, access_key_id, webhook_id, event_type, status, target,
  http_status, device_id, device_code, message, request_payload,
  response_payload, completed_at_ms) AS (VALUES )sql";
        std::vector<ruvia::DbValue> params;
        params.reserve(messages.size() * 14);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (index != 0)
                sql.push_back(',');
            const auto base = params.size() + 1;
            sql += "($" + std::to_string(base) + "::bigint,$" +
                   std::to_string(base + 1) + "::uuid,$" +
                   std::to_string(base + 2) + "::uuid,$" +
                   std::to_string(base + 3) + "::uuid,$" +
                   std::to_string(base + 4) + "::text,$" +
                   std::to_string(base + 5) + "::text,$" +
                   std::to_string(base + 6) + "::text,$" +
                   std::to_string(base + 7) + "::bigint,$" +
                   std::to_string(base + 8) + "::uuid,$" +
                   std::to_string(base + 9) + "::text,$" +
                   std::to_string(base + 10) + "::text,$" +
                   std::to_string(base + 11) + "::jsonb,$" +
                   std::to_string(base + 12) + "::jsonb,$" +
                   std::to_string(base + 13) + "::bigint)";
            const auto httpStatus = service::common::parseInt64(
                std::optional<std::string_view>(messages[index].get("http_status")));
            const auto completedAt = service::common::parseInt64(
                std::optional<std::string_view>(messages[index].get("completed_at_ms")));
            params.emplace_back(static_cast<std::int64_t>(index));
            params.emplace_back(messages[index].get("log_id"));
            params.emplace_back(messages[index].get("access_key_id"));
            params.emplace_back(messages[index].get("webhook_id"));
            params.emplace_back(messages[index].get("event_type"));
            params.emplace_back(messages[index].get("status"));
            params.emplace_back(messages[index].get("target"));
            params.emplace_back(httpStatus.value_or(0));
            params.emplace_back(messages[index].get("device_id"));
            params.emplace_back(messages[index].get("device_code"));
            params.emplace_back(messages[index].get("message"));
            params.emplace_back(messages[index].get("request_payload"));
            params.emplace_back(messages[index].get("response_payload"));
            params.emplace_back(completedAt.value_or(service::message::utcNowMilliseconds()));
        }
        sql += R"sql(), latest AS (
  SELECT DISTINCT ON (webhook_id) *
  FROM incoming
  ORDER BY webhook_id, completed_at_ms DESC, sequence DESC
), summary AS (
  SELECT webhook_id,
         MAX(completed_at_ms) FILTER (WHERE status = 'success') AS last_success_ms,
         MAX(completed_at_ms) FILTER (WHERE status = 'failed') AS last_failure_ms
  FROM incoming GROUP BY webhook_id
), updated AS (
  UPDATE open_webhook webhook
  SET last_triggered_at = GREATEST(
        COALESCE(webhook.last_triggered_at, to_timestamp(0)),
        to_timestamp(latest.completed_at_ms::double precision / 1000.0)),
      last_success_at = CASE WHEN summary.last_success_ms IS NULL
        THEN webhook.last_success_at ELSE GREATEST(
          COALESCE(webhook.last_success_at, to_timestamp(0)),
          to_timestamp(summary.last_success_ms::double precision / 1000.0)) END,
      last_failure_at = CASE WHEN summary.last_failure_ms IS NULL
        THEN webhook.last_failure_at ELSE GREATEST(
          COALESCE(webhook.last_failure_at, to_timestamp(0)),
          to_timestamp(summary.last_failure_ms::double precision / 1000.0)) END,
      last_http_status = CASE
        WHEN webhook.last_triggered_at IS NULL OR webhook.last_triggered_at <=
             to_timestamp(latest.completed_at_ms::double precision / 1000.0)
        THEN NULLIF(latest.http_status, 0) ELSE webhook.last_http_status END,
      last_error = CASE
        WHEN webhook.last_triggered_at IS NULL OR webhook.last_triggered_at <=
             to_timestamp(latest.completed_at_ms::double precision / 1000.0)
        THEN NULLIF(latest.message, '') ELSE webhook.last_error END,
      updated_at = NOW()
  FROM summary JOIN latest USING (webhook_id)
  WHERE webhook.id = summary.webhook_id AND webhook.deleted_at IS NULL
  RETURNING webhook.id
)
INSERT INTO open_access_log(
  id, access_key_id, webhook_id, direction, action, event_type, status,
  http_method, target, http_status, device_id, device_code, message,
  request_payload, response_payload)
SELECT log_id, access_key_id, webhook_id, 'push', 'webhook', event_type, status,
       'POST', target, NULLIF(http_status, 0), device_id, NULLIF(device_code, ''),
       NULLIF(message, ''), request_payload, response_payload
FROM incoming
ON CONFLICT (id) DO NOTHING)sql";
        (void)co_await context.db().execute(sql, params);
    }

    static ruvia::Task<void> persistAudits(
        ruvia::WebWorkerContext& context,
        const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        std::string sql = R"sql(
WITH incoming(
  sequence, log_id, access_key_id, action, http_method, target, request_ip,
  http_status, device_id, request_payload, response_payload, used_at_ms) AS (VALUES )sql";
        std::vector<ruvia::DbValue> params;
        params.reserve(messages.size() * 12);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (index != 0)
                sql.push_back(',');
            const auto base = params.size() + 1;
            sql += "($" + std::to_string(base) + "::bigint,$" +
                   std::to_string(base + 1) + "::uuid,$" +
                   std::to_string(base + 2) + "::uuid,$" +
                   std::to_string(base + 3) + "::text,$" +
                   std::to_string(base + 4) + "::text,$" +
                   std::to_string(base + 5) + "::text,$" +
                   std::to_string(base + 6) + "::text,$" +
                   std::to_string(base + 7) + "::integer,NULLIF($" +
                   std::to_string(base + 8) + ", '')::uuid,$" +
                   std::to_string(base + 9) + "::jsonb,$" +
                   std::to_string(base + 10) + "::jsonb,$" +
                   std::to_string(base + 11) + "::bigint)";
            const auto httpStatus = service::common::parseInt64(
                std::optional<std::string_view>(messages[index].get("http_status")));
            const auto usedAt = service::common::parseInt64(
                std::optional<std::string_view>(messages[index].get("used_at_ms")));
            params.emplace_back(static_cast<std::int64_t>(index));
            params.emplace_back(messages[index].get("log_id"));
            params.emplace_back(messages[index].get("access_key_id"));
            params.emplace_back(messages[index].get("action"));
            params.emplace_back(messages[index].get("http_method"));
            params.emplace_back(messages[index].get("target"));
            params.emplace_back(messages[index].get("request_ip"));
            params.emplace_back(httpStatus.value_or(0));
            params.emplace_back(messages[index].get("device_id"));
            params.emplace_back(messages[index].get("request_payload"));
            params.emplace_back(messages[index].get("response_payload"));
            params.emplace_back(usedAt.value_or(service::message::utcNowMilliseconds()));
        }
        sql += R"sql(), latest_usage AS MATERIALIZED (
  SELECT DISTINCT ON (access_key_id)
         access_key_id, request_ip, used_at_ms
  FROM incoming
  ORDER BY access_key_id, used_at_ms DESC, sequence DESC
), usage_updated AS (
  UPDATE open_access_key key
  SET last_used_at = to_timestamp(latest.used_at_ms::double precision / 1000.0),
      last_used_ip = NULLIF(latest.request_ip, '')
  FROM latest_usage latest
  WHERE key.id = latest.access_key_id
    AND (key.last_used_at IS NULL OR key.last_used_at <=
         to_timestamp(latest.used_at_ms::double precision / 1000.0))
  RETURNING key.id
)
INSERT INTO open_access_log(
  id, access_key_id, direction, action, status, http_method, target,
  request_ip, http_status, device_id, request_payload, response_payload)
SELECT incoming.log_id, incoming.access_key_id, 'pull', incoming.action, 'success',
       NULLIF(incoming.http_method, ''), NULLIF(incoming.target, ''),
       NULLIF(incoming.request_ip, ''), NULLIF(incoming.http_status, 0),
       incoming.device_id, incoming.request_payload, incoming.response_payload
FROM incoming
CROSS JOIN (SELECT count(*) AS updated_count FROM usage_updated) update_barrier
WHERE update_barrier.updated_count >= 0
ON CONFLICT (id) DO NOTHING)sql";
        (void)co_await context.db().execute(sql, params);
    }

    WebhookHttpClient http_;
    Catalog catalog_;
    std::shared_future<void> stopped_;
    std::atomic_bool running_{false};
};

} // namespace service::access
