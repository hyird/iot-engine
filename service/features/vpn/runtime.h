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

#include <ruvia/core/Task.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/http.h"
#include "service/features/vpn/cidr.h"
#include "service/features/vpn/firewall.h"
#include "service/features/vpn/hub-config.h"
#include "service/features/vpn/wireguard.h"

namespace service::vpn {

class Runtime final {
  public:
    explicit Runtime(wireguard::HubConfig config) : hubConfig_(std::move(config)) {}
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime() { stop(); }

    void start(std::vector<ruvia::WebWorkerHandle> workers) {
        if (running_.exchange(true))
            return;
        if (workers.empty()) {
            running_.store(false);
            throw std::runtime_error("VPN runtime requires a service worker");
        }
        worker_ = workers.front();
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
        if (!running_.exchange(false))
            return;
        if (stopped_.valid())
            (void)stopped_.wait_for(std::chrono::seconds(3));
        stopped_ = {};
        worker_ = {};
    }

  private:
    static ruvia::Task<void> reconcile(ruvia::WebWorkerContext& context,
                                       const wireguard::HubConfig& fallback) {
        const auto config = co_await hub_config::loadOrInitialize(context, fallback);
        if (!config)
            co_return;
        if (!wireguard::validKey(config->privateKey) || config->listenPort == 0)
            co_return;
        const auto rows = co_await context.db().query(R"sql(
SELECT p.public_key, host(p.assigned_ipv4), p.peer_type,
       COALESCE(string_agg(r.virtual_cidr, ', ' ORDER BY r.virtual_cidr), ''),
       COALESCE((SELECT string_agg(value, ', ' ORDER BY value)
                 FROM jsonb_array_elements_text(p.allowed_routes) value), '')
FROM vpn_peer p JOIN vpn_network n ON n.id = p.network_id
LEFT JOIN vpn_route r ON r.edge_peer_id = p.id AND r.enabled
WHERE p.status = 'active' AND n.status = 'enabled' AND p.public_key <> ''
GROUP BY p.id, p.public_key, p.assigned_ipv4, p.peer_type, p.allowed_routes
ORDER BY p.id)sql");
        auto& controller = wireguard::controller();
        const auto configured = controller.configure(*config);
        if (!configured.configured)
            co_return;
        std::vector<firewall::ClientAccess> clients;
        for (const auto& row : rows) {
            const auto publicKey = std::string(row[0].value().value_or(std::string_view{}));
            const auto assigned = std::string(row[1].value().value_or(std::string_view{}));
            const auto peerType = std::string(row[2].value().value_or(std::string_view{}));
            if (!wireguard::validKey(publicKey) || !parseIpv4(assigned))
                continue;
            wireguard::Peer peer;
            peer.publicKey = publicKey;
            peer.allowedIps.emplace_back(assigned + "/32");
            if (peerType == "edge") {
                std::stringstream routes(
                    std::string(row[3].value().value_or(std::string_view{})));
                std::string route;
                while (std::getline(routes, route, ',')) {
                    if (!route.empty() && route.front() == ' ')
                        route.erase(route.begin());
                    if (!route.empty())
                        peer.allowedIps.push_back(std::move(route));
                }
            } else if (peerType == "windows") {
                firewall::ClientAccess client{.assignedIpv4 = assigned};
                std::stringstream routes(
                    std::string(row[4].value().value_or(std::string_view{})));
                std::string route;
                while (std::getline(routes, route, ',')) {
                    if (!route.empty() && route.front() == ' ')
                        route.erase(route.begin());
                    if (!route.empty())
                        client.allowedRoutes.push_back(std::move(route));
                }
                clients.push_back(std::move(client));
            }
            (void)controller.upsertPeer(*config, peer);
        }
        if (const auto currentPeers = controller.peerKeys(*config)) {
            std::unordered_set<std::string> expected;
            expected.reserve(rows.size());
            for (const auto& row : rows) {
                const auto publicKey = std::string(row[0].value().value_or(std::string_view{}));
                const auto assigned = std::string(row[1].value().value_or(std::string_view{}));
                if (wireguard::validKey(publicKey) && parseIpv4(assigned))
                    expected.insert(publicKey);
            }
            for (const auto& publicKey : *currentPeers)
                if (!expected.contains(publicKey))
                    (void)controller.removePeer(*config, publicKey);
        }
        (void)firewall::apply(config->interfaceName, clients);
        if (const auto handshakes = controller.peerHandshakes(*config)) {
            for (const auto& [publicKey, seconds] : *handshakes) {
                if (seconds == 0 || seconds >
                                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    continue;
                (void)co_await context.db().execute(
                    "UPDATE vpn_peer SET last_handshake_at = to_timestamp($2::double precision), "
                    "updated_at = NOW() WHERE public_key = $1 AND status = 'active' "
                    "AND (last_handshake_at IS NULL OR last_handshake_at < "
                    "to_timestamp($2::double precision))",
                    service::common::dbParams(publicKey, static_cast<std::int64_t>(seconds)));
            }
        }
    }

    ruvia::Task<void> run(ruvia::WebWorkerContext& context,
                          const std::shared_ptr<std::promise<void>>& ready,
                          const std::shared_ptr<std::promise<void>>& stopped) {
        try {
            ready->set_value();
            while (running_.load() && !context.stopToken().stopRequested()) {
                try {
                    co_await reconcile(context, hubConfig_);
                } catch (const std::exception& error) {
                    std::cerr << "VPN runtime reconciliation failed: " << error.what() << '\n';
                }
                for (int tick = 0; tick < 10 && running_.load() &&
                                     !context.stopToken().stopRequested(); ++tick)
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1));
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

    std::atomic_bool running_{false};
    wireguard::HubConfig hubConfig_;
    ruvia::WebWorkerHandle worker_;
    std::shared_future<void> stopped_;
};

} // namespace service::vpn
