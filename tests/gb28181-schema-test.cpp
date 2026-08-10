#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/domains/gb28181/gb28181.schema.h"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

bool rejects(std::string_view value) {
    try {
        (void)service::gb28181::parsePtzSpeed(value);
        return false;
    } catch (...) {
        return true;
    }
}

void testPtzSpeedValidation() {
    require(service::gb28181::parsePtzSpeed(std::nullopt) == 80,
            "missing PTZ speed should use default");
    require(service::gb28181::parsePtzSpeed(std::string_view{"0"}) == 0,
            "PTZ speed 0 should be accepted");
    require(service::gb28181::parsePtzSpeed(std::string_view{"255"}) == 255,
            "PTZ speed 255 should be accepted");
    require(rejects("12x"), "PTZ speed with trailing bytes should be rejected");
    require(rejects("-1"), "PTZ speed below 0 should be rejected");
    require(rejects("256"), "PTZ speed above 255 should be rejected");
}

} // namespace

int main() {
    try {
        testPtzSpeedValidation();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "gb28181-schema-test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
