#pragma once

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/redis/RedisTypes.h>

#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"
#include "service/features/collector/types.h"

namespace service::collector::config {

namespace redis = service::message::redis;

inline constexpr std::string_view kActiveVersionKey = "iot:config:runtime:active-version";
inline constexpr std::string_view kVersionsKey = "iot:config:runtime:versions";
inline constexpr std::int64_t kSnapshotGraceMilliseconds = 120000;

inline std::string prefix(std::string_view version) {
    return "iot:config:runtime:" + std::string(version);
}

inline std::string linkKey(std::string_view version, std::string_view linkId) {
    return prefix(version) + ":link:" + std::string(linkId);
}

inline std::string targetKey(std::string_view version, std::string_view linkId,
                             std::string_view targetId) {
    return linkKey(version, linkId) + ":target:" + std::string(targetId);
}

inline std::string deviceKey(std::string_view version, std::string_view deviceId) {
    return prefix(version) + ":device:" + std::string(deviceId);
}

inline std::string metadataKey(std::string_view version) { return prefix(version) + ":metadata"; }

// A deterministic content signature makes periodic Service reconciliation cheap and
// idempotent. It is not a security hash; length-prefixed FNV-1a is sufficient to distinguish
// runtime snapshots and remains stable across process restarts.
inline std::string signature(const RuntimeSnapshot& snapshot) {
    std::uint64_t value = 14695981039346656037ULL;
    const auto byte = [&value](std::uint8_t input) {
        value ^= input;
        value *= 1099511628211ULL;
    };
    const auto number = [&byte](std::uint64_t input) {
        for (std::size_t shift = 0; shift < sizeof(input) * 8; shift += 8)
            byte(static_cast<std::uint8_t>(input >> shift));
    };
    const auto text = [&number, &byte](std::string_view input) {
        number(input.size());
        for (const auto character : input)
            byte(static_cast<std::uint8_t>(character));
    };
    const auto bytes = [&number, &byte](const std::vector<std::uint8_t>& input) {
        number(input.size());
        for (const auto character : input)
            byte(character);
    };
    const auto integer = [&number](std::int64_t input) {
        number(static_cast<std::uint64_t>(input));
    };

    number(snapshot.links.size());
    for (const auto& link : snapshot.links) {
        text(link.id);
        text(link.name);
        text(link.mode);
        text(link.protocol);
        text(link.ip);
        number(link.port);
        text(link.status);
        number(link.targets.size());
        for (const auto& target : link.targets) {
            text(target.id);
            text(target.name);
            text(target.ip);
            number(target.port);
            text(target.status);
        }
    }
    number(snapshot.devices.size());
    for (const auto& device : snapshot.devices) {
        text(device.id);
        text(device.code);
        text(device.name);
        text(device.linkId);
        text(device.linkMode);
        text(device.targetId);
        text(device.protocol);
        text(device.timezone);
        integer(device.onlineTimeout);
        text(device.heartbeatMode);
        bytes(device.heartbeatBytes);
        text(device.registrationMode);
        bytes(device.registrationBytes);
        text(device.modbusMode);
        number(device.slaveId);
        text(device.s7ConnectionMode);
        text(device.s7ConnectionType);
        integer(device.s7Rack);
        integer(device.s7Slot);
        text(device.s7LocalTsap);
        text(device.s7RemoteTsap);
        integer(device.s7HandshakeTimeoutMs);
        integer(device.s7DirectProbeTimeoutMs);
        text(device.s7ProbeMode);
        integer(device.pollInterval);
        number(device.elements.size());
        for (const auto& element : device.elements) {
            text(element.id);
            text(element.name);
            text(element.unit);
            text(element.dataType);
            text(element.byteOrder);
            text(element.registerType);
            integer(element.address);
            integer(element.quantity);
            number(std::bit_cast<std::uint64_t>(element.scale));
            integer(element.decimals);
            number(element.writable ? 1 : 0);
            text(element.area);
            integer(element.dbNumber);
            integer(element.start);
            integer(element.startBit);
            integer(element.size);
            text(element.functionCode);
            text(element.direction);
            text(element.guideHex);
            text(element.encoding);
            integer(element.length);
            integer(element.digits);
            number(element.responseElement ? 1 : 0);
        }
    }
    std::array<char, 16> output{};
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < output.size(); ++index)
        output[index] = digits[(value >> ((output.size() - index - 1) * 4)) & 0x0F];
    return {output.data(), output.size()};
}

inline std::string elementKey(std::string_view version, std::string_view deviceId,
                              std::string_view elementId) {
    return deviceKey(version, deviceId) + ":element:" + std::string(elementId);
}

namespace detail {

inline std::string stringReply(const ruvia::RedisValue& value, std::string_view operation) {
    if (value.kind() != ruvia::RedisValue::Kind::kString)
        redis::throwValue(operation, value);
    return std::string(value.string());
}

inline std::vector<std::string> stringArray(const ruvia::RedisValue& value,
                                            std::string_view operation) {
    if (value.kind() != ruvia::RedisValue::Kind::kArray)
        redis::throwValue(operation, value);
    std::vector<std::string> result;
    result.reserve(value.array().size());
    for (const auto& entry : value.array()) {
        if (entry.kind() != ruvia::RedisValue::Kind::kString)
            redis::throwValue(operation, value);
        result.emplace_back(entry.string());
    }
    return result;
}

inline std::vector<message::StreamField> hashFields(const ruvia::RedisValue& value,
                                                   std::string_view operation) {
    if (value.kind() != ruvia::RedisValue::Kind::kArray)
        redis::throwValue(operation, value);
    const auto values = value.array();
    if (values.size() % 2 != 0)
        redis::throwValue(operation, value);
    std::vector<message::StreamField> result;
    result.reserve(values.size() / 2);
    for (std::size_t index = 0; index < values.size(); index += 2) {
        if (values[index].kind() != ruvia::RedisValue::Kind::kString ||
            values[index + 1].kind() != ruvia::RedisValue::Kind::kString)
            redis::throwValue(operation, value);
        result.push_back(
            {std::string(values[index].string()), std::string(values[index + 1].string())});
    }
    return result;
}

template <typename Redis>
ruvia::Task<std::pmr::vector<ruvia::RedisValue>>
execute(const Redis& redis, const std::vector<std::vector<std::string>>& commands) {
    if (commands.empty())
        co_return std::pmr::vector<ruvia::RedisValue>{};
    if constexpr (requires { redis.pipeline(); }) {
        auto pipeline = redis.pipeline();
        for (const auto& command : commands) {
            std::vector<std::string_view> views(command.begin(), command.end());
            pipeline.command(views);
        }
        auto replies = co_await std::move(pipeline).exec();
        for (const auto& reply : replies)
            if (reply.kind() == ruvia::RedisValue::Kind::kError)
                redis::throwValue("runtime pipeline", reply);
        co_return replies;
    } else {
        static constexpr std::string_view script = R"lua(
local replies = {}
for index = 1, #ARGV, 2 do
  replies[#replies + 1] = redis.call(ARGV[index], ARGV[index + 1])
end
return replies
)lua";
        std::vector<std::string> arguments;
        arguments.reserve(commands.size() * 2);
        for (const auto& command : commands) {
            if (command.size() != 2 || (command[0] != "HGETALL" && command[0] != "SMEMBERS"))
                throw std::runtime_error("unsupported runtime batch command");
            arguments.insert(arguments.end(), command.begin(), command.end());
        }
        std::vector<std::string_view> views(arguments.begin(), arguments.end());
        const auto reply = co_await redis.eval(script, {}, views);
        if (reply.kind() != ruvia::RedisValue::Kind::kArray)
            redis::throwValue("runtime batch", reply);
        std::pmr::vector<ruvia::RedisValue> replies;
        replies.reserve(reply.array().size());
        for (const auto& value : reply.array())
            replies.push_back(value);
        co_return replies;
    }
}

template <typename Redis>
ruvia::Task<void> flush(const Redis& redis, std::vector<std::vector<std::string>>& commands) {
    if (commands.empty())
        co_return;
    (void)co_await execute(redis, commands);
    commands.clear();
}

inline void appendRememberedKey(std::vector<std::vector<std::string>>& commands,
                                std::string_view version, const std::string& key) {
    commands.push_back({"SADD", prefix(version) + ":keys", key});
}

inline void appendHash(std::vector<std::vector<std::string>>& commands, std::string_view version,
                       std::string key, const std::vector<message::StreamField>& fields) {
    std::vector<std::string> command{"HSET", key};
    command.reserve(2 + fields.size() * 2);
    for (const auto& field : fields) {
        command.push_back(field.name);
        command.push_back(field.value);
    }
    commands.push_back(std::move(command));
    appendRememberedKey(commands, version, key);
}

inline void appendId(std::vector<std::vector<std::string>>& commands, std::string_view version,
                     std::string key, std::string_view id) {
    commands.push_back({"SADD", key, std::string(id)});
    appendRememberedKey(commands, version, key);
}

inline std::string_view field(const std::vector<message::StreamField>& fields,
                              std::string_view name) noexcept {
    for (const auto& value : fields)
        if (value.name == name)
            return value.value;
    return {};
}

inline std::int64_t integer(const std::vector<message::StreamField>& fields, std::string_view name,
                            std::int64_t fallback = 0) {
    const auto value = field(fields, name);
    if (value.empty())
        return fallback;
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size())
        throw std::runtime_error("invalid runtime integer: " + std::string(name));
    return result;
}

inline double decimal(const std::vector<message::StreamField>& fields, std::string_view name,
                      double fallback = 0) {
    const auto value = field(fields, name);
    return value.empty() ? fallback : std::stod(std::string(value));
}

inline DeviceDefinition device(const std::vector<message::StreamField>& fields) {
    DeviceDefinition result;
    result.id = field(fields, "id");
    result.code = field(fields, "code");
    result.name = field(fields, "name");
    result.linkId = field(fields, "link_id");
    result.linkMode = field(fields, "link_mode");
    result.targetId = field(fields, "target_id");
    result.protocol = field(fields, "protocol");
    result.timezone = field(fields, "timezone");
    result.onlineTimeout = integer(fields, "online_timeout", 300);
    result.heartbeatMode = field(fields, "heartbeat_mode");
    result.heartbeatBytes = message::fromHex(field(fields, "heartbeat_hex"));
    result.registrationMode = field(fields, "registration_mode");
    result.registrationBytes = message::fromHex(field(fields, "registration_hex"));
    result.modbusMode = field(fields, "modbus_mode");
    result.slaveId = static_cast<std::uint8_t>(integer(fields, "slave_id", 1));
    result.modbusMergeGap = integer(fields, "modbus_merge_gap", 100);
    result.modbusMaxQuantity = integer(fields, "modbus_max_quantity", 125);
    result.s7ConnectionMode = field(fields, "s7_connection_mode");
    result.s7ConnectionType = field(fields, "s7_connection_type");
    result.s7Rack = integer(fields, "s7_rack");
    result.s7Slot = integer(fields, "s7_slot", 1);
    result.s7LocalTsap = field(fields, "s7_local_tsap");
    result.s7RemoteTsap = field(fields, "s7_remote_tsap");
    result.s7HandshakeTimeoutMs = integer(fields, "s7_handshake_timeout_ms", 5000);
    result.s7DirectProbeTimeoutMs = integer(fields, "s7_direct_probe_timeout_ms", 5000);
    result.s7ProbeMode = field(fields, "s7_probe_mode");
    if (result.s7ProbeMode.empty())
        result.s7ProbeMode = "STANDARD";
    result.pollInterval = integer(fields, "poll_interval", 5);
    result.storageInterval = integer(fields, "storage_interval", 1);
    result.commandFastReadDuration = integer(fields, "command_fast_read_duration", 60);
    result.commandFastReadInterval = integer(fields, "command_fast_read_interval", 1);
    return result;
}

inline ElementDefinition element(const std::vector<message::StreamField>& fields) {
    ElementDefinition result;
    result.configKey = field(fields, "config_key");
    result.id = field(fields, "id");
    result.name = field(fields, "name");
    result.unit = field(fields, "unit");
    result.dataType = field(fields, "data_type");
    result.byteOrder = field(fields, "byte_order");
    result.registerType = field(fields, "register_type");
    result.address = integer(fields, "address");
    result.quantity = integer(fields, "quantity");
    result.scale = decimal(fields, "scale", 1);
    result.decimals = integer(fields, "decimals", -1);
    result.writable = integer(fields, "writable") != 0;
    result.area = field(fields, "area");
    result.dbNumber = integer(fields, "db_number");
    result.start = integer(fields, "start");
    result.startBit = integer(fields, "start_bit");
    result.size = integer(fields, "size");
    result.functionCode = field(fields, "function_code");
    result.direction = field(fields, "direction");
    result.guideHex = field(fields, "guide_hex");
    result.encoding = field(fields, "encoding");
    result.length = integer(fields, "length");
    result.digits = integer(fields, "digits");
    result.responseElement = integer(fields, "response_element") != 0;
    return result;
}

template <typename Redis>
ruvia::Task<std::vector<std::string>> members(const Redis& redis, std::string key) {
    co_return stringArray(co_await redis::command(redis, {"SMEMBERS", std::move(key)}),
                          "SMEMBERS runtime");
}

template <typename Redis>
ruvia::Task<void> eraseSnapshot(const Redis& redis, std::string_view version) {
    const auto keysSet = prefix(version) + ":keys";
    auto keys = co_await members(redis, keysSet);
    keys.push_back(keysSet);
    constexpr std::size_t batchSize = 128;
    for (std::size_t offset = 0; offset < keys.size(); offset += batchSize) {
        std::vector<std::string> args{"UNLINK"};
        const auto end = std::min(keys.size(), offset + batchSize);
        args.insert(args.end(), keys.begin() + static_cast<std::ptrdiff_t>(offset),
                    keys.begin() + static_cast<std::ptrdiff_t>(end));
        (void)co_await redis::command(redis, args);
    }
}

} // namespace detail

// Service projection. The version pointer is switched only after every readable
// Hash/Set has been written, so Collector Workers never observe a partial snapshot.
template <typename Redis>
ruvia::Task<std::string> project(const Redis& redis, const RuntimeSnapshot& snapshot) {
    const auto snapshotSignature = signature(snapshot);
    const auto activeReply =
        co_await redis::command(redis, {"GET", std::string(kActiveVersionKey)});
    if (!activeReply.null()) {
        const auto active = detail::stringReply(activeReply, "GET active runtime");
        const auto signatureReply =
            co_await redis::command(redis, {"HGET", metadataKey(active), "signature"});
        if (!signatureReply.null() &&
            detail::stringReply(signatureReply, "HGET runtime signature") ==
                snapshotSignature)
            co_return active;
    }

    const auto version = message::nextMessageId();
    const auto createdAt = message::utcNowMilliseconds();
    (void)co_await redis::command(
        redis, {"ZADD", std::string(kVersionsKey), std::to_string(createdAt), version});
    const auto snapshotPrefix = prefix(version);
    const auto linksKey = snapshotPrefix + ":links";
    const auto devicesKey = snapshotPrefix + ":devices";
    std::vector<std::vector<std::string>> commands;
    commands.reserve(1024);
    constexpr std::size_t pipelineSize = 512;

    detail::appendHash(commands, version, metadataKey(version),
                       {{"version", version},
                        {"signature", snapshotSignature},
                        {"created_at_ms", std::to_string(createdAt)}});

    for (const auto& link : snapshot.links) {
        detail::appendId(commands, version, linksKey, link.id);
        detail::appendHash(commands, version, linkKey(version, link.id),
                           {{"id", link.id},
                            {"name", link.name},
                            {"mode", link.mode},
                            {"protocol", link.protocol},
                            {"ip", link.ip},
                            {"port", std::to_string(link.port)},
                            {"status", link.status}});
        const auto targetsKey = linkKey(version, link.id) + ":targets";
        for (const auto& target : link.targets) {
            detail::appendId(commands, version, targetsKey, target.id);
            detail::appendHash(commands, version, targetKey(version, link.id, target.id),
                               {{"id", target.id},
                                {"name", target.name},
                                {"ip", target.ip},
                                {"port", std::to_string(target.port)},
                                {"status", target.status}});
        }
        if (commands.size() >= pipelineSize)
            co_await detail::flush(redis, commands);
    }

    for (const auto& device : snapshot.devices) {
        detail::appendId(commands, version, devicesKey, device.id);
        detail::appendHash(
            commands, version, deviceKey(version, device.id),
            {{"id", device.id},
             {"code", device.code},
             {"name", device.name},
             {"link_id", device.linkId},
             {"link_mode", device.linkMode},
             {"target_id", device.targetId},
             {"protocol", device.protocol},
             {"timezone", device.timezone},
             {"online_timeout", std::to_string(device.onlineTimeout)},
             {"heartbeat_mode", device.heartbeatMode},
             {"heartbeat_hex", message::toHex(device.heartbeatBytes)},
             {"registration_mode", device.registrationMode},
             {"registration_hex", message::toHex(device.registrationBytes)},
             {"modbus_mode", device.modbusMode},
             {"slave_id", std::to_string(device.slaveId)},
             {"modbus_merge_gap", std::to_string(device.modbusMergeGap)},
             {"modbus_max_quantity", std::to_string(device.modbusMaxQuantity)},
             {"s7_connection_mode", device.s7ConnectionMode},
             {"s7_connection_type", device.s7ConnectionType},
             {"s7_rack", std::to_string(device.s7Rack)},
             {"s7_slot", std::to_string(device.s7Slot)},
             {"s7_local_tsap", device.s7LocalTsap},
             {"s7_remote_tsap", device.s7RemoteTsap},
             {"s7_handshake_timeout_ms", std::to_string(device.s7HandshakeTimeoutMs)},
             {"s7_direct_probe_timeout_ms", std::to_string(device.s7DirectProbeTimeoutMs)},
             {"s7_probe_mode", device.s7ProbeMode},
             {"poll_interval", std::to_string(device.pollInterval)},
             {"storage_interval", std::to_string(device.storageInterval)},
             {"command_fast_read_duration", std::to_string(device.commandFastReadDuration)},
             {"command_fast_read_interval", std::to_string(device.commandFastReadInterval)}});
        const auto elementsKey = deviceKey(version, device.id) + ":elements";
        for (const auto& element : device.elements) {
            const auto& configKey = element.configKey.empty() ? element.id : element.configKey;
            detail::appendId(commands, version, elementsKey, configKey);
            detail::appendHash(commands, version, elementKey(version, device.id, configKey),
                               {{"config_key", configKey},
                                {"id", element.id},
                                {"name", element.name},
                                {"unit", element.unit},
                                {"data_type", element.dataType},
                                {"byte_order", element.byteOrder},
                                {"register_type", element.registerType},
                                {"address", std::to_string(element.address)},
                                {"quantity", std::to_string(element.quantity)},
                                {"scale", std::to_string(element.scale)},
                                {"decimals", std::to_string(element.decimals)},
                                {"writable", element.writable ? "1" : "0"},
                                {"area", element.area},
                                {"db_number", std::to_string(element.dbNumber)},
                                {"start", std::to_string(element.start)},
                                {"start_bit", std::to_string(element.startBit)},
                                {"size", std::to_string(element.size)},
                                {"function_code", element.functionCode},
                                {"direction", element.direction},
                                {"guide_hex", element.guideHex},
                                {"encoding", element.encoding},
                                {"length", std::to_string(element.length)},
                                {"digits", std::to_string(element.digits)},
                                {"response_element", element.responseElement ? "1" : "0"}});
            if (commands.size() >= pipelineSize)
                co_await detail::flush(redis, commands);
        }
    }
    co_await detail::flush(redis, commands);

    const auto previousReply =
        co_await redis::command(redis, {"GET", std::string(kActiveVersionKey)});
    if (!previousReply.null()) {
        const auto previous = detail::stringReply(previousReply, "GET active runtime");
        if (!previous.empty() && previous != version)
            (void)co_await redis::command(redis, {"ZADD", std::string(kVersionsKey), "NX",
                                                        std::to_string(createdAt), previous});
    }
    (void)co_await redis::command(redis, {"SET", std::string(kActiveVersionKey), version});

    // A worker may have read the previous pointer just before this switch. Keep completed
    // snapshots for a bounded grace window so its multi-key load cannot be torn down midway.
    const auto expired =
        detail::stringArray(co_await redis::command(
                                redis, {"ZRANGEBYSCORE", std::string(kVersionsKey), "-inf",
                                        std::to_string(createdAt - kSnapshotGraceMilliseconds)}),
                            "ZRANGEBYSCORE runtime versions");
    for (const auto& candidate : expired) {
        if (candidate == version)
            continue;
        co_await detail::eraseSnapshot(redis, candidate);
        (void)co_await redis::command(redis, {"ZREM", std::string(kVersionsKey), candidate});
    }
    co_return version;
}

template <typename Redis> ruvia::Task<std::string> activeVersion(const Redis& redis) {
    const auto versionReply =
        co_await redis::command(redis, {"GET", std::string(kActiveVersionKey)});
    if (versionReply.null())
        throw std::runtime_error("runtime is not ready");
    co_return detail::stringReply(versionReply, "GET active runtime");
}

template <typename Redis>
ruvia::Task<RuntimeSnapshot> load(const Redis& redis, std::string version) {
    RuntimeSnapshot snapshot;

    for (const auto& linkId : co_await detail::members(redis, prefix(version) + ":links")) {
        const auto fields = co_await redis::hashEntries(redis, linkKey(version, linkId));
        LinkDefinition link;
        link.id = detail::field(fields, "id");
        link.name = detail::field(fields, "name");
        link.mode = detail::field(fields, "mode");
        link.protocol = detail::field(fields, "protocol");
        link.ip = detail::field(fields, "ip");
        link.port = static_cast<std::uint16_t>(detail::integer(fields, "port"));
        link.status = detail::field(fields, "status");
        for (const auto& targetId :
             co_await detail::members(redis, linkKey(version, linkId) + ":targets")) {
            const auto targetFields =
                co_await redis::hashEntries(redis, targetKey(version, linkId, targetId));
            LinkTargetDefinition target;
            target.id = detail::field(targetFields, "id");
            target.name = detail::field(targetFields, "name");
            target.ip = detail::field(targetFields, "ip");
            target.port = static_cast<std::uint16_t>(detail::integer(targetFields, "port"));
            target.status = detail::field(targetFields, "status");
            link.targets.push_back(std::move(target));
        }
        snapshot.links.push_back(std::move(link));
    }

    const auto deviceIds = co_await detail::members(redis, prefix(version) + ":devices");
    std::vector<std::vector<std::string>> deviceCommands;
    deviceCommands.reserve(deviceIds.size() * 2);
    for (const auto& deviceId : deviceIds) {
        deviceCommands.push_back({"HGETALL", deviceKey(version, deviceId)});
        deviceCommands.push_back({"SMEMBERS", deviceKey(version, deviceId) + ":elements"});
    }
    const auto deviceReplies = co_await detail::execute(redis, deviceCommands);
    struct ElementLoad {
        std::size_t deviceIndex;
        std::string key;
    };
    std::vector<ElementLoad> elementLoads;
    snapshot.devices.reserve(deviceIds.size());
    for (std::size_t index = 0; index < deviceIds.size(); ++index) {
        snapshot.devices.push_back(
            detail::device(detail::hashFields(deviceReplies[index * 2], "device config")));
        const auto elements =
            detail::stringArray(deviceReplies[index * 2 + 1], "device element ids");
        const auto deviceIndex = snapshot.devices.size() - 1;
        for (const auto& elementId : elements)
            elementLoads.push_back({deviceIndex, elementKey(version, deviceIds[index], elementId)});
    }

    constexpr std::size_t elementBatchSize = 512;
    for (std::size_t offset = 0; offset < elementLoads.size(); offset += elementBatchSize) {
        const auto end = std::min(elementLoads.size(), offset + elementBatchSize);
        std::vector<std::vector<std::string>> elementCommands;
        elementCommands.reserve(end - offset);
        for (std::size_t index = offset; index < end; ++index)
            elementCommands.push_back({"HGETALL", elementLoads[index].key});
        const auto elementReplies = co_await detail::execute(redis, elementCommands);
        for (std::size_t index = offset; index < end; ++index) {
            auto fields = detail::hashFields(elementReplies[index - offset], "element config");
            snapshot.devices[elementLoads[index].deviceIndex].elements.push_back(
                detail::element(fields));
        }
    }
    co_return snapshot;
}

template <typename Redis> ruvia::Task<RuntimeSnapshot> load(const Redis& redis) {
    co_return co_await load(redis, co_await activeVersion(redis));
}

} // namespace service::collector::config
