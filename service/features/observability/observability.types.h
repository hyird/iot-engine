#pragma once

#include <cstdint>
#include <string>

namespace service::observability {

enum class ComponentState { Stopped, Starting, Ready, Failed };

struct ComponentStatus {
    ComponentState state{ComponentState::Stopped};
    std::string detail;
    std::int64_t changedAtMs{};
};

struct AlertStatus {
    bool active{false};
    std::string detail;
    std::int64_t changedAtMs{};
};

} // namespace service::observability
