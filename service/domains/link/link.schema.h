#pragma once

#include <ruvia/web/Controller.h>

#include "service/domains/link/link.types.h"
#include "service/common/id.validation.h"

namespace service::link {

class LinkTargetValidator final : public ruvia::Middleware<LinkTargetValidator> {
  public:
    RUVIA_VALIDATE_JSON(LinkTargetBody,
                        RUVIA_RULE(id, RUVIA_REQUIRED("目标 ID 不能为空"),
                                   RUVIA_MAX(64, "目标 ID 不能超过 64 个字符")),
                        RUVIA_RULE(name, RUVIA_REQUIRED("目标名称不能为空"),
                                   RUVIA_MAX(100, "目标名称不能超过 100 个字符")),
                        RUVIA_RULE(ip, RUVIA_REQUIRED("目标 IP 不能为空"),
                                   RUVIA_MAX(50, "目标 IP 不能超过 50 个字符")),
                        RUVIA_RULE(port, RUVIA_REQUIRED("目标端口不能为空"),
                                   RUVIA_MIN(1, "目标端口必须在 1 - 65535 之间"),
                                   RUVIA_MAX(65535, "目标端口必须在 1 - 65535 之间")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("目标状态无效", "enabled", "disabled")))
};

class SaveLinkValidator final : public ruvia::Middleware<SaveLinkValidator> {
  public:
    RUVIA_VALIDATE_JSON(SaveLinkBody,
                        RUVIA_RULE(name, RUVIA_REQUIRED("链路名称不能为空"),
                                   RUVIA_MAX(100, "链路名称不能超过 100 个字符")),
                        RUVIA_RULE(mode, RUVIA_REQUIRED("链路模式不能为空"),
                                   RUVIA_ONE_OF("链路模式无效", "TCP Server", "TCP Client")),
                        RUVIA_RULE(protocol, RUVIA_REQUIRED("协议不能为空"),
                                   RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7")),
                        RUVIA_RULE(ip, RUVIA_MAX(50, "监听 IP 不能超过 50 个字符")),
                        RUVIA_RULE(port, RUVIA_MIN(0, "端口必须在 0 - 65535 之间"),
                                   RUVIA_MAX(65535, "端口必须在 0 - 65535 之间")),
                        RUVIA_RULE(targets, RUVIA_REQUIRED("目标列表不能为空"),
                                   RUVIA_MAX(100, "单条链路最多配置 100 个目标"),
                                   RUVIA_EACH(LinkTargetValidator)),
                        RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")))
};

class LinkListQueryValidator final : public ruvia::Middleware<LinkListQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(LinkListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
                         RUVIA_RULE_NAME("pageSize", pageSize,
                                         RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"),
                                         RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
                         RUVIA_RULE(mode, RUVIA_ONE_OF("链路模式无效", "TCP Server", "TCP Client")),
                         RUVIA_RULE(protocol, RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7")),
                         RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")))
};

class LinkIdParamsValidator final : public ruvia::Middleware<LinkIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(LinkIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

} // namespace service::link
