#pragma once

#include <ruvia/web/Model.h>

#include "service/common/uuid.h"

namespace service::common {

inline bool isUuidField(const ruvia::String& value) noexcept { return isUuid(value.view()); }

inline bool isOptionalUuidField(const ruvia::String& value) noexcept {
    return value.empty() || isUuid(value.view());
}

} // namespace service::common
