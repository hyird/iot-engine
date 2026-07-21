#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/bridge/message.contract.h"

namespace service::southbridge {

struct DtuSession {
    std::string connectionId;
    std::string linkId;
    std::string remoteAddress;
    std::string dtuKey;
    std::int64_t connectedAtMs = 0;
    std::int64_t lastSeenAtMs = 0;
};

struct BindResult {
    bool bound = false;
    std::string displacedConnectionId;
};

class SessionRegistry {
  public:
    void connected(std::string connectionId, std::string linkId, std::string remoteAddress) {
        const auto now = service::bridge::utcNowMilliseconds();
        std::lock_guard lock(mutex_);
        sessions_[connectionId] = {connectionId, linkId, std::move(remoteAddress), {}, now, now};
    }

    std::optional<DtuSession> disconnected(std::string_view connectionId) {
        std::lock_guard lock(mutex_);
        const auto current = sessions_.find(std::string(connectionId));
        if (current == sessions_.end())
            return std::nullopt;
        auto session = current->second;
        if (!session.dtuKey.empty()) {
            const auto binding = dtuBindings_.find(session.dtuKey);
            if (binding != dtuBindings_.end() && binding->second == connectionId)
                dtuBindings_.erase(binding);
        }
        sessions_.erase(current);
        return session;
    }

    void touch(std::string_view connectionId) {
        std::lock_guard lock(mutex_);
        const auto session = sessions_.find(std::string(connectionId));
        if (session != sessions_.end())
            session->second.lastSeenAtMs = service::bridge::utcNowMilliseconds();
    }

    [[nodiscard]] std::optional<DtuSession> find(std::string_view connectionId) const {
        std::lock_guard lock(mutex_);
        const auto session = sessions_.find(std::string(connectionId));
        if (session == sessions_.end())
            return std::nullopt;
        return session->second;
    }

    [[nodiscard]] std::optional<DtuSession> findByDtuKey(std::string_view dtuKey) const {
        std::lock_guard lock(mutex_);
        const auto binding = dtuBindings_.find(std::string(dtuKey));
        if (binding == dtuBindings_.end())
            return std::nullopt;
        const auto session = sessions_.find(binding->second);
        if (session == sessions_.end())
            return std::nullopt;
        return session->second;
    }

    [[nodiscard]] std::vector<DtuSession> unboundByLink(std::string_view linkId) const {
        std::vector<DtuSession> result;
        std::lock_guard lock(mutex_);
        for (const auto& [connectionId, session] : sessions_) {
            (void)connectionId;
            if (session.linkId == linkId && session.dtuKey.empty())
                result.push_back(session);
        }
        return result;
    }

    BindResult bind(std::string_view connectionId, std::string dtuKey) {
        std::lock_guard lock(mutex_);
        const auto session = sessions_.find(std::string(connectionId));
        if (session == sessions_.end())
            return {};
        BindResult result;
        const auto existing = dtuBindings_.find(dtuKey);
        if (existing != dtuBindings_.end() && existing->second != connectionId) {
            result.displacedConnectionId = existing->second;
            const auto displaced = sessions_.find(existing->second);
            if (displaced != sessions_.end())
                displaced->second.dtuKey.clear();
        }
        if (!session->second.dtuKey.empty())
            dtuBindings_.erase(session->second.dtuKey);
        session->second.dtuKey = std::move(dtuKey);
        session->second.lastSeenAtMs = service::bridge::utcNowMilliseconds();
        dtuBindings_[session->second.dtuKey] = session->second.connectionId;
        result.bound = true;
        return result;
    }

  private:
    mutable std::mutex mutex_;
    std::map<std::string, DtuSession> sessions_;
    std::map<std::string, std::string> dtuBindings_;
};

} // namespace service::southbridge
