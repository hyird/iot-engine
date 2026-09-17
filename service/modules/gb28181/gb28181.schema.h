#pragma once
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/timestamp.h"
#include "service/modules/gb28181/gb28181.types.h"
#include "service/utils/json.h"
#include "service/utils/text.h"

namespace service::gb28181 {
class GbPayloadValidator final {
  public:
    static bool identifier(const ruvia::String& value) { return !service::utils::trim(value.view()).empty(); }

    static std::string text(const ruvia::JsonValue& body, std::string_view field, std::size_t maximum = 128) {
        const auto value = body.get<ruvia::String>(field);
        if (!value || value->empty() || value->size() > maximum) {
            service::common::fail(10001, std::string(field) + " 必须是有效的非空字符串", 400);
        }
        auto result = service::utils::trim(value->view());
        if (result.empty()) {
            service::common::fail(10001, std::string(field) + " 不能为空", 400);
        }
        return result;
    }

    static std::string timestamp(const ruvia::JsonValue& body, std::string_view field) {
        const auto value = service::common::canonicalUtcTimestamp(text(body, field));
        if (value.empty()) {
            service::common::fail(10001, std::string(field) + " 必须是有效的 RFC 3339 时间", 400);
        }
        return value;
    }

    static std::uint8_t speed(const ruvia::JsonValue& body) {
        if (!service::utils::jsonField(body, "speed")) {
            return 80;
        }
        const auto value = body.get<ruvia::Int64>("speed");
        if (!value || static_cast<std::int64_t>(*value) < 0 || static_cast<std::int64_t>(*value) > 255) {
            service::common::fail(10001, "speed 必须是 0 - 255 的整数", 400);
        }
        return static_cast<std::uint8_t>(static_cast<std::int64_t>(*value));
    }

    static double number(const ruvia::JsonValue& body, std::string_view field, double minimum, double maximum) {
        const auto value = body.get<ruvia::Double>(field);
        if (!value || !std::isfinite(static_cast<double>(*value)) || static_cast<double>(*value) < minimum || static_cast<double>(*value) > maximum) {
            service::common::fail(10001, std::string(field) + " 必须是允许范围内的有限数字", 400);
        }
        return static_cast<double>(*value);
    }
};

class GbDeviceValidator final : public ruvia::Middleware<GbDeviceValidator> {
  public:
    RUVIA_VALIDATE_PARAM(GbRouteParams, RUVIA_RULE(deviceId, RUVIA_REQUIRED("deviceId 不能为空"), RUVIA_MAX(128, "deviceId 长度超出限制"), RUVIA_CUSTOM("deviceId 不能为空白", GbPayloadValidator::identifier)))
};

class GbChannelValidator final : public ruvia::Middleware<GbChannelValidator> {
  public:
    RUVIA_VALIDATE_PARAM(GbRouteParams, RUVIA_RULE(deviceId, RUVIA_REQUIRED("deviceId 不能为空"), RUVIA_MAX(128, "deviceId 长度超出限制"), RUVIA_CUSTOM("deviceId 不能为空白", GbPayloadValidator::identifier)), RUVIA_RULE(channelId, RUVIA_REQUIRED("channelId 不能为空"), RUVIA_MAX(128, "channelId 长度超出限制"), RUVIA_CUSTOM("channelId 不能为空白", GbPayloadValidator::identifier)))
};

class GbStreamValidator final : public ruvia::Middleware<GbStreamValidator> {
  public:
    RUVIA_VALIDATE_PARAM(GbRouteParams, RUVIA_RULE(streamId, RUVIA_REQUIRED("streamId 不能为空"), RUVIA_MAX(128, "streamId 长度超出限制"), RUVIA_CUSTOM("streamId 不能为空白", GbPayloadValidator::identifier)))
};

class GbSessionValidator final : public ruvia::Middleware<GbSessionValidator> {
  public:
    RUVIA_VALIDATE_PARAM(GbRouteParams, RUVIA_RULE(sessionId, RUVIA_REQUIRED("sessionId 不能为空"), RUVIA_MAX(128, "sessionId 长度超出限制"), RUVIA_CUSTOM("sessionId 不能为空白", GbPayloadValidator::identifier)))
};

class GbPtzRouteValidator final : public ruvia::Middleware<GbPtzRouteValidator> {
  public:
    RUVIA_VALIDATE_PARAM(GbRouteParams, RUVIA_RULE(deviceId, RUVIA_REQUIRED("deviceId 不能为空"), RUVIA_MAX(128, "deviceId 长度超出限制"), RUVIA_CUSTOM("deviceId 不能为空白", GbPayloadValidator::identifier)), RUVIA_RULE(channelId, RUVIA_REQUIRED("channelId 不能为空"), RUVIA_MAX(128, "channelId 长度超出限制"), RUVIA_CUSTOM("channelId 不能为空白", GbPayloadValidator::identifier)), RUVIA_RULE(action, RUVIA_REQUIRED("action 不能为空"), RUVIA_MAX(128, "action 长度超出限制"), RUVIA_CUSTOM("action 不能为空白", GbPayloadValidator::identifier), RUVIA_ONE_OF("不支持的云台动作", "left", "right", "up", "down", "zoomin", "zoomout", "stop")))
};

class GbDeviceNameValidator final {
  public:
    static GbDeviceNameInput parse(const ruvia::JsonValue& object, std::string deviceId) {
        if (!object.isObject()) {
            service::common::fail(10001, "请求体必须是对象", 400);
        }
        GbDeviceNameInput result{ std::move(deviceId), GbPayloadValidator::text(object, "name", 255) };
        return result;
    }
};

class GbChannelNameValidator final {
  public:
    static GbChannelNameInput parse(const ruvia::JsonValue& object, std::string deviceId, std::string channelId) {
        if (!object.isObject()) {
            service::common::fail(10001, "请求体必须是对象", 400);
        }
        GbChannelNameInput result{ std::move(deviceId), std::move(channelId), GbPayloadValidator::text(object, "name", 255) };
        return result;
    }
};

class GbMappingValidator final {
  public:
    static GbMappingInput parse(const ruvia::JsonValue& object, std::string deviceId) {
        if (!object.isObject()) {
            service::common::fail(10001, "请求体必须是对象", 400);
        }
        GbMappingInput result{ std::move(deviceId), GbPayloadValidator::text(object, "mapped_device_id") };
        service::common::requireUuid(10001, result.mapped_device_id, "mapped_device_id 必须是 UUID");
        return result;
    }
};

class GbPtzValidator final {
  public:
    static GbPtzInput parse(const ruvia::JsonValue& object, std::string deviceId, std::string channelId, std::string action) {
        if (!object.isObject()) {
            service::common::fail(10001, "请求体必须是对象", 400);
        }
        GbPtzInput result{ std::move(deviceId), std::move(channelId), std::move(action), GbPayloadValidator::speed(object) };
        if (result.action != "left" && result.action != "right" && result.action != "up" && result.action != "down" && result.action != "zoomin" && result.action != "zoomout" && result.action != "stop") {
            service::common::fail(10001, "不支持的云台动作", 400);
        }
        return result;
    }
};

class GbPositionValidator final {
  public:
    static GbPositionInput parse(const ruvia::JsonValue& object, std::string deviceId, std::string channelId) {
        if (!object.isObject()) {
            service::common::fail(10001, "请求体必须是对象", 400);
        }
        GbPositionInput result{ std::move(deviceId), std::move(channelId), GbPayloadValidator::number(object, "pan", 0, 360), GbPayloadValidator::number(object, "tilt", -30, 90), GbPayloadValidator::number(object, "zoom", 1, 1000) };
        return result;
    }
};

class GbRecordValidator final {
  public:
    static GbRecordInput parse(const ruvia::JsonValue& object, std::string deviceId, std::string channelId) {
        if (!object.isObject()) {
            service::common::fail(10001, "请求体必须是对象", 400);
        }
        GbRecordInput result{ std::move(deviceId), std::move(channelId), GbPayloadValidator::timestamp(object, "start_time"), GbPayloadValidator::timestamp(object, "end_time") };
        return result;
    }
};

} // namespace service::gb28181
