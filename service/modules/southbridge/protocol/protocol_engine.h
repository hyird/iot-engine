#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/modules/southbridge/protocol/protocol_runtime.registry.h"

namespace service::southbridge {

struct ProtocolConnectionInfo {
    std::string connectionId;
    std::string linkId;
    std::string remoteAddress;
    std::string targetId;
    std::uint64_t sessionEpoch = 0;
};

// Worker-affine protocol engine. It owns every socket's protocol session and is the only layer
// shared by Modbus, S7 and SL651. Scheduling semantics remain inside each concrete session.
class ProtocolEngine final {
  public:
    explicit ProtocolEngine(ProtocolRuntimeRegistry registry) : registry_(std::move(registry)) {}

    void reload(RuntimeSnapshot snapshot) {
        for (const auto& link : snapshot.links) {
            const auto& runtime = registry_.require(link.protocol);
            validateProtocolLink(runtime, link);
        }
        snapshot_ = std::move(snapshot);
        links_.clear();
        for (const auto& link : snapshot_.links)
            links_.emplace(link.id, &link);

        // Configuration generations are immutable for an established connection. Close/recreate
        // is handled by the link runtime; retaining a session across incompatible config would
        // allow stale device routes and command responses.
        sessions_.clear();
    }

    void reload(RuntimeSnapshot snapshot, const std::set<std::string, std::less<>>& affectedLinks) {
        for (const auto& link : snapshot.links) {
            const auto& runtime = registry_.require(link.protocol);
            validateProtocolLink(runtime, link);
        }
        for (auto current = sessions_.begin(); current != sessions_.end();) {
            if (affectedLinks.contains(current->second.info.linkId))
                current = sessions_.erase(current);
            else
                ++current;
        }
        snapshot_ = std::move(snapshot);
        links_.clear();
        for (const auto& link : snapshot_.links)
            links_.emplace(link.id, &link);
    }

    [[nodiscard]] std::vector<ProtocolAction> connected(ProtocolConnectionInfo info) {
        const auto link = links_.find(info.linkId);
        if (link == links_.end())
            return {{.kind = ProtocolActionKind::Close,
                     .connectionId = info.connectionId,
                     .reason = "link_config_not_found"}};
        const auto& runtime = registry_.require(link->second->protocol);
        auto session =
            runtime.createSession(*link->second, info.connectionId, info.targetId, snapshot_);
        if (!session)
            throw std::runtime_error("protocol runtime returned a null session");
        auto actions = session->connected();
        auto connectionId = info.connectionId;
        sessions_.insert_or_assign(std::move(connectionId),
                                   SessionEntry{std::move(info), std::move(session)});
        return actions;
    }

    [[nodiscard]] std::vector<ProtocolAction> consume(const bridge::IngressPacket& packet) {
        const auto current = sessions_.find(packet.connectionId);
        if (current == sessions_.end())
            return {{.kind = ProtocolActionKind::Close,
                     .connectionId = packet.connectionId,
                     .reason = "protocol_session_not_found"}};
        ProtocolInput input{.messageId = packet.messageId,
                            .linkId = packet.linkId,
                            .connectionId = packet.connectionId,
                            .remoteAddress = packet.remoteAddress,
                            .receivedAtMs = packet.occurredAtMs,
                            .bytes = packet.payload};
        return current->second.session->consume(input);
    }

    [[nodiscard]] std::vector<ProtocolAction> disconnected(std::string_view connectionId,
                                                           std::string_view reason) {
        const auto current = sessions_.find(connectionId);
        if (current == sessions_.end())
            return {};
        auto actions = current->second.session->disconnected(reason);
        sessions_.erase(current);
        return actions;
    }

    [[nodiscard]] std::vector<ProtocolAction> execute(std::string_view connectionId,
                                                      ProtocolCommand command) {
        const auto current = sessions_.find(connectionId);
        if (current == sessions_.end())
            return {{.kind = ProtocolActionKind::FailCommand,
                     .connectionId = std::string(connectionId),
                     .commandId = std::move(command.id),
                     .reason = "device_offline"}};
        const auto link = links_.find(current->second.info.linkId);
        if (!command.protocol.empty() &&
            (link == links_.end() || link->second->protocol != command.protocol))
            return {{.kind = ProtocolActionKind::FailCommand,
                     .connectionId = std::string(connectionId),
                     .commandId = std::move(command.id),
                     .reason = "protocol_route_mismatch"}};
        const auto capability =
            dynamic_cast<CommandCapabilitySession*>(current->second.session.get());
        if (!capability)
            return {{.kind = ProtocolActionKind::FailCommand,
                     .connectionId = std::string(connectionId),
                     .commandId = std::move(command.id),
                     .reason = "protocol_command_not_supported"}};
        return capability->execute(std::move(command));
    }

    [[nodiscard]] std::vector<ProtocolAction> deadline(std::string_view connectionId,
                                                       std::uint64_t token) {
        const auto current = sessions_.find(connectionId);
        if (current == sessions_.end())
            return {};
        const auto capability =
            dynamic_cast<DeadlineCapabilitySession*>(current->second.session.get());
        return capability ? capability->deadline(token) : std::vector<ProtocolAction>{};
    }

    [[nodiscard]] bool contains(std::string_view connectionId) const noexcept {
        return sessions_.contains(connectionId);
    }

  private:
    struct SessionEntry {
        ProtocolConnectionInfo info;
        std::unique_ptr<ProtocolSession> session;
    };

    ProtocolRuntimeRegistry registry_;
    RuntimeSnapshot snapshot_;
    std::map<std::string, const LinkDefinition*, std::less<>> links_;
    std::map<std::string, SessionEntry, std::less<>> sessions_;
};

} // namespace service::southbridge
