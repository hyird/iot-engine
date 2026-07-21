#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace service::southbridge {

struct LinkTargetDefinition {
    std::string id;
    std::string name;
    std::string ip;
    std::uint16_t port = 0;
    std::string status;
};

struct LinkDefinition {
    std::string id;
    std::string name;
    std::string mode;
    std::string protocol;
    std::string ip;
    std::uint16_t port = 0;
    std::string status;
    std::vector<LinkTargetDefinition> targets;
};

struct ElementDefinition {
    std::string id;
    std::string name;
    std::string unit;
    std::string dataType;
    std::string byteOrder;
    std::string registerType;
    std::int64_t address = 0;
    std::int64_t quantity = 0;
    double scale = 1.0;
    std::int64_t decimals = -1;
    std::string area;
    std::int64_t dbNumber = 0;
    std::int64_t start = 0;
    std::int64_t startBit = 0;
    std::int64_t size = 0;
    std::string functionCode;
    std::string direction;
    std::string guideHex;
    std::string encoding;
    std::int64_t length = 0;
    std::int64_t digits = 0;
};

struct DeviceDefinition {
    std::string id;
    std::string code;
    std::string name;
    std::string linkId;
    std::string linkMode;
    std::string targetId;
    std::string protocol;
    std::string timezone = "+08:00";
    std::int64_t onlineTimeout = 300;
    std::string heartbeatMode = "OFF";
    std::vector<std::uint8_t> heartbeatBytes;
    std::string registrationMode = "OFF";
    std::vector<std::uint8_t> registrationBytes;
    std::string modbusMode;
    std::uint8_t slaveId = 1;
    std::vector<ElementDefinition> elements;
};

struct DtuDefinition {
    std::string key;
    std::string linkId;
    std::string targetId;
    std::string protocol;
    std::vector<std::uint8_t> registrationBytes;
    std::vector<DeviceDefinition> devices;
};

struct RuntimeSnapshot {
    std::vector<LinkDefinition> links;
    std::vector<DeviceDefinition> devices;
};

} // namespace service::southbridge
