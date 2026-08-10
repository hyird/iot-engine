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

std::string persistenceSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/features/telemetry/persistence.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open telemetry persistence source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeUnsignedParsing(std::string_view source) {
    require(source.find("std::stoull(") == std::string_view::npos,
            "telemetry persistence uses unsafe/partial stoull parsing");
}

} // namespace

int main() {
    try {
        requireNoUnsafeUnsignedParsing(persistenceSource());
        std::cout << "telemetry persistence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "telemetry persistence test failed: " << error.what() << '\n';
        return 1;
    }
}
