#pragma once

#include <optional>

#include <ruvia/web/Model.h>

#include "service/common/message.h"
#include "service/utils/network.h"

namespace service::vpn {

namespace module {

using service::message::vpn::kOverlayPool;
using service::message::vpn::kVirtualLanPool;
using service::message::vpn::mappedVirtualCidr;
using service::utils::network::Ipv4Cidr;
using service::utils::network::hostAddress;
using service::utils::network::isPrivateIpv4;
using service::utils::network::networkCidr;
using service::utils::network::parseCidr;
using service::utils::network::parseIpv4;

inline bool virtualMappingConflicts(const Ipv4Cidr& candidate,
    const std::optional<Ipv4Cidr>& realTarget,
    const std::optional<Ipv4Cidr>& existingVirtual) {
    return (realTarget && candidate.overlaps(*realTarget)) ||
           (existingVirtual && candidate.overlaps(*existingVirtual));
}

inline bool realTargetConflictsVirtual(const Ipv4Cidr& candidate,
    const std::optional<Ipv4Cidr>& existingVirtual) {
    return existingVirtual && candidate.overlaps(*existingVirtual);
}

} // namespace module

RUVIA_REQUEST_MODEL(VpnIdParams,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String));

RUVIA_REQUEST_MODEL(VpnListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20)),
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(VpnClientConfigQuery,
    RUVIA_OPTIONAL_FIELD_NAME("peerId", peerId, ruvia::String));

} // namespace service::vpn
