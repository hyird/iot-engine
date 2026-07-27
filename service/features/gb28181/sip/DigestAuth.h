#pragma once

#include "sip/SipMessage.h"

#include <string>

class DigestAuth {
public:
    static std::string makeNonce();
    static bool verifyRegister(const SipMessage& message, const std::string& realm, const std::string& password);
};
