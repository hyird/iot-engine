#include "service.h"
#include "../common/win32.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <exception>
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
Json VpnServiceStatus::toJson() const {
    return {{"state", state}, {"serverUrl", PlatformUrl}, {"username", username}, {"peerId", peerId},
        {"assignedIpv4", assignedIpv4}, {"edgeNodeIds", edgeNodeIds}, {"allowedRoutes", allowedRoutes},
        {"lastSyncAt", lastSyncAt.empty() ? Json(nullptr) : Json(lastSyncAt)}, {"error", error}, {"tunnelRunning", tunnelRunning}};
}
VpnConnectionService::VpnConnectionService(std::shared_ptr<IClientStateStore> store, std::shared_ptr<IPlatformVpnApi> api, std::unique_ptr<WireGuardTunnel> tunnel)
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
VpnServiceStatus VpnConnectionService::status() const { std::lock_guard lock(statusGate_); return status_; }
Json VpnConnectionService::success() const { return {{"success", true}, {"status", status().toJson()}}; }
void VpnConnectionService::setStatus(std::string name, std::string error, bool synced) {
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
void VpnConnectionService::invalidateWatch() { ++generation_; if (watch_) watch_->request_stop(); changes_.notify_all(); }
void VpnConnectionService::requireSession() const {
    if (!state_.contains("session") || !state_["session"].is_object() || flag(state_, "authenticationRequired"))
        throw PlatformVpnApiError("请先登录平台。", 401, 11004);
}
void VpnConnectionService::requirePeer() const {
    requireSession();
    if (value(state_, "peerId").empty() || value(state_, "publicKey").empty() || value(state_, "privateKey").empty())
        throw std::invalid_argument("请先选择设备并应用。");
}
bool VpnConnectionService::shouldWatch() const {
    return flag(state_, "connectRequested") && !flag(state_, "authenticationRequired") && state_.contains("session") && state_["session"].is_object() && !value(state_, "peerId").empty();
}
void VpnConnectionService::refreshLocked(std::string_view expected, std::stop_token stop) {
    requireSession();
    const auto previous = state_["session"];
    if (value(previous, "token") != expected) return;
    auto next = sessionFromLogin(api_->refresh(value(previous, "refreshToken"), stop));
    if (value(next, "userId") != value(previous, "userId")) throw PlatformVpnApiError("刷新身份与当前账号不一致。", 401, 11006);
    state_["session"] = std::move(next); store_->save(state_);
}
Json VpnConnectionService::authorized(const std::function<Json(std::string_view)>& request, std::stop_token stop) {
    requireSession();
    auto token = value(state_["session"], "token");
    try { return request(token); }
    catch (const PlatformVpnApiError& error) {
        if (!error.isTokenExpired()) throw;
        refreshLocked(token, stop);
        return request(value(state_["session"], "token"));
    }
}
void VpnConnectionService::rememberPendingRevocation(std::string_view peerId) {
    const auto id = canonicalId(peerId);
    auto& pending = state_["pendingRevocations"];
    if (!pending.is_array()) pending = Json::array();
    const auto owner = value(state_.value("session", Json::object()), "userId");
    for (const auto& entry : pending)
        if (value(entry, "peerId") == id && value(entry, "userId") == owner) return;
    pending.push_back({{"peerId", id}, {"userId", owner}});
}
void VpnConnectionService::clearLocalPeer(std::string_view peerId) {
    if (peerId.empty() || value(state_, "peerId") != peerId) return;
    state_.erase("peerId"); state_.erase("publicKey"); state_.erase("privateKey"); state_.erase("lastConfig");
}
Json VpnConnectionService::recoverPendingEnrollment(std::stop_token stop, bool release) {
    if (!state_.contains("pendingEnrollment")) return Json();
    if (!state_["pendingEnrollment"].is_object() || state_["pendingEnrollment"].empty())
        throw std::runtime_error("待确认的客户端注册信息无效。");
    requireSession();
    const auto owner = value(state_["pendingEnrollment"], "userId"), currentOwner = value(state_["session"], "userId");
    if (owner.empty() || owner != currentOwner)
        throw std::runtime_error("上一账号的客户端注册尚未确认，请先恢复原账号完成释放。");
    auto body = state_["pendingEnrollment"];
    body.erase("userId");
    const auto publicKey = value(body, "publicKey");
    const auto ids = strings(body, "edgeNodeIds");
    if (publicKey.empty() || ids.empty()) throw std::runtime_error("待确认的客户端注册信息无效。");
    const auto before = state_;
    Json config;
    try {
        config = authorized([&](std::string_view token) { return api_->apply(token, {}, body, stop); }, stop);
    } catch (const PlatformVpnApiError& error) {
        if (error.httpStatus != 410 || error.code != 21009) throw;
        // The server has already discarded the idempotent enrollment. It is
        // safe to remove the marker and generate a fresh key on the next apply.
        state_.erase("pendingEnrollment");
        if (value(state_, "peerId").empty()) {
            state_.erase("publicKey"); state_.erase("privateKey"); state_.erase("lastConfig");
        }
        try { store_->save(state_); }
        catch (...) {
            const auto session = state_.value("session", Json()); state_ = before;
            if (!session.is_null()) state_["session"] = session;
            throw;
        }
        return Json();
    }
    const auto peer = canonicalId(value(config, "peerId"));
    if (value(config, "publicKey") != publicKey) throw std::runtime_error("平台返回的待确认客户端密钥不一致。");
    const auto enrollment = state_["pendingEnrollment"];
    state_["peerId"] = peer; state_["publicKey"] = publicKey;
    state_["edgeNodeIds"] = ids; state_.erase("pendingEnrollment");
    if (release) rememberPendingRevocation(peer);
    try { store_->save(state_); }
    catch (...) {
        const auto session = state_.value("session", Json()); state_ = before;
        if (!session.is_null()) state_["session"] = session;
        state_["pendingEnrollment"] = enrollment;
        throw;
    }
    return config;
}
void VpnConnectionService::retryPendingRevocations(std::stop_token stop) {
    const auto owner = value(state_.value("session", Json::object()), "userId");
    if (owner.empty()) return;
    std::exception_ptr firstFailure;
    if (!state_.contains("pendingRevocations") || !state_["pendingRevocations"].is_array()) {
        return;
    }
    Json remaining = Json::array(); bool changed = false;
    for (const auto& entry : state_["pendingRevocations"]) {
        const auto peer = value(entry, "peerId"), entryOwner = value(entry, "userId");
        if (peer.empty() || entryOwner.empty()) {
            if (!firstFailure) firstFailure = std::make_exception_ptr(std::runtime_error("待撤销客户端身份无效。"));
            remaining.push_back(entry); continue;
        }
        if (entryOwner != owner) { remaining.push_back(entry); continue; }
        try {
            authorized([&](std::string_view token) { api_->remove(token, peer, stop); return Json(); }, stop);
            clearLocalPeer(peer); changed = true;
        } catch (const PlatformVpnApiError& error) {
            if (error.isPeerUnavailable()) { clearLocalPeer(peer); changed = true; }
            else { if (!firstFailure) firstFailure = std::current_exception(); remaining.push_back(entry); }
        } catch (...) {
            if (!firstFailure) firstFailure = std::current_exception(); remaining.push_back(entry);
        }
    }
    if (changed) {
        if (remaining.empty()) state_.erase("pendingRevocations");
        else state_["pendingRevocations"] = std::move(remaining);
        store_->save(state_);
    }
    if (firstFailure) std::rethrow_exception(firstFailure);
}
Json VpnConnectionService::disconnectLocked(std::stop_token stop) {
    const auto selected = state_.value("edgeNodeIds", Json::array());
    const auto peer = value(state_, "peerId");
    state_["connectRequested"] = false;
    tunnel_->stop();
    if (!peer.empty() && state_.contains("session") && state_["session"].is_object()) rememberPendingRevocation(peer);
    // Persist the stopped state and pending identity before any network request. A failed
    // DELETE must leave enough information to retry after a restart or re-login.
    store_->save(state_);
    if (!peer.empty() || state_.contains("pendingEnrollment")) requireSession();
    if (state_.contains("pendingEnrollment")) {
        recoverPendingEnrollment(stop, true);
    }
    retryPendingRevocations(stop);
    state_["edgeNodeIds"] = selected;
    state_["connectRequested"] = false;
    state_.erase("peerId"); state_.erase("publicKey"); state_.erase("privateKey"); state_.erase("lastConfig");
    state_.erase("pendingEnrollment");
    store_->save(state_); setStatus("Disconnected"); return success();
}
void VpnConnectionService::apiFailure(const PlatformVpnApiError& error) {
    if (error.isAuth() || error.isPeerUnavailable()) {
        state_["connectRequested"] = false; state_["authenticationRequired"] = error.isAuth();
        tunnel_->stop(); setStatus(error.isAuth() ? "AuthorizationRequired" : "Revoked", error.what()); store_->save(state_);
    } else { setStatus("Retrying", error.what()); store_->save(state_); }
}
void VpnConnectionService::loginLocked(const Json& request, std::stop_token stop) {
    if (request.contains("serverUrl") && !request["serverUrl"].is_null() && !isPlatformOrigin(value(request, "serverUrl")))
        throw std::invalid_argument("平台地址已固定为 https://i.a-z.xin。");
    const auto user = value(request, "username"), password = value(request, "password");
    if (user.empty() || password.empty()) throw std::invalid_argument("请输入用户名和密码。");
    auto next = sessionFromLogin(api_->login(user, password, stop));
    const auto nextOwner = value(next, "userId");
    const auto previous = state_.value("session", Json::object());
    const auto previousOwner = value(previous, "userId");
    const auto pending = state_.value("pendingRevocations", Json::array());
    const auto pendingEnrollment = state_.value("pendingEnrollment", Json::object());
    auto hasForeignPending = [&](std::string_view owner) {
        if (state_.contains("pendingEnrollment") && (!pendingEnrollment.is_object() || pendingEnrollment.empty())) return true;
        if (pendingEnrollment.is_object() && !pendingEnrollment.empty()) {
            const auto enrollmentOwner = value(pendingEnrollment, "userId");
            if (enrollmentOwner.empty() || enrollmentOwner != owner) return true;
        }
        if (pending.is_array()) {
            for (const auto& entry : pending) {
                const auto entryOwner = value(entry, "userId");
                if (entryOwner.empty() || entryOwner != owner) return true;
            }
        } else if (!pending.is_null() && !pending.empty()) return true;
        return false;
    };
    if (hasForeignPending(previousOwner.empty() ? nextOwner : previousOwner))
        throw std::runtime_error("上一账号的客户端尚未完成释放，请先恢复原账号完成清理。");
    if (!previousOwner.empty() && previousOwner != nextOwner) {
        tunnel_->stop();
        const auto oldPeer = value(state_, "peerId");
        if (!oldPeer.empty()) rememberPendingRevocation(oldPeer);
        state_["connectRequested"] = false;
        store_->save(state_);
        try {
            if (state_.contains("pendingEnrollment")) {
                recoverPendingEnrollment(stop, true);
            }
            retryPendingRevocations(stop);
        } catch (...) {
            state_["connectRequested"] = false; store_->save(state_);
            throw std::runtime_error("切换账号前无法撤销旧连接，请恢复原账号权限或在平台撤销后重试。");
        }
        const auto remaining = state_.value("pendingRevocations", Json::array());
        if (state_.contains("pendingEnrollment") || (remaining.is_array() && !remaining.empty()) ||
            (!remaining.is_array() && !remaining.empty())) {
            state_["connectRequested"] = false; store_->save(state_);
            throw std::runtime_error("切换账号前无法完成旧连接清理，请恢复原账号权限或在平台撤销后重试。");
        }
        state_ = Json::object();
        state_["session"] = std::move(next); state_["authenticationRequired"] = false;
        store_->save(state_); setStatus("Authenticated");
        return;
    }
    state_["session"] = std::move(next); state_["authenticationRequired"] = false;
    if (state_.contains("pendingEnrollment") || (pending.is_array() && !pending.empty())) state_["connectRequested"] = false;
    store_->save(state_);
    if (state_.contains("pendingEnrollment") || (pending.is_array() && !pending.empty())) {
        try {
            if (state_.contains("pendingEnrollment")) {
                recoverPendingEnrollment(stop, true);
            }
            retryPendingRevocations(stop);
        } catch (...) {
            state_["connectRequested"] = false; store_->save(state_);
            throw std::runtime_error("当前账号仍有待清理的客户端连接，请网络恢复后重试。");
        }
        const auto remaining = state_.value("pendingRevocations", Json::array());
        if (state_.contains("pendingEnrollment") || (remaining.is_array() && !remaining.empty()) ||
            (!remaining.is_array() && !remaining.empty())) {
            state_["connectRequested"] = false; store_->save(state_);
            throw std::runtime_error("当前账号仍有待清理的客户端连接，请完成清理后重试。");
        }
    }
    setStatus(flag(state_, "connectRequested") ? "Connecting" : "Authenticated");
}
void VpnConnectionService::applyLocked(const Json& request, std::stop_token stop) {
    requireSession();
    if (!request.contains("edgeNodeIds") || !request["edgeNodeIds"].is_array() || request["edgeNodeIds"].size() > 64)
        throw std::invalid_argument("请选择有效设备，单个客户端最多选择 64 台。");
    std::set<std::string> selected;
    for (const auto& id : request["edgeNodeIds"]) {
        if (!id.is_string()) throw std::invalid_argument("设备编号无效。");
        selected.insert(canonicalId(id.get<std::string>()));
    }
    const std::vector<std::string> ids(selected.begin(), selected.end());
    retryPendingRevocations(stop);
    const auto currentOwner = value(state_["session"], "userId");
    if (state_.contains("pendingRevocations") && state_["pendingRevocations"].is_array()) {
        for (const auto& entry : state_["pendingRevocations"])
            if (value(entry, "userId") != currentOwner)
                throw std::runtime_error("上一账号的客户端尚未完成释放，请先恢复原账号完成清理。");
    }
    if (state_.contains("pendingEnrollment") &&
        (!state_["pendingEnrollment"].is_object() || state_["pendingEnrollment"].empty()))
        throw std::runtime_error("待确认的客户端注册信息无效。");
    if (state_.contains("pendingEnrollment") && value(state_["pendingEnrollment"], "userId") != currentOwner)
        throw std::runtime_error("上一账号的客户端注册尚未确认，请先恢复原账号完成释放。");
    if (state_.contains("pendingEnrollment")) {
        const auto pendingIds = strings(state_["pendingEnrollment"], "edgeNodeIds");
        if (ids == pendingIds) {
            state_["edgeNodeIds"] = ids; state_["connectRequested"] = true; store_->save(state_);
            const auto config = recoverPendingEnrollment(stop);
            if (config.is_object()) {
                state_["connectRequested"] = true; store_->save(state_);
                acceptConfig(config, stop);
                return;
            }
        }
        // Confirm the previous POST with its original key before changing the
        // selection. The confirmed peer is then revoked before a fresh key is used.
        recoverPendingEnrollment(stop, true);
        retryPendingRevocations(stop);
    }
    const auto peer = value(state_, "peerId");
    if (ids.empty()) {
        state_["edgeNodeIds"] = Json::array();
        if (!peer.empty()) { (void)disconnectLocked(stop); return; }
        state_["connectRequested"] = false; state_.erase("publicKey"); state_.erase("privateKey"); state_.erase("lastConfig");
        tunnel_->stop(); setStatus("Disconnected"); store_->save(state_); return;
    }
    Json body{{"edgeNodeIds", ids}};
    if (peer.empty()) {
        if (value(state_, "publicKey").empty() || value(state_, "privateKey").empty()) {
            state_.erase("publicKey"); state_.erase("privateKey"); state_.erase("lastConfig");
            auto keys = tunnel_->generateKeys(); state_["publicKey"] = keys.first; state_["privateKey"] = keys.second;
        }
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD count = MAX_COMPUTERNAME_LENGTH + 1;
        body["name"] = GetComputerNameW(computer, &count) ? utf8({computer, count}) : "Windows";
        body["publicKey"] = state_["publicKey"];
        body["userId"] = currentOwner;
        state_["edgeNodeIds"] = ids;
        state_["pendingEnrollment"] = body;
        store_->save(state_);
    }
    auto requestBody = body;
    requestBody.erase("userId");
    auto config = authorized([&](std::string_view token) { return api_->apply(token, peer, requestBody, stop); }, stop);
    const auto assignedPeer = canonicalId(value(config, "peerId"));
    if (!peer.empty() && canonicalId(peer) != assignedPeer) { tunnel_->stop(); throw std::runtime_error("平台返回了其他客户端身份。"); }
    state_["peerId"] = assignedPeer; state_["edgeNodeIds"] = ids; state_["connectRequested"] = !ids.empty(); state_.erase("pendingEnrollment");
    if (ids.empty() || strings(config, "allowedRoutes").empty()) tunnel_->stop();
    store_->save(state_); acceptConfig(config, stop);
}
Json VpnConnectionService::logoutLocked(std::stop_token stop) {
    state_["connectRequested"] = false; tunnel_->stop();
    const auto peer = value(state_, "peerId");
    if (!peer.empty() && state_.contains("session") && state_["session"].is_object()) rememberPendingRevocation(peer);
    store_->save(state_);
    std::string warning;
    try {
        if (state_.contains("pendingEnrollment")) {
            recoverPendingEnrollment(stop, true);
        }
        retryPendingRevocations(stop);
    } catch (...) {
        clearLocalPeer(peer);
        warning = "本机已退出，平台客户端未能撤销，恢复原账号后将继续重试。";
    }
    const auto pending = state_.value("pendingRevocations", Json::array());
    const bool hasEnrollment = state_.contains("pendingEnrollment");
    const auto enrollment = state_.value("pendingEnrollment", Json::object());
    const auto publicKey = value(state_, "publicKey"), privateKey = value(state_, "privateKey");
    state_ = Json::object();
    if (!pending.empty()) state_["pendingRevocations"] = pending;
    if (hasEnrollment) {
        state_["pendingEnrollment"] = enrollment;
        if (enrollment.is_object()) state_["edgeNodeIds"] = strings(enrollment, "edgeNodeIds");
        if (!publicKey.empty()) state_["publicKey"] = publicKey;
        if (!privateKey.empty()) state_["privateKey"] = privateKey;
        if (warning.empty()) warning = "本机已退出，平台客户端注册尚未确认，恢复原账号后将继续重试。";
    }
    store_->save(state_); setStatus("LoggedOut", warning);
    auto result = success(); if (!warning.empty()) result["message"] = warning; return result;
}
Json VpnConnectionService::handle(const Json& request, std::stop_token stop) {
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
            requireSession();
            // A reconnect must settle every locally retained revoke before it
            // can read the old configuration or allocate a new peer.
            retryPendingRevocations(stop);
            if (command == "connect" && value(state_, "peerId").empty()) {
                const auto selected = strings(state_, "edgeNodeIds");
                if (selected.empty()) throw std::invalid_argument("请先选择设备并应用。");
                applyLocked({{"edgeNodeIds", selected}}, stop);
            } else {
                requirePeer();
                if (command == "connect") { state_["connectRequested"] = true; store_->save(state_); setStatus("Connecting"); }
                auto config = authorized([&](std::string_view token) { return api_->config(token, value(state_, "peerId"), stop); }, stop);
                acceptConfig(config, stop);
            }
        } else if (command == "disconnect") {
            return disconnectLocked(stop);
        } else if (command == "logout") return logoutLocked(stop);
        else return {{"success", false}, {"message", "不支持的客户端命令。"}, {"status", status().toJson()}};
        if (mutates) changes_.notify_all();
        return success();
    } catch (const PlatformVpnApiError& error) {
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
bool VpnConnectionService::equivalentConfig(const Json& a, const Json& b) {
    for (const auto* key : {"publicKey", "assignedIpv4", "hubPublicKey", "hubEndpoint", "hubListenPort", "mtu", "persistentKeepalive"})
        if (a.value(key, Json()) != b.value(key, Json())) return false;
    auto left = strings(a, "allowedRoutes"), right = strings(b, "allowedRoutes");
    std::sort(left.begin(), left.end()); std::sort(right.begin(), right.end()); return left == right;
}
void VpnConnectionService::acceptConfig(const Json& config, std::stop_token stop) try {
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
void VpnConnectionService::run(std::stop_token stop) {
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
            } catch (const PlatformVpnApiError& error) {
                std::lock_guard lock(gate_);
                if (generation != generation_ || watch->stop_requested()) break;
                try {
                    if (!refreshed && error.isTokenExpired()) {
                        refreshLocked(accessToken, watch->get_token()); accessToken = value(state_["session"], "token"); refreshed = true; continue;
                    }
                    apiFailure(error);
                } catch (const PlatformVpnApiError& refreshError) { apiFailure(refreshError); }
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
void VpnConnectionService::stop() {
    std::lock_guard lock(gate_); invalidateWatch(); tunnel_->stop(); setStatus("Stopped");
}
}
