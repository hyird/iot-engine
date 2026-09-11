#pragma once

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/collector/collector.protocol.h"

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

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "service/features/collector/collector.types.h"

namespace service::collector {

namespace detail {

inline const LinkDefinition* findLink(
    const std::map<std::string, LinkDefinition, std::less<>>& links,
    std::string_view id) {
    const auto current = links.find(id);
    return current == links.end() ? nullptr : &current->second;
}

inline bool targetAllowsSessionRefresh(const RuntimeSnapshot& snapshot,
                                       const ClientTargetKey& target) {
    return std::none_of(snapshot.devices.begin(), snapshot.devices.end(),
                        [&target](const auto& device) {
                            return device.linkId == target.first &&
                                   device.targetId == target.second &&
                                   (device.protocol != "Modbus" ||
                                    device.modbusMode != "TCP");
                        });
}

inline void addChangedDeviceTransport(
    RuntimeReconcilePlan& plan,
    const std::map<std::string, LinkDefinition, std::less<>>& links,
    const DeviceDefinition& device) {
    plan.affectedLinks.insert(device.linkId);
    const auto* link = findLink(links, device.linkId);
    if (!link || link->mode != "TCP Client") {
        plan.restartLinks.insert(device.linkId);
        return;
    }
    if (device.targetId.empty()) {
        plan.restartLinks.insert(device.linkId);
        return;
    }
    if (link->protocol == "Modbus" && device.modbusMode == "TCP")
        plan.refreshClientSessions.emplace(device.linkId, device.targetId);
    else
        plan.restartClientTargets.emplace(device.linkId, device.targetId);
}

} // namespace detail

[[nodiscard]] inline RuntimeReconcilePlan
planRuntimeReconcile(const RuntimeSnapshot& previous, const RuntimeSnapshot& next) {
    std::map<std::string, LinkDefinition, std::less<>> previousLinks;
    std::map<std::string, LinkDefinition, std::less<>> nextLinks;
    std::map<std::string, DeviceDefinition, std::less<>> previousDevices;
    std::map<std::string, DeviceDefinition, std::less<>> nextDevices;

    for (const auto& link : previous.links)
        previousLinks.insert_or_assign(link.id, link);
    for (const auto& link : next.links)
        nextLinks.insert_or_assign(link.id, link);
    for (const auto& device : previous.devices)
        previousDevices.insert_or_assign(device.id, device);
    for (const auto& device : next.devices)
        nextDevices.insert_or_assign(device.id, device);

    RuntimeReconcilePlan plan;
    for (const auto& [id, link] : previousLinks) {
        const auto current = nextLinks.find(id);
        if (current == nextLinks.end() || current->second != link)
            plan.affectedLinks.insert(id);
    }
    for (const auto& [id, link] : nextLinks) {
        const auto old = previousLinks.find(id);
        if (old == previousLinks.end() || old->second != link)
            plan.affectedLinks.insert(id);
    }

    for (const auto& [id, device] : previousDevices) {
        const auto current = nextDevices.find(id);
        if (current == nextDevices.end() || current->second != device) {
            detail::addChangedDeviceTransport(plan, previousLinks, device);
            if (current != nextDevices.end())
                detail::addChangedDeviceTransport(plan, nextLinks, current->second);
        }
    }
    for (const auto& [id, device] : nextDevices) {
        if (!previousDevices.contains(id))
            detail::addChangedDeviceTransport(plan, nextLinks, device);
    }
    for (auto current = plan.refreshClientSessions.begin();
         current != plan.refreshClientSessions.end();) {
        if (!detail::targetAllowsSessionRefresh(previous, *current) ||
            !detail::targetAllowsSessionRefresh(next, *current)) {
            plan.restartClientTargets.insert(*current);
            current = plan.refreshClientSessions.erase(current);
        } else
            ++current;
    }
    return plan;
}


struct ProtocolConnectionInfo {
    std::string connectionId;
    std::string linkId;
    std::string remoteAddress;
    std::string targetId;
    std::uint64_t sessionEpoch = 0;
};

struct ProtocolSessionRefresh {
    std::string connectionId;
    std::vector<ProtocolAction> retiredActions;
    std::vector<ProtocolAction> startedActions;
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
        snapshot_ = std::make_shared<RuntimeSnapshot>(std::move(snapshot));
        links_.clear();
        for (const auto& link : snapshot_->links)
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
        (void)affectedLinks;
        snapshot_ = std::make_shared<RuntimeSnapshot>(std::move(snapshot));
        links_.clear();
        for (const auto& link : snapshot_->links)
            links_.emplace(link.id, &link);

        // TcpTransport owns transport reconciliation. Sessions are retained here until either the target
        // emits a real disconnected event or Worker selectively refreshes an eligible Modbus TCP
        // session after rotating its transport epoch.
    }

    [[nodiscard]] std::vector<ProtocolSessionRefresh>
    refreshClientSessions(const std::set<ClientTargetKey>& targets) {
        std::vector<ProtocolSessionRefresh> refreshed;
        for (auto& [connectionId, entry] : sessions_) {
            if (entry.info.targetId.empty() ||
                !targets.contains({entry.info.linkId, entry.info.targetId}))
                continue;

            const auto link = links_.find(entry.info.linkId);
            if (link == links_.end() || link->second->mode != "TCP Client" ||
                link->second->status != "enabled")
                continue;
            const auto target = std::find_if(
                link->second->targets.begin(), link->second->targets.end(),
                [&entry](const auto& candidate) {
                    return candidate.id == entry.info.targetId && candidate.status == "enabled";
                });
            // A removed or disabled target is being closed by TcpTransport. Let its real disconnected
            // event retire the protocol session and connection epoch in the normal order.
            if (target == link->second->targets.end())
                continue;

            const auto& runtime = registry_.require(link->second->protocol);
            auto nextSession = runtime.createSession(*link->second, connectionId,
                                                     entry.info.targetId, snapshot_);
            if (!nextSession)
                throw std::runtime_error("protocol runtime returned a null session");
            nextSession->inheritTransportState(*entry.session);
            auto startedActions = nextSession->connected();
            auto retiredActions = entry.session->disconnected("configuration_reloaded");
            entry.session = std::move(nextSession);
            refreshed.push_back({.connectionId = connectionId,
                                 .retiredActions = std::move(retiredActions),
                                 .startedActions = std::move(startedActions)});
        }
        return refreshed;
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

    [[nodiscard]] std::vector<ProtocolAction> consume(const message::IngressPacket& packet) {
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
    std::shared_ptr<const RuntimeSnapshot> snapshot_ = std::make_shared<RuntimeSnapshot>();
    std::map<std::string, const LinkDefinition*, std::less<>> links_;
    std::map<std::string, SessionEntry, std::less<>> sessions_;
};

} // namespace service::collector
