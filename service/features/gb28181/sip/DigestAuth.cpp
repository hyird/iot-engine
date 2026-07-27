#include "sip/DigestAuth.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace {

std::string md5Hex(const std::string& input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength = 0;

    auto* context = EVP_MD_CTX_new();
    EVP_DigestInit_ex(context, EVP_md5(), nullptr);
    EVP_DigestUpdate(context, input.data(), input.size());
    EVP_DigestFinal_ex(context, digest.data(), &digestLength);
    EVP_MD_CTX_free(context);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLength; ++i) {
        output << std::setw(2) << static_cast<int>(digest[i]);
    }
    return output.str();
}

std::unordered_map<std::string, std::string> parseDigestParams(std::string value) {
    constexpr auto prefix = "Digest ";
    if (value.rfind(prefix, 0) == 0) {
        value.erase(0, std::char_traits<char>::length(prefix));
    }

    std::unordered_map<std::string, std::string> params;
    std::string key;
    std::string current;
    bool inQuote = false;

    auto flush = [&]() {
        const auto equal = current.find('=');
        if (equal == std::string::npos) {
            current.clear();
            return;
        }
        auto name = current.substr(0, equal);
        auto parsedValue = current.substr(equal + 1);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
            name.erase(name.begin());
        }
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
            name.pop_back();
        }
        if (parsedValue.size() >= 2 && parsedValue.front() == '"' && parsedValue.back() == '"') {
            parsedValue = parsedValue.substr(1, parsedValue.size() - 2);
        }
        params[name] = parsedValue;
        current.clear();
    };

    for (char c : value) {
        if (c == '"') {
            inQuote = !inQuote;
        }
        if (c == ',' && !inQuote) {
            flush();
            continue;
        }
        current.push_back(c);
    }
    flush();

    return params;
}

} // namespace

std::string DigestAuth::makeNonce() {
    std::array<unsigned char, 16> bytes{};
    RAND_bytes(bytes.data(), static_cast<int>(bytes.size()));

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        output << std::setw(2) << static_cast<int>(byte);
    }
    return output.str();
}

bool DigestAuth::verifyRegister(const SipMessage& message, const std::string& realm, const std::string& password) {
    const auto authorization = message.header("Authorization");
    if (authorization.empty()) {
        return false;
    }

    const auto params = parseDigestParams(authorization);
    const auto username = params.find("username");
    const auto response = params.find("response");
    const auto nonce = params.find("nonce");
    const auto uri = params.find("uri");

    if (username == params.end() || response == params.end() || nonce == params.end() || uri == params.end()) {
        return false;
    }

    const auto ha1 = md5Hex(username->second + ":" + realm + ":" + password);
    const auto ha2 = md5Hex(message.method + ":" + uri->second);

    const auto qop = params.find("qop");
    std::string expected;
    if (qop != params.end()) {
        const auto nc = params.find("nc");
        const auto cnonce = params.find("cnonce");
        if (nc == params.end() || cnonce == params.end()) {
            return false;
        }
        expected = md5Hex(ha1 + ":" + nonce->second + ":" + nc->second + ":" + cnonce->second + ":" + qop->second + ":" + ha2);
    } else {
        expected = md5Hex(ha1 + ":" + nonce->second + ":" + ha2);
    }

    return expected == response->second;
}
