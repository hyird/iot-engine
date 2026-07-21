#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "service/modules/southbridge/protocol/protocol_adapter.h"

namespace service::southbridge {

class ProtocolRegistry {
  public:
    void registerAdapter(std::unique_ptr<ProtocolAdapter> adapter) {
        if (!adapter)
            return;
        const std::string name(adapter->protocol());
        std::lock_guard lock(mutex_);
        adapters_[name] = std::move(adapter);
    }

    [[nodiscard]] const ProtocolAdapter* find(std::string_view protocol) const {
        std::lock_guard lock(mutex_);
        const auto adapter = adapters_.find(std::string(protocol));
        return adapter == adapters_.end() ? nullptr : adapter->second.get();
    }

    [[nodiscard]] std::vector<const ProtocolAdapter*> all() const {
        std::vector<const ProtocolAdapter*> result;
        std::lock_guard lock(mutex_);
        result.reserve(adapters_.size());
        for (const auto& [name, adapter] : adapters_) {
            (void)name;
            result.push_back(adapter.get());
        }
        return result;
    }

  private:
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<ProtocolAdapter>> adapters_;
};

} // namespace service::southbridge
