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

std::string accessSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/domains/access/access.service.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open access service source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeParsers(std::string_view source) {
    require(source.find("COALESCE((protocol_params->>'remote_control')::boolean") ==
                std::string_view::npos,
            "access service directly casts remote_control");
    require(source.find("std::stoll(") == std::string_view::npos,
            "access service uses unsafe/partial stoll parsing");
}

void requireStrictPresentFieldTypes(std::string_view source) {
    require(source.find("std::string(field) + \" 必须是字符串\"") != std::string_view::npos,
            "access service treats present non-string optional fields as absent");
    require(source.find("std::string(field) + \" 必须是整数\"") != std::string_view::npos,
            "access service treats present non-integer optional fields as absent");
    require(source.find(
                "const auto scopes = payload.get<ruvia::Array<ruvia::String>>(\"scopes\")") ==
                std::string_view::npos,
            "access key update ignores present non-array scopes");
    require(source.find(
                "const auto accessKeyId = payload.get<ruvia::String>(\"accessKeyId\")") ==
                std::string_view::npos,
            "webhook update ignores present non-string accessKeyId");
    require(source.find(
                "const auto events = payload.get<ruvia::Array<ruvia::String>>(\"eventTypes\")") ==
                std::string_view::npos,
            "webhook update ignores present non-array eventTypes");
    require(source.find("payload.get<ruvia::Bool>(field)") != std::string_view::npos,
            "webhook TLS verification flag is not strictly parsed as boolean");
    require(source.find("CASE WHEN skip_tls_verify THEN '1' ELSE '0' END") !=
                    std::string_view::npos &&
                source.find("skip_tls_verify::text") == std::string_view::npos,
            "stored webhook TLS verification flag is parsed ambiguously");
}

void requireCanonicalBooleanPointValues(std::string_view source) {
    require(source.find("service::telemetry::latest::canonicalPointJson(") !=
                    std::string_view::npos &&
                source.find("jsonb_typeof(filtered.data->'values'->point.id->'value') = "
                            "'boolean'") != std::string_view::npos,
            "public device data does not canonicalize BOOL points to 0/1");
}

} // namespace

int main() {
    try {
        const auto source = accessSource();
        requireNoUnsafeParsers(source);
        requireStrictPresentFieldTypes(source);
        requireCanonicalBooleanPointValues(source);
        std::cout << "access service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "access service test failed: " << error.what() << '\n';
        return 1;
    }
}
