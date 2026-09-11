#pragma once
#include "../common/common.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <set>
#include <thread>

namespace iotvpn::gui {
inline std::string text(const Json& object, const char* name, std::string fallback = {}) {
    return object.is_object() && object.contains(name) && object[name].is_string() ? object[name].get<std::string>() : fallback;
}
inline std::set<std::string> selection(const Json& values) {
    std::set<std::string> result;
    if (values.is_array()) for (const auto& id : values) if (id.is_string()) result.insert(id.get<std::string>());
    return result;
}
inline bool requiresLogin(const Json& status) {
    const auto state = text(status,"state");
    return state.empty() || state == "LoggedOut" || state == "AuthorizationRequired";
}
struct EdgeDevice { std::string id, name, imei, subnet, address; bool online = false; };
class ConnectionController {
public:
    using Transport = std::function<Json(const Json&, std::stop_token)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    explicit ConnectionController(Transport transport, bool synchronous = false, Clock clock = [] { return std::chrono::steady_clock::now(); })
        : transport_(std::move(transport)), synchronous_(synchronous), clock_(std::move(clock)) {}
    ~ConnectionController() { worker_.request_stop(); if (worker_.joinable()) worker_.join(); }
    ConnectionController(const ConnectionController&) = delete;
    ConnectionController& operator=(const ConnectionController&) = delete;
    bool loggedIn = false, busy = false, connected = false;
    std::string username, message;
    Json status = {{"state","LoggedOut"}};
    std::set<std::string> selected, applied;
    std::vector<EdgeDevice> devices;

    bool changed() const { return selected != applied; }
    std::vector<EdgeDevice> filtered(std::string_view query) const {
        const auto lower = [](std::string value) { for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return value; };
        const auto needle = lower(std::string(query)); std::vector<EdgeDevice> result;
        for (const auto& device : devices)
            if (needle.empty() || lower(device.name + ' ' + device.imei + ' ' + device.id).find(needle) != std::string::npos) result.push_back(device);
        return result;
    }
    void choose(const std::string& id, bool enabled) {
        const bool changed = enabled ? selected.insert(id).second : selected.erase(id) != 0;
        if (changed) ++selectionRevision_;
    }
    void request(std::string command, Json data = Json::object()) {
        if (busy) return;
        // Bound automatic retries even if the upstream device request fails.
        if (command == "devices") nextDevices_ = clock_() + std::chrono::seconds(10);
        if (command == "apply") {
            if (selected.size() > 64) { message = "单个客户端最多选择 64 台设备。"; return; }
            data["edgeNodeIds"] = selected;
        }
        data["command"] = command;
        if (synchronous_) {
            Json response;
            try { response = transport_(data, {}); }
            catch (const std::exception& error) { response = {{"success",false},{"message",error.what()}}; }
            complete(command,response,selectionRevision_); return;
        }
        busy = true; pendingCommand_ = std::move(command); pendingSelectionRevision_ = selectionRevision_;
        std::promise<Json> promise; pending_ = promise.get_future(); const auto transport = transport_;
        worker_ = std::jthread([transport,data=std::move(data),promise=std::move(promise)](std::stop_token stop) mutable {
            Json result;
            try { result = transport(data,stop); }
            catch (const std::exception& error) { result = {{"success",false},{"message",error.what()}}; }
            catch (...) { result = {{"success",false},{"message","本机服务请求未完成。"}}; }
            if (data.contains("password") && data["password"].is_string()) {
                auto& password = data["password"].get_ref<std::string&>(); std::fill(password.begin(),password.end(),'\0'); data.erase("password");
            }
            promise.set_value(std::move(result));
        });
    }
    void tick(bool automatic = true) {
        if (busy && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto result = pending_.get(); if (worker_.joinable()) worker_.join(); busy = false;
            try { complete(pendingCommand_,result,pendingSelectionRevision_); }
            catch (const std::exception&) { message = "本机服务返回的数据无效，请重试。"; }
        }
        if (!automatic || busy) return;
        const auto now = clock_();
        if (loggedIn && (loadDevices_ || now >= nextDevices_)) { loadDevices_ = false; request("devices"); }
        else if (loggedIn && now >= nextStatus_) { nextStatus_ = now + std::chrono::seconds(5); request("status"); }
    }
private:
    void update(const Json& snapshot, bool reset, bool preserveSelection = false) {
        if (!snapshot.is_object()) return;
        status = snapshot; const auto user = text(snapshot,"username");
        const bool accountChanged = !user.empty() && !username.empty() && user != username;
        if (!user.empty()) username = user;
        if (requiresLogin(snapshot)) {
            loggedIn = connected = false; selected.clear(); applied.clear(); devices.clear(); loadDevices_ = false; return;
        }
        const bool wasLoggedIn = loggedIn; loggedIn = true;
        const auto next = selection(snapshot.value("edgeNodeIds",Json::array()));
        if (reset || accountChanged || !wasLoggedIn || (!preserveSelection && selected == applied)) selected = next;
        if (reset || accountChanged) { devices.clear(); loadDevices_ = true; }
        applied = next; connected = snapshot.value("tunnelRunning",text(snapshot,"state") == "Connected");
        if (!wasLoggedIn) loadDevices_ = true;
    }
    void loadDevices(const Json& snapshot) {
        if (!snapshot.is_array()) throw std::runtime_error("Invalid device snapshot");
        std::vector<EdgeDevice> next; std::set<std::string> seen;
        for (const auto& entry : snapshot) {
            if (!entry.is_object()) continue;
            const auto id = text(entry,"id"); if (id.empty() || !seen.insert(id).second) continue;
            std::string subnets;
            if (entry.contains("virtualCidrs") && entry["virtualCidrs"].is_array()) for (const auto& route : entry["virtualCidrs"])
                if (route.is_string()) { if (!subnets.empty()) subnets += ", "; subnets += route.get<std::string>(); }
            next.push_back({id,text(entry,"name","未命名设备"),text(entry,"imei","—"),subnets.empty()?"—":subnets,
                text(entry,"assignedIpv4","未分配"),entry.value("online",false)});
        }
        for (const auto& id : selected) if (!seen.contains(id)) next.push_back({id,"不可用设备","—","—","未分配",false});
        devices = std::move(next);
    }
    void complete(const std::string& command, const Json& response, std::uint64_t requestSelectionRevision) {
        const bool success = response.is_object() && response.value("success",false);
        if (response.contains("status")) update(response["status"],success && command == "login",requestSelectionRevision != selectionRevision_);
        if (!success) { message = text(response,"message","操作未完成，请稍后重试。"); return; }
        if (command != "status") message = text(response,"message");
        const auto error = text(status,"error"); if (!error.empty()) message = error;
        if (message.size() > 1600) message.resize(1600);
        if (command == "devices" && loggedIn) loadDevices(response.value("devices",Json::array()));
        if (command == "login") loadDevices_ = true;
        if (command == "logout") { update({{"state","LoggedOut"}},true); message = text(response,"message"); }
        nextStatus_ = clock_() + std::chrono::seconds(5);
    }
    Transport transport_; bool synchronous_ = false, loadDevices_ = false;
    Clock clock_;
    std::jthread worker_; std::future<Json> pending_; std::string pendingCommand_;
    std::uint64_t selectionRevision_ = 0, pendingSelectionRevision_ = 0;
    std::chrono::steady_clock::time_point nextStatus_{}, nextDevices_{};
};
}
