#pragma once

#include <charconv>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/features/vpn/cidr.h"

#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace service::vpn::wireguard {

struct Peer final {
    std::string publicKey;
    std::vector<std::string> allowedIps;
};

struct HubConfig final {
    std::string interfaceName{"wg-iot"};
    std::string privateKey;
    std::string publicKey;
    std::string endpoint;
    std::uint16_t listenPort{51820};
    std::string address{"100.96.0.1/32"};
};

struct RuntimeStatus final {
    bool supported{};
    bool configured{};
    std::string code;
    std::string message;
    std::size_t peerCount{};
};

inline bool validKey(std::string_view value) noexcept {
    if (value.size() != 44 || value.back() != '=')
        return false;
    for (const auto character : value)
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '+' || character == '/' ||
              character == '='))
            return false;
    return true;
}

class IWireGuardController {
  public:
    virtual ~IWireGuardController() = default;
    virtual RuntimeStatus configure(const HubConfig& config) = 0;
    virtual RuntimeStatus upsertPeer(const HubConfig& config, const Peer& peer) = 0;
    virtual RuntimeStatus removePeer(const HubConfig& config, std::string_view publicKey) = 0;
    virtual RuntimeStatus status(const HubConfig& config) = 0;
    virtual std::optional<std::vector<std::string>> peerKeys(const HubConfig& config) = 0;
    virtual std::optional<std::unordered_map<std::string, std::uint64_t>> peerHandshakes(
        const HubConfig& config) = 0;
};

class UnsupportedController final : public IWireGuardController {
  public:
    RuntimeStatus configure(const HubConfig&) override { return unsupported(); }
    RuntimeStatus upsertPeer(const HubConfig&, const Peer&) override { return unsupported(); }
    RuntimeStatus removePeer(const HubConfig&, std::string_view) override { return unsupported(); }
    RuntimeStatus status(const HubConfig&) override { return unsupported(); }
    std::optional<std::vector<std::string>> peerKeys(const HubConfig&) override { return std::nullopt; }
    std::optional<std::unordered_map<std::string, std::uint64_t>>
    peerHandshakes(const HubConfig&) override {
        return std::nullopt;
    }

  private:
    static RuntimeStatus unsupported() {
        return {.supported = false,
                .configured = false,
                .code = "unsupported_platform",
                .message = "WireGuard hub is only supported on Linux",
                .peerCount = 0};
    }
};

#ifdef __linux__
class LinuxController final : public IWireGuardController {
  public:
    RuntimeStatus configure(const HubConfig& config) override {
        if (!validConfig(config))
            return failure("invalid_config", "WireGuard hub configuration is invalid");
        auto exists = execute({"ip", "link", "show", "dev", config.interfaceName});
        if (exists.exitCode != 0) {
            const auto created = execute({"ip", "link", "add", config.interfaceName, "type", "wireguard"});
            if (created.exitCode != 0)
                return failure("interface_create_failed", trim(created.output));
        }
        if (const auto address = execute({"ip", "address", "replace", config.address,
                                          "dev", config.interfaceName});
            address.exitCode != 0)
            return failure("address_configure_failed", trim(address.output));
        if (const auto key = execute({"wg", "set", config.interfaceName, "private-key", "/dev/stdin",
                                      "listen-port", std::to_string(config.listenPort)},
                                     config.privateKey + "\n");
            key.exitCode != 0)
            return failure("private_key_configure_failed", trim(key.output));
        if (const auto up = execute({"ip", "link", "set", "up", "dev", config.interfaceName});
            up.exitCode != 0)
            return failure("interface_up_failed", trim(up.output));
        return success(true);
    }

    RuntimeStatus upsertPeer(const HubConfig& config, const Peer& peer) override {
        if (!validConfig(config) || !validKey(peer.publicKey) || peer.allowedIps.empty())
            return failure("invalid_peer", "WireGuard peer configuration is invalid");
        std::string allowed;
        for (const auto& cidr : peer.allowedIps) {
            if (!parseCidr(cidr, 0, 32))
                return failure("invalid_peer_route", "WireGuard peer route is invalid");
            if (!allowed.empty())
                allowed.push_back(',');
            allowed += cidr;
        }
        const auto result = execute({"wg", "set", config.interfaceName, "peer", peer.publicKey,
                                     "allowed-ips", allowed});
        return result.exitCode == 0 ? success(true) : failure("peer_configure_failed", trim(result.output));
    }

    RuntimeStatus removePeer(const HubConfig& config, std::string_view publicKey) override {
        if (!validConfig(config) || !validKey(publicKey))
            return failure("invalid_peer", "WireGuard peer public key is invalid");
        const auto result = execute({"wg", "set", config.interfaceName, "peer", std::string(publicKey), "remove"});
        return result.exitCode == 0 ? success(true) : failure("peer_remove_failed", trim(result.output));
    }

    RuntimeStatus status(const HubConfig& config) override {
        if (!validConfig(config))
            return failure("invalid_config", "WireGuard hub configuration is invalid");
        const auto result = execute({"wg", "show", config.interfaceName, "dump"});
        if (result.exitCode != 0)
            return failure("status_unavailable", trim(result.output));
        std::size_t peers = 0;
        for (std::size_t index = 0; index < result.output.size(); ++index)
            if (result.output[index] == '\n')
                ++peers;
        return {.supported = true, .configured = true, .code = "ok",
                .message = "WireGuard hub is configured", .peerCount = peers > 0 ? peers - 1 : 0};
    }

    std::optional<std::vector<std::string>> peerKeys(const HubConfig& config) override {
        if (!validConfig(config))
            return std::nullopt;
        const auto result = execute({"wg", "show", config.interfaceName, "peers"});
        if (result.exitCode != 0)
            return std::nullopt;
        std::vector<std::string> peers;
        std::stringstream lines(result.output);
        std::string line;
        while (std::getline(lines, line)) {
            if (validKey(line))
                peers.push_back(std::move(line));
        }
        return peers;
    }

    std::optional<std::unordered_map<std::string, std::uint64_t>>
    peerHandshakes(const HubConfig& config) override {
        if (!validConfig(config))
            return std::nullopt;
        const auto result = execute({"wg", "show", config.interfaceName, "dump"});
        if (result.exitCode != 0)
            return std::nullopt;
        std::unordered_map<std::string, std::uint64_t> handshakes;
        std::stringstream lines(result.output);
        std::string line;
        bool header = true;
        while (std::getline(lines, line)) {
            if (header) {
                header = false;
                continue;
            }
            std::stringstream fields(line);
            std::string field;
            std::vector<std::string> values;
            while (std::getline(fields, field, '\t'))
                values.push_back(std::move(field));
            if (values.size() < 5 || !validKey(values[0]))
                continue;
            std::uint64_t seconds{};
            const auto [end, error] = std::from_chars(
                values[4].data(), values[4].data() + values[4].size(), seconds);
            if (error == std::errc{} && end == values[4].data() + values[4].size())
                handshakes.emplace(std::move(values[0]), seconds);
        }
        return handshakes;
    }

  private:
    struct CommandResult final {
        int exitCode{-1};
        std::string output;
    };

    static RuntimeStatus success(bool configured) {
        return {.supported = true, .configured = configured, .code = "ok",
                .message = "WireGuard hub is configured", .peerCount = 0};
    }

    static RuntimeStatus failure(std::string code, std::string message) {
        return {.supported = true, .configured = false, .code = std::move(code),
                .message = std::move(message), .peerCount = 0};
    }

    static bool validConfig(const HubConfig& config) noexcept {
        return !config.interfaceName.empty() && config.interfaceName.size() <= 15 &&
               validKey(config.privateKey) && parseCidr(config.address, 1, 32).has_value() &&
               config.listenPort != 0;
    }

    static std::string trim(std::string value) {
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
            value.pop_back();
        return value;
    }

    static CommandResult execute(const std::vector<std::string>& args, std::string_view input = {}) {
        int inputPipe[2]{};
        int outputPipe[2]{};
        if (pipe(inputPipe) != 0 || pipe(outputPipe) != 0)
            return {-1, std::strerror(errno)};
        const auto pid = fork();
        if (pid == -1)
            return {-1, std::strerror(errno)};
        if (pid == 0) {
            dup2(inputPipe[0], STDIN_FILENO);
            dup2(outputPipe[1], STDOUT_FILENO);
            dup2(outputPipe[1], STDERR_FILENO);
            close(inputPipe[0]); close(inputPipe[1]); close(outputPipe[0]); close(outputPipe[1]);
            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const auto& arg : args)
                argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            execvp(argv.front(), argv.data());
            _exit(127);
        }
        close(inputPipe[0]); close(outputPipe[1]);
        const auto* data = input.data();
        std::size_t remaining = input.size();
        while (remaining > 0) {
            const auto written = write(inputPipe[1], data, remaining);
            if (written <= 0)
                break;
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }
        close(inputPipe[1]);
        std::string output;
        char buffer[512];
        ssize_t readBytes = 0;
        while ((readBytes = read(outputPipe[0], buffer, sizeof(buffer))) > 0)
            output.append(buffer, static_cast<std::size_t>(readBytes));
        close(outputPipe[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, std::move(output)};
    }
};

#endif

inline IWireGuardController& controller() {
#ifdef __linux__
    static LinuxController value;
#else
    static UnsupportedController value;
#endif
    return value;
}

} // namespace service::vpn::wireguard
