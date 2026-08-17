#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::string readProjectorSource() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                            "service" / "features" / "edge" / "projector.h";
    std::ifstream input(sourcePath, std::ios::binary);
    require(input.good(), "cannot open edge projector source");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    try {
        const auto source = readProjectorSource();
        require(source.find("COALESCE((edge_node.capability->>'terminal')::boolean") ==
                    std::string::npos,
                "edge projector directly casts terminal capability");
        require(source.find("COALESCE((status->'config'->>'activeVersion')::bigint") ==
                    std::string::npos,
                "edge projector directly casts activeVersion");
        require(source.find("COALESCE((status->'config'->>'desiredVersion')::bigint") ==
                    std::string::npos,
                "edge projector directly casts desiredVersion");
        require(source.find("COALESCE((node.status->'config'->>'desiredVersion')::bigint") ==
                    std::string::npos,
                "edge projector directly casts node desiredVersion");
        require(source.find("CASE lower(COALESCE(edge_node.capability->>'terminal', ''))") !=
                    std::string::npos,
                "edge projector does not guard terminal capability");
        require(source.find("status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'") !=
                    std::string::npos,
                "edge projector does not guard desiredVersion");
        require(source.find("status, last_seen_at, updated_at") != std::string::npos,
                "edge hello insert does not record presence");
        require(source.find("WHEN edge_node.enrollment_status IN ('pending', 'approved') THEN NOW()") !=
                    std::string::npos,
                "edge hello update does not refresh pending and approved presence");
        require(source.find("ELSE edge_node.last_seen_at END") != std::string::npos,
                "edge hello update refreshes rejected presence");
        std::cout << "edge projector tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "edge projector test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
