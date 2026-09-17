#pragma once
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/modules/command/command.types.h"

namespace service::command {
class CommandStatusValidator final : public ruvia::Middleware<CommandStatusValidator> {
  public:
    RUVIA_VALIDATE_PARAM(CommandStatusQuery, RUVIA_RULE(id, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

class SubmitCommandValidator final : public ruvia::Middleware<SubmitCommandValidator> {
  public:
    RUVIA_VALIDATE_JSON(service::device::DeviceCommandBody, RUVIA_RULE(elements, RUVIA_REQUIRED("下发要素不能为空"), RUVIA_MIN(1, "请至少选择一个下发要素"), RUVIA_MAX(256, "单次最多下发 256 个要素")))
};
} // namespace service::command
