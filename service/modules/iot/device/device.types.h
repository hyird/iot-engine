#pragma once

#include <ruvia/web/Model.h>

namespace service::device {

RUVIA_REQUEST_MODEL(DeviceIdParams, RUVIA_FIELD(id, ruvia::Int64));

} // namespace service::device
