#pragma once

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/collector/protocol.h"

namespace service::collector {

// The registry is owned and accessed by one Collector Worker. It deliberately has no mutex and no
// process-wide singleton; every Worker gets isolated protocol runtime objects.
class ProtocolRuntimeRegistry final {
  public:
    void add(std::unique_ptr<ProtocolRuntime> runtime) {
        if (!runtime)
            throw std::invalid_argument("protocol runtime is null");
        const std::string name(runtime->protocol());
        if (name.empty())
            throw std::invalid_argument("protocol runtime name is empty");
        if (!runtimes_.emplace(name, std::move(runtime)).second)
            throw std::invalid_argument("duplicate protocol runtime: " + name);
    }

    [[nodiscard]] const ProtocolRuntime* find(std::string_view protocol) const noexcept {
        const auto current = runtimes_.find(protocol);
        return current == runtimes_.end() ? nullptr : current->second.get();
    }

    [[nodiscard]] const ProtocolRuntime& require(std::string_view protocol) const {
        const auto* runtime = find(protocol);
        if (!runtime)
            throw std::runtime_error("protocol runtime is not registered: " +
                                     std::string(protocol));
        return *runtime;
    }

  private:
    std::map<std::string, std::unique_ptr<ProtocolRuntime>, std::less<>> runtimes_;
};

} // namespace service::collector
