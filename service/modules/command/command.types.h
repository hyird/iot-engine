#pragma once

#include <ruvia/web/Model.h>

#include "service/modules/device/device.types.h"

namespace service::command {

RUVIA_MODEL(SubmitCommandBody, RUVIA_OPTIONAL_FIELD_NAME("idempotency_key", idempotencyKey, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("deviceId", deviceId, ruvia::String), RUVIA_REQUIRED_FIELD(elements, ruvia::Array<service::device::DeviceCommandElementBody>, RUVIA_MIN(1, "请至少选择一个下发要素"), RUVIA_MAX(256, "单次最多下发 256 个要素")));

RUVIA_MODEL(CommandStatusQuery, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));

RUVIA_MODEL(PreparedElement, RUVIA_OPTIONAL_FIELD(elementId, ruvia::String), RUVIA_OPTIONAL_FIELD(value, ruvia::String));

RUVIA_MODEL(PreparedOperation, RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD(deviceId, ruvia::String), RUVIA_OPTIONAL_FIELD(deviceCode, ruvia::String), RUVIA_OPTIONAL_FIELD(protocol, ruvia::String), RUVIA_OPTIONAL_FIELD(elements, ruvia::BoxedArray<PreparedElement>), RUVIA_OPTIONAL_FIELD(payload, ruvia::Array<ruvia::String>));

RUVIA_MODEL(PreparedBatch, RUVIA_OPTIONAL_FIELD(queue, ruvia::String), RUVIA_OPTIONAL_FIELD(kind, ruvia::String), RUVIA_OPTIONAL_FIELD(maximum, ruvia::Int64), RUVIA_OPTIONAL_FIELD(nodeId, ruvia::String), RUVIA_OPTIONAL_FIELD(commands, ruvia::BoxedArray<PreparedOperation>));

} // namespace service::command
