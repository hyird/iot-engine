#pragma once

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/modules/vpn/vpn.types.h"
#include "service/utils/json.h"

namespace service::vpn {

class VpnIdValidator final : public ruvia::Middleware<VpnIdValidator> {
  public:
    RUVIA_VALIDATE_PARAM(VpnIdParams, RUVIA_RULE(id, RUVIA_REQUIRED("VPN ID 不能为空"), RUVIA_CUSTOM("VPN ID 必须是 UUID", service::common::isUuidField)));
};

class VpnListValidator final : public ruvia::Middleware<VpnListValidator> {
  public:
    RUVIA_VALIDATE_QUERY(
        VpnListQuery,
        RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
        RUVIA_RULE_NAME("pageSize", pageSize, RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
        RUVIA_RULE(status, RUVIA_ONE_OF("VPN 状态无效", "enabled", "disabled"))
    );
};

class VpnFilterValidator final : public ruvia::Middleware<VpnFilterValidator> {
  public:
    RUVIA_VALIDATE_QUERY(VpnFilterQuery, RUVIA_RULE(networkId, RUVIA_CUSTOM("networkId 必须是 UUID", service::common::isUuidField)), RUVIA_RULE(edgeNodeId, RUVIA_CUSTOM("edgeNodeId 必须是 UUID", service::common::isUuidField)));
};

class VpnPayloadValidator final {
  public:
    static std::optional<std::string> text(const ruvia::JsonValue& object, std::string_view field, std::size_t maximum, bool required = false, bool uuid = false) {
        if (!object.isObject()) {
            service::common::fail(21001, "VPN 请求必须是 JSON 对象", 400);
        }
        if (!service::utils::jsonField(object, field)) {
            if (required) {
                service::common::fail(21001, std::string(field) + " 不能为空", 400);
            }
            return std::nullopt;
        }
        const auto value = object.get<ruvia::String>(field);
        if (!value || value->size() > maximum || (required && value->empty()) || (uuid && !service::common::isUuid(value->view()))) {
            service::common::fail(21001, std::string(field) + " 格式无效", 400);
        }
        return std::string(value->view());
    }

    static std::optional<std::int64_t> integer(const ruvia::JsonValue& object, std::string_view field) {
        if (!object.isObject()) {
            service::common::fail(21001, "VPN 请求必须是 JSON 对象", 400);
        }
        if (!service::utils::jsonField(object, field)) {
            return std::nullopt;
        }
        const auto value = object.get<ruvia::Int64>(field);
        if (!value) {
            service::common::fail(21001, std::string(field) + " 必须是整数", 400);
        }
        return static_cast<std::int64_t>(*value);
    }

    static std::optional<bool> boolean(const ruvia::JsonValue& object, std::string_view field) {
        if (!object.isObject()) {
            service::common::fail(21001, "VPN 请求必须是 JSON 对象", 400);
        }
        if (!service::utils::jsonField(object, field)) {
            return std::nullopt;
        }
        const auto value = object.get<ruvia::Bool>(field);
        if (!value) {
            service::common::fail(21001, std::string(field) + " 必须是布尔值", 400);
        }
        return static_cast<bool>(*value);
    }

    static std::vector<std::string> array(const ruvia::JsonValue& object, std::string_view field, bool nodes) {
        if (!nodes && !service::utils::jsonField(object, field)) {
            return {};
        }
        const auto values = object.get<ruvia::Array<ruvia::String>>(field);
        if (!values || values->size() > (nodes ? 64 : 10000)) {
            service::common::fail(21001, std::string(field) + " 必须是大小受限的字符串数组", 400);
        }
        std::vector<std::string> result;
        std::unordered_set<std::string> unique;
        for (const auto& value : *values) {
            std::string id(value.view());
            if (nodes) {
                if (!service::common::isUuid(id)) {
                    service::common::fail(21001, "Edge 节点 ID 必须为 UUID", 400);
                }
                std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (!unique.insert(id).second) {
                    service::common::fail(21001, "Edge 节点 ID 不能重复", 400);
                }
            }
            result.push_back(std::move(id));
        }
        if (nodes) {
            std::sort(result.begin(), result.end());
        }
        return result;
    }

    static VpnNetworkInput parseNetworkInput(const ruvia::JsonValue& object) { return VpnNetworkInput{ text(object, "name", 100, false, false), text(object, "overlayCidr", 32, false, false), text(object, "hubEndpoint", 255, false, false), integer(object, "hubListenPort") }; }

    static VpnRouteInput parseRouteInput(const ruvia::JsonValue& object) { return VpnRouteInput{ *text(object, "networkId", 36, true, true), *text(object, "edgePeerId", 36, true, true), *text(object, "targetCidr", 18, true, false), text(object, "virtualCidr", 18, false, false) }; }

    static VpnRoutePatch parseRoutePatch(const ruvia::JsonValue& object) { return VpnRoutePatch{ text(object, "targetCidr", 18, false, false), text(object, "lanInterface", 64, false, false), text(object, "mode", 32, false, false), boolean(object, "enabled"), text(object, "virtualCidr", 18, false, false) }; }

    static VpnPeerInput parsePeerInput(const ruvia::JsonValue& object) { return VpnPeerInput{ text(object, "networkId", 36, false, true), *text(object, "peerType", 16, true, false), *text(object, "name", 100, true, false), text(object, "publicKey", 64, false, false), text(object, "edgeNodeId", 36, false, true), array(object, "allowedRoutes", false) }; }

    static VpnPeerKeyInput parsePeerKeyInput(const ruvia::JsonValue& object) { return VpnPeerKeyInput{ *text(object, "publicKey", 64, true, false) }; }

    static VpnEnrollmentInput parseEnrollmentInput(const ruvia::JsonValue& object) { return VpnEnrollmentInput{ text(object, "networkId", 36, false, true), integer(object, "expiresInSec") }; }

    static VpnClientEnrollmentInput parseClientEnrollmentInput(const ruvia::JsonValue& object) { return VpnClientEnrollmentInput{ *text(object, "token", 128, true, false), *text(object, "publicKey", 64, true, false), text(object, "name", 100, false, false) }; }

    static VpnDesktopPeerInput parseDesktopPeerInput(const ruvia::JsonValue& object) { return VpnDesktopPeerInput{ *text(object, "name", 100, true, false), *text(object, "publicKey", 64, true, false), array(object, "edgeNodeIds", true) }; }

    static VpnDesktopSelectionInput parseDesktopSelectionInput(const ruvia::JsonValue& object) { return VpnDesktopSelectionInput{ array(object, "edgeNodeIds", true) }; }
};

} // namespace service::vpn
