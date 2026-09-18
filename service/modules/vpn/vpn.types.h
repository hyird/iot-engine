#pragma once
#include "service/utils/json.h"
#include "service/common/http.h"
#include <unordered_set>
#include <cctype>
#include <algorithm>

#include "service/common/uuid.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <ruvia/web/Validation.h>

#include "service/common/message.h"
#include "service/utils/network.h"

namespace service::vpn {

namespace module {

using service::message::vpn::kOverlayPool;
using service::message::vpn::kVirtualLanPool;
using service::message::vpn::mappedVirtualCidr;
using service::utils::network::hostAddress;
using service::utils::network::Ipv4Cidr;
using service::utils::network::isPrivateIpv4;
using service::utils::network::networkCidr;
using service::utils::network::parseCidr;
using service::utils::network::parseIpv4;

inline bool virtualMappingConflicts(const Ipv4Cidr& candidate, const std::optional<Ipv4Cidr>& realTarget, const std::optional<Ipv4Cidr>& existingVirtual) {
    return (realTarget && candidate.overlaps(*realTarget)) ||
        (existingVirtual && candidate.overlaps(*existingVirtual));
}

inline bool realTargetConflictsVirtual(const Ipv4Cidr& candidate, const std::optional<Ipv4Cidr>& existingVirtual) {
    return existingVirtual && candidate.overlaps(*existingVirtual);
}

} // namespace module

RUVIA_REQUEST_MODEL(VpnIdParams, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("VPN ID 必须是 UUID", service::common::isUuidField)));

RUVIA_REQUEST_MODEL(VpnListQuery, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")), RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20), RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")), RUVIA_OPTIONAL_FIELD(keyword, ruvia::String), RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("VPN 状态无效", "enabled", "disabled")));

struct VpnNetworkInput final {
    std::optional<std::string> name;
    std::optional<std::string> overlayCidr;
    std::optional<std::string> hubEndpoint;
    std::optional<std::int64_t> hubListenPort;
};

RUVIA_REQUEST_MODEL(VpnFilterQuery, RUVIA_OPTIONAL_FIELD(networkId, ruvia::String, RUVIA_CUSTOM("networkId 必须是 UUID", service::common::isUuidField)), RUVIA_OPTIONAL_FIELD(edgeNodeId, ruvia::String, RUVIA_CUSTOM("edgeNodeId 必须是 UUID", service::common::isUuidField)));

struct VpnRouteInput final {
    std::string networkId;
    std::string edgePeerId;
    std::string targetCidr;
    std::optional<std::string> virtualCidr;
};

struct VpnRoutePatch final {
    std::optional<std::string> targetCidr;
    std::optional<std::string> lanInterface;
    std::optional<std::string> mode;
    std::optional<bool> enabled;
    std::optional<std::string> virtualCidr;
};

struct VpnPeerInput final {
    std::optional<std::string> networkId;
    std::string peerType;
    std::string name;
    std::optional<std::string> publicKey;
    std::optional<std::string> edgeNodeId;
    std::vector<std::string> allowedRoutes;
};

struct VpnPeerKeyInput final {
    std::string publicKey;
};

struct VpnEnrollmentInput final {
    std::optional<std::string> networkId;
    std::optional<std::int64_t> expiresInSec;
};

struct VpnClientEnrollmentInput final {
    std::string token;
    std::string publicKey;
    std::optional<std::string> name;
};

struct VpnDesktopPeerInput final {
    std::string name;
    std::string publicKey;
    std::vector<std::string> edgeNodeIds;
};

struct VpnDesktopSelectionInput final {
    std::vector<std::string> edgeNodeIds;
};



namespace vpnRequest {
inline std::optional<std::string> text(const ruvia::JsonValue& object, std::string_view field, std::size_t maximum, bool required = false, bool uuid = false);
inline std::optional<std::int64_t> integer(const ruvia::JsonValue& object, std::string_view field);
inline std::optional<bool> boolean(const ruvia::JsonValue& object, std::string_view field);
inline std::vector<std::string> array(const ruvia::JsonValue& object, std::string_view field, bool nodes);
inline VpnNetworkInput parseNetworkInput(const ruvia::JsonValue& object);
inline VpnRouteInput parseRouteInput(const ruvia::JsonValue& object);
inline VpnRoutePatch parseRoutePatch(const ruvia::JsonValue& object);
inline VpnPeerInput parsePeerInput(const ruvia::JsonValue& object);
inline VpnPeerKeyInput parsePeerKeyInput(const ruvia::JsonValue& object);
inline VpnEnrollmentInput parseEnrollmentInput(const ruvia::JsonValue& object);
inline VpnClientEnrollmentInput parseClientEnrollmentInput(const ruvia::JsonValue& object);
inline VpnDesktopPeerInput parseDesktopPeerInput(const ruvia::JsonValue& object);
inline VpnDesktopSelectionInput parseDesktopSelectionInput(const ruvia::JsonValue& object);

inline std::optional<std::string> text(const ruvia::JsonValue& object, std::string_view field, std::size_t maximum, bool required, bool uuid) {
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

inline std::optional<std::int64_t> integer(const ruvia::JsonValue& object, std::string_view field) {
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

inline std::optional<bool> boolean(const ruvia::JsonValue& object, std::string_view field) {
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

inline std::vector<std::string> array(const ruvia::JsonValue& object, std::string_view field, bool nodes) {
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

inline VpnNetworkInput parseNetworkInput(const ruvia::JsonValue& object) { return VpnNetworkInput{ text(object, "name", 100, false, false), text(object, "overlayCidr", 32, false, false), text(object, "hubEndpoint", 255, false, false), integer(object, "hubListenPort") }; }

inline VpnRouteInput parseRouteInput(const ruvia::JsonValue& object) { return VpnRouteInput{ *text(object, "networkId", 36, true, true), *text(object, "edgePeerId", 36, true, true), *text(object, "targetCidr", 18, true, false), text(object, "virtualCidr", 18, false, false) }; }

inline VpnRoutePatch parseRoutePatch(const ruvia::JsonValue& object) { return VpnRoutePatch{ text(object, "targetCidr", 18, false, false), text(object, "lanInterface", 64, false, false), text(object, "mode", 32, false, false), boolean(object, "enabled"), text(object, "virtualCidr", 18, false, false) }; }

inline VpnPeerInput parsePeerInput(const ruvia::JsonValue& object) { return VpnPeerInput{ text(object, "networkId", 36, false, true), *text(object, "peerType", 16, true, false), *text(object, "name", 100, true, false), text(object, "publicKey", 64, false, false), text(object, "edgeNodeId", 36, false, true), array(object, "allowedRoutes", false) }; }

inline VpnPeerKeyInput parsePeerKeyInput(const ruvia::JsonValue& object) { return VpnPeerKeyInput{ *text(object, "publicKey", 64, true, false) }; }

inline VpnEnrollmentInput parseEnrollmentInput(const ruvia::JsonValue& object) { return VpnEnrollmentInput{ text(object, "networkId", 36, false, true), integer(object, "expiresInSec") }; }

inline VpnClientEnrollmentInput parseClientEnrollmentInput(const ruvia::JsonValue& object) { return VpnClientEnrollmentInput{ *text(object, "token", 128, true, false), *text(object, "publicKey", 64, true, false), text(object, "name", 100, false, false) }; }

inline VpnDesktopPeerInput parseDesktopPeerInput(const ruvia::JsonValue& object) { return VpnDesktopPeerInput{ *text(object, "name", 100, true, false), *text(object, "publicKey", 64, true, false), array(object, "edgeNodeIds", true) }; }

inline VpnDesktopSelectionInput parseDesktopSelectionInput(const ruvia::JsonValue& object) { return VpnDesktopSelectionInput{ array(object, "edgeNodeIds", true) }; }
} // namespace vpnRequest

} // namespace service::vpn
