#include "sip/DigestAuth.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <array>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
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

std::string hmacSha256Hex(std::string_view secret, std::string_view payload) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength{};
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(payload.data()), payload.size(),
             digest.data(), &digestLength) == nullptr)
        return {};
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digestLength; ++index)
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    return output.str();
}

bool constantEqual(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
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

std::optional<std::int64_t> nonceTimestamp(std::string_view nonce,
                                           std::string_view secret) {
    const auto first = nonce.find('.');
    const auto second =
        first == std::string_view::npos ? std::string_view::npos
                                        : nonce.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos)
        return std::nullopt;
    const auto payload = nonce.substr(0, second);
    const auto signature = nonce.substr(second + 1);
    const auto expected = hmacSha256Hex(secret, payload);
    if (!constantEqual(signature, expected))
        return std::nullopt;
    std::int64_t timestamp{};
    const auto parsed =
        std::from_chars(nonce.data(), nonce.data() + first, timestamp, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != nonce.data() + first)
        return std::nullopt;
    return timestamp;
}

} // namespace

std::string DigestAuth::makeNonce(const std::string& secret) {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error("Unable to create SIP digest nonce");

    const auto timestamp =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    std::ostringstream payload;
    payload << std::hex << timestamp << '.';
    for (const auto byte : bytes)
        payload << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(byte);
    const auto value = payload.str();
    return value + "." + hmacSha256Hex(secret, value);
}

std::optional<DigestAuth::Proof>
DigestAuth::verifyRegister(const SipMessage& message, const std::string& realm,
                           const std::string& password,
                           const std::string& nonceSecret,
                           int nonceTtlSeconds) {
    const auto authorization = message.header("Authorization");
    if (authorization.empty() || nonceTtlSeconds <= 0)
        return std::nullopt;

    const auto params = parseDigestParams(authorization);
    const auto username = params.find("username");
    const auto response = params.find("response");
    const auto nonce = params.find("nonce");
    const auto uri = params.find("uri");

    if (username == params.end() || response == params.end() ||
        nonce == params.end() || uri == params.end() ||
        uri->second != message.requestUri)
        return std::nullopt;
    if (const auto suppliedRealm = params.find("realm");
        suppliedRealm != params.end() && suppliedRealm->second != realm)
        return std::nullopt;

    const auto issuedAt = nonceTimestamp(nonce->second, nonceSecret);
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (!issuedAt || *issuedAt > now + 5 || now - *issuedAt > nonceTtlSeconds)
        return std::nullopt;

    const auto ha1 = md5Hex(username->second + ":" + realm + ":" + password);
    const auto ha2 = md5Hex(message.method + ":" + uri->second);

    const auto qop = params.find("qop");
    std::string expected;
    std::uint32_t nonceCount = 1;
    if (qop != params.end()) {
        const auto nc = params.find("nc");
        const auto cnonce = params.find("cnonce");
        if (qop->second != "auth" || nc == params.end() || cnonce == params.end())
            return std::nullopt;
        const auto parsed = std::from_chars(nc->second.data(),
                                            nc->second.data() + nc->second.size(),
                                            nonceCount, 16);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != nc->second.data() + nc->second.size() ||
            nonceCount == 0)
            return std::nullopt;
        expected = md5Hex(ha1 + ":" + nonce->second + ":" + nc->second + ":" + cnonce->second + ":" + qop->second + ":" + ha2);
    } else {
        expected = md5Hex(ha1 + ":" + nonce->second + ":" + ha2);
    }

    if (!constantEqual(expected, response->second))
        return std::nullopt;
    return Proof{
        .nonce = nonce->second,
        .username = username->second,
        .nonceCount = nonceCount,
    };
}
