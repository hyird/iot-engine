#pragma once
#include <optional>
#include <string>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/modules/protocol/protocol.types.h"
#include "service/utils/json.h"

namespace service::protocol {
class ProtocolListQueryValidator final : public ruvia::Middleware<ProtocolListQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(ProtocolListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")), RUVIA_RULE_NAME("pageSize", pageSize, RUVIA_MIN(1, "pageSize 必须在 1 - 1000 之间"), RUVIA_MAX(1000, "pageSize 必须在 1 - 1000 之间")), RUVIA_RULE(protocol, RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7", "MC", "FINS", "DLT645")))
};

class ProtocolIdParamsValidator final : public ruvia::Middleware<ProtocolIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(ProtocolIdParams, RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"), RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

// Decode only the request shape here. Protocol-specific configuration rules
// depend on the persisted protocol during an update and belong to the service.
class ProtocolRequestFields final {
  public:
    static std::optional<std::string> text(const ruvia::JsonValue& object, std::string_view name, std::size_t maximum) {
        const auto raw = service::utils::jsonField(object, name);
        if (!raw) {
            return std::nullopt;
        }
        const auto value = object.get<ruvia::String>(name);
        if (!value) {
            service::common::fail(16002, std::string(name) + " 必须是字符串", 400);
        }
        if (value->view().size() > maximum) {
            service::common::fail(16002, std::string(name) + " 长度超出限制", 400);
        }
        return std::string(value->view());
    }

    static std::optional<std::string> remark(const ruvia::JsonValue& object) {
        const auto raw = service::utils::jsonField(object, "remark");
        if (!raw || raw->isNull()) {
            return std::nullopt;
        }
        if (!raw->isString()) {
            service::common::fail(16002, "remark 必须是字符串或 null", 400);
        }
        auto value = text(object, "remark", 500);
        return value && !value->empty() ? value : std::nullopt;
    }

    static std::optional<bool> enabled(const ruvia::JsonValue& object) {
        const auto raw = service::utils::jsonField(object, "enabled");
        if (!raw) {
            return std::nullopt;
        }
        if (!raw->isBoolean()) {
            service::common::fail(16004, "enabled 必须是布尔值", 400);
        }
        return static_cast<bool>(*object.get<ruvia::Bool>("enabled"));
    }
};

class CreateProtocolValidator final {
  public:
    static CreateProtocolBody parse(const ruvia::JsonValue& object) {
        if (!object.isObject()) {
            service::common::fail(16002, "请求体必须是对象", 400);
        }
        const auto protocol = ProtocolRequestFields::text(object, "protocol", 16);
        const auto name = ProtocolRequestFields::text(object, "name", 64);
        if (!protocol || protocol->empty()) {
            service::common::fail(16002, "协议不能为空", 400);
        }
        if (!name || name->empty()) {
            service::common::fail(16002, "配置名称不能为空", 400);
        }
        return CreateProtocolBody{ *protocol, *name, ProtocolRequestFields::remark(object), ProtocolRequestFields::enabled(object).value_or(true), service::utils::jsonField(object, "config") };
    }
};

class UpdateProtocolValidator final {
  public:
    static UpdateProtocolBody parse(const ruvia::JsonValue& object) {
        if (!object.isObject()) {
            service::common::fail(16002, "请求体必须是对象", 400);
        }

        return UpdateProtocolBody{ ProtocolRequestFields::text(object, "protocol", 16), ProtocolRequestFields::text(object, "name", 64), ProtocolRequestFields::remark(object), service::utils::jsonField(object, "remark").has_value(), ProtocolRequestFields::enabled(object), service::utils::jsonField(object, "config") };
    }
};
} // namespace service::protocol
