#pragma once

#include <algorithm>
#include <array>
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <openssl/ssl.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace service::access::stream {

inline constexpr std::string_view kEventBase{ "iot:channel:open-access:event" };
inline constexpr std::string_view kAuditBase{ "iot:channel:open-access:audit" };
inline constexpr std::string_view kDeliveryResultBase{
    "iot:channel:open-access:delivery-result"
};
inline constexpr std::string_view kCatalogChangesBase{
    "iot:channel:open-access:config-change"
};
inline constexpr std::string_view kSessionChangesBase{
    "iot:channel:open-access:session-change"
};

inline std::string event() {
    return std::string(kEventBase);
}

inline std::string audit() {
    return std::string(kAuditBase);
}

inline std::string deliveryResult() {
    return std::string(kDeliveryResultBase);
}

inline std::string catalogChanges(std::size_t workerIndex) {
    return std::string(kCatalogChangesBase) + ":" + std::to_string(workerIndex);
}

inline std::string sessionChanges() {
    return std::string(kSessionChangesBase);
}

} // namespace service::access::stream

#include <array>
#include <vector>

#include <ruvia/core/Task.h>

#include "service/common/message.h"
#include "service/features/messaging/messaging.transport.h"

namespace service::access::event {

inline constexpr std::int64_t kPublicationTtlSeconds = 7 * 24 * 60 * 60;

inline std::string publicationKey(std::string_view eventId, std::string_view eventType) {
    return "iot:open-access:event:published:" + std::string(eventType) + ":" +
        std::string(eventId);
}

inline constexpr std::string_view kPublishScript = R"lua(
if redis.call('EXISTS', KEYS[2]) ~= 0 then return false end
local arguments = {'MAXLEN', '~', ARGV[1], '*'}
for index = 5, #ARGV do arguments[#arguments + 1] = ARGV[index] end
local id = redis.call('XADD', KEYS[1], unpack(arguments))
redis.call('SET', KEYS[2], '1', 'EX', ARGV[2])
redis.call('XADD', KEYS[3], 'MAXLEN', '~', ARGV[3], '*', 'task', ARGV[4])
return id
)lua";

template <typename Pipeline>
void queue(Pipeline& pipeline, std::string_view scriptSha, std::string_view eventId, std::string_view eventType, std::string_view deviceId, std::string_view deviceCode, std::int64_t occurredAtMs, std::string_view dataJson) {
    const auto publishedKey = publicationKey(eventId, eventType);
    const auto occurredAt = std::to_string(occurredAtMs);
    const auto outputStream = stream::event();
    const auto wakeStream = service::message::workerWakeStream(std::nullopt);
    const std::array<std::string_view, 3> keys{ outputStream, publishedKey, wakeStream };
    const std::array<std::string_view, 16> arguments{
        "100000",
        "604800",
        "100000",
        "webhook",
        "event_id",
        eventId,
        "event_type",
        eventType,
        "device_id",
        deviceId,
        "device_code",
        deviceCode,
        "occurred_at_ms",
        occurredAt,
        "data_json",
        dataJson,
    };
    message::redis::queueEvalSha(pipeline, scriptSha, keys, arguments);
}

template <typename Redis>
ruvia::Task<void> publish(const Redis& redis, std::string_view eventId, std::string_view eventType, std::string_view deviceId, std::string_view deviceCode, std::int64_t occurredAtMs, std::string_view dataJson) {
    const std::vector<std::string> keyStore{
        stream::event(),
        publicationKey(eventId, eventType),
        service::message::workerWakeStream(std::nullopt)
    };
    const std::vector<std::string> argumentStore{
        "100000",
        std::to_string(kPublicationTtlSeconds),
        std::to_string(service::message::kWorkerWakeCapacity),
        std::string(service::message::workerStreamTaskName(service::message::WorkerStreamTask::Webhook)),
        "event_id",
        std::string(eventId),
        "event_type",
        std::string(eventType),
        "device_id",
        std::string(deviceId),
        "device_code",
        std::string(deviceCode),
        "occurred_at_ms",
        std::to_string(occurredAtMs),
        "data_json",
        std::string(dataJson),
    };
    const std::vector<std::string_view> keys(keyStore.begin(), keyStore.end());
    const std::vector<std::string_view> arguments(argumentStore.begin(), argumentStore.end());
    const auto reply = co_await redis.eval(kPublishScript, keys, arguments);
    if (!reply.null() && reply.kind() != ruvia::RedisValue::Kind::kString) {
        message::redis::throwValue("publish open-access event", reply);
    }
}

template <typename Redis>
ruvia::Task<void> publishMany(
    const Redis& redis,
    const std::vector<message::ParsedDeviceMessage>& messages
) {
    if (messages.empty()) {
        co_return;
    }
    const auto scriptSha = co_await redis.scriptLoad(kPublishScript);
    auto pipeline = redis.pipeline();
    for (const auto& parsed : messages) {
        const auto eventType =
            parsed.eventKind == "image"
            ? "device.image.reported"
            : "device.data.reported";
        queue(pipeline, scriptSha, parsed.messageId, eventType, parsed.deviceId, parsed.deviceCode, parsed.observedAtMs, parsed.valuesJson);
    }
    const auto replies = co_await std::move(pipeline).exec();
    message::redis::requirePipelineSuccess("publish open-access events", replies);
}

} // namespace service::access::event

#include <cstdint>

#include "service/common/uuid.h"

namespace service::access::audit {

inline constexpr std::size_t kCapacity = 100000;

template <typename Redis>
ruvia::Task<void> publish(const Redis& redis, std::string_view action, std::string_view accessKeyId, std::string_view method, std::string_view target, std::string_view requestIp, std::int64_t httpStatus, std::string_view deviceId = {}, std::string_view requestPayload = "{}", std::string_view responsePayload = "{}") {
    const std::vector<service::message::StreamField> fields{
        { "log_id", service::common::nextUuidV7() },
        { "access_key_id", std::string(accessKeyId) },
        { "action", std::string(action) },
        { "http_method", std::string(method) },
        { "target", std::string(target) },
        { "request_ip", std::string(requestIp) },
        { "http_status", std::to_string(httpStatus) },
        { "device_id", std::string(deviceId) },
        { "request_payload", std::string(requestPayload) },
        { "response_payload", std::string(responsePayload) },
        { "used_at_ms", std::to_string(service::message::utcNowMilliseconds()) },
    };
    (void)co_await service::message::redis::addAndWake(
        redis,
        stream::audit(),
        fields,
        std::nullopt,
        service::message::WorkerStreamTask::Webhook,
        kCapacity
    );
}

} // namespace service::access::audit

namespace service::access {

struct WebhookHttpResponse final {
    std::int64_t status{ 0 };
    std::string body;
    std::string error;
};

struct WebhookUrl final {
    bool tls{ false };
    std::string host;
    std::string port;
    std::string target;
};

inline bool webhookHeaderNameEquals(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
           });
}

inline bool validWebhookHeaderSyntax(std::string_view name, std::string_view value) {
    if (name.empty()) {
        return false;
    }
    for (const auto ch : name) {
        const auto token = std::isalnum(static_cast<unsigned char>(ch)) || ch == '!' ||
            ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
            ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' ||
            ch == '_' || ch == '`' || ch == '|' || ch == '~';
        if (!token) {
            return false;
        }
    }
    return std::none_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte < 0x20 || byte == 0x7f;
    });
}

inline bool managedWebhookHeader(std::string_view name, bool includeApplicationHeaders) {
    static constexpr std::array<std::string_view, 11> transportHeaders{
        "host",
        "content-length",
        "transfer-encoding",
        "connection",
        "proxy-connection",
        "trailer",
        "te",
        "upgrade",
        "expect",
        "content-type",
        "user-agent"
    };
    if (std::any_of(transportHeaders.begin(), transportHeaders.end(), [&](auto reserved) {
            return webhookHeaderNameEquals(name, reserved);
        })) {
        return true;
    }
    if (!includeApplicationHeaders) {
        return false;
    }
    static constexpr std::array<std::string_view, 4> applicationHeaders{
        "x-iot-event",
        "x-iot-timestamp",
        "x-iot-delivery",
        "x-iot-signature"
    };
    return std::any_of(applicationHeaders.begin(), applicationHeaders.end(), [&](auto reserved) {
        return webhookHeaderNameEquals(name, reserved);
    });
}

inline void validateWebhookHeader(std::string_view name, std::string_view value, bool customHeader) {
    if (!validWebhookHeaderSyntax(name, value) || managedWebhookHeader(name, customHeader)) {
        throw std::invalid_argument("Webhook header is invalid or reserved");
    }
}

inline WebhookUrl parseWebhookUrl(std::string_view value) {
    if (value.find_first_of("\r\n") != std::string_view::npos ||
        value.find('#') != std::string_view::npos) {
        throw std::invalid_argument("Webhook URL contains invalid request characters");
    }
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
    if (result.target.starts_with('?')) {
        result.target.insert(result.target.begin(), '/');
    }
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        throw std::invalid_argument("Webhook URL authority is invalid");
    }
    if (authority.front() == '[') {
        const auto closing = authority.find(']');
        if (closing == std::string_view::npos) {
            throw std::invalid_argument("Webhook IPv6 host is invalid");
        }
        result.host = std::string(authority.substr(1, closing - 1));
        if (closing + 1 < authority.size()) {
            if (authority[closing + 1] != ':') {
                throw std::invalid_argument("Webhook URL port is invalid");
            }
            result.port = std::string(authority.substr(closing + 2));
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        result.host = std::string(authority.substr(0, colon));
        result.port = std::string(authority.substr(colon + 1));
    } else {
        result.host = std::string(authority);
    }
    if (result.host.empty() || result.port.empty()) {
        throw std::invalid_argument("Webhook URL host or port is invalid");
    }
    return result;
}

class WebhookHttpClient final {
  public:
    WebhookHttpClient()
        : tls_(asio::ssl::context::tls_client), work_(asio::make_work_guard(io_)),
          thread_([this] {
              io_.run();
          }) {
        tls_.set_default_verify_paths();
        tls_.set_verify_mode(asio::ssl::verify_peer);
    }

    ~WebhookHttpClient() {
        work_.reset();
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    WebhookHttpClient(const WebhookHttpClient&) = delete;
    WebhookHttpClient& operator=(const WebhookHttpClient&) = delete;

    void post(WebhookUrl url, std::string request, std::chrono::seconds timeout, bool skipTlsVerify, std::function<void(WebhookHttpResponse)> done) {
        asio::co_spawn(
            io_,
            [this, url = std::move(url), request = std::move(request), timeout, skipTlsVerify]() -> asio::awaitable<WebhookHttpResponse> {
                if (url.tls) {
                    co_return co_await exchangeTls(url, request, timeout, skipTlsVerify);
                }
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
            }
        );
    }

  private:
    template <typename Stream>
    static asio::awaitable<std::string> readResponse(Stream& stream) {
        std::string response;
        std::array<char, 8192> buffer{};
        while (response.size() < 65536) {
            std::error_code error;
            const auto size = co_await stream.async_read_some(
                asio::buffer(buffer),
                asio::redirect_error(asio::use_awaitable, error)
            );
            if (error == asio::error::eof || error == asio::ssl::error::stream_truncated ||
                size == 0) {
                break;
            }
            if (error) {
                throw std::system_error(error);
            }
            response.append(buffer.data(), std::min<std::size_t>(size, 65536 - response.size()));
        }
        co_return response;
    }

  public:
    static asio::ssl::verify_mode tlsVerifyMode(bool skipTlsVerify) noexcept {
        return skipTlsVerify ? asio::ssl::verify_none : asio::ssl::verify_peer;
    }

    static WebhookHttpResponse parseResponse(std::string response) {
        std::size_t offset = 0;
        for (int informational = 0; informational < 8; ++informational) {
            const auto body = response.find("\r\n\r\n", offset);
            if (body == std::string::npos) {
                throw std::runtime_error("Webhook returned incomplete HTTP headers");
            }
            const auto lineEnd = response.find("\r\n", offset);
            if (lineEnd == std::string::npos || lineEnd > body) {
                throw std::runtime_error("Webhook returned an invalid HTTP response");
            }
            const auto line = std::string_view(response).substr(offset, lineEnd - offset);
            const auto firstSpace = line.find(' ');
            const auto version = line.substr(0, firstSpace);
            if ((version != "HTTP/1.0" && version != "HTTP/1.1") ||
                firstSpace == std::string_view::npos || firstSpace + 4 > line.size() ||
                (line.size() > firstSpace + 4 && line[firstSpace + 4] != ' ')) {
                throw std::runtime_error("Webhook returned an invalid HTTP status");
            }
            const auto statusText = line.substr(firstSpace + 1, 3);
            if (!std::ranges::all_of(statusText, [](unsigned char character) {
                    return std::isdigit(character) != 0;
                })) {
                throw std::runtime_error("Webhook returned an invalid HTTP status");
            }
            const auto status = service::common::parseInt64(
                std::optional<std::string_view>(statusText)
            );
            if (!status || *status < 100 || *status > 599) {
                throw std::runtime_error("Webhook returned an invalid HTTP status");
            }
            if (*status >= 100 && *status < 200 && *status != 101) {
                offset = body + 4;
                continue;
            }
            WebhookHttpResponse result;
            result.status = *status;
            result.body = response.substr(body + 4, 8192);
            return result;
        }
        throw std::runtime_error("Webhook returned too many informational responses");
    }

  private:
    static std::string hostHeader(const WebhookUrl& url) {
        const bool defaultPort = (url.tls && url.port == "443") || (!url.tls && url.port == "80");
        const auto host = url.host.find(':') == std::string::npos ? url.host : '[' + url.host + ']';
        return host + (defaultPort ? "" : ":" + url.port);
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
    exchangeTls(const WebhookUrl& url, const std::string& request, std::chrono::seconds timeout, bool skipTlsVerify) {
        auto executor = co_await asio::this_coro::executor;
        using Stream = asio::ssl::stream<asio::ip::tcp::socket>;
        auto stream = std::make_shared<Stream>(executor, tls_);
        auto resolver = std::make_shared<asio::ip::tcp::resolver>(executor);
        if (SSL_set_tlsext_host_name(stream->native_handle(), url.host.c_str()) != 1) {
            throw std::runtime_error("Webhook TLS SNI setup failed");
        }
        stream->set_verify_mode(tlsVerifyMode(skipTlsVerify));
        if (!skipTlsVerify) {
            stream->set_verify_callback(asio::ssl::host_name_verification(url.host));
        }
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
    static std::string request(const WebhookUrl& url, std::string_view body, const std::vector<std::pair<std::string, std::string>>& headers) {
        std::string result = "POST " + url.target + " HTTP/1.1\r\nHost: " + hostHeader(url) +
            "\r\nUser-Agent: iot-engine-webhook/1.0\r\n";
        for (const auto& [name, value] : headers) {
            validateWebhookHeader(name, value, false);
            result += name + ": " + value + "\r\n";
        }
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

} // namespace service::access
