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

std::string webhookSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/features/access/webhook.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open access webhook source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeStoll(std::string_view source) {
    require(source.find("std::stoll(") == std::string_view::npos,
            "access webhook uses unsafe/partial stoll parsing");
}

void requireUnambiguousTlsVerificationFlag(std::string_view source) {
    require(source.find("CASE WHEN webhook.skip_tls_verify THEN '1' ELSE '0' END") !=
                    std::string_view::npos &&
                source.find("webhook.skip_tls_verify::text") == std::string_view::npos,
            "webhook runtime parses PostgreSQL boolean text ambiguously");
}

void requireCanonicalBooleanPointValues(std::string_view source) {
    require(source.find("parsed->get<ruvia::String>(\"dataType\")") !=
                    std::string_view::npos &&
                source.find("telemetry::latest::canonicalPointJson(") !=
                    std::string_view::npos,
            "webhook payloads do not canonicalize BOOL points to 0/1");
}

} // namespace

int main() {
    try {
        const auto source = webhookSource();
        requireNoUnsafeStoll(source);
        requireUnambiguousTlsVerificationFlag(source);
        requireCanonicalBooleanPointValues(source);
        std::cout << "access webhook tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "access webhook test failed: " << error.what() << '\n';
        return 1;
    }
}
