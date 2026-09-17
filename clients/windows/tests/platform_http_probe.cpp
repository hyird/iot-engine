#include <chrono>
#include <iostream>
#include <thread>

#include "../common/json.h"
#include "../service/platform_vpn_api.h"
#include <atomic>
using namespace iotvpn;
using namespace iotvpn::service;

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    try {
        if(argc==3 && std::string_view(argv[2])=="backend") {
            std::string input;std::getline(std::cin,input);const auto fixture=parseJson(input);
            WinHttpPlatformVpnApi api(L"127.0.0.1",static_cast<unsigned short>(std::stoi(argv[1])),false);
            const auto login=api.login(fixture.at("username").get<std::string>(),fixture.at("password").get<std::string>(),{});
            const auto renewed=api.refresh(login.at("refresh_token").get<std::string>(),{});
            const auto token=renewed.at("token").get<std::string>();
            const auto devices=api.devices(token,{});
            if(devices.size()!=1 || devices[0].at("id")!=fixture.at("edge"))throw std::runtime_error("native device query mismatch");
            const auto created=api.apply(token,{},Json{{"name","Native fixture"},{"publicKey",fixture.at("publicKey")},{"edgeNodeIds",Json::array({fixture.at("edge")})}},{});
            const auto peer=created.at("peerId").get<std::string>();
            const auto configuration=api.config(token,peer,{});
            if(configuration.at("edgeNodeIds").size()!=1)throw std::runtime_error("native config mismatch");
            std::atomic<int> selected=-1;std::exception_ptr watchFailure;
            std::jthread watch([&](std::stop_token stop){try{api.watchConfig(token,peer,[&](const Json& value){selected=static_cast<int>(value.at("edgeNodeIds").size());},stop);}catch(...){watchFailure=std::current_exception();}});
            const auto waitSelected=[&](int count){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);while(selected!=count){if(std::chrono::steady_clock::now()>end)throw std::runtime_error("real backend push timed out");std::this_thread::sleep_for(std::chrono::milliseconds(5));}};
            waitSelected(1);
            const auto updated=api.apply(token,peer,Json{{"edgeNodeIds",Json::array()}},{});
            if(!updated.at("edgeNodeIds").empty())throw std::runtime_error("native update mismatch");
            waitSelected(0);watch.request_stop();watch.join();if(watchFailure)std::rethrow_exception(watchFailure);
            api.remove(token,peer,{});
            try{(void)api.config(token,peer,{});throw std::runtime_error("revoked peer accepted");}
            catch(const PlatformVpnApiError& error){if(!error.isPeerUnavailable())throw;}
            api.closeSession();
            if(api.devices(token,{}).size()!=1)throw std::runtime_error("native authentication restore failed");
            api.closeSession();
            std::cout<<"PASS native HTTP/SSE with real backend: login/refresh, devices, peer create/config/update, change push, cancel, revoke error and reconnect authentication\n";
            return 0;
        }
        if(argc==3 && std::string_view(argv[2])=="api") {
            WinHttpPlatformVpnApi api(L"127.0.0.1",static_cast<unsigned short>(std::stoi(argv[1])),false);
            auto login=api.login("fixture-user","fixture-password",{});
            if(login.at("token")!="token-one")throw std::runtime_error("login failed");
            auto renewed=api.refresh("refresh-one",{});
            if(renewed.at("token")!="token-two")throw std::runtime_error("refresh failed");
            if(api.devices("token-two",{}).size()!=1)throw std::runtime_error("devices failed");
            const std::string peer="00000000-0000-4000-8000-000000000002";
            if(api.apply("token-two",{},Json::object(),{}).at("id")!=peer)throw std::runtime_error("create failed");
            if(api.config("token-two",peer,{}).at("revision")!=1)throw std::runtime_error("config failed");
            std::atomic<int> revision=0;
            std::exception_ptr watchFailure;
            std::jthread watch([&](std::stop_token stop){try{api.watchConfig("token-two",peer,[&](const Json& value){revision=value.at("revision").get<int>();},stop);}catch(...){watchFailure=std::current_exception();}});
            const auto waitRevision=[&](int value){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(revision.load()!=value){if(std::chrono::steady_clock::now()>end)throw std::runtime_error("push timed out");std::this_thread::sleep_for(std::chrono::milliseconds(5));}};
            waitRevision(1);
            api.apply("token-two",peer,Json::object(),{});
            waitRevision(2);
            watch.request_stop();watch.join();if(watchFailure)std::rethrow_exception(watchFailure);
            if(api.devices("token-two",{}).size()!=1)throw std::runtime_error("subscription cancellation closed shared connection");
            revision=0;std::atomic<bool> refreshedWatchEnded=false;
            std::jthread oldWatch([&](std::stop_token stop){try{api.watchConfig("token-two",peer,[&](const Json& value){revision=value.at("revision").get<int>();},stop);}catch(...){} refreshedWatchEnded=true;});
            waitRevision(1);
            api.closeSession();
            const auto refreshDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(!refreshedWatchEnded){if(std::chrono::steady_clock::now()>refreshDeadline)throw std::runtime_error("session close left old subscription waiting forever");std::this_thread::sleep_for(std::chrono::milliseconds(5));}
            oldWatch.join();
            api.remove("token-two",peer,{});
            api.closeSession();
            if(api.devices("token-two",{}).size()!=1)throw std::runtime_error("session restore failed");
            api.closeSession();
            std::cout<<"PASS native platform login/refresh, query/create/update/delete, concurrent push, isolated cancellation\n";
            return 0;
        }
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
