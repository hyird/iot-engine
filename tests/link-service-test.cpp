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

std::string linkSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/domains/link/link.service.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open link service source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeParsing(std::string_view source) {
    require(source.find("std::stoll") == std::string_view::npos,
            "link service uses unsafe/partial stoll parsing");
    require(source.find("COALESCE((endpoint->>'port')::integer") == std::string_view::npos,
            "link service directly casts endpoint port");
}

void requireInputValidation(std::string_view source) {
    require(source.find("if (!value || value->view().empty())") != std::string_view::npos,
            "link service accepts empty required strings");
    require(source.find("mode != \"TCP Server\" && mode != \"TCP Client\"") !=
                std::string_view::npos,
            "link service accepts unknown mode as TCP Client");
    require(source.find("protocol != \"SL651\" && protocol != \"Modbus\" && protocol != \"S7\"") !=
                std::string_view::npos,
            "link service accepts unknown protocol");
    require(source.find("status != \"enabled\" && status != \"disabled\"") !=
                std::string_view::npos,
            "link service accepts unknown link status");
    require(source.find("targetStatus != \"enabled\" && targetStatus != \"disabled\"") !=
                std::string_view::npos,
            "link service accepts unknown target status");
}

} // namespace

int main() {
    try {
        const auto source = linkSource();
        requireNoUnsafeParsing(source);
        requireInputValidation(source);
        std::cout << "link service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "link service test failed: " << error.what() << '\n';
        return 1;
    }
}
