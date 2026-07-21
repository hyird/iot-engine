#pragma once

#include <ruvia/web/Controller.h>

#include "service/modules/iot/device/device.types.h"

namespace service::device {

class DeviceIdParamsValidator final : public ruvia::Middleware<DeviceIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(DeviceIdParams, RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                                    RUVIA_MIN(1, "id 必须是正整数")))
};

} // namespace service::device
