#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace service::observability {

enum class ComponentState { Stopped, Starting, Ready, Failed };

struct ComponentStatus {
    ComponentState state{ComponentState::Stopped};
    std::string detail;
    std::int64_t changedAtMs{};
};

class Registry final {
  public:
    void component(std::string name, ComponentState state, std::string detail = {}) {
        std::lock_guard lock(mutex_);
        components_.insert_or_assign(
            std::move(name), ComponentStatus{state, std::move(detail), nowMilliseconds()});
    }

    void increment(std::string_view name, std::uint64_t amount = 1) {
        std::lock_guard lock(mutex_);
        counters_[std::string(name)] += amount;
    }

    void gauge(std::string_view name, std::int64_t value) {
        std::lock_guard lock(mutex_);
        gauges_.insert_or_assign(std::string(name), value);
    }

    [[nodiscard]] bool ready() const {
        std::lock_guard lock(mutex_);
        if (components_.empty())
            return false;
        for (const auto& [_, component] : components_)
            if (component.state != ComponentState::Ready)
                return false;
        return true;
    }

    [[nodiscard]] std::string healthJson() const {
        std::lock_guard lock(mutex_);
        std::ostringstream output;
        output << "{\"status\":\"" << (allReadyLocked() ? "ready" : "not_ready")
               << "\",\"components\":{";
        bool first = true;
        for (const auto& [name, component] : components_) {
            if (!first)
                output << ',';
            first = false;
            output << '\"' << escape(name) << "\":{\"state\":\""
                   << stateName(component.state) << "\",\"detail\":\""
                   << escape(component.detail) << "\",\"changed_at_ms\":"
                   << component.changedAtMs << '}';
        }
        output << "}}";
        return output.str();
    }

    [[nodiscard]] std::string prometheus() const {
        std::lock_guard lock(mutex_);
        std::ostringstream output;
        output << "# TYPE iot_engine_ready gauge\n"
               << "iot_engine_ready " << (allReadyLocked() ? 1 : 0) << "\n";
        output << "# TYPE iot_engine_component_ready gauge\n";
        for (const auto& [name, component] : components_)
            output << "iot_engine_component_ready{component=\"" << escape(name) << "\"} "
                   << (component.state == ComponentState::Ready ? 1 : 0) << "\n";
        for (const auto& [name, value] : counters_)
            output << sanitizeMetric(name) << " " << value << "\n";
        for (const auto& [name, value] : gauges_)
            output << sanitizeMetric(name) << " " << value << "\n";
        return output.str();
    }

  private:
    static std::int64_t nowMilliseconds() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    [[nodiscard]] bool allReadyLocked() const {
        if (components_.empty())
            return false;
        for (const auto& [_, component] : components_)
            if (component.state != ComponentState::Ready)
                return false;
        return true;
    }

    static std::string_view stateName(ComponentState state) {
        switch (state) {
        case ComponentState::Stopped: return "stopped";
        case ComponentState::Starting: return "starting";
        case ComponentState::Ready: return "ready";
        case ComponentState::Failed: return "failed";
        }
        return "unknown";
    }

    static std::string escape(std::string_view input) {
        std::string output;
        output.reserve(input.size());
        for (const char ch : input) {
            if (ch == '\\' || ch == '\"')
                output.push_back('\\');
            if (ch == '\n') {
                output += "\\n";
                continue;
            }
            output.push_back(ch);
        }
        return output;
    }

    static std::string sanitizeMetric(std::string_view input) {
        std::string output;
        output.reserve(input.size());
        for (const char ch : input)
            output.push_back((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                     (ch >= '0' && ch <= '9') || ch == '_' || ch == ':'
                                 ? ch
                                 : '_');
        return output;
    }

    mutable std::mutex mutex_;
    std::map<std::string, ComponentStatus, std::less<>> components_;
    std::map<std::string, std::uint64_t, std::less<>> counters_;
    std::map<std::string, std::int64_t, std::less<>> gauges_;
};

inline std::atomic<Registry*> gProcessRegistry{nullptr};

inline void configureProcessRegistry(Registry& registry) noexcept {
    gProcessRegistry.store(&registry, std::memory_order_release);
}

inline Registry* processRegistry() noexcept {
    return gProcessRegistry.load(std::memory_order_acquire);
}

} // namespace service::observability
