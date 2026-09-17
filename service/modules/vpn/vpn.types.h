#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <ruvia/web/Model.h>

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

RUVIA_REQUEST_MODEL(VpnIdParams, RUVIA_OPTIONAL_FIELD(id, ruvia::String));

RUVIA_REQUEST_MODEL(VpnListQuery, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)), RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20)), RUVIA_OPTIONAL_FIELD(keyword, ruvia::String), RUVIA_OPTIONAL_FIELD(status, ruvia::String));

struct VpnNetworkInput final {
    std::optional<std::string> name;
    std::optional<std::string> overlayCidr;
    std::optional<std::string> hubEndpoint;
    std::optional<std::int64_t> hubListenPort;
};

RUVIA_REQUEST_MODEL(VpnFilterQuery, RUVIA_OPTIONAL_FIELD(networkId, ruvia::String), RUVIA_OPTIONAL_FIELD(edgeNodeId, ruvia::String));

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

} // namespace service::vpn
