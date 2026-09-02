#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/id.validation.h"
#include "service/domains/vpn/vpn.types.h"

namespace service::vpn {

class VpnIdValidator final : public ruvia::Middleware<VpnIdValidator> {
  public:
    RUVIA_VALIDATE_PARAM(VpnIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("VPN ID 不能为空"),
                                    RUVIA_CUSTOM("VPN ID 必须是 UUID", service::common::isUuidField)));
};

class VpnListValidator final : public ruvia::Middleware<VpnListValidator> {
  public:
    RUVIA_VALIDATE_QUERY(
        VpnListQuery,
        RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
        RUVIA_RULE_NAME("pageSize", pageSize,
                        RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"),
                        RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
        RUVIA_RULE(status, RUVIA_ONE_OF("VPN 状态无效", "enabled", "disabled")));
};

class VpnClientConfigValidator final : public ruvia::Middleware<VpnClientConfigValidator> {
  public:
    RUVIA_VALIDATE_QUERY(
        VpnClientConfigQuery,
        RUVIA_RULE_NAME("peerId", peerId,
                        RUVIA_REQUIRED("Peer ID 不能为空"),
                        RUVIA_CUSTOM("Peer ID 必须是 UUID", service::common::isUuidField)));
};

} // namespace service::vpn
