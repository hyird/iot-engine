#pragma once

#include <optional>
#include <string>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelObject.h>

namespace service::protocol {

// Configuration JSON stays exact until PostgreSQL jsonb normalization. Request
// fields have explicit types; an omitted remark differs from clearing it.
struct CreateProtocolBody {
    std::string protocol;
    std::string name;
    std::optional<std::string> remark;
    bool enabled{ true };
    std::optional<ruvia::JsonValue> config;
};

struct UpdateProtocolBody {
    std::optional<std::string> protocol;
    std::optional<std::string> name;
    std::optional<std::string> remark;
    bool remarkPresent{};
    std::optional<bool> enabled;
    std::optional<ruvia::JsonValue> config;
};

RUVIA_REQUEST_MODEL(ProtocolListQuery, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)), RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10)), RUVIA_OPTIONAL_FIELD(protocol, ruvia::String));

RUVIA_REQUEST_MODEL(ProtocolIdParams, RUVIA_OPTIONAL_FIELD(id, ruvia::String));

} // namespace service::protocol
