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

std::string deviceSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/domains/device/device.service.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open device service source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string deviceFormSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "web/pages/iot/device/DeviceFormModal.tsx";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open device form source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeParsing(std::string_view source) {
    require(source.find("std::stoll") == std::string_view::npos,
            "device service uses unsafe/partial stoll parsing");
    require(source.find("std::stod") == std::string_view::npos,
            "device service uses unsafe/partial stod parsing");
    require(source.find("COALESCE((protocol_params->>'remote_control')::boolean") ==
                std::string_view::npos,
            "device service directly casts remote_control");
    require(source.find("COALESCE((d.protocol_params->>'online_timeout')::integer") ==
                std::string_view::npos,
            "device service directly casts online_timeout");
    require(source.find("COALESCE((d.protocol_params->>'remote_control')::boolean") ==
                std::string_view::npos,
            "device service directly casts item remote_control");
    require(source.find("(d.protocol_params->>'slave_id')::integer") == std::string_view::npos,
            "device service directly casts slave_id");
    require(source.find("(p.config->>'readInterval')::numeric") == std::string_view::npos,
            "device service directly casts readInterval");
    require(source.find("(p.config->>'storageInterval')::numeric") == std::string_view::npos,
            "device service directly casts storageInterval");
    require(source.find("(l.endpoint->>'port')::integer") == std::string_view::npos,
            "device service directly casts edge endpoint port");
    require(source.find("(l.endpoint->>'rs485')::boolean") == std::string_view::npos,
            "device service directly casts edge rs485");
    require(source.find("COALESCE((element->>'writable')::boolean") == std::string_view::npos,
            "device service directly casts writable");
    require(source.find("COALESCE((n.capability->>'deviceConfig')::boolean") ==
                std::string_view::npos,
            "device service directly casts edge deviceConfig capability");
}

} // namespace

int main() {
    try {
        const auto service = deviceSource();
        requireNoUnsafeParsing(service);
        require(service.find("设备所属链路不可修改") == std::string::npos &&
                    service.find("设备所属边缘节点不可修改") == std::string::npos &&
                    service.find("设备类型不可修改") == std::string::npos,
                "device connection fields are still immutable");
        require(service.find("createEdgeLink") != std::string::npos &&
                    service.find("retireEdgeLink") != std::string::npos,
                "device connection-mode changes do not migrate edge links");
        const auto form = deviceFormSource();
        require(form.find("disabled={!!editing}") == std::string::npos,
                "device edit form still disables connection fields");
        std::cout << "device service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "device service test failed: " << error.what() << '\n';
        return 1;
    }
}
