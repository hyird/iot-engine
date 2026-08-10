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

std::string latestSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/features/telemetry/latest.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open telemetry latest source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeParsing(std::string_view source) {
    require(source.find("std::stoll") == std::string_view::npos,
            "telemetry latest uses unsafe/partial stoll parsing");
    require(source.find("COALESCE((d.protocol_params->>'online_timeout')::bigint") ==
                std::string_view::npos,
            "telemetry latest directly casts online_timeout");
    require(source.find("COALESCE((p.config->>'readInterval')::numeric") ==
                std::string_view::npos,
            "telemetry latest directly casts readInterval");
    require(source.find("(p.config->>'pollInterval')::numeric, 5") == std::string_view::npos,
            "telemetry latest directly casts pollInterval");
    require(source.find("COALESCE(d.protocol_params->>'online_timeout', '') ~ "
                        "'^-?[0-9]{1,18}$'") != std::string_view::npos,
            "telemetry latest does not guard online_timeout casts");
    require(source.find("COALESCE(p.config->>'readInterval', '') ~") != std::string_view::npos,
            "telemetry latest does not guard readInterval casts");
    require(source.find("COALESCE(p.config->>'pollInterval', '') ~") != std::string_view::npos,
            "telemetry latest does not guard pollInterval casts");
    require(source.find("NULLIF(numbered.element->>'scale', '')::numeric") ==
                std::string_view::npos,
            "telemetry latest directly casts element scale");
    require(source.find("numbered.element->>'digits'), '')::bigint") == std::string_view::npos,
            "telemetry latest directly casts element decimals/digits");
}

} // namespace

int main() {
    try {
        requireNoUnsafeParsing(latestSource());
        std::cout << "telemetry latest tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "telemetry latest test failed: " << error.what() << '\n';
        return 1;
    }
}
