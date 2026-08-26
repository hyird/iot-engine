#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/id.validation.h"
#include "service/domains/system/outbox.types.h"

namespace service::system {

class OutboxEventIdValidator final
    : public ruvia::Middleware<OutboxEventIdValidator> {
public:
  RUVIA_VALIDATE_PARAM(OutboxEventIdParams,
                       RUVIA_RULE(id, RUVIA_REQUIRED("事件 ID 不能为空"),
                                  RUVIA_CUSTOM("事件 ID 必须是 UUID",
                                               service::common::isUuidField)))
};

} // namespace service::system
