#pragma once

#include <cstddef>
#include <string_view>
#include "service/common/message.h"
#include "service/features/collector/mc/mc.types.h"
#include "service/features/collector/fins/fins.types.h"
#include "service/features/collector/dlt645/dlt645.types.h"

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
    bool debugEnabled = false;
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
    std::string positionMode = "GUIDE";
    std::int64_t byteOffset = 0;
    bool operator==(const ElementDefinition&) const = default;
};

struct DeviceDefinition {
    std::string calculationConfig;
    bool debugEnabled = false;
    std::string id;
    std::string modelId;
    std::string code;
    std::string name;
    std::string linkId;
    std::string linkMode;
    std::string targetId;
    std::string protocol;
    std::string timezone = "+08:00";
    std::string sl651ResponseMode = "M1";
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
    mc::Connection mcConnection;
    fins::Connection finsConnection;
    dlt645::Connection dlt645Connection;
    std::int64_t readInterval = 300;
    std::string storagePolicy = "report";
    std::int64_t commandFastReadDuration = 60;
    std::int64_t commandFastReadInterval = 1;
    std::vector<ElementDefinition> elements;
    bool operator==(const DeviceDefinition&) const = default;
};

} // namespace service::collector

namespace service::collector {

// 现有 Hash 使用原始键，Redis ORM 的固定键前缀不能直接映射。
struct CollectorWorkerRecord final {
    std::size_t workerIndex{};
    std::string version;
    std::string state;
    std::int64_t appliedAtMs{};

    [[nodiscard]] std::vector<message::StreamField> fields() const {
        return {{"worker_id", std::to_string(workerIndex)}, {"version", version},
                {"state", state}, {"applied_at_ms", std::to_string(appliedAtMs)}};
    }

    static std::string key(std::size_t index, std::string_view instance) {
        return "iot:runtime:collector:" + std::string(instance) + ":" + std::to_string(index);
    }
};

// 链路快照保留事件携带的动态字段；消息 ID 和创建时间不属于存储快照。
struct CollectorLinkRecord final {
    std::vector<message::StreamField> fields;

    static CollectorLinkRecord fromEvent(const message::StreamMessage& event,
                                        std::int64_t updatedAtMs) {
        CollectorLinkRecord record;
        record.fields.reserve(event.fields.size() + 1);
        for (const auto& field : event.fields) {
            if (field.name != "message_id" && field.name != "created_at_ms")
                record.fields.push_back(field);
        }
        record.fields.push_back({"updated_at_ms", std::to_string(updatedAtMs)});
        return record;
    }

    static std::string key(std::string_view id, std::size_t index, std::string_view instance) {
        return "iot:runtime:link:" + std::string(id) + ":worker:" +
            std::string(instance) + ":" + std::to_string(index);
    }
};

struct DeviceRouteOwner final {
    std::string workerId;
    std::string connectionId;
    std::string instanceId;
};

} // namespace service::collector
