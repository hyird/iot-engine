#pragma once

#include <optional>
#include <string>
#include <unordered_map>

class SipMessage {
public:
    std::string startLine;
    std::string method;
    std::string requestUri;
    int statusCode{0};
    std::string reasonPhrase;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    static std::optional<SipMessage> parse(const std::string& raw);
    std::string header(const std::string& name) const;
};


#include <cstdint>

class DigestAuth {
public:
    struct Proof {
        std::string nonce;
        std::string username;
        std::uint32_t nonceCount{0};
    };

    static std::string makeNonce(const std::string& secret);
    static std::optional<Proof>
    verifyRegister(const SipMessage& message, const std::string& realm,
                   const std::string& password, const std::string& nonceSecret,
                   int nonceTtlSeconds);
};

#include <chrono>
#include <string_view>

class SipUnsupportedRequestGuard final {
public:
    using Clock = std::chrono::steady_clock;

    struct Decision {
        bool allowed{true};
        std::uint64_t suppressedToReport{0};
    };

    explicit SipUnsupportedRequestGuard(
        std::chrono::seconds reportInterval = std::chrono::seconds(30))
        : reportInterval_(reportInterval > std::chrono::seconds::zero()
                              ? reportInterval
                              : std::chrono::seconds(1)) {}

    [[nodiscard]] Decision inspect(std::string_view packet,
                                   Clock::time_point now = Clock::now()) {
        if (!requiresGuard(packet))
            return {};

        if (!initialized_) {
            initialized_ = true;
            lastReport_ = now;
        }

        ++suppressed_;
        if (now - lastReport_ < reportInterval_)
            return {.allowed = false};

        const auto report = suppressed_;
        suppressed_ = 0;
        lastReport_ = now;
        return {.allowed = false, .suppressedToReport = report};
    }

    [[nodiscard]] static bool requiresGuard(std::string_view packet) noexcept {
        const auto tokenEnd = packet.find_first_of(" \r\n\t");
        if (tokenEnd == std::string_view::npos || tokenEnd == 0)
            return true;
        const auto token = packet.substr(0, tokenEnd);
        return token == "INVITE";
    }

private:
    std::chrono::seconds reportInterval_;
    bool initialized_{false};
    Clock::time_point lastReport_{};
    std::uint64_t suppressed_{0};
};
