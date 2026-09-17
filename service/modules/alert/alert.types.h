#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <ruvia/web/Model.h>

namespace service::alert {

struct ApplyTemplateInput final {
    std::string templateId;
    std::vector<std::string> deviceIds;
};

RUVIA_REQUEST_MODEL(AlertIdParams, RUVIA_OPTIONAL_FIELD(id, ruvia::String));

RUVIA_REQUEST_MODEL(AlertListQuery, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)), RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20)), RUVIA_OPTIONAL_FIELD(keyword, ruvia::String), RUVIA_OPTIONAL_FIELD(deviceId, ruvia::String), RUVIA_OPTIONAL_FIELD(ruleId, ruvia::String), RUVIA_OPTIONAL_FIELD(status, ruvia::String), RUVIA_OPTIONAL_FIELD(severity, ruvia::String), RUVIA_OPTIONAL_FIELD(category, ruvia::String));

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

RUVIA_REQUEST_MODEL(AlertGroupedQuery, RUVIA_OPTIONAL_FIELD(days, ruvia::Int64, RUVIA_DEFAULT(7)));
} // namespace service::alert
