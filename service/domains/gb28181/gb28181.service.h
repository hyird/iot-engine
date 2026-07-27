#pragma once

#include <future>
#include <string>
#include <string_view>

#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/domains/gb28181/gb28181.types.h"
#include "service/features/gb28181/runtime.h"

namespace service::gb28181 {

class Gb28181Service final {
  public:
    [[nodiscard]] std::string health() const {
        const auto& instance = runtime();
        return "{\"status\":" +
               jsonQuoted(instance.started() ? "ok"
                                             : (instance.enabled() ? "error" : "disabled")) +
               ",\"service\":\"iot-engine-gb28181\",\"enabled\":" +
               (instance.enabled() ? "true" : "false") + ",\"started\":" +
               (instance.started() ? "true" : "false") + ",\"error\":" +
               jsonQuoted(instance.lastError()) + "}";
    }

    [[nodiscard]] std::string sipConfig() const {
        const auto& config = runtime().config().sip;
        return "{\"domain\":" + jsonQuoted(config.domain) + ",\"id\":" +
               jsonQuoted(config.id) + ",\"host\":" + jsonQuoted(config.host) +
               ",\"public_ip\":" + jsonQuoted(config.publicIp) +
               ",\"port\":" + std::to_string(config.port) +
               ",\"transport\":" + jsonQuoted(config.transport) + "}";
    }

    [[nodiscard]] std::string devices() const {
        const auto values = runtime().devices();
        std::string result{"{\"items\":["};
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index)
                result.push_back(',');
            result += deviceJson(values[index]);
        }
        result += "]}";
        return result;
    }

    [[nodiscard]] std::string device(std::string_view id) const {
        const auto value = runtime().device(id);
        if (!value)
            service::common::fail(10003, "设备不存在", 404);
        return deviceJson(*value);
    }

    [[nodiscard]] std::string streams() const {
        const auto values = runtime().streams();
        std::string result{"{\"items\":["};
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index)
                result.push_back(',');
            result += streamJson(values[index]);
        }
        result += "]}";
        return result;
    }

    [[nodiscard]] std::string stream(std::string_view id) const {
        const auto value = runtime().stream(id);
        if (!value)
            service::common::fail(10003, "流不存在", 404);
        return streamJson(*value);
    }

    void mockRegister(std::string deviceId, std::string remoteAddress) const {
        runtime().mockRegister(std::move(deviceId), std::move(remoteAddress));
    }

    void queryCatalog(std::string_view deviceId) const {
        if (!runtime().queryCatalog(deviceId))
            service::common::fail(10003, "设备不在线", 404);
    }

    void queryRecords(std::string_view deviceId, std::string_view channelId,
                      std::string_view startTime, std::string_view endTime) const {
        if (!runtime().queryRecords(deviceId, channelId, startTime, endTime))
            service::common::fail(10003, "设备或通道不可用", 404);
    }

    void ptz(std::string_view deviceId, std::string_view channelId, std::string_view action,
             std::uint8_t speed) const {
        if (!runtime().ptz(deviceId, channelId, action, speed))
            service::common::fail(10003, "设备或通道不可用", 404);
    }

    void ptzPosition(std::string_view deviceId, std::string_view channelId, double pan,
                     double tilt, double zoom) const {
        if (!runtime().ptzPosition(deviceId, channelId, pan, tilt, zoom))
            service::common::fail(10003, "设备或通道不可用", 404);
    }

    void authorizeHook(const ruvia::JsonValue& payload) const {
        const auto& expected = runtime().config().media.zlmSecret;
        if (expected.empty())
            return;
        const auto supplied = payload.get<ruvia::String>("secret");
        if (!supplied || supplied->view() != expected)
            service::common::fail(11004, "ZLM hook secret 无效", 401);
    }

    void streamChanged(const ruvia::JsonValue& payload) const {
        authorizeHook(payload);
        const auto stream = stringField(payload, "stream");
        if (stream.empty())
            return;
        bool online = true;
        if (const auto value = payload.get<ruvia::Bool>("regist"))
            online = static_cast<bool>(*value);
        else if (const auto value = payload.get<ruvia::Int64>("regist"))
            online = static_cast<std::int64_t>(*value) != 0;
        runtime().streamChanged(stringField(payload, "app"), stream,
                                stringField(payload, "schema"), online);
    }

    void streamNoneReader(const ruvia::JsonValue& payload) const {
        authorizeHook(payload);
        runtime().streamNoneReader(stringField(payload, "app"), stringField(payload, "stream"),
                                   stringField(payload, "schema"));
    }

    [[nodiscard]] static std::string stringField(const ruvia::JsonValue& payload,
                                                 std::string_view name) {
        const auto value = payload.get<ruvia::String>(name);
        return value ? std::string(value->view()) : std::string{};
    }

    [[nodiscard]] static bool isManagedStream(std::string_view id) {
        return id.starts_with("gb_") || id.starts_with("gb_playback_");
    }
};

inline Gb28181Service& gb28181Service() {
    static Gb28181Service value;
    return value;
}

} // namespace service::gb28181
