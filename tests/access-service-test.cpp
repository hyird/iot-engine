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
                "service/modules/open_access/open_access.service.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open access service source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string accessFeatureSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/features/access/access.service.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open access feature service source");
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

}

void requireCanonicalBooleanPointValues(std::string_view source) {
    require(source.find("service::telemetry::latest::canonicalPointJson(") !=
                    std::string_view::npos &&
                source.find("normalizedValues.call(\"jsonb_typeof\", {normalizedPointValue})") != std::string_view::npos &&
                source.find("items.call(\"to_jsonb\", {itemPointNumber})") != std::string_view::npos,
            "public device data does not canonicalize BOOL points to 0/1");
}

void requireAccessQueriesUseOrm(std::string_view source) {
    require(source.find("catalog.caseWhen(") != std::string_view::npos &&
                source.find("catalog.column(\"skip_tls_verify\", \"webhook\")") !=
                    std::string_view::npos &&
                source.find("catalog.value(\"1\")") != std::string_view::npos &&
                source.find("catalog.value(\"0\")") != std::string_view::npos &&
                source.find("skip_tls_verify::text") == std::string_view::npos,
            "stored webhook TLS verification flag is parsed ambiguously");
    require(source.find("#include <ruvia/web/db/DbQuery.h>") != std::string_view::npos,
            "access feature service does not include the pinned DbQuery API");
    require(source.find("R\"sql") == std::string_view::npos &&
                source.find("transaction.query(\"") == std::string_view::npos &&
                source.find("context.db().query(\"") == std::string_view::npos &&
                source.find("context.db().execute(\"") == std::string_view::npos,
            "access feature service still embeds raw SQL");
    require(source.find("transaction.query(advisoryLock)") != std::string_view::npos,
            "access session refresh does not use a DbQuery advisory lock");
    require(source.find("snapshot.filter(") != std::string_view::npos &&
                source.find(".joinFunction(ruvia::DbJoinType::kCross") !=
                    std::string_view::npos,
            "access projections lost aggregate FILTER or LATERAL JSON semantics");
    require(source.find("catalog.caseWhen(") != std::string_view::npos &&
                source.find("catalog.column(\"skip_tls_verify\", \"webhook\")") !=
                    std::string_view::npos &&
                source.find("catalog.value(\"1\")") != std::string_view::npos &&
                source.find("catalog.value(\"0\")") != std::string_view::npos,
            "access webhook projection does not preserve boolean CASE semantics");
    require(source.find(".distinctOn({latest.column(\"webhook_id\", \"incoming\")})") !=
                std::string_view::npos &&
                source.find(".distinctOn({latestUsage.column(\"access_key_id\", \"incoming\")})") !=
                    std::string_view::npos,
            "access batch persistence lost DISTINCT ON selection");
    require(source.find(".with(\"updated\", updated)") != std::string_view::npos &&
                source.find(".with(\"usage_updated\", usageUpdated)") !=
                    std::string_view::npos,
            "access batch persistence lost data-changing CTEs");
    require(source.find(".onConflict({.columns = {\"id\"}, .doNothing = true})") !=
                std::string_view::npos,
            "access batch persistence lost idempotent ON CONFLICT handling");
}

} // namespace

int main() {
    try {
        const auto source = accessSource();
        const auto featureSource = accessFeatureSource();
        requireNoUnsafeParsers(source);
        requireStrictPresentFieldTypes(source);
        requireCanonicalBooleanPointValues(source);
        requireAccessQueriesUseOrm(featureSource);
        std::cout << "access service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "access service test failed: " << error.what() << '\n';
        return 1;
    }
}
