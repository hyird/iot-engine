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

std::string repositorySource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/features/runtime/repository.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open runtime repository source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeJsonCasts(std::string_view source) {
    require(source.find("std::stoll") == std::string_view::npos,
            "runtime repository integer parser accepts unsafe/partial values");
    require(source.find("(endpoint->>'port')::integer") == std::string_view::npos,
            "runtime repository directly casts link endpoint port");
    require(source.find("(d.protocol_params->>'online_timeout')::integer") ==
                std::string_view::npos,
            "runtime repository directly casts online_timeout");
    require(source.find("(d.protocol_params->>'slave_id')::integer") == std::string_view::npos,
            "runtime repository directly casts slave_id");
    require(source.find("(element->>'writable')::boolean") == std::string_view::npos,
            "runtime repository directly casts element writable");
    require(source.find("(element->>'address')::integer") == std::string_view::npos,
            "runtime repository directly casts modbus address in ORDER BY");
    require(source.find("(element->>'start')::integer") == std::string_view::npos,
            "runtime repository directly casts s7 start in ORDER BY");
}

} // namespace

int main() {
    try {
        requireNoUnsafeJsonCasts(repositorySource());
        std::cout << "runtime repository tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime repository test failed: " << error.what() << '\n';
        return 1;
    }
}
