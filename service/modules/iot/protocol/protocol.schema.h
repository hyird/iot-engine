#pragma once

#include <ruvia/web/Controller.h>

#include "service/modules/iot/protocol/protocol.types.h"

namespace service::protocol {

class ProtocolListQueryValidator final : public ruvia::Middleware<ProtocolListQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(ProtocolListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
                         RUVIA_RULE_NAME("pageSize", pageSize,
                                         RUVIA_MIN(1, "pageSize 必须在 1 - 1000 之间"),
                                         RUVIA_MAX(1000, "pageSize 必须在 1 - 1000 之间")),
                         RUVIA_RULE(protocol, RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7")))
};

class ProtocolIdParamsValidator final : public ruvia::Middleware<ProtocolIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(ProtocolIdParams, RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                                      RUVIA_MIN(1, "id 必须是正整数")))
};

} // namespace service::protocol
