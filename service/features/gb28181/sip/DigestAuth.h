#pragma once

#include "sip/SipMessage.h"

#include <cstdint>
#include <optional>
#include <string>

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
