#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "service/modules/gb28181/gb28181.schema.h"

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Validator>
auto parseBody(std::string_view raw) {
    const auto value = ruvia::JsonValue::parse(raw);
    if (!value) {
        throw std::runtime_error("invalid fixture JSON");
    }
    if constexpr (std::is_same_v<Validator, service::gb28181::GbPtzValidator>) {
        return Validator::parse(*value, "device", "channel", service::gb28181::GbPayloadValidator::text(*value, "action"));
    } else if constexpr (std::is_same_v<Validator, service::gb28181::GbDeviceNameValidator> || std::is_same_v<Validator, service::gb28181::GbMappingValidator>) {
        return Validator::parse(*value, "device");
    } else {
        return Validator::parse(*value, "device", "channel");
    }
}

template <typename Validator>
bool accepts(std::string_view raw) {
    try {
        (void)parseBody<Validator>(raw);
        return true;
    } catch (const ruvia::HttpError&) {
        return false;
    }
}

void testPtzValidation() {
    using namespace service::gb28181;
    const auto base = std::string(R"({"deviceId":"device","channelId":"channel","action":"left")");
    const auto missing = parseBody<GbPtzValidator>(base + "}");
    require(missing.speed == 80, "missing speed uses 80");
    for (const auto raw : { "0", "255", "80" }) {
        require(accepts<GbPtzValidator>(base + ",\"speed\":" + raw + "}"), "valid integer speed");
    }
    for (const auto raw : { "-1", "256", "12.5", "\"80\"", "null", "true", "{}", "[]" }) {
        require(!accepts<GbPtzValidator>(base + ",\"speed\":" + raw + "}"), "invalid speed rejected");
    }
    require(!accepts<GbPtzValidator>(R"({"deviceId":"device","channelId":"channel","action":"arbitrary"})"), "unsupported PTZ action");
    require(accepts<GbPositionValidator>(R"({"deviceId":"device","channelId":"channel","pan":360,"tilt":-30,"zoom":1000})"), "position boundaries");
    for (const auto raw : { R"("pan":361,"tilt":0,"zoom":1)", R"("pan":0,"tilt":91,"zoom":1)", R"("pan":0,"tilt":0,"zoom":0)", R"("pan":"1","tilt":0,"zoom":1)", R"("pan":1e999,"tilt":0,"zoom":1)" }) {
        require(!accepts<GbPositionValidator>(std::string(R"({"deviceId":"device","channelId":"channel",)") + raw + "}"), "invalid position rejected");
    }
}

void testIdentityAndTimeValidation() {
    using namespace service::gb28181;
    const auto name = parseBody<GbDeviceNameValidator>(R"({"name":"  camera  "})");
    require(name.name == "camera", "name trims whitespace");
    require(!accepts<GbDeviceNameValidator>(R"({"deviceId":"device","name":" \t "})"), "blank name rejected");
    require(!accepts<GbMappingValidator>(R"({"deviceId":"device","mapped_device_id":"invalid"})"), "mapping requires UUID");
    const auto records = parseBody<GbRecordValidator>(R"({"start_time":"2026-01-01T08:00:00+08:00","end_time":"2026-01-01T09:00:00+08:00"})");
    require(records.start_time == "2026-01-01T00:00:00Z", "record time converts to UTC");
    require(!accepts<GbRecordValidator>(R"({"deviceId":"device","channelId":"channel","start_time":"invalid","end_time":"2026-01-01T00:00:00Z"})"), "invalid record date rejected");
}
} // namespace

int main() {
    try {
        testPtzValidation();
        testIdentityAndTimeValidation();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "gb28181-schema-test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
