#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/utils/base64.h"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void require_decode_error(std::string_view encoded, std::size_t limit) {
    try {
        (void)service::utils::decode_base64(encoded, limit);
    } catch (const service::utils::base64_decode_error&) {
        return;
    }
    throw std::runtime_error("Invalid Base64 input was accepted");
}

} // namespace

int main() {
    using service::utils::decode_base64;
    using service::utils::encode_base64;

    // RFC 4648 section 10 test vectors. Terminal input must contain at least one byte.
    const std::array<std::string_view, 7> plain{ "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    const std::array<std::string_view, 7> encoded{ "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    for (std::size_t index = 0; index < plain.size(); ++index) {
        require(encode_base64(plain[index]) == encoded[index], "Base64 encoding differs from RFC vector");
        if (!plain[index].empty()) {
            require(decode_base64(encoded[index], plain[index].size()) == plain[index], "Base64 decoding differs from RFC vector");
        }
    }

    for (const std::string_view input : { "", "Zg", "Zg=", "Zg===", "=Zg=", "====", "Zg==\n", "Z g=", "Zg-_", "Zh==", "Zm9=" }) {
        require_decode_error(input, 16384);
    }
    require_decode_error(std::string_view("Z\0==", 4), 16384);
    require_decode_error("Zg==", 0);
    require_decode_error("Zm9v", 2);
    require(decode_base64("Zm9v", std::numeric_limits<std::size_t>::max()) == "foo", "Maximum output limit overflowed");

    std::string binary;
    for (unsigned value = 0; value < 256; ++value) {
        binary.push_back(static_cast<char>(value));
    }
    require(decode_base64(encode_base64(binary), binary.size()) == binary, "Binary bytes did not round-trip");

    for (const std::size_t limit : { 4096U, 16384U }) {
        const std::string bytes(limit, '\xff');
        require(decode_base64(encode_base64(bytes), limit) == bytes, "Terminal input at byte limit was rejected");
        require_decode_error(encode_base64(bytes), limit - 1);
        require_decode_error(encode_base64(bytes + 'x'), limit);
    }
}
