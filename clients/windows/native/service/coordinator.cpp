#include "service.h"
#include "../common/win32.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <set>

namespace iotvpn::service {
namespace {
std::string value(const Json& object, const char* key) {
    return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string{};
}
std::vector<std::string> strings(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_array()) return {};
    return object[key].get<std::vector<std::string>>();
}
std::string now() {
    const auto stamp = std::time(nullptr); std::tm utc{}; gmtime_s(&utc, &stamp);
    char output[32]{}; std::strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utc); return output;
}
Json sessionFromLogin(const Json& data) {
    const Json user = data.value("user", Json::object());
    Json result{{"serverUrl", PlatformUrl}, {"userId", value(user, "id")}, {"username", value(user, "username")},
        {"token", value(data, "token")}, {"refreshToken", value(data, "refresh_token")}};
    for (const auto* key : {"userId", "username", "token", "refreshToken"})
        if (value(result, key).empty()) throw std::runtime_error("平台登录响应不完整。");
    for (const auto* key : {"token", "refreshToken"}) {
        const auto text = value(result, key);
        if (text.size() > 16384 || std::any_of(text.begin(), text.end(), [](unsigned char c) { return c <= 32 || c >= 127; }))
            throw std::runtime_error("平台令牌格式无效。");
    }
    return result;
}
bool flag(const Json& state, const char* name) { return state.contains(name) && state[name].is_boolean() && state[name].get<bool>(); }
}
std::string canonicalId(std::string_view text) {
    if (text.size() != 36) throw std::invalid_argument("客户端或设备编号无效。");
    std::string id(text);
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') throw std::invalid_argument("客户端或设备编号无效。"); }
        else if (!std::isxdigit(static_cast<unsigned char>(id[i]))) throw std::invalid_argument("客户端或设备编号无效。");
        id[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(id[i])));
    }
    return id;
}
bool isPlatformOrigin(std::string_view url) {
    std::string normalized(url);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized == PlatformUrl || normalized == std::string(PlatformUrl) + "/" ||
        normalized == std::string(PlatformUrl) + ":443" || normalized == std::string(PlatformUrl) + ":443/";
}
Json Status::toJson() const {
    return {{"state", state}, {"serverUrl", PlatformUrl}, {"username", username}, {"peerId", peerId},
        {"assignedIpv4", assignedIpv4}, {"edgeNodeIds", edgeNodeIds}, {"allowedRoutes", allowedRoutes},
        {"lastSyncAt", lastSyncAt.empty() ? Json(nullptr) : Json(lastSyncAt)}, {"error", error}, {"tunnelRunning", tunnelRunning}};
}
Coordinator::Coordinator(std::shared_ptr<IStateStore> store, std::shared_ptr<IApiTransport> api, std::unique_ptr<Tunnel> tunnel)
    : store_(std::move(store)), api_(std::move(api)), tunnel_(std::move(tunnel)) {
    tunnel_->stop(); // No cached authorization may start the tunnel on a new service boot.
    state_ = store_->load();
    if (!state_.is_object()) throw std::runtime_error("本机登录状态无效。");
    if (state_.contains("session") && state_["session"].is_object()) {
        const auto& old = state_["session"];
        if (!isPlatformOrigin(value(old, "serverUrl"))) {
            state_ = Json::object(); store_->save(state_);
        } else {
            // Validate existing token fields using the same rules as fresh login.
            state_["session"] = sessionFromLogin({{"token", value(old, "token")}, {"refresh_token", value(old, "refreshToken")},
                {"user", {{"id", value(old, "userId")}, {"username", value(old, "username")}}}});
        }
    }
    setStatus(state_.contains("session") && state_["session"].is_object() ? flag(state_, "authenticationRequired") ? "AuthorizationRequired" : "Authenticated" : "LoggedOut");
}
Status Coordinator::status() const { std::lock_guard lock(statusGate_); return status_; }
Json Coordinator::success() const { return {{"success", true}, {"status", status().toJson()}}; }
void Coordinator::setStatus(std::string name, std::string error, bool synced) {
    auto current = status(); current.state = std::move(name); current.error = std::move(error);
    current.username = value(state_.value("session", Json::object()), "username"); current.peerId = value(state_, "peerId");
    current.edgeNodeIds = strings(state_, "edgeNodeIds");
    const auto config = state_.value("lastConfig", Json::object());
    current.assignedIpv4 = value(config, "assignedIpv4");
    current.allowedRoutes = current.state == "Connected" ? strings(config, "allowedRoutes") : std::vector<std::string>{};
    try { current.tunnelRunning = tunnel_->running(); } catch (...) { current.tunnelRunning = false; }
    if (synced) current.lastSyncAt = now();
    std::lock_guard lock(statusGate_); status_ = std::move(current);
}
void Coordinator::invalidateWatch() { ++generation_; if (watch_) watch_->request_stop(); changes_.notify_all(); }
void Coordinator::requireSession() const {
    if (!state_.contains("session") || !state_["session"].is_object() || flag(state_, "authenticationRequired"))
        throw ApiError("请先登录平台。", 401, 11004);
}
void Coordinator::requirePeer() const {
    requireSession();
    if (value(state_, "peerId").empty() || value(state_, "publicKey").empty() || value(state_, "privateKey").empty())
        throw std::invalid_argument("请先选择设备并应用。");
}
bool Coordinator::shouldWatch() const {
    return flag(state_, "connectRequested") && !flag(state_, "authenticationRequired") && state_.contains("session") && state_["session"].is_object() && !value(state_, "peerId").empty();
}
void Coordinator::refreshLocked(std::string_view expected, std::stop_token stop) {
    requireSession();
    const auto previous = state_["session"];
    if (value(previous, "token") != expected) return;
    auto next = sessionFromLogin(api_->refresh(value(previous, "refreshToken"), stop));
    if (value(next, "userId") != value(previous, "userId")) throw ApiError("刷新身份与当前账号不一致。", 401, 11006);
    state_["session"] = std::move(next); store_->save(state_);
}
Json Coordinator::authorized(const std::function<Json(std::string_view)>& request, std::stop_token stop) {
    requireSession();
    auto token = value(state_["session"], "token");
    try { return request(token); }
    catch (const ApiError& error) {
        if (!error.isTokenExpired()) throw;
        refreshLocked(token, stop);
        return request(value(state_["session"], "token"));
    }
}
void Coordinator::apiFailure(const ApiError& error) {
    if (error.isAuth() || error.isPeerUnavailable()) {
        state_["connectRequested"] = false; state_["authenticationRequired"] = error.isAuth();
        tunnel_->stop(); setStatus(error.isAuth() ? "AuthorizationRequired" : "Revoked", error.what()); store_->save(state_);
    } else { setStatus("Retrying", error.what()); store_->save(state_); }
}
void Coordinator::loginLocked(const Json& request, std::stop_token stop) {
    if (request.contains("serverUrl") && !request["serverUrl"].is_null() && !isPlatformOrigin(value(request, "serverUrl")))
        throw std::invalid_argument("平台地址已固定为 https://i.a-z.xin。");
    const auto user = value(request, "username"), password = value(request, "password");
    if (user.empty() || password.empty()) throw std::invalid_argument("请输入用户名和密码。");
    auto next = sessionFromLogin(api_->login(user, password, stop));
    const auto previous = state_.value("session", Json::object());
    if (previous.is_object() && !value(previous, "userId").empty() && value(previous, "userId") != value(next, "userId")) {
        tunnel_->stop();
        if (!value(state_, "peerId").empty()) {
            try { authorized([&](std::string_view token) { api_->remove(token, value(state_, "peerId"), stop); return Json(); }, stop); }
            catch (const ApiError& error) {
                if (!error.isPeerUnavailable()) { state_["connectRequested"] = false; store_->save(state_); throw std::runtime_error("切换账号前无法撤销旧连接，请恢复原账号权限或在平台撤销后重试。"); }
            }
            catch (...) { state_["connectRequested"] = false; store_->save(state_); throw; }
        }
        state_ = Json::object();
    }
    state_["session"] = std::move(next); state_["authenticationRequired"] = false;
    store_->save(state_); setStatus(flag(state_, "connectRequested") ? "Connecting" : "Authenticated");
}
void Coordinator::applyLocked(const Json& request, std::stop_token stop) {
    requireSession();
    if (!request.contains("edgeNodeIds") || !request["edgeNodeIds"].is_array() || request["edgeNodeIds"].size() > 64)
        throw std::invalid_argument("请选择有效设备，单个客户端最多选择 64 台。");
    std::set<std::string> selected;
    for (const auto& id : request["edgeNodeIds"]) {
        if (!id.is_string()) throw std::invalid_argument("设备编号无效。");
        selected.insert(canonicalId(id.get<std::string>()));
    }
    const std::vector<std::string> ids(selected.begin(), selected.end());
    const auto peer = value(state_, "peerId");
    if (ids.empty() && peer.empty()) {
        state_["edgeNodeIds"] = Json::array(); state_["connectRequested"] = false; state_.erase("lastConfig");
        tunnel_->stop(); setStatus("Disconnected"); store_->save(state_); return;
    }
    if (value(state_, "publicKey").empty() || value(state_, "privateKey").empty()) {
        auto keys = tunnel_->generateKeys(); state_["publicKey"] = keys.first; state_["privateKey"] = keys.second; store_->save(state_);
    }
    Json body{{"edgeNodeIds", ids}};
    if (peer.empty()) {
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD count = MAX_COMPUTERNAME_LENGTH + 1;
        body["name"] = GetComputerNameW(computer, &count) ? utf8({computer, count}) : "Windows";
        body["publicKey"] = state_["publicKey"];
    }
    auto config = authorized([&](std::string_view token) { return api_->apply(token, peer, body, stop); }, stop);
    const auto assignedPeer = canonicalId(value(config, "peerId"));
    if (!peer.empty() && canonicalId(peer) != assignedPeer) { tunnel_->stop(); throw std::runtime_error("平台返回了其他客户端身份。"); }
    state_["peerId"] = assignedPeer; state_["edgeNodeIds"] = ids; state_["connectRequested"] = !ids.empty();
    if (ids.empty() || strings(config, "allowedRoutes").empty()) tunnel_->stop();
    store_->save(state_); acceptConfig(config, stop);
}
Json Coordinator::logoutLocked(std::stop_token stop) {
    state_["connectRequested"] = false; tunnel_->stop(); store_->save(state_);
    std::string warning;
    if (!value(state_, "peerId").empty() && state_.contains("session") && state_["session"].is_object()) {
        try { authorized([&](std::string_view token) { api_->remove(token, value(state_, "peerId"), stop); return Json(); }, stop); }
        catch (const ApiError& error) { if (!error.isPeerUnavailable()) warning = "本机已退出，平台客户端未能撤销，请由管理员确认撤销。"; }
        catch (...) { warning = "本机已退出，平台客户端未能撤销，请由管理员确认撤销。"; }
    }
    state_ = Json::object(); store_->save(state_); setStatus("LoggedOut", warning);
    auto result = success(); if (!warning.empty()) result["message"] = warning; return result;
}
Json Coordinator::handle(const Json& request, std::stop_token stop) {
    const auto command = value(request, "command");
    if (command == "status") return success();
    std::lock_guard lock(gate_);
    const bool mutates = command == "login" || command == "apply" || command == "connect" || command == "sync" || command == "disconnect" || command == "logout";
    if (mutates) invalidateWatch();
    try {
        if (stop.stop_requested()) throw std::runtime_error("操作已取消。");
        if (command == "login") loginLocked(request, stop);
        else if (command == "devices") {
            auto devices = authorized([&](std::string_view token) { return api_->devices(token, stop); }, stop);
            auto result = success(); result["devices"] = std::move(devices); return result;
        } else if (command == "apply") applyLocked(request, stop);
        else if (command == "connect" || command == "sync") {
            requirePeer();
            if (command == "connect") { state_["connectRequested"] = true; store_->save(state_); setStatus("Connecting"); }
            auto config = authorized([&](std::string_view token) { return api_->config(token, value(state_, "peerId"), stop); }, stop);
            acceptConfig(config, stop);
        } else if (command == "disconnect") {
            state_["connectRequested"] = false; tunnel_->stop(); setStatus("Disconnected"); store_->save(state_);
        } else if (command == "logout") return logoutLocked(stop);
        else return {{"success", false}, {"message", "不支持的客户端命令。"}, {"status", status().toJson()}};
        if (mutates) changes_.notify_all();
        return success();
    } catch (const ApiError& error) {
        try { apiFailure(error); } catch (const std::exception& failure) { setStatus("Error", failure.what()); }
        changes_.notify_all(); return {{"success", false}, {"message", error.what()}, {"status", status().toJson()}};
    } catch (const std::exception& error) {
        if (mutates) {
            try { tunnel_->stop(); } catch (...) { }
        }
        setStatus("Error", error.what()); changes_.notify_all();
        return {{"success", false}, {"message", error.what()}, {"status", status().toJson()}};
    }
}
bool Coordinator::equivalentConfig(const Json& a, const Json& b) {
    for (const auto* key : {"publicKey", "assignedIpv4", "hubPublicKey", "hubEndpoint", "hubListenPort", "mtu", "persistentKeepalive"})
        if (a.value(key, Json()) != b.value(key, Json())) return false;
    auto left = strings(a, "allowedRoutes"), right = strings(b, "allowedRoutes");
    std::sort(left.begin(), left.end()); std::sort(right.begin(), right.end()); return left == right;
}
void Coordinator::acceptConfig(const Json& config, std::stop_token stop) try {
    if (stop.stop_requested()) throw std::runtime_error("操作已取消。");
    const auto selected = strings(state_, "edgeNodeIds"), edges = strings(config, "edgeNodeIds");
    if (value(config, "peerId") != value(state_, "peerId") || value(config, "publicKey") != value(state_, "publicKey") ||
        std::any_of(edges.begin(), edges.end(), [&](const auto& id) { return std::find(selected.begin(), selected.end(), canonicalId(id)) == selected.end(); })) {
        tunnel_->stop(); throw std::runtime_error("平台配置与当前客户端身份或设备选择不一致。");
    }
    const auto routes = strings(config, "allowedRoutes");
    if (!flag(state_, "connectRequested") || routes.empty()) {
        tunnel_->stop(); state_["lastConfig"] = config; store_->save(state_);
        setStatus("Disconnected", routes.empty() && flag(state_, "connectRequested") ? "所选设备暂时没有可用网络。" : "", true); return;
    }
    if (!tunnel_->running() || !state_.contains("lastConfig") || !state_["lastConfig"].is_object() || !equivalentConfig(state_["lastConfig"], config))
        tunnel_->apply(config, value(state_, "privateKey"));
    state_["lastConfig"] = config; store_->save(state_); setStatus("Connected", "", true);
} catch (...) {
    // Never leave a tunnel running after rejecting or failing to persist a new configuration.
    tunnel_->stop();
    throw;
}
void Coordinator::run(std::stop_token stop) {
    unsigned failures = 0;
    while (!stop.stop_requested()) {
        std::shared_ptr<std::stop_source> watch;
        std::string peer, accessToken;
        std::uint64_t generation;
        {
            std::unique_lock lock(gate_);
            changes_.wait(lock, stop, [&] { return shouldWatch(); });
            if (stop.stop_requested()) break;
            watch = std::make_shared<std::stop_source>(); watch_ = watch;
            generation = generation_; peer = value(state_, "peerId"); accessToken = value(state_["session"], "token");
        }
        std::stop_callback cancel(stop, [watch] { watch->request_stop(); });
        bool refreshed = false;
        for (;;) {
            try {
                api_->watchConfig(accessToken, peer, [&](const Json& config) {
                    std::lock_guard lock(gate_);
                    if (generation != generation_ || !shouldWatch() || watch->stop_requested()) return;
                    acceptConfig(config, watch->get_token()); failures = 0;
                }, watch->get_token());
                if (!watch->stop_requested()) throw std::runtime_error("配置订阅已结束，正在重新连接。");
            } catch (const ApiError& error) {
                std::lock_guard lock(gate_);
                if (generation != generation_ || watch->stop_requested()) break;
                try {
                    if (!refreshed && error.isTokenExpired()) {
                        refreshLocked(accessToken, watch->get_token()); accessToken = value(state_["session"], "token"); refreshed = true; continue;
                    }
                    apiFailure(error);
                } catch (const ApiError& refreshError) { apiFailure(refreshError); }
                catch (const std::exception& failure) { setStatus("Retrying", failure.what()); }
                ++failures;
            } catch (const std::exception& error) {
                std::lock_guard lock(gate_);
                if (generation == generation_ && !watch->stop_requested()) { setStatus("Retrying", error.what()); ++failures; }
            }
            break;
        }
        std::unique_lock lock(gate_);
        if (watch_ == watch) watch_.reset();
        const auto delay = std::chrono::seconds(std::min(30U, 1U << std::min(failures, 5U)));
        changes_.wait_for(lock, stop, delay, [&] { return generation_ != generation || !shouldWatch(); });
    }
}
void Coordinator::stop() {
    std::lock_guard lock(gate_); invalidateWatch(); tunnel_->stop(); setStatus("Stopped");
}
}
