#pragma once

#include <ruvia/web/Model.h>

namespace service::protocol {

// 说明：协议配置项的核心字段 config 是各协议的任意 jsonb 对象（Modbus 寄存器、
// S7 areas、SL651 funcs 等深度嵌套变体）。RUVIA 模型不支持原始 JSON 透传字段
// （RUVIA_FIELD 只接受 Ruvia value 类型或嵌套模型），故 protocol 的读写保持
// jsonb 输出/入参，不套用 typed DTO；协议相关的 config 校验在 service 内完成。

struct ProtocolListQuery final {
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1));
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10));
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String);
    RUVIA_MODEL(ProtocolListQuery, page, pageSize, protocol);
};

struct ProtocolIdParams final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_MODEL(ProtocolIdParams, id);
};

} // namespace service::protocol
