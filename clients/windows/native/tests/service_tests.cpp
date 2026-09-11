#include "../service/service.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <thread>

using namespace iotvpn;
using namespace iotvpn::service;
using namespace std::chrono_literals;
namespace {
const std::string Edge = "11111111-1111-4111-8111-111111111111";
const std::string Other = "22222222-2222-4222-8222-222222222222";
const std::string PeerTwo = "33333333-3333-4333-8333-333333333333";
const std::string Key = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=";
const std::string KeyTwo = "AgEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=";
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Json loginResponse(std::string token = "access", std::string refresh = "refresh") {
    return {{"token", token}, {"refresh_token", refresh}, {"user", {{"id", "account"}, {"username", "alice"}}}};
}
Json config(std::string route = "172.16.0.0/16", int revision = 1) {
    return {{"peerId", Edge}, {"publicKey", Key}, {"assignedIpv4", "100.96.0.2"}, {"hubPublicKey", Key},
        {"hubEndpoint", "vpn.example"}, {"hubListenPort", 51820}, {"mtu", 1420}, {"persistentKeepalive", 25},
        {"edgeNodeIds", {Edge}}, {"allowedRoutes", {route, "100.96.0.3/32"}}, {"edgeAddresses", {"100.96.0.3"}}, {"configRevision", revision}};
}
Json initialState(bool peer = true) {
    Json state{{"session", {{"serverUrl", PlatformUrl}, {"userId", "account"}, {"username", "alice"}, {"token", "access"}, {"refreshToken", "refresh"}}}};
    if (peer) {
        state["peerId"] = Edge; state["publicKey"] = Key; state["privateKey"] = Key; state["edgeNodeIds"] = {Edge};
    }
    return state;
}
struct Store final : IClientStateStore {
    Json state = Json::object(); bool failLoad = false, failSave = false; int saveCount = 0;
    Json load() override { if (failLoad) throw std::runtime_error("corrupt DPAPI state"); return state; }
    void save(const Json& value) override { ++saveCount; if (failSave) throw std::runtime_error("disk full"); state = value; }
};
struct TunnelLog { std::atomic<bool> on{false}; std::atomic<int> applies{0}, stops{0}, keys{0}; };
struct FakeTunnel final : WireGuardTunnel {
    std::shared_ptr<TunnelLog> log;
    explicit FakeTunnel(std::shared_ptr<TunnelLog> value) : log(std::move(value)) {}
    std::pair<std::string, std::string> generateKeys() override { return ++log->keys == 1 ? std::pair{Key, Key} : std::pair{KeyTwo, KeyTwo}; }
    bool running() override { return log->on; }
    void stop() override { log->on = false; ++log->stops; }
    void apply(const Json& value, const std::string& privateKey) override {
        validateTunnelConfig(value, privateKey); log->on = true; ++log->applies;
    }
};
struct Api final : IPlatformVpnApi {
    std::atomic<int> loginCalls{0}, refreshCalls{0}, deviceCalls{0}, removeCalls{0}, applyCalls{0}, postCalls{0};
    bool firstDeviceExpired = false, removeFails = false, losePostResponse = false, enrollmentAlreadyRevoked = false, enrollmentNetworkUnavailable = false;
    int removeFailures = 0;
    Json loginData = loginResponse(), refreshedData = loginResponse("new-access", "new-refresh"), current = ::config();
    Json lastApply;
    std::string lastPeer, lastAccess, lastRefresh;
    std::string registeredPublicKey, registeredPeer;
    std::vector<std::string> removedPeers;
    std::function<void()> beforeRemove;
    std::function<void(const std::function<void(const Json&)>&, std::stop_token)> watch;
    Json login(std::string_view, std::string_view, std::stop_token) override { ++loginCalls; return loginData; }
    Json refresh(std::string_view token, std::stop_token) override { ++refreshCalls; lastRefresh = token; return refreshedData; }
    Json devices(std::string_view token, std::stop_token) override {
        lastAccess = token;
        if (++deviceCalls == 1 && firstDeviceExpired) throw PlatformVpnApiError("expired", 401, 11006);
        return Json::array({{{"id", Edge}}});
    }
    Json apply(std::string_view, std::string_view peer, const Json& body, std::stop_token) override {
        ++applyCalls; lastPeer = peer; lastApply = body;
        Json result = current;
        if (peer.empty()) {
            ++postCalls;
            if (enrollmentNetworkUnavailable) throw PlatformVpnApiError("network unavailable", 404);
            if (enrollmentAlreadyRevoked) {
                enrollmentAlreadyRevoked = false;
                throw PlatformVpnApiError("enrollment already revoked", 410, 21009);
            }
            const auto publicKey = body.at("publicKey").get<std::string>();
            if (registeredPublicKey != publicKey || registeredPeer.empty()) {
                registeredPublicKey = publicKey; registeredPeer = PeerTwo;
            }
            result["peerId"] = registeredPeer; result["publicKey"] = registeredPublicKey;
        } else {
            result["peerId"] = std::string(peer); result["publicKey"] = Json(Key);
        }
        result["edgeNodeIds"] = body.at("edgeNodeIds");
        if (body.at("edgeNodeIds").empty()) { result["allowedRoutes"] = Json::array(); result["edgeAddresses"] = Json::array(); }
        if (peer.empty() && losePostResponse) {
            losePostResponse = false;
            throw std::runtime_error("response lost after registration");
        }
        return result;
    }
    Json config(std::string_view, std::string_view, std::stop_token) override { return current; }
    void remove(std::string_view, std::string_view peer, std::stop_token) override {
        ++removeCalls; removedPeers.emplace_back(peer); if (beforeRemove) beforeRemove();
        if (removeFailures > 0) { --removeFailures; throw PlatformVpnApiError("platform offline", 503); }
        if (removeFails) throw PlatformVpnApiError("platform offline", 503);
    }
    void watchConfig(std::string_view, std::string_view, const std::function<void(const Json&)>& receive, std::stop_token stop) override {
        if (watch) watch(receive, stop);
    }
};
struct Fixture {
    std::shared_ptr<Store> store = std::make_shared<Store>();
    std::shared_ptr<Api> api = std::make_shared<Api>();
    std::shared_ptr<TunnelLog> tunnel = std::make_shared<TunnelLog>();
    std::unique_ptr<VpnConnectionService> coordinator;
    explicit Fixture(Json state = initialState()) {
        if (state.contains("publicKey")) tunnel->keys = 1;
        store->state = std::move(state); coordinator = std::make_unique<VpnConnectionService>(store, api, std::make_unique<FakeTunnel>(tunnel));
    }
    Json command(const char* name) { return coordinator->handle({{"command", name}}); }
};
void waitFor(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!condition()) { if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("timed out waiting for test event"); std::this_thread::sleep_for(2ms); }
}
void holdUntilCancelled(std::stop_token stop) {
    std::mutex mutex; std::condition_variable_any condition; std::unique_lock lock(mutex);
    condition.wait(lock, stop, [] { return false; });
}
int passedTestCount = 0, failedTestCount = 0;
template<class Test> void test(const char* name, Test body) {
    try { body(); ++passedTestCount; std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) { ++failedTestCount; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
}
}
int main() {
    test("fixed origin and prior-origin state", [] {
        Fixture fixture;
        const auto response = fixture.coordinator->handle({{"command", "login"}, {"serverUrl", "https://foreign.test"}, {"username", "alice"}, {"password", "secret"}});
        require(!response.value("success", true) && fixture.api->loginCalls == 0, "foreign credentials reached transport");
        auto state = initialState(); state["session"]["serverUrl"] = "https://foreign.test";
        Fixture migrated(state); require(migrated.store->state.empty(), "foreign-origin state retained");
    });
    test("login and actual expired-token refresh", [] {
        Fixture fixture(Json::object());
        require(fixture.coordinator->handle({{"command", "login"}, {"username", "alice"}, {"password", "secret"}}).value("success", false), "login failed");
        fixture.api->firstDeviceExpired = true;
        require(fixture.command("devices").value("success", false), "refresh request failed");
        require(fixture.api->refreshCalls == 1 && fixture.api->deviceCalls == 2 && fixture.api->lastRefresh == "refresh" && fixture.api->lastAccess == "new-access", "token retry contract incorrect");
        require(fixture.store->state["session"]["refreshToken"] == "new-refresh", "rotated refresh token not saved");
    });
    test("first selection registers public key without private key", [] {
        Fixture fixture(initialState(false));
        require(fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}}).value("success", false), "first selection failed");
        require(fixture.api->lastPeer.empty() && fixture.api->lastApply["publicKey"] == Key && !fixture.api->lastApply.contains("privateKey"), "POST body leaks key or uses existing peer");
        require(fixture.tunnel->keys == 1 && fixture.tunnel->on, "local keys and connection missing");
    });
    test("existing selection uses peer and empty selection stops", [] {
        Fixture fixture;
        require(fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}}).value("success", false), "selection failed");
        require(fixture.api->lastPeer == Edge && !fixture.api->lastApply.contains("publicKey"), "existing selection did not use PATCH contract");
        fixture.api->beforeRemove = [&] { require(!fixture.tunnel->on, "DELETE happened before tunnel stop"); };
        require(fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", Json::array()}}).value("success", false), "empty selection failed");
        require(!fixture.tunnel->on && !fixture.store->state["connectRequested"].get<bool>() && fixture.api->removeCalls == 1 &&
            !fixture.store->state.contains("peerId") && !fixture.store->state.contains("privateKey") &&
            fixture.store->state["edgeNodeIds"].empty(), "clearing selection did not release the peer");
    });
    test("disconnect sync and reconnect", [] {
        Fixture fixture;
        require(fixture.command("connect").value("success", false) && fixture.tunnel->on, "connect failed");
        require(fixture.command("disconnect").value("success", false) && !fixture.tunnel->on && fixture.api->removeCalls == 1, "disconnect failed");
        require(!fixture.store->state.contains("peerId") && fixture.store->state.contains("session") &&
            fixture.store->state["edgeNodeIds"].size() == 1, "disconnect did not preserve the session and selection");
        fixture.api->current = config("172.17.0.0/16", 2);
        require(!fixture.command("sync").value("success", true) && !fixture.tunnel->on, "sync bypassed peer registration");
        require(fixture.command("connect").value("success", false) && fixture.tunnel->applies == 2 && fixture.api->postCalls == 1 &&
            fixture.api->lastPeer.empty() && fixture.api->lastApply["publicKey"] == KeyTwo && fixture.store->state["peerId"] == PeerTwo,
            "reconnect did not register a fresh key and peer");
    });
    test("disconnect failure retains identity and reconnect retries revoke", [] {
        Fixture fixture; require(fixture.command("connect").value("success", false), "setup connect");
        fixture.api->removeFailures = 1;
        const auto failed = fixture.command("disconnect");
        require(!failed.value("success", true) && !fixture.tunnel->on && fixture.store->state["peerId"] == Edge &&
            fixture.store->state["pendingRevocations"].size() == 1, "failed disconnect lost the pending peer");
        require(fixture.command("connect").value("success", false) && fixture.api->removeCalls == 2 && fixture.api->postCalls == 1 &&
            fixture.store->state["peerId"] == PeerTwo && !fixture.store->state.contains("pendingRevocations") &&
            fixture.api->lastApply["publicKey"] == KeyTwo, "reconnect did not retry revoke before registering");
    });
    test("logout clears local state on platform failure", [] {
        Fixture fixture; require(fixture.command("connect").value("success", false), "setup connect"); fixture.api->removeFails = true;
        const auto result = fixture.command("logout");
        require(result.value("success", false) && result.contains("message") && fixture.store->state.empty() == false &&
            !fixture.store->state.contains("session") && !fixture.store->state.contains("peerId") &&
            fixture.store->state["pendingRevocations"].size() == 1 && !fixture.tunnel->on, "logout lost the pending revoke");
    });
    test("account switch keeps an old account pending revoke", [] {
        Fixture fixture; require(fixture.command("connect").value("success", false), "setup connect");
        fixture.api->removeFailures = 1; fixture.api->loginData["user"]["id"] = "second-account";
        const auto result = fixture.coordinator->handle({{"command", "login"}, {"username", "bob"}, {"password", "secret"}});
        require(!result.value("success", true) && fixture.store->state["session"]["userId"] == "account" &&
            fixture.store->state["peerId"] == Edge && fixture.store->state["pendingRevocations"].size() == 1 && !fixture.tunnel->on,
            "account switch crossed an uncleared peer");
        fixture.api->removeFailures = 0;
        const auto retried = fixture.coordinator->handle({{"command", "login"}, {"username", "bob"}, {"password", "secret"}});
        require(retried.value("success", false) && fixture.store->state["session"]["userId"] == "second-account" &&
            !fixture.store->state.contains("peerId") && !fixture.store->state.contains("pendingRevocations"),
            "account switch did not complete after revoke retry");
    });
    test("lost registration response reuses the same enrollment key", [] {
        Fixture fixture(initialState(false)); fixture.api->losePostResponse = true;
        const auto failed = fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}});
        require(!failed.value("success", true) && fixture.tunnel->keys == 1 && !fixture.store->state.contains("peerId") &&
            fixture.store->state["pendingEnrollment"]["publicKey"] == Key && fixture.store->state["pendingEnrollment"]["edgeNodeIds"] == Json::array({Edge}),
            "lost registration response did not persist the enrollment request");
        const auto retry = fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}});
        require(retry.value("success", false) && fixture.api->postCalls == 2 && fixture.tunnel->keys == 1 &&
            fixture.api->lastApply["publicKey"] == Key && fixture.store->state["peerId"] == PeerTwo &&
            !fixture.store->state.contains("pendingEnrollment"), "lost registration response generated a new key");
    });
    test("already revoked enrollment is replaced after the explicit server result", [] {
        Fixture fixture(initialState(false)); fixture.api->losePostResponse = true;
        require(!fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}}).value("success", true), "setup lost POST did not fail");
        fixture.api->enrollmentAlreadyRevoked = true;
        const auto retry = fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}});
        require(retry.value("success", false) && fixture.api->postCalls == 3 && fixture.tunnel->keys == 2 &&
            fixture.api->lastApply["publicKey"] == KeyTwo && fixture.store->state["peerId"] == PeerTwo &&
            !fixture.store->state.contains("pendingEnrollment"), "explicit revoked enrollment was not replaced");
    });
    test("network failure never clears an unconfirmed enrollment", [] {
        Fixture fixture(initialState(false)); fixture.api->losePostResponse = true;
        require(!fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}}).value("success", true), "setup lost POST did not fail");
        fixture.api->enrollmentNetworkUnavailable = true;
        const auto failed = fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}});
        require(!failed.value("success", true) && fixture.tunnel->keys == 1 && fixture.store->state["pendingEnrollment"]["publicKey"] == Key,
            "network failure discarded the pending enrollment");
        fixture.api->enrollmentNetworkUnavailable = false;
        const auto retry = fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}});
        require(retry.value("success", false) && fixture.tunnel->keys == 1 && fixture.api->postCalls == 3 &&
            fixture.store->state["peerId"] == PeerTwo && !fixture.store->state.contains("pendingEnrollment"),
            "enrollment did not retry after network recovery");
    });
    test("release intent survives restart after enrollment recovery", [] {
        Fixture fixture(initialState(false)); fixture.api->losePostResponse = true;
        require(!fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}}).value("success", true), "setup lost POST did not fail");
        fixture.api->beforeRemove = [] { throw std::runtime_error("simulated process interruption"); };
        const auto failed = fixture.command("disconnect");
        require(!failed.value("success", true) && fixture.store->state["pendingRevocations"].size() == 1 &&
            !fixture.store->state.contains("pendingEnrollment") && fixture.store->state["peerId"] == PeerTwo,
            "recovered enrollment did not persist release intent");
        fixture.coordinator.reset(); fixture.api->beforeRemove = {};
        fixture.coordinator = std::make_unique<VpnConnectionService>(fixture.store, fixture.api, std::make_unique<FakeTunnel>(fixture.tunnel));
        require(fixture.command("connect").value("success", false) && fixture.api->removeCalls == 2 && fixture.api->postCalls == 3 &&
            fixture.tunnel->keys == 2 && fixture.store->state["peerId"] == PeerTwo &&
            !fixture.store->state.contains("pendingRevocations"), "restart lost the release intent");
    });
    test("selection change and empty selection resolve an unconfirmed registration first", [] {
        Fixture fixture(initialState(false)); fixture.api->losePostResponse = true;
        require(!fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}}).value("success", true), "setup lost POST did not fail");
        const auto changed = fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Other}}});
        require(changed.value("success", false) && fixture.api->postCalls == 3 && fixture.api->removeCalls == 1 &&
            fixture.tunnel->keys == 2 && fixture.api->lastApply["publicKey"] == KeyTwo && fixture.store->state["edgeNodeIds"] == Json::array({Other}),
            "selection change abandoned the unconfirmed registration");
        const auto cleared = fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", Json::array()}});
        require(cleared.value("success", false) && fixture.api->removeCalls == 2 && !fixture.store->state.contains("peerId") &&
            !fixture.store->state.contains("pendingEnrollment") && !fixture.store->state.contains("privateKey") &&
            fixture.store->state["edgeNodeIds"].empty(), "empty selection did not release the replacement peer");
    });
    test("offline logout preserves pending enrollment for re-login", [] {
        Fixture fixture(initialState(false)); fixture.api->losePostResponse = true;
        require(!fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}}).value("success", true), "setup lost POST did not fail");
        fixture.api->losePostResponse = true;
        const auto loggedOut = fixture.command("logout");
        require(loggedOut.value("success", false) && fixture.store->state["pendingEnrollment"]["userId"] == "account" &&
            fixture.store->state.contains("publicKey") && fixture.store->state.contains("privateKey") && !fixture.store->state.contains("session"),
            "logout discarded the unconfirmed enrollment");
        fixture.api->losePostResponse = true;
        const auto blocked = fixture.coordinator->handle({{"command", "login"}, {"username", "alice"}, {"password", "secret"}});
        require(!blocked.value("success", true) && fixture.store->state["session"]["userId"] == "account" &&
            fixture.store->state.contains("pendingEnrollment"), "offline re-login discarded pending enrollment");
        fixture.api->losePostResponse = false;
        const auto retried = fixture.coordinator->handle({{"command", "login"}, {"username", "alice"}, {"password", "secret"}});
        require(retried.value("success", false) && fixture.api->postCalls == 4 && !fixture.store->state.contains("pendingEnrollment") &&
            !fixture.store->state.contains("pendingRevocations"), "re-login did not recover and release pending enrollment");
    });
    test("cross-account login revokes previous peer", [] {
        Fixture fixture; fixture.command("connect"); fixture.api->loginData["user"]["id"] = "second-account";
        const auto result = fixture.coordinator->handle({{"command", "login"}, {"username", "bob"}, {"password", "secret"}});
        require(result.value("success", false) && fixture.api->removeCalls == 1 && !fixture.store->state.contains("privateKey") && !fixture.tunnel->on, "account state crossed login boundary");
    });
    test("effective configuration ignores revision and route ordering", [] {
        auto previous = config(); auto same = config("172.16.0.0/16", 2); std::reverse(same["allowedRoutes"].begin(), same["allowedRoutes"].end());
        require(VpnConnectionService::equivalentConfig(previous, same), "revision caused effective change");
        require(!VpnConnectionService::equivalentConfig(previous, config("172.17.0.0/16")), "route change ignored");
    });
    test("SSE fragmentation CRLF multiline and early stop", [] {
        SseParser parser; std::vector<std::pair<std::string, std::string>> events;
        const std::string wire = ": ping\r\nevent: snapshot\r\ndata: first\r\ndata: second\r\n\r\nevent: error\ndata: denied\n\n";
        for (const char byte : wire) parser.feed({&byte, 1}, [&](std::string_view event, std::string_view data) { events.emplace_back(event, data); return true; });
        require(events.size() == 2 && events[0] == std::pair<std::string, std::string>{"snapshot", "first\nsecond"} && events[1].first == "error", "fragmented event was lost");
        SseParser once; int calls = 0;
        require(!once.feed("data: one\n\ndata: two\n\n", [&](auto, auto) { ++calls; return false; }) && calls == 1, "one-shot stream did not stop");
    });
    test("consecutive SSE configurations apply only effective changes", [] {
        auto state = initialState(); state["connectRequested"] = true; Fixture fixture(state);
        fixture.api->watch = [](const auto& receive, std::stop_token stop) { receive(config()); receive(config("172.16.0.0/16", 2)); receive(config("172.17.0.0/16", 3)); holdUntilCancelled(stop); };
        std::jthread worker([&](std::stop_token stop) { fixture.coordinator->run(stop); });
        waitFor([&] { return fixture.tunnel->applies == 2; }); worker.request_stop(); worker.join();
        require(fixture.tunnel->applies == 2, "unnecessary tunnel restart on revision change");
    });
    test("stale stream event cannot reconnect after disconnect", [] {
        auto state = initialState(); state["connectRequested"] = true; Fixture fixture(state);
        std::atomic<bool> release{false}, delivered{false};
        fixture.api->watch = [&](const auto& receive, std::stop_token) {
            receive(config());
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!release && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
            if (release) { receive(config("172.17.0.0/16", 2)); delivered = true; }
        };
        std::jthread worker([&](std::stop_token stop) { fixture.coordinator->run(stop); });
        waitFor([&] { return fixture.tunnel->applies == 1; });
        require(fixture.command("disconnect").value("success", false), "disconnect failed"); release = true;
        waitFor([&] { return delivered.load(); }); worker.request_stop(); worker.join();
        require(!fixture.tunnel->on && fixture.tunnel->applies == 1, "stale callback restarted tunnel");
    });
    test("permission revocation stops a live stream", [] {
        auto state = initialState(); state["connectRequested"] = true; Fixture fixture(state);
        fixture.api->watch = [](const auto& receive, std::stop_token) { receive(config()); throw PlatformVpnApiError("permission removed", 403, 11007); };
        std::jthread worker([&](std::stop_token stop) { fixture.coordinator->run(stop); });
        waitFor([&] { return fixture.coordinator->status().state == "AuthorizationRequired"; }); worker.request_stop(); worker.join();
        require(!fixture.tunnel->on && fixture.tunnel->applies == 1, "revoked tunnel remained active");
    });
    test("configuration persistence failure stops a running tunnel", [] {
        Fixture fixture; require(fixture.command("connect").value("success", false), "setup connect");
        fixture.api->current = config("172.17.0.0/16", 2); fixture.store->failSave = true;
        require(!fixture.command("sync").value("success", true) && !fixture.tunnel->on, "failed persistence retained new tunnel");
    });
    test("invalid replacement configuration stops previous tunnel", [] {
        Fixture fixture; require(fixture.command("connect").value("success", false), "setup connect");
        fixture.api->current["allowedRoutes"] = {"0.0.0.0/0"};
        require(!fixture.command("sync").value("success", true) && !fixture.tunnel->on, "invalid replacement retained previous tunnel");
    });
    test("selection persistence failure stops previous tunnel", [] {
        Fixture fixture; require(fixture.command("connect").value("success", false), "setup connect"); fixture.store->failSave = true;
        const auto result = fixture.coordinator->handle({{"command", "apply"}, {"edgeNodeIds", {Edge}}});
        require(!result.value("success", true) && !fixture.tunnel->on, "failed selection persistence retained tunnel");
    });
    test("state-load failure stops stale tunnel before throwing", [] {
        auto store = std::make_shared<Store>(); store->failLoad = true; auto tunnel = std::make_shared<TunnelLog>(); tunnel->on = true;
        bool threw = false;
        try { VpnConnectionService coordinator(store, std::make_shared<Api>(), std::make_unique<FakeTunnel>(tunnel)); } catch (...) { threw = true; }
        require(threw && !tunnel->on && tunnel->stops == 1, "state load happened before tunnel stop");
    });
    std::cout << passedTestCount << '/' << passedTestCount + failedTestCount << " groups passed\n";
    return failedTestCount ? 1 : 0;
}
