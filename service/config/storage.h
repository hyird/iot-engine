#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/web/Dotenv.h>

namespace service::config {

struct DeviceDataStoragePolicy final {
    bool compressionEnabled{true};
    std::int64_t chunkIntervalHours{7 * 24};
    std::int64_t compressionAfterHours{7 * 24};
    std::int64_t mutableWindowHours{2 * 24};
};

inline constexpr std::int64_t kMaximumStoragePolicyHours = 10 * 365 * 24;

inline bool parseStoragePolicyFlag(std::string_view name, std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    if (normalized == "1" || normalized == "true" || normalized == "yes" ||
        normalized == "on")
        return true;
    if (normalized == "0" || normalized == "false" || normalized == "no" ||
        normalized == "off")
        return false;
    throw std::invalid_argument(std::string(name) +
                                " must be true/false, yes/no, on/off, or 1/0");
}

inline std::int64_t parseStoragePolicyHours(std::string_view name,
                                           std::string_view value,
                                           bool allowZero = false) {
    std::int64_t hours{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), hours);
    const auto minimum = allowZero ? 0 : 1;
    if (error != std::errc{} || end != value.data() + value.size() ||
        hours < minimum || hours > kMaximumStoragePolicyHours) {
        throw std::invalid_argument(std::string(name) + " must be an integer between " +
                                    std::to_string(minimum) + " and " +
                                    std::to_string(kMaximumStoragePolicyHours));
    }
    return hours;
}

inline void validateDeviceDataStoragePolicy(const DeviceDataStoragePolicy& policy) {
    if (policy.chunkIntervalHours <= 0 ||
        policy.chunkIntervalHours > kMaximumStoragePolicyHours)
        throw std::invalid_argument(
            "DEVICE_DATA_CHUNK_INTERVAL_HOURS is outside the supported range");
    if (policy.compressionAfterHours <= 0 ||
        policy.compressionAfterHours > kMaximumStoragePolicyHours)
        throw std::invalid_argument(
            "DEVICE_DATA_COMPRESSION_AFTER_HOURS is outside the supported range");
    if (policy.mutableWindowHours < 0 ||
        policy.mutableWindowHours > kMaximumStoragePolicyHours)
        throw std::invalid_argument(
            "DEVICE_DATA_MUTABLE_WINDOW_HOURS is outside the supported range");
    if (policy.compressionEnabled &&
        policy.compressionAfterHours <= policy.mutableWindowHours) {
        throw std::invalid_argument(
            "DEVICE_DATA_COMPRESSION_AFTER_HOURS must be greater than "
            "DEVICE_DATA_MUTABLE_WINDOW_HOURS while compression is enabled");
    }
}

inline DeviceDataStoragePolicy deviceDataStoragePolicy(const ruvia::Env& env) {
    DeviceDataStoragePolicy policy;
    if (const auto value = env.get("DEVICE_DATA_COMPRESSION_POLICY_ENABLED"))
        policy.compressionEnabled =
            parseStoragePolicyFlag("DEVICE_DATA_COMPRESSION_POLICY_ENABLED", *value);
    if (const auto value = env.get("DEVICE_DATA_CHUNK_INTERVAL_HOURS"))
        policy.chunkIntervalHours =
            parseStoragePolicyHours("DEVICE_DATA_CHUNK_INTERVAL_HOURS", *value);
    if (const auto value = env.get("DEVICE_DATA_COMPRESSION_AFTER_HOURS"))
        policy.compressionAfterHours =
            parseStoragePolicyHours("DEVICE_DATA_COMPRESSION_AFTER_HOURS", *value);
    if (const auto value = env.get("DEVICE_DATA_MUTABLE_WINDOW_HOURS"))
        policy.mutableWindowHours =
            parseStoragePolicyHours("DEVICE_DATA_MUTABLE_WINDOW_HOURS", *value, true);
    validateDeviceDataStoragePolicy(policy);
    return policy;
}

struct DeviceDataStoragePolicyMigration final {
    std::string id;
    std::string sql;
};

inline DeviceDataStoragePolicyMigration
deviceDataStoragePolicyMigration(const DeviceDataStoragePolicy& policy) {
    validateDeviceDataStoragePolicy(policy);

    DeviceDataStoragePolicyMigration migration;
    migration.id = "runtime_device_data_storage_policy_v1_";
    migration.id += policy.compressionEnabled ? "on" : "off";
    migration.id += "_chunk_" + std::to_string(policy.chunkIntervalHours);
    migration.id += "_after_" + std::to_string(policy.compressionAfterHours);
    migration.id += "_mutable_" + std::to_string(policy.mutableWindowHours);

    migration.sql = R"sql(
DO $storage_policy$
BEGIN
DELETE FROM sys_schema_migrations
WHERE starts_with(migration_id, 'runtime_device_data_storage_policy_v1_');

PERFORM remove_compression_policy('device_data', if_exists => TRUE);

PERFORM set_chunk_time_interval(
    'device_data',
    make_interval(hours => )sql";
    migration.sql += std::to_string(policy.chunkIntervalHours);
    migration.sql += R"sql()
);
)sql";

    if (policy.compressionEnabled) {
        migration.sql += R"sql(
PERFORM add_compression_policy(
    'device_data',
    compress_after => make_interval(hours => )sql";
        migration.sql += std::to_string(policy.compressionAfterHours);
        migration.sql += R"sql(),
    if_not_exists => TRUE
);
)sql";
    }
    migration.sql += R"sql(
END
$storage_policy$;
)sql";
    return migration;
}

} // namespace service::config
