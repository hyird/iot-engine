#pragma once

#include <ruvia/web/Model.h>
#include <cstdint>
#include <string>

namespace service::alert {

RUVIA_REQUEST_MODEL(AlertIdParams,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String));

RUVIA_REQUEST_MODEL(AlertListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20)));

    struct RuleInput final {
        std::string name;
        std::string deviceId;
        std::string severity;
        std::string conditions;
        std::string logic;
        std::int64_t silenceDuration{};
        std::string recoveryCondition;
        std::int64_t recoveryWaitSeconds{};
        std::string status;
        std::string remark;
    };

    struct TemplateInput final {
        std::string name;
        std::string category;
        std::string description;
        std::string severity;
        std::string conditions;
        std::string logic;
        std::int64_t silenceDuration{};
        std::string recoveryCondition;
        std::int64_t recoveryWaitSeconds{};
        std::string applicableProtocols;
        std::string protocolConfigId;
    };

} // namespace service::alert
