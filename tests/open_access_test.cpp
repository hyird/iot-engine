#include <iostream>
#include <stdexcept>
#include <string>

#include "service/modules/northbridge/open/open_access.common.h"

namespace open = service::open_access;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    try {
        require(open::sha256("abc") ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA-256 contract changed");
        require(open::hmacSha256("key", "The quick brown fox jumps over the lazy dog") ==
                    "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
                "HMAC-SHA256 contract changed");
        require(open::jsonQuoted("a\n\"b") == "\"a\\n\\\"b\"", "JSON escaping contract changed");
        require(open::iso8601(0) == "1970-01-01T00:00:00.000Z", "UTC timestamp contract changed");
        const auto key = open::generateAccessKey();
        require(key.starts_with("ak_") && key.size() == 51, "AccessKey format contract changed");
        require(open::supportedScope("device:command") && !open::supportedScope("device:admin"),
                "scope allowlist contract changed");
        require(open::supportedEvent("device.data.reported") &&
                    !open::supportedEvent("device.deleted"),
                "event allowlist contract changed");
        std::cout << "open access tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "open access test failed: " << error.what() << '\n';
        return 1;
    }
}
