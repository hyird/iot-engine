#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/id.validation.h"
#include "service/domains/alert/alert.types.h"

namespace service::alert {

class AlertIdValidator final : public ruvia::Middleware<AlertIdValidator> {
  public:
    RUVIA_VALIDATE_PARAM(AlertIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

class AlertListValidator final : public ruvia::Middleware<AlertListValidator> {
  public:
    RUVIA_VALIDATE_QUERY(AlertListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
                         RUVIA_RULE_NAME("pageSize", pageSize,
                                         RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"),
                                         RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")))
};

} // namespace service::alert
