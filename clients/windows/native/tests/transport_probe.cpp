#include "../service/service.h"
#include <iostream>
#include <chrono>
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--local") {
        try {
            const auto status = iotvpn::pipeRequest({{"command","status"}},15000);
            if (!status.value("success",false)) throw std::runtime_error("Local status failed");
            const auto devices = iotvpn::pipeRequest({{"command","devices"}},30000);
            if (!devices.value("success",false)) throw std::runtime_error(devices.value("message","Device request failed"));
            std::cout << "PASS installed service identity, IPC and devices=" << devices.at("devices").size() << '\n';
            return 0;
        } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
    }
    std::string token; std::getline(std::cin,token);
    if (token.empty()) return 2;
    try {
        iotvpn::service::WinHttpTransport api;
        const auto start=std::chrono::steady_clock::now();
        const auto devices=api.devices(token,{});
        std::cout << "PASS devices=" << devices.size() << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count() << '\n';
        return devices.is_array() && !devices.empty() ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
