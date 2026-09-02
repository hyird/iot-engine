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
        require(source.find("last_seen_at = NOW()") != std::string::npos,
                "edge hello update does not refresh node presence");
        require(source.find("edge_node.enrollment_status IN ('pending', 'approved')") ==
                    std::string::npos,
                "edge hello update still carries rejected enrollment compatibility");
        require(source.find("result.actual_values_size()") != std::string::npos &&
                    source.find("actual_value_count") != std::string::npos &&
                    source.find("actual.has_value()") != std::string::npos &&
                    source.find("scalarText(actual.value())") != std::string::npos,
                "edge command projector drops physical readback values");
        require(source.find("return value.bool_value() ? \"true\" : \"false\";") ==
                    std::string::npos &&
                    source.find("return value.bool_value() ? \"1\" : \"0\";") !=
                    std::string::npos,
                "edge BOOL values are not projected as 0/1");
        require(source.find(",\\\"dataType\\\":\\\"") != std::string::npos &&
                    source.find("scalarKind(item.value())") != std::string::npos,
                "edge telemetry does not retain point data types");
        std::cout << "edge projector tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "edge projector test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
