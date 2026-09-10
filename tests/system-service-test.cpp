#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "service/common/message.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string readSource(const char* relative) {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / relative;
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open system service source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeStoll(const char* relative, const char* message) {
    const auto source = readSource(relative);
    require(source.find("std::stoll(") == std::string::npos, message);
}

void requireRolePermissionValidation() {
    const auto source = readSource("service/modules/system/role/role.service.h");
    require(source.find("权限编码不能为空") != std::string::npos,
            "role service accepts empty permission codes");
    require(source.find("value.size() > 128") != std::string::npos,
            "role service accepts overlong permission codes");
}

void requireUserRoleIdsDeduplicated() {
    const auto source = readSource("service/modules/system/user/user.service.h");
    require(source.find("角色不能重复") != std::string::npos,
            "user service accepts duplicate role_ids");
    require(source.find("seenRoleIds") != std::string::npos,
            "user service does not track duplicate role_ids before replacing roles");
}

void requireWorkerMetricSnapshotContract() {
    const auto metrics0 = service::message::worker_metrics::metricsSnapshotKey(0, "test");
    const auto metrics1 = service::message::worker_metrics::metricsSnapshotKey(1, "test");
    const auto readiness0 =
        service::message::worker_metrics::readinessSnapshotKey(0, "test");
    require(metrics0 != metrics1, "worker metric snapshots share a Redis key");
    require(metrics0.find(":test:worker:0") != std::string::npos,
            "worker metric snapshot key omits worker identity");
    require(metrics0 != readiness0, "metric and readiness snapshots share a Redis key");

    const auto encoded = service::message::worker_metrics::encodeReadinessSnapshot(
        true, R"({"status":"ready"})");
    const auto decoded =
        service::message::worker_metrics::decodeReadinessSnapshot(encoded);
    require(decoded.has_value() && decoded->ready &&
                decoded->healthJson == R"({"status":"ready"})",
            "readiness snapshot payload does not round-trip");
    require(!service::message::worker_metrics::decodeReadinessSnapshot("1\n{}x").has_value(),
            "malformed readiness snapshot was accepted");
}

} // namespace

int main() {
    try {
        requireNoUnsafeStoll("service/modules/system/user/user.service.h",
                             "user service uses unsafe/partial stoll parsing");
        requireNoUnsafeStoll("service/modules/system/role/role.service.h",
                             "role service uses unsafe/partial stoll parsing");
        requireNoUnsafeStoll("service/modules/system/dept/dept.service.h",
                             "department service uses unsafe/partial stoll parsing");
        requireRolePermissionValidation();
        requireUserRoleIdsDeduplicated();
        requireWorkerMetricSnapshotContract();
        std::cout << "system service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "system service test failed: " << error.what() << '\n';
        return 1;
    }
}
