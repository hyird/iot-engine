#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <memory_resource>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <ruvia/web/ConnInfo.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelObject.h>

#include "service/common/http.h"
#include "service/common/uuid.h"

namespace service::access {

inline constexpr std::string_view kScopeRealtime = "device:realtime";
inline constexpr std::string_view kScopeHistory = "device:history";
inline constexpr std::string_view kScopeCommand = "device:command";
inline constexpr std::string_view kScopeAlert = "alert:read";

inline constexpr std::array<std::string_view, 6> kWebhookEvents{
    "device.data.reported",     "device.image.reported",  "device.command.dispatched",
    "device.command.responded", "device.alert.triggered", "device.alert.resolved",
};

struct Page final {
    std::int64_t page{1};
    std::int64_t pageSize{20};
    std::int64_t offset{0};
};

inline Page page(const ruvia::ContextRequest& request) {
    const auto integer = [&request](std::string_view name, std::int64_t fallback) {
        return service::common::parseInt64(request.query(name)).value_or(fallback);
    };
    Page result;
    result.page = std::max<std::int64_t>(1, integer("page", 1));
    result.pageSize = std::clamp<std::int64_t>(integer("pageSize", 20), 1, 100);
    result.offset = (result.page - 1) * result.pageSize;
    return result;
}

inline std::string trim(std::string_view input) {
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
        input.remove_prefix(1);
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
        input.remove_suffix(1);
    return std::string(input);
}

inline std::string jsonEscape(std::string_view value) {
    static constexpr std::array<char, 16> hex{'0', '1', '2', '3', '4', '5', '6', '7',
                                              '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string output;
    output.reserve(value.size() + 8);
    for (const auto byte : value) {
        const auto ch = static_cast<unsigned char>(byte);
        switch (ch) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20) {
                output += "\\u00";
                output.push_back(hex[ch >> 4U]);
                output.push_back(hex[ch & 0x0FU]);
            } else {
                output.push_back(static_cast<char>(ch));
            }
        }
    }
    return output;
}

inline std::string jsonQuoted(std::string_view value) { return "\"" + jsonEscape(value) + "\""; }

inline std::string webhookEnvelope(std::string_view eventType, std::string_view occurredAt,
                                   std::string_view deliveryId, std::string_view dataJson) {
    return "{\"event\":" + jsonQuoted(eventType) + ",\"time\":" + jsonQuoted(occurredAt) +
           ",\"deliveryId\":" + jsonQuoted(deliveryId) + ",\"data\":" +
           std::string(dataJson) + "}";
}

inline std::optional<ruvia::JsonValue> jsonField(const ruvia::JsonValue& object,
                                                 std::string_view field) {
    if (!object.isObject())
        return std::nullopt;
    std::optional<ruvia::JsonValue> result;
    const auto valid = ruvia::detail::visitJsonObjectFields(
        ruvia::detail::ResolvedPmrResourceTag{}, object.view(), std::pmr::get_default_resource(),
        [&](std::string_view key, std::string_view value) {
            if (key == field)
                result = ruvia::JsonValue::parse(value);
            return true;
        });
    return valid ? result : std::nullopt;
}

inline std::string hex(const unsigned char* data, std::size_t size) {
    static constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string output;
    output.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        output.push_back(digits[data[index] >> 4U]);
        output.push_back(digits[data[index] & 0x0FU]);
    }
    return output;
}

inline std::string sha256(std::string_view input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(input.data(), input.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("sha256 failed");
    return hex(digest.data(), length);
}

inline std::string hmacSha256(std::string_view secret, std::string_view input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data(),
             &length) == nullptr)
        throw std::runtime_error("hmac-sha256 failed");
    return hex(digest.data(), length);
}

inline std::string generateAccessKey() {
    std::array<unsigned char, 24> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        throw std::runtime_error("access-key random generation failed");
    return "ak_" + hex(random.data(), random.size());
}

inline std::string accessKey(const ruvia::ContextRequest& request) {
    return trim(request.header("X-Access-Key").value_or(""));
}

inline std::string clientIp(const ruvia::Context& context) {
    if (const auto value = context.req().header("X-Real-IP")) {
        const auto resolved = trim(*value);
        if (!resolved.empty())
            return resolved;
    }
    return std::string(ruvia::getConnInfo(context).remote().address());
}

inline std::string sanitize(std::string_view value, std::size_t maximum = 1000) {
    std::string output;
    output.reserve(std::min(maximum, value.size()));
    for (const auto ch : value) {
        if (output.size() == maximum)
            break;
        output.push_back(ch == '\n' || ch == '\r' ? ' ' : ch);
    }
    return trim(output);
}

inline std::string iso8601(std::int64_t milliseconds) {
    const auto seconds = milliseconds / 1000;
    const auto remainder = std::llabs(milliseconds % 1000);
    const auto time = static_cast<std::time_t>(seconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
           << remainder << 'Z';
    return output.str();
}

inline std::string nowIso8601() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return iso8601(now);
}

inline bool supportedScope(std::string_view value) {
    return value == kScopeRealtime || value == kScopeHistory || value == kScopeCommand ||
           value == kScopeAlert;
}

inline bool supportedEvent(std::string_view value) {
    return std::find(kWebhookEvents.begin(), kWebhookEvents.end(), value) != kWebhookEvents.end();
}

inline void requireUuid(std::string_view value, std::string_view message) {
    if (!service::common::isUuid(value))
        service::common::fail(19002, std::string(message), 400);
}

} // namespace service::access
