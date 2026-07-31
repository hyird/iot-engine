#include <iostream>
#include <stdexcept>
#include <string>

#include "service/config/storage.h"

namespace storage = service::config;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Action>
void requireInvalid(Action&& action, const char* message) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message);
}

int main() {
    try {
        storage::DeviceDataStoragePolicy defaults;
        storage::validateDeviceDataStoragePolicy(defaults);
        require(defaults.compressionEnabled, "compression must default to enabled");
        require(defaults.chunkIntervalHours == 168,
                "device_data chunks must default to seven days");
        require(defaults.compressionAfterHours == 168,
                "device_data compression must default to seven days");
        require(defaults.mutableWindowHours == 48,
                "device_data mutable window must default to two days");

        require(storage::parseStoragePolicyFlag("flag", "ON"),
                "case-insensitive true flag parsing failed");
        require(!storage::parseStoragePolicyFlag("flag", "false"),
                "false flag parsing failed");
        require(storage::parseStoragePolicyHours("hours", "168") == 168,
                "hour parsing failed");
        requireInvalid(
            [] { (void)storage::parseStoragePolicyHours("hours", "7d"); },
            "hour parsing must reject unit suffixes");

        auto unsafe = defaults;
        unsafe.compressionAfterHours = unsafe.mutableWindowHours;
        requireInvalid(
            [&unsafe] { storage::validateDeviceDataStoragePolicy(unsafe); },
            "compression must not overlap the mutable write-back window");

        unsafe.compressionEnabled = false;
        storage::validateDeviceDataStoragePolicy(unsafe);

        const auto enabled = storage::deviceDataStoragePolicyMigration(defaults);
        require(enabled.id.find("_on_chunk_168_after_168_mutable_48") !=
                    std::string::npos,
                "enabled policy identity is incomplete");
        require(enabled.sql.find("remove_compression_policy") != std::string::npos,
                "policy replacement must remove the previous job");
        require(enabled.sql.find("make_interval(hours => 168)") != std::string::npos,
                "configured intervals were not rendered");
        require(enabled.sql.find("add_compression_policy") != std::string::npos,
                "enabled policy must add the compression job");
        require(enabled.sql.find("DELETE FROM sys_schema_migrations") !=
                    std::string::npos,
                "policy changes must remain reversible to a previous setting");

        auto disabled = defaults;
        disabled.compressionEnabled = false;
        const auto disabledMigration =
            storage::deviceDataStoragePolicyMigration(disabled);
        require(disabledMigration.sql.find("remove_compression_policy") !=
                    std::string::npos,
                "disabled policy must remove the compression job");
        require(disabledMigration.sql.find("add_compression_policy") ==
                    std::string::npos,
                "disabled policy must not add a compression job");

        std::cout << "storage policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "storage policy test failed: " << error.what() << '\n';
        return 1;
    }
}
