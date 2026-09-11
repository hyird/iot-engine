#pragma once
#include "../vendor/json.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iotvpn {
using Json = nlohmann::json;
inline constexpr char ProductName[] = "iot-egine";
inline constexpr char PlatformUrl[] = "https://i.a-z.xin";
inline constexpr wchar_t AgentService[] = L"iot-egine.tunnel";
inline constexpr wchar_t PipeName[] = L"\\\\.\\pipe\\IotEngineVpn.Controller";
inline constexpr std::size_t MaxMessageBytes = 1024 * 1024;

std::wstring utf16(std::string_view text);
std::string utf8(std::wstring_view text);
std::filesystem::path stateDirectory();
std::filesystem::path moduleDirectory();
Json parseJson(std::string_view text);
Json pipeRequest(const Json& request, std::uint32_t timeoutMs = 90000, std::stop_token stop = {});
void runPipeServer(std::stop_token stop, const std::function<Json(const Json&)>& handler);

std::vector<std::uint8_t> protectData(std::span<const std::uint8_t> data, std::string_view entropy = {}, std::wstring_view description = {});
std::vector<std::uint8_t> unprotectData(std::span<const std::uint8_t> data, std::string_view entropy = {});
std::vector<std::uint8_t> readFile(const std::filesystem::path& path, std::size_t limit = MaxMessageBytes);
void atomicWrite(const std::filesystem::path& path, std::span<const std::uint8_t> data);
Json loadState();
void saveState(const Json& state);

class WireGuardTunnel {
public:
    virtual ~WireGuardTunnel() = default;
    // public key first, private key second
    virtual std::pair<std::string, std::string> generateKeys() = 0;
    virtual bool running() = 0;
    virtual Json diagnostics() { return {{"adapterUp",running()},{"linkState","Unknown"}}; }
    virtual void stop() = 0;
    virtual void apply(const Json& config, const std::string& privateKey) = 0;
};
std::unique_ptr<WireGuardTunnel> createWindowsWireGuardTunnel();
void validateTunnelConfig(const Json& config, std::string_view privateKey);
std::string renderTunnelConfig(const Json& config, std::string_view privateKey);
}
