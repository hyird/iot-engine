#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "service/modules/alert/alert.service.h"
#include "service/features/alert/alert.runtime.h"

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void requireAbsent(const std::string& haystack, const char* needle, const char* message) {
    require(haystack.find(needle) == std::string::npos, message);
}

std::string readSource(const char* relative) {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / relative;
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open alert source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void expectInvalidConditions(std::string_view raw, const char* message) {
    bool rejected = false;
    try {
        service::alert::AlertService::validateConditionsForTest(raw);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}

int main() {
    try {
        const auto sql = service::alert::AlertEvaluationService::evaluationTailForTest();
        require(sql.find("^[0-9]{1,10}$") != std::string::npos, "offline duration must be guarded before casting");
        require(sql.find("^-?[0-9]+([.][0-9]+)?$") != std::string::npos, "numeric conditions must be guarded before casting");
        require(sql.find("^([0-9]|[1-5][0-9]|6[0-2])$") != std::string::npos, "bit index must be bounded before shifting");
        require(sql.find("int8shr") != std::string::npos, "bit conditions must use the parsed index");
        require(sql.find("CASE WHEN") != std::string::npos, "numeric casts must retain guarded CASE expressions");
        const auto refreshSql = service::alert::metadata::detail::refreshQuery().compile(ruvia::DbDriver::kPostgreSql, nullptr, ruvia::DbParameterMode::kLiteral);
        require(refreshSql.sql().find("^[0-9]{1,10}$") != std::string_view::npos, "metadata refresh must guard offline duration");
        const auto metadataSource = readSource("service/features/alert/alert.service.h");
        requireAbsent(metadataSource, "std::stoll(",
                      "alert metadata uses unsafe/partial stoll parsing");
        const auto runtimeSource = readSource("service/features/alert/alert.runtime.h");
        requireAbsent(runtimeSource, "std::stoull(",
                      "alert runtime uses unsafe/partial stoull parsing");
        const auto serviceSource = readSource("service/modules/alert/alert.service.h");
        require(serviceSource.find("std::string(field) + \" 必须是字符串\"") !=
                    std::string::npos,
                "alert service treats present non-string optional fields as absent");
        require(serviceSource.find("std::string(field) + \" 必须是整数\"") !=
                    std::string::npos,
                "alert service treats present non-integer fields as default values");

        service::alert::AlertService::validateConditionsForTest(
            R"([{"type":"threshold","elementKey":"temperature","operator":">","value":"12.5"}])");
        service::alert::AlertService::validateConditionsForTest(
            R"([{"type":"offline","duration":300}])");
        service::alert::AlertService::validateConditionsForTest(
            R"([{"type":"rate_of_change","elementKey":"flow","changeRate":"15","changeDirection":"rise"}])");
        expectInvalidConditions(
            R"([{"type":"threshold","elementKey":"temperature","operator":">","value":"abc"}])",
            "alert rule accepted a non-numeric threshold for a numeric operator");
        expectInvalidConditions(
            R"([{"type":"rate_of_change","elementKey":"flow","changeRate":"abc"}])",
            "alert rule accepted a non-numeric rate-of-change threshold");
        expectInvalidConditions(
            R"([{"type":"threshold","elementKey":"status","operator":"==","value":"1","bitIndex":"bad"}])",
            "alert rule accepted a malformed bitIndex");
        expectInvalidConditions(R"([{"type":"threshold","operator":">","value":"1"}])",
                                "alert rule accepted a condition without elementKey");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "alert runtime test failed: " << error.what() << '\n';
        return 1;
    }
}
