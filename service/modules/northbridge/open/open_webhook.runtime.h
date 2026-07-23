#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <functional>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <optional>
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
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/modules/northbridge/open/open_access.common.h"
#include "service/modules/northbridge/open/open_access.event.h"
#include "service/modules/northbridge/open/open_access.service.h"
#include "service/modules/southbridge/queue/redis_stream_async.h"

namespace service::open_access {

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

class OpenWebhookRuntime final {
  public:
    OpenWebhookRuntime() = default;
    OpenWebhookRuntime(const OpenWebhookRuntime&) = delete;
    OpenWebhookRuntime& operator=(const OpenWebhookRuntime&) = delete;
    ~OpenWebhookRuntime() { stop(); }

    void start(const std::vector<ruvia::WebWorkerHandle>& workers) {
        if (running_.exchange(true))
            return;
        if (workers.empty()) {
            running_.store(false);
            throw std::runtime_error("open webhook runtime requires a north worker");
        }
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        auto readiness = ready->get_future();
        stopped_ = stopped->get_future().share();
        const auto posted =
            workers.front().post([this, ready, stopped](ruvia::WebWorkerContext& context) {
                return run(context, ready, stopped);
            });
        if (!posted.accepted()) {
            running_.store(false);
            throw std::runtime_error("north worker rejected open webhook runtime");
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

    struct Target final {
        std::string id;
        std::string accessKeyId;
        std::string url;
        std::string secret;
        std::string headers;
        std::string deviceName;
        std::int64_t timeout{5};
    };

    struct Delivery final {
        std::string id;
        std::string eventType;
        std::string deviceId;
        std::string deviceCode;
        std::string occurredAt;
        std::string body;
    };

    ruvia::Task<void> run(ruvia::WebWorkerContext& context,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            co_await bridge::redis_async::ensureGroup(redis, event::kStream, kGroup);
            ready->set_value();
            bool recovering = true;
            while (running_.load() && !context.stopToken().stopRequested()) {
                const auto messages = co_await bridge::redis_async::readGroup(
                    redis, event::kStream, kGroup, "north-0", recovering ? "0" : ">",
                    std::chrono::milliseconds(0), 50);
                if (recovering && messages.empty())
                    recovering = false;
                if (messages.empty()) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::milliseconds(20));
                    continue;
                }
                bool failed = false;
                try {
                    for (const auto& message : messages) {
                        co_await deliver(context, message);
                        co_await bridge::redis_async::acknowledgeAndDelete(redis, event::kStream,
                                                                           kGroup, message.id);
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

    ruvia::Task<void> deliver(ruvia::WebWorkerContext& context,
                              const bridge::StreamMessage& message) {
        const auto eventType = message.get("event_type");
        const auto deviceId = message.get("device_id");
        if (!supportedEvent(eventType) || !service::common::isUuid(deviceId))
            co_return;
        const std::string deviceIdValue(deviceId);
        const std::string eventTypeValue(eventType);
        const auto rows =
            co_await context.db().query(R"sql(
SELECT webhook.id::text, webhook.access_key_id::text, webhook.url,
       COALESCE(webhook.secret, ''), webhook.headers::text, webhook.timeout_seconds,
       device.name
FROM open_webhook webhook
JOIN open_access_key key ON key.id = webhook.access_key_id
JOIN open_access_key_device binding ON binding.access_key_id = key.id
JOIN device ON device.id = binding.device_id
WHERE webhook.deleted_at IS NULL AND webhook.status = 'enabled'
  AND key.deleted_at IS NULL AND key.status = 'enabled'
  AND (key.expires_at IS NULL OR key.expires_at > NOW())
  AND device.deleted_at IS NULL
  AND binding.device_id = $1::uuid AND webhook.event_types ? $2
ORDER BY webhook.id)sql",
                                        service::common::dbParams(deviceIdValue, eventTypeValue));
        std::vector<Target> targets;
        targets.reserve(rows.rows().size());
        for (const auto& row : rows.rows())
            targets.push_back({std::string(row[0].text()),
                               std::string(row[1].text()),
                               std::string(row[2].text()),
                               std::string(row[3].text()),
                               std::string(row[4].text()),
                               std::string(row[6].text()),
                               std::stoll(std::string(row[5].text()))});
        if (targets.empty())
            co_return;
        const auto delivery = co_await buildDelivery(context, message, targets.front().deviceName);
        for (const auto& target : targets)
            co_await deliverTarget(context, target, delivery);
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

    static ruvia::Task<Delivery> buildDelivery(ruvia::WebWorkerContext& context,
                                                const bridge::StreamMessage& message,
                                                std::string_view deviceName) {
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
            deviceReference(delivery.deviceId, delivery.deviceCode, deviceName);
        const auto rawData = message.get("data_json");
        std::string data;
        if (delivery.eventType == "device.data.reported") {
            data = co_await openAccessService().realtimeData(context, delivery.deviceId);
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
        co_await record(context, target, delivery.eventType, delivery.deviceId,
                        delivery.deviceCode, delivery.body, response, success);
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

    static ruvia::Task<void> record(ruvia::WebWorkerContext& context, const Target& target,
                                    const std::string& eventType, const std::string& deviceId,
                                    const std::string& deviceCode, const std::string& requestBody,
                                    const WebhookHttpResponse& response, bool success) {
        const auto error = sanitize(response.error, 1000);
        const auto status = success ? "success" : "failed";
        const auto logId = service::common::nextUuidV7();
        const auto responseJson = "{\"httpStatus\":" + std::to_string(response.status) +
                                  ",\"body\":" + jsonQuoted(sanitize(response.body, 2000)) +
                                  (error.empty() ? "" : ",\"error\":" + jsonQuoted(error)) + "}";
        auto transaction = co_await context.db().beginTransaction();
        (void)co_await transaction.execute(
            R"sql(
UPDATE open_webhook
SET last_triggered_at = NOW(),
    last_success_at = CASE WHEN $2 = 'success' THEN NOW() ELSE last_success_at END,
    last_failure_at = CASE WHEN $2 = 'failed' THEN NOW() ELSE last_failure_at END,
    last_http_status = NULLIF($3, 0), last_error = NULLIF($4, ''), updated_at = NOW()
WHERE id = $1::uuid AND deleted_at IS NULL)sql",
            service::common::dbParams(target.id, status, response.status, error));
        (void)co_await transaction.execute(
            R"sql(
INSERT INTO open_access_log(
  id, access_key_id, webhook_id, direction, action, event_type, status,
  http_method, target, http_status, device_id, device_code, message,
  request_payload, response_payload)
VALUES ($1::uuid, $2::uuid, $3::uuid, 'push', 'webhook', $4, $5,
        'POST', $6, NULLIF($7, 0), $8::uuid, NULLIF($9, ''), NULLIF($10, ''),
        $11::jsonb, $12::jsonb))sql",
            service::common::dbParams(logId, target.accessKeyId, target.id, eventType, status,
                                      target.url, response.status, deviceId, deviceCode, error,
                                      requestBody, responseJson));
        co_await transaction.commit();
    }

    WebhookHttpClient http_;
    std::shared_future<void> stopped_;
    std::atomic_bool running_{false};
};

} // namespace service::open_access
