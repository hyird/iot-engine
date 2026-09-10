#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class DeviceChange {
    Status,
    Catalog,
    Records,
    Mapping,
    DeviceName,
    ChannelName,
};

struct Channel {
    std::string id;
    std::string name;
    std::string customName;
    std::string manufacturer;
    bool online{ false };
    int ptzType{ -1 };

    [[nodiscard]] std::string_view displayName() const noexcept {
        return customName.empty() ? std::string_view{ name }
                                  : std::string_view{ customName };
    }
};

struct RecordItem {
    std::string deviceId;
    std::string name;
    std::string filePath;
    std::string address;
    std::string startTime;
    std::string endTime;
    std::string type;
    std::string recorderId;
};

struct Device {
    std::string id;
    std::string name;
    std::string customName;
    std::string manufacturer;
    std::string remoteAddress;
    std::string registrationSource{ "sip" };
    std::string mappedDeviceId;
    bool online{ false };
    std::chrono::system_clock::time_point lastSeen{};
    std::vector<Channel> channels;
    std::vector<RecordItem> records;
    // A registration generation fences delayed events from a previous SIP
    // session on the same Collector.  It is combined with the process
    // incarnation and Collector index in the Redis owner token.
    std::uint64_t sessionGeneration{ 0 };

    [[nodiscard]] std::string_view displayName() const noexcept {
        return customName.empty() ? std::string_view{ name }
                                  : std::string_view{ customName };
    }
};
