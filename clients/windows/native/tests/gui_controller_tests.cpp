#include "../gui/controller.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>

using iotvpn::Json;
using iotvpn::gui::Controller;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Json status(std::string state, std::string username, std::initializer_list<const char*> ids) {
    Json result{{"state", std::move(state)}, {"username", std::move(username)}, {"tunnelRunning", true}};
    result["edgeNodeIds"] = Json::array();
    for (const auto* id : ids) result["edgeNodeIds"].push_back(id);
    return result;
}

Json devices(std::initializer_list<const char*> ids) {
    Json result = Json::array();
    for (const auto* id : ids)
        result.push_back({{"id", id}, {"name", std::string("Device ") + id}, {"imei", "imei-" + std::string(id)},
                          {"virtualCidrs", Json::array({"100.96.0.0/24"})}, {"assignedIpv4", "100.96.0.2"}, {"online", true}});
    return result;
}

Json success(Json response = Json::object()) {
    response["success"] = true;
    return response;
}

void waitIdle(Controller& controller) {
    for (int i = 0; i != 200 && controller.busy; ++i) {
        controller.tick(false);
        std::this_thread::sleep_for(1ms);
    }
    controller.tick(false);
    require(!controller.busy, "controller remained busy");
}

void testStatusRestore() {
    Controller controller([](const Json& request, std::stop_token) {
        require(request["command"] == "status", "restore command mismatch");
        return success({{"status", status("Connected", "alice", {"a"})}});
    }, true);
    controller.request("status");
    require(controller.loggedIn && controller.connected, "status did not restore connection");
    require(controller.username == "alice" && controller.selected == std::set<std::string>{"a"}, "status restore state mismatch");
    require(controller.applied == controller.selected, "restored selection is not applied");
}

void testLoginAndDevices() {
    int calls = 0;
    Controller controller([&](const Json& request, std::stop_token) {
        ++calls;
        if (request["command"] == "login") return success({{"status", status("Connected", "alice", {"a", "b"})}});
        require(request["command"] == "devices", "device load command mismatch");
        return success({{"devices", devices({"a", "b"})}});
    }, true);
    controller.request("login", {{"username", "alice"}, {"password", "secret"}});
    controller.tick(true);
    require(calls == 2 && controller.loggedIn, "login did not trigger device load");
    require(controller.devices.size() == 2 && controller.devices[0].subnet == "100.96.0.0/24", "device snapshot not loaded");
    require(controller.devices[1].online, "device online state lost");
}

void testSelectionAcrossFilterRefreshAndStatus() {
    int calls = 0;
    Controller controller([&](const Json& request, std::stop_token) {
        ++calls;
        if (request["command"] == "devices") return success({{"devices", devices({"a", "b", "c"})}});
        require(request["command"] == "status", "refresh status command mismatch");
        return success({{"status", status("Connected", "alice", {"a"})}});
    }, true);
    controller.loggedIn = true;
    controller.applied = {"a"}; controller.selected = {"a"};
    controller.request("devices");
    require(controller.devices.size() == 3, "refresh did not update devices");
    controller.choose("b", true);
    require(controller.filtered("DEVICE B").size() == 1, "filter did not find selected device");
    controller.request("status");
    require(calls == 2 && controller.selected == std::set<std::string>{"a", "b"}, "status overwrote later selection");
    require(controller.applied == std::set<std::string>{"a"}, "server selection snapshot changed unexpectedly");
}

void testApplySnapshotsSelection() {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    Json received;
    Controller controller([&](const Json& request, std::stop_token) {
        {
            std::lock_guard lock(mutex); received = request; entered = true;
        }
        cv.notify_one();
        std::unique_lock lock(mutex); cv.wait(lock, [&] { return release; });
        return success({{"status", status("Connected", "alice", {"a"})}});
    });
    controller.loggedIn = true; controller.applied = {"a"}; controller.selected = {"a"};
    controller.request("apply");
    { std::unique_lock lock(mutex); require(cv.wait_for(lock, 1s, [&] { return entered; }), "apply transport was not entered"); }
    controller.choose("b", true);
    { std::lock_guard lock(mutex); release = true; }
    cv.notify_one();
    waitIdle(controller);
    require(received["edgeNodeIds"] == Json::array({"a"}), "apply did not snapshot selection at request time");
    require(controller.selected == std::set<std::string>{"a", "b"}, "later selection was lost after apply");
}

void testApplyPreservesLatestEditWhenItMatchesOldApplied() {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    Json received;
    Controller controller([&](const Json& request, std::stop_token stop) {
        {
            std::lock_guard lock(mutex);
            received = request;
            entered = true;
        }
        cv.notify_one();
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 1500ms, [&] { return release || stop.stop_requested(); });
        if (stop.stop_requested()) return success();
        return success({{"status", status("Connected", "alice", {"a", "b"})}});
    });
    controller.loggedIn = true;
    controller.applied = {"a"};
    controller.selected = {"a", "b"};
    controller.request("apply");
    {
        std::unique_lock lock(mutex);
        require(cv.wait_for(lock, 1s, [&] { return entered; }), "revision apply transport was not entered");
    }
    controller.choose("b", false);
    require(controller.selected == std::set<std::string>{"a"}, "latest edit was not applied locally");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_one();
    waitIdle(controller);
    require(received["edgeNodeIds"] == Json::array({"a", "b"}), "revision apply did not snapshot initial selection");
    require(controller.applied == std::set<std::string>{"a", "b"}, "apply response did not update applied selection");
    require(controller.selected == std::set<std::string>{"a"}, "apply response overwrote latest matching edit");
}

void testLogoutAndAuthFailures() {
    int mode = 0;
    Controller controller([&](const Json& request, std::stop_token) {
        if (mode == 0) { require(request["command"] == "logout", "logout command mismatch"); return success({{"message", "已退出"}}); }
        require(request["command"] == "login", "auth command mismatch");
        return Json{{"success", false}, {"message", "登录失败"}, {"status", Json{{"state", "AuthorizationRequired"}}}};
    }, true);
    controller.loggedIn = true; controller.selected = {"a"}; controller.applied = {"a"}; controller.devices = {{"a", "A", "i", "s", "ip", true}}; controller.message = "旧警告";
    controller.request("logout");
    require(!controller.loggedIn && controller.selected.empty() && controller.applied.empty() && controller.devices.empty(), "logout did not clear IPC state");
    require(controller.message == "已退出" && controller.status["state"] == "LoggedOut", "logout warning/state mismatch");
    mode = 1; controller.request("login", {{"username", "alice"}, {"password", "bad"}});
    require(!controller.loggedIn && controller.selected.empty() && controller.message == "登录失败", "auth failure did not return to login");
}

void testCrossAccountClear() {
    Controller controller([](const Json& request, std::stop_token) {
        require(request["command"] == "login", "cross-account command mismatch");
        return success({{"status", status("Connected", "bob", {"b"})}});
    }, true);
    controller.loggedIn = true; controller.username = "alice"; controller.selected = {"a"}; controller.applied = {"a"}; controller.devices = {{"a", "A", "i", "s", "ip", true}};
    controller.request("login", {{"username", "bob"}, {"password", "secret"}});
    require(controller.username == "bob" && controller.selected == std::set<std::string>{"b"}, "cross-account selection leaked");
    require(controller.applied == std::set<std::string>{"b"}, "cross-account applied selection leaked");
    require(controller.devices.empty(), "cross-account devices leaked");
}

void testAsyncExceptions() {
    int calls = 0;
    Controller controller([&](const Json&, std::stop_token) -> Json {
        if (++calls == 1) throw std::runtime_error("transport exploded");
        throw 7;
    });
    controller.request("status"); waitIdle(controller);
    require(controller.message == "transport exploded", "async standard exception was not surfaced");
    controller.request("status"); waitIdle(controller);
    require(controller.message == "本机服务请求未完成。", "async unknown exception was not surfaced");
}

void testDestructorCancelsBlockedTransport() {
    std::atomic<bool> entered = false, cancelled = false;
    const auto start = std::chrono::steady_clock::now();
    {
        Controller controller([&](const Json&, std::stop_token stop) {
            entered = true;
            while (!stop.stop_requested()) std::this_thread::sleep_for(1ms);
            cancelled = true;
            return success();
        });
        controller.request("status");
        for (int i = 0; i != 200 && !entered; ++i) std::this_thread::sleep_for(1ms);
        require(entered, "blocked transport was not entered");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(cancelled, "destructor did not request transport cancellation");
    require(elapsed < 2s, "controller destructor exceeded two second cancellation bound");
}

void testAutomaticDeviceRefresh() {
    auto now=std::chrono::steady_clock::time_point{};
    int deviceCalls=0; bool online=false, fail=false;
    Controller controller([&](const Json& request,std::stop_token) {
        if(request["command"]=="devices") {
            ++deviceCalls;
            if(fail) return Json{{"success",false},{"message","Temporary network failure"}};
            auto snapshot=devices({"a","b"}); snapshot[0]["online"]=online;
            return success({{"status",status("Connected","alice",{"a"})},{"devices",snapshot}});
        }
        if(request["command"]=="logout") return success();
        return success({{"status",status("Connected","alice",{"a"})}});
    },true,[&]{return now;});
    controller.request("status"); controller.tick();
    require(deviceCalls==1 && !controller.devices[0].online,"initial offline snapshot missing");
    controller.choose("b",true);
    online=true; now+=5s; controller.tick();
    require(deviceCalls==1,"device polling ran too early");
    now+=5s; controller.tick();
    require(deviceCalls==2 && controller.devices[0].online,"online transition required manual refresh");
    require(controller.selected==std::set<std::string>{"a","b"},"automatic refresh overwrote pending selection");
    require(controller.filtered("Device B").size()==1,"refresh broke active filtering");
    fail=true; now+=10s; controller.tick();
    require(deviceCalls==3 && controller.devices[0].online,"failed refresh fabricated offline devices");
    for(int i=0;i<20;++i) controller.tick();
    require(deviceCalls==3,"failed refresh caused a tight retry loop");
    fail=false; online=false; now+=10s; controller.tick();
    require(deviceCalls==4 && !controller.devices[0].online,"offline transition was not refreshed");
    controller.request("logout"); now+=20s; controller.tick();
    require(deviceCalls==4,"device polling continued after logout");
}

} // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"status restore", testStatusRestore}, {"login and devices", testLoginAndDevices},
        {"selection preservation", testSelectionAcrossFilterRefreshAndStatus}, {"apply snapshot", testApplySnapshotsSelection},
        {"apply selection revision", testApplyPreservesLatestEditWhenItMatchesOldApplied},
        {"logout and auth failure", testLogoutAndAuthFailures}, {"cross-account clear", testCrossAccountClear},
        {"async exceptions", testAsyncExceptions}, {"destructor cancellation", testDestructorCancelsBlockedTransport},
        {"automatic device refresh", testAutomaticDeviceRefresh},
    };
    int passed = 0;
    for (const auto& [name, test] : tests) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
    }
    std::cout << passed << "/" << std::size(tests) << " GUI controller checks passed\n";
    return 0;
}
