#pragma once
#include "client_state_store.h"
#include "platform_vpn_api.h"
#include "wireguard_tunnel.h"
#include <condition_variable>
#include <mutex>
#include <optional>

namespace iotvpn::service {
struct VpnServiceStatus {
    std::string state = "LoggedOut", username, peerId, assignedIpv4, error, lastSyncAt;
    std::vector<std::string> edgeNodeIds, allowedRoutes;
    bool tunnelRunning = false;
    Json toJson() const;
};
class VpnConnectionService {
public:
    VpnConnectionService(std::shared_ptr<IClientStateStore>, std::shared_ptr<IPlatformVpnApi>, std::unique_ptr<WireGuardTunnel>);
    Json handle(const Json& request, std::stop_token stop = {});
    void run(std::stop_token stop);
    void stop();
    VpnServiceStatus status() const;
    static bool equivalentConfig(const Json& a, const Json& b);
private:
    void invalidateWatch();
    bool shouldWatch() const;
    void requireSession() const;
    void requirePeer() const;
    Json authorized(const std::function<Json(std::string_view)>& request, std::stop_token stop);
    void refreshLocked(std::string_view expectedToken, std::stop_token stop);
    void loginLocked(const Json& request, std::stop_token stop);
    void applyLocked(const Json& request, std::stop_token stop);
    void rememberPendingRevocation(std::string_view peerId);
    void clearLocalPeer(std::string_view peerId);
    Json recoverPendingEnrollment(std::stop_token stop, bool release = false);
    void retryPendingRevocations(std::stop_token stop);
    Json disconnectLocked(std::stop_token stop);
    Json logoutLocked(std::stop_token stop);
    void acceptConfig(const Json& config, std::stop_token stop);
    void apiFailure(const PlatformVpnApiError& error);
    void setStatus(std::string name, std::string error = {}, bool synced = false);
    Json success() const;
    std::shared_ptr<IClientStateStore> store_;
    std::shared_ptr<IPlatformVpnApi> api_;
    std::unique_ptr<WireGuardTunnel> tunnel_;
    Json state_;
    mutable std::mutex gate_, statusGate_;
    std::condition_variable_any changes_;
    std::shared_ptr<std::stop_source> watch_;
    std::uint64_t generation_ = 0;
    VpnServiceStatus status_;
};
}
