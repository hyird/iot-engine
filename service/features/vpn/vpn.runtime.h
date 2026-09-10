#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ruvia/core/StopToken.h>
#include <ruvia/core/Task.h>
#include <ruvia/web/WebWorker.h>

#include "service/features/vpn/firewall/firewall.transport.h"
#include "service/features/vpn/vpn.service.h"
#include "service/features/vpn/wireguard/wireguard.transport.h"
#include "service/utils/json.h"

namespace service::vpn {

inline std::string runtimeStatusJson(const wireguard::RuntimeStatus& result) {
    return "{\"platformSupported\":" +
        std::string(result.supported ? "true" : "false") +
        ",\"configured\":" + std::string(result.configured ? "true" : "false") +
        ",\"code\":" + service::utils::jsonQuoted(result.code) +
        ",\"message\":" + service::utils::jsonQuoted(result.message) +
        ",\"runtimePeerCount\":" + std::to_string(result.peerCount) + "}";
}

class Runtime final {
  public:
    explicit Runtime(wireguard::HubConfig config) : hubConfig_(std::move(config)) {}

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    ~Runtime() { stop(); }

    void start(ruvia::WebWorkerHandle worker) {
        if (running_.exchange(true)) {
            return;
        }
        worker_ = worker;
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        stopped_ = stopped->get_future().share();
        const auto posted = worker_.post([this, ready, stopped](ruvia::WebWorkerContext& context) {
            return run(context, ready, stopped);
        });
        if (!posted.accepted()) {
            running_.store(false);
            worker_ = {};
            stopped_ = {};
            throw std::runtime_error("service rejected VPN runtime");
        }
        ready->get_future().get();
    }

    void stop() noexcept {
        if (!running_.exchange(false)) {
            return;
        }
        if (stopped_.valid()) {
            stopped_.wait();
        }
        stopped_ = {};
        worker_ = {};
    }

    static ruvia::Task<wireguard::RuntimeStatus> reconcileNow(
        ruvia::WebWorkerContext& context,
        const wireguard::HubConfig& fallback
    ) {
        co_return co_await reconcile(context, fallback);
    }

    static ruvia::Task<wireguard::RuntimeStatus> status(
        ruvia::WebWorkerContext& context,
        const wireguard::HubConfig& fallback
    ) {
        const auto config = co_await hub_config::loadOrInitialize(context, fallback);
        if (!config) {
            co_return wireguard::RuntimeStatus{
                .supported = true,
                .configured = false,
                .code = "hub_config_missing",
                .message = "WireGuard Hub 配置尚未初始化",
                .peerCount = 0
            };
        }
        co_return wireguard::controller().status(*config);
    }

  private:
    static ruvia::Task<wireguard::RuntimeStatus> reconcile(
        ruvia::WebWorkerContext& context,
        const wireguard::HubConfig& fallback,
        bool background = false
    ) {
        // The interface is an external singleton. All workers run the same job,
        // but only the transaction owner applies a particular reconciliation.
        // A separate worker-local connection keeps the lock while business
        // queries use their ordinary connection, without pool self-deadlock.
        auto ownership = co_await context.db("vpn-coordination").beginTransaction();
        if (background) {
            const auto locked = co_await ownership.query(
                "SELECT pg_try_advisory_xact_lock(5282804697543808071::bigint)"
            );
            if (locked.empty() || locked[0][0].value().value_or("") != "t") {
                co_return wireguard::RuntimeStatus{ .code = "reconciliation_in_progress" };
            }
        } else {
            (void)co_await ownership.query(
                "SELECT pg_advisory_xact_lock(5282804697543808071::bigint)"
            );
        }
        const auto key = "iot:vpn:reconciled:" + service::runtime::instanceId();
        if (background) {
            const auto previous = co_await service::message::redis::command(context.redis(), { "GET", key });
            if (previous.kind() == ruvia::RedisValue::Kind::kString) {
                co_return wireguard::RuntimeStatus{ .code = "reconciliation_current" };
            }
            if (previous.kind() == ruvia::RedisValue::Kind::kError) {
                service::message::redis::throwValue("VPN reconciliation schedule", previous);
            }
        }
        auto result = co_await reconcileLocal(context, fallback);
        const auto scheduled = co_await service::message::redis::command(
            context.redis(),
            { "SET", key, "1", "PX", "10000" }
        );
        if (scheduled.kind() == ruvia::RedisValue::Kind::kError) {
            service::message::redis::throwValue("VPN reconciliation schedule", scheduled);
        }
        co_await ownership.commit();
        co_return result;
    }

    static ruvia::Task<wireguard::RuntimeStatus> reconcileLocal(
        ruvia::WebWorkerContext& context,
        const wireguard::HubConfig& fallback
    ) {
        const auto config = co_await hub_config::loadOrInitialize(context, fallback);
        if (!config) {
            co_return wireguard::RuntimeStatus{
                .supported = true,
                .configured = false,
                .code = "hub_config_missing",
                .message = "WireGuard Hub 配置尚未初始化",
                .peerCount = 0
            };
        }
        auto& controller = wireguard::controller();
        auto result = controller.configure(*config);
        if (!result.configured) {
            co_return result;
        }
        const auto peers = co_await VpnRuntimeService::loadActivePeers(context);
        std::vector<firewall::ClientAccess> clients;
        std::vector<std::string> expectedRoutes;
        std::size_t configuredPeers = 0;
        for (const auto& peerRecord : peers) {
            const auto& publicKey = peerRecord.publicKey;
            const auto& assigned = peerRecord.assignedIpv4;
            const auto& peerType = peerRecord.peerType;
            if (!wireguard::validKey(publicKey) || !parseIpv4(assigned)) {
                continue;
            }
            wireguard::Peer peer;
            peer.publicKey = publicKey;
            peer.allowedIps.emplace_back(assigned + "/32");
            if (peerType == "edge") {
                firewall::ClientAccess client{ .assignedIpv4 = assigned };
                std::stringstream routes(peerRecord.sourceRoutes);
                std::string route;
                while (std::getline(routes, route, ',')) {
                    if (!route.empty() && route.front() == ' ') {
                        route.erase(route.begin());
                    }
                    if (!route.empty()) {
                        peer.allowedIps.push_back(route);
                        client.sourceRoutes.push_back(std::move(route));
                    }
                }
                std::stringstream allowedRoutes(peerRecord.allowedRoutes);
                while (std::getline(allowedRoutes, route, ',')) {
                    if (!route.empty() && route.front() == ' ') {
                        route.erase(route.begin());
                    }
                    if (!route.empty()) {
                        client.allowedRoutes.push_back(std::move(route));
                    }
                }
                clients.push_back(std::move(client));
            } else if (peerType == "windows") {
                firewall::ClientAccess client{ .assignedIpv4 = assigned };
                std::stringstream routes(peerRecord.allowedRoutes);
                std::string route;
                while (std::getline(routes, route, ',')) {
                    if (!route.empty() && route.front() == ' ') {
                        route.erase(route.begin());
                    }
                    if (!route.empty()) {
                        client.allowedRoutes.push_back(std::move(route));
                    }
                }
                std::stringstream edgeAddresses(peerRecord.edgeAddresses);
                std::string edgeAddress;
                while (std::getline(edgeAddresses, edgeAddress, ',')) {
                    if (!edgeAddress.empty() && edgeAddress.front() == ' ') {
                        edgeAddress.erase(edgeAddress.begin());
                    }
                    if (!edgeAddress.empty()) {
                        client.edgeAddresses.push_back(std::move(edgeAddress));
                    }
                }
                clients.push_back(std::move(client));
            }
            const auto peerResult = controller.upsertPeer(*config, peer);
            if (!peerResult.configured) {
                co_return peerResult;
            }
            expectedRoutes.insert(expectedRoutes.end(), peer.allowedIps.begin(), peer.allowedIps.end());
            ++configuredPeers;
        }
        if (const auto currentPeers = controller.peerKeys(*config)) {
            std::unordered_set<std::string> expected;
            expected.reserve(peers.size());
            for (const auto& peerRecord : peers) {
                const auto& publicKey = peerRecord.publicKey;
                const auto& assigned = peerRecord.assignedIpv4;
                if (wireguard::validKey(publicKey) && parseIpv4(assigned)) {
                    expected.insert(publicKey);
                }
            }
            for (const auto& publicKey : *currentPeers) {
                if (!expected.contains(publicKey)) {
                    (void)controller.removePeer(*config, publicKey);
                }
            }
        }
        const auto routeResult = controller.reconcileRoutes(*config, expectedRoutes);
        if (!routeResult.configured) {
            co_return routeResult;
        }
        const auto firewallResult = firewall::apply(config->interfaceName, clients);
        if (!firewallResult.configured) {
            co_return wireguard::RuntimeStatus{
                .supported = true,
                .configured = false,
                .code = "firewall_configure_failed",
                .message = firewallResult.message,
                .peerCount = configuredPeers
            };
        }
        if (const auto handshakes = controller.peerHandshakes(*config)) {
            for (const auto& [publicKey, seconds] : *handshakes) {
                if (seconds == 0 || seconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    continue;
                }
                co_await VpnRuntimeService::updatePeerHandshake(
                    context,
                    publicKey,
                    static_cast<std::int64_t>(seconds)
                );
            }
        }
        result.peerCount = configuredPeers;
        result.message = "WireGuard hub is configured";
        co_return result;
    }

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, const std::shared_ptr<std::promise<void>>& ready, const std::shared_ptr<std::promise<void>>& stopped) {
        try {
            ready->set_value();
            while (running_.load() && !context.stopToken().stopRequested()) {
                try {
                    co_await reconcile(context, hubConfig_, true);
                } catch (const std::exception& error) {
                    std::cerr << "VPN runtime reconciliation failed: " << error.what() << '\n';
                }
                for (int tick = 0; tick < 10 && running_.load() &&
                     !context.stopToken().stopRequested();
                     ++tick) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1));
                }
            }
        } catch (...) {
            try {
                ready->set_exception(std::current_exception());
            } catch (...) {
            }
        }
        try {
            stopped->set_value();
        } catch (...) {
        }
    }

    std::atomic_bool running_{ false };
    wireguard::HubConfig hubConfig_;
    ruvia::WebWorkerHandle worker_;
    std::shared_future<void> stopped_;
};

class VpnControlRuntime final {
  public:
    explicit VpnControlRuntime(
        wireguard::HubConfig fallback,
        std::string platformId =
            std::string(service::edge::protocol::kDefaultPlatformId)
    )
        : fallback_(std::move(fallback)), platformId_(std::move(platformId)) {}

    ruvia::Task<std::string> handle(ruvia::WebWorkerContext& context, std::string_view operation, std::string_view payload, ruvia::StopToken stop) {
        if (stop.stopRequested()) {
            service::common::fail(10004, "VPN background operation cancelled", 503);
        }

        if (operation == "queue-edge-config") {
            const auto separator = payload.find('\n');
            if (separator == std::string_view::npos || separator == 0 ||
                separator + 1 >= payload.size() ||
                !service::common::isUuid(payload.substr(0, separator)) ||
                !service::common::isUuid(payload.substr(separator + 1))) {
                service::common::fail(10002, "VPN Edge 配置请求无效", 400);
            }
            co_await queueEdgeConfig(context, payload.substr(0, separator), payload.substr(separator + 1), platformId_);
            co_return "{}";
        }

        if (operation == "reconcile" || operation == "wireguard-reconcile" ||
            operation == "firewall-reconcile") {
            const auto result = co_await Runtime::reconcileNow(context, fallback_);
            co_return runtimeStatusJson(result);
        }

        if (operation == "wireguard-status") {
            const auto result = co_await Runtime::status(context, fallback_);
            co_return runtimeStatusJson(result);
        }

        if (operation == "wireguard-remove-peer") {
            if (!wireguard::validKey(payload)) {
                service::common::fail(10002, "WireGuard Peer 公钥无效", 400);
            }
            auto ownership = co_await context.db("vpn-coordination").beginTransaction();
            (void)co_await ownership.query("SELECT pg_advisory_xact_lock(5282804697543808071::bigint)");
            const auto config = co_await hub_config::loadOrInitialize(context, fallback_);
            if (config) {
                (void)wireguard::controller().removePeer(*config, payload);
            }
            co_await ownership.commit();
            co_return "{}";
        }

        service::common::fail(10002, "Unknown VPN background operation", 400);
    }

  private:
    wireguard::HubConfig fallback_;
    std::string platformId_;
};

} // namespace service::vpn
