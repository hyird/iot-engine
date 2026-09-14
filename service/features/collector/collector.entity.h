#pragma once

#include <cstddef>
#include <string_view>
#include "service/common/message.h"

#include <cstdint>
#include <string>
#include <vector>

namespace service::collector {

// Redis 配置快照采用版本化键、动态元素字段和原子发布；Ruvia Redis ORM
// 的固定 Hash 实体不能映射此多键协议。以下类型是实际读写的存储映射。
struct LinkTargetDefinition {
    std::string id;
    std::string name;
    std::string ip;
    std::uint16_t port = 0;
    std::string status;
    bool operator==(const LinkTargetDefinition&) const = default;
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
    bool operator==(const LinkDefinition&) const = default;
};

struct ElementDefinition {
    std::string configKey;
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
    bool writable = false;
    bool responseElement = false;
    bool operator==(const ElementDefinition&) const = default;
};

struct DeviceDefinition {
    std::string id;
    std::string modelId;
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
    std::int64_t modbusMergeGap = 100;
    std::int64_t modbusMaxQuantity = 125;
    std::string s7ConnectionMode = "RACK_SLOT";
    std::string s7ConnectionType = "PG";
    std::int64_t s7Rack = 0;
    std::int64_t s7Slot = 1;
    std::string s7LocalTsap = "0100";
    std::string s7RemoteTsap = "0101";
    std::int64_t s7HandshakeTimeoutMs = 5000;
    std::int64_t s7DirectProbeTimeoutMs = 5000;
    std::string s7ProbeMode = "STANDARD";
    std::int64_t readInterval = 1;
    std::string storagePolicy = "report";
    std::int64_t commandFastReadDuration = 60;
    std::int64_t commandFastReadInterval = 1;
    std::vector<ElementDefinition> elements;
    bool operator==(const DeviceDefinition&) const = default;
};

} // namespace service::collector

namespace service::collector {

// 与在线状态、路由及版本化配置共享的 Redis 键。动态字段和带所有者校验的
// 原子更新不能由 Redis ORM 的固定 Hash 实体替代。
struct CollectorStateRecord final {
    static std::string workerKey(std::size_t index) {
        return "iot:runtime:collector:" + service::runtime::instanceId() + ":" + std::to_string(index);
    }
    static std::string linkKey(std::string_view id, std::size_t index) {
        return "iot:runtime:link:" + std::string(id) + ":worker:" +
            service::runtime::instanceId() + ":" + std::to_string(index);
    }
    static std::string connectionKey(std::string_view id) {
        return "iot:runtime:connection:" + std::string(id);
    }
};

struct DeviceRouteOwner final {
    std::string workerId;
    std::string connectionId;
    std::string instanceId;
};

} // namespace service::collector
