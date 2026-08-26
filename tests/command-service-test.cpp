#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string commandSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/features/command/service.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open command service source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string projectSource(std::string_view relative) {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                std::filesystem::path(relative);
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open command integration source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeJsonCasts(std::string_view source) {
    require(source.find("COALESCE((protocol_params->>'remote_control')::boolean") ==
                std::string_view::npos,
            "command service directly casts remote_control");
    require(source.find("COALESCE((d.protocol_params->>'remote_control')::boolean") ==
                std::string_view::npos,
            "command service directly casts device remote_control");
    require(source.find("COALESCE((n.capability->>'deviceConfig')::boolean") ==
                std::string_view::npos,
            "command service directly casts edge deviceConfig capability");
    require(source.find("COALESCE((n.status->'config'->>'activeVersion')::bigint") ==
                std::string_view::npos,
            "command service directly casts edge activeVersion");
    require(source.find("COALESCE((item->>'writable')::boolean") == std::string_view::npos,
            "command service directly casts item writable");
    require(source.find("COALESCE((item->>'size')::integer") == std::string_view::npos,
            "command service directly casts item size");
    require(source.find("COALESCE((item->>'length')::integer") == std::string_view::npos,
            "command service directly casts item length");
    require(source.find("COALESCE((item->>'digits')::integer") == std::string_view::npos,
            "command service directly casts item digits");
}

void requireDuplicateElementRejected(std::string_view source) {
    require(source.find("seenElementIds") != std::string_view::npos,
            "command service does not track duplicate command elements");
    require(source.find("下发要素不能重复") != std::string_view::npos,
            "command service does not reject duplicate command elements");
}

} // namespace

int main() {
    try {
        const auto source = commandSource();
        requireNoUnsafeJsonCasts(source);
        requireDuplicateElementRejected(source);
        require(source.find("DeviceCommandWaitDto") != std::string::npos,
                "command service has no batched wait operation");
        const auto client = projectSource("web/pages/iot/device/device.service.ts");
        require(client.find("waitForDeviceCommands(command.command_ids)") != std::string::npos,
                "web command flow does not use the batched wait operation");
        require(client.find("window.setTimeout(resolve, 150)") == std::string::npos,
                "web command flow still polls every command at 150 ms");
        std::cout << "command service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "command service test failed: " << error.what() << '\n';
        return 1;
    }
}
