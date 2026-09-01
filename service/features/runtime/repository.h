#pragma once

// 基于 ruvia DbHandle 的南桥运行时配置加载。
//
// 注意：M2 增量，被协程实例化前不会完整编译。

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbHandle.h>

#include "service/common/message/contract.h"
#include "service/features/collector/types.h"
#include "service/utils/number.h"

namespace service::runtime::repository {

using service::collector::DeviceDefinition;
using service::collector::ElementDefinition;
using service::collector::LinkDefinition;
using service::collector::LinkTargetDefinition;
using service::collector::RealtimeDeviceDefinition;
using service::collector::RealtimePointDefinition;
using service::collector::RuntimeSnapshot;

// DTU 注册包/心跳包内容 → 字节。
inline std::vector<std::uint8_t> packetBytes(std::string_view mode, std::string_view content) {
    if (mode == "OFF" || content.empty())
        return {};
    if (mode == "ASCII") {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(content.size());
        for (std::size_t index = 0; index < content.size(); ++index) {
            if (content[index] != '\\' || index + 1 >= content.size()) {
                bytes.push_back(static_cast<std::uint8_t>(content[index]));
                continue;
            }
            switch (content[index + 1]) {
            case 'r':
                bytes.push_back(0x0D);
                ++index;
                break;
            case 'n':
                bytes.push_back(0x0A);
                ++index;
                break;
            case 't':
                bytes.push_back(0x09);
                ++index;
                break;
            case '\\':
                bytes.push_back('\\');
                ++index;
                break;
            default:
                bytes.push_back(static_cast<std::uint8_t>(content[index]));
                break;
            }
        }
        return bytes;
    }
    if (mode != "HEX")
        throw std::runtime_error("Unsupported DTU registration mode: " + std::string(mode));
    std::string normalized;
    normalized.reserve(content.size());
    for (const auto character : content)
        if (!std::isspace(static_cast<unsigned char>(character)))
            normalized.push_back(character);
    const auto bytes = message::fromHex(normalized);
    if (bytes.empty() && !normalized.empty())
        throw std::runtime_error("Invalid HEX DTU registration content");
    return bytes;
}

namespace detail {
inline std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

inline std::int64_t integer(std::string_view value, std::int64_t fallback = 0) noexcept {
    value = trim(value);
    if (value.empty())
        return fallback;
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size() ? result : fallback;
}

inline std::int64_t integerInRange(std::string_view value, std::int64_t minimum,
                                   std::int64_t maximum, std::int64_t fallback) noexcept {
    const auto parsed = integer(value, fallback);
    return parsed < minimum || parsed > maximum ? fallback : parsed;
}

template <typename Row> std::string cell(const Row& row, std::size_t column) {
    return std::string(row[column].value().value_or(std::string_view{}));
}
template <typename Row>
std::int64_t cellInt(const Row& row, std::size_t column, std::int64_t fallback = 0) {
    return integer(row[column].value().value_or(std::string_view{}), fallback);
}
template <typename Row>
std::uint16_t cellPort(const Row& row, std::size_t column, std::uint16_t fallback = 0) {
    return static_cast<std::uint16_t>(integerInRange(
        row[column].value().value_or(std::string_view{}), 0, std::numeric_limits<std::uint16_t>::max(), fallback));
}
template <typename Row>
std::uint8_t cellUInt8(const Row& row, std::size_t column, std::uint8_t fallback = 0) {
    return static_cast<std::uint8_t>(integerInRange(
        row[column].value().value_or(std::string_view{}), 0, std::numeric_limits<std::uint8_t>::max(), fallback));
}
template <typename Row> bool cellBool(const Row& row, std::size_t column) {
    const auto value = row[column].value().value_or(std::string_view{});
    return value == "t" || value == "true" || value == "1";
}
inline double decimal(std::string_view value, std::string_view name) {
    const auto parsed = service::utils::decimal(value);
    if (!parsed)
        throw std::runtime_error("invalid runtime repository decimal: " + std::string(name));
    return *parsed;
}
} // namespace detail

template <typename Database> inline ruvia::Task<RuntimeSnapshot> loadRuntimeSnapshot(Database& db) {
    using detail::cell;
    using detail::cellBool;
    using detail::cellInt;
    using detail::decimal;
    RuntimeSnapshot snapshot;

    const auto links = co_await db.query(R"sql(
SELECT id::text, name, endpoint->>'mode', protocol, COALESCE(endpoint->>'ip', ''),
       COALESCE(NULLIF(endpoint->>'port', ''), '0'), status
FROM link
WHERE deleted_at IS NULL AND execution = 'collector'
ORDER BY id)sql");
    for (const auto& row : links) {
        LinkDefinition link;
        link.id = cell(row, 0);
        link.name = cell(row, 1);
        link.mode = cell(row, 2);
        link.protocol = cell(row, 3);
        link.ip = cell(row, 4);
        link.port = detail::cellPort(row, 5);
        link.status = cell(row, 6);
        snapshot.links.push_back(std::move(link));
    }
    std::unordered_map<std::string_view, std::size_t> linkIndexes;
    linkIndexes.reserve(snapshot.links.size());
    for (std::size_t index = 0; index < snapshot.links.size(); ++index)
        linkIndexes.emplace(snapshot.links[index].id, index);

    const auto targets = co_await db.query(R"sql(
SELECT l.id::text, target->>'id', target->>'name', target->>'ip',
       COALESCE(NULLIF(target->>'port', ''), '0'),
       COALESCE(target->>'status', 'enabled')
FROM link l
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(l.endpoint->'targets', '[]'::jsonb)) AS target
WHERE l.deleted_at IS NULL AND l.execution = 'collector'
  AND l.endpoint->>'mode' = 'TCP Client'
ORDER BY l.id)sql");
    for (const auto& row : targets) {
        const auto linkId = cell(row, 0);
        const auto link = linkIndexes.find(linkId);
        if (link == linkIndexes.end())
            continue;
        LinkTargetDefinition target;
        target.id = cell(row, 1);
        target.name = cell(row, 2);
        target.ip = cell(row, 3);
        target.port = detail::cellPort(row, 4);
        target.status = cell(row, 5);
        snapshot.links[link->second].targets.push_back(std::move(target));
    }

    const auto devices = co_await db.query(R"sql(
SELECT d.id::text, d.protocol_params->>'device_code', d.name, d.link_id::text,
       l.endpoint->>'mode',
       COALESCE(d.protocol_params->>'target_id', ''), p.protocol,
       COALESCE(d.protocol_params->>'timezone', '+08:00'),
       COALESCE(NULLIF(d.protocol_params->>'online_timeout', ''), '300'),
       COALESCE(d.protocol_params->'heartbeat'->>'mode', 'OFF'),
       COALESCE(d.protocol_params->'heartbeat'->>'content', ''),
       COALESCE(d.protocol_params->'registration'->>'mode', 'OFF'),
       COALESCE(d.protocol_params->'registration'->>'content', ''),
       COALESCE(d.protocol_params->>'modbus_mode', ''),
       COALESCE(NULLIF(d.protocol_params->>'slave_id', ''), '1'),
       COALESCE(p.config->'connection'->>'mode', 'RACK_SLOT'),
       COALESCE(p.config->'connection'->>'connectionType', 'PG'),
       COALESCE(p.config->'connection'->>'rack', '0'),
       COALESCE(p.config->'connection'->>'slot', '1'),
       COALESCE(p.config->'connection'->>'localTSAP', '0100'),
       COALESCE(p.config->'connection'->>'remoteTSAP', '0101'),
       COALESCE(p.config->'connection'->>'handshakeTimeout', p.config->>'handshakeTimeout', '5000'),
       COALESCE(p.config->'connection'->>'directProbeTimeout', p.config->>'directProbeTimeout', '5000'),
       COALESCE(p.config->'connection'->>'probeMode', p.config->>'probeMode', 'STANDARD'),
       COALESCE(NULLIF(p.config->>'readInterval', ''), '1'),
       COALESCE(p.config->>'storageInterval', '1'),
       COALESCE(p.config->>'commandFastReadDuration', '60'),
       COALESCE(p.config->>'commandFastReadInterval', '1'),
       COALESCE(p.config->'packet'->>'mergeGap', '100'),
       COALESCE(p.config->'packet'->>'maxQuantity', '125')
FROM device d
JOIN link l ON l.id = d.link_id AND l.deleted_at IS NULL
  AND l.status = 'enabled' AND l.execution = 'collector'
JOIN protocol_config p ON p.id = d.protocol_config_id
  AND p.deleted_at IS NULL AND p.enabled = TRUE
WHERE d.deleted_at IS NULL AND d.status = 'enabled'
ORDER BY d.link_id, d.id)sql");
    for (const auto& row : devices) {
        DeviceDefinition device;
        device.id = cell(row, 0);
        device.code = cell(row, 1);
        device.name = cell(row, 2);
        device.linkId = cell(row, 3);
        device.linkMode = cell(row, 4);
        device.targetId = cell(row, 5);
        device.protocol = cell(row, 6);
        device.timezone = cell(row, 7);
        device.onlineTimeout = cellInt(row, 8, 300);
        device.heartbeatMode = cell(row, 9);
        device.heartbeatBytes = packetBytes(device.heartbeatMode, row[10].value().value_or(std::string_view{}));
        device.registrationMode = cell(row, 11);
        device.registrationBytes = packetBytes(device.registrationMode, row[12].value().value_or(std::string_view{}));
        if (device.linkMode != "TCP Server" || device.protocol == "SL651") {
            device.heartbeatMode = "OFF";
            device.heartbeatBytes.clear();
            device.registrationMode = "OFF";
            device.registrationBytes.clear();
        }
        device.modbusMode = cell(row, 13);
        device.slaveId = detail::cellUInt8(row, 14, 1);
        device.s7ConnectionMode = cell(row, 15);
        device.s7ConnectionType = cell(row, 16);
        device.s7Rack = cellInt(row, 17);
        device.s7Slot = cellInt(row, 18);
        device.s7LocalTsap = cell(row, 19);
        device.s7RemoteTsap = cell(row, 20);
        device.s7HandshakeTimeoutMs = cellInt(row, 21);
        device.s7DirectProbeTimeoutMs = cellInt(row, 22);
        device.s7ProbeMode = cell(row, 23);
        device.readInterval = cellInt(row, 24);
        device.storageInterval = cellInt(row, 25);
        device.commandFastReadDuration = cellInt(row, 26);
        device.commandFastReadInterval = cellInt(row, 27);
        device.modbusMergeGap = cellInt(row, 28);
        device.modbusMaxQuantity = cellInt(row, 29);
        snapshot.devices.push_back(std::move(device));
    }
    std::unordered_map<std::string_view, std::size_t> deviceIndexes;
    deviceIndexes.reserve(snapshot.devices.size());
    for (std::size_t index = 0; index < snapshot.devices.size(); ++index)
        deviceIndexes.emplace(snapshot.devices[index].id, index);
    const auto findDevice = [&snapshot, &deviceIndexes](std::string_view id) -> DeviceDefinition* {
        const auto device = deviceIndexes.find(id);
        return device == deviceIndexes.end() ? nullptr : &snapshot.devices[device->second];
    };

    // This read model covers every non-deleted device, including edge-executed and disabled
    // devices. It replaces request-time PostgreSQL lookups in the Open Access realtime API.
    const auto realtimeRows = co_await db.query(R"sql(
WITH configured AS (
  SELECT d.id AS device_id, element,
         1 AS protocol_order, position AS function_order, 0::bigint AS element_order
  FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position)
  WHERE d.deleted_at IS NULL AND COALESCE(element->>'encode', '') <> 'JPEG'
  UNION ALL
  SELECT d.id, element, 2, position, 0::bigint
  FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position)
  WHERE d.deleted_at IS NULL AND COALESCE(element->>'encode', '') <> 'JPEG'
  UNION ALL
  SELECT d.id, element, 3, function_position, element_position
  FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]'::jsonb))
    WITH ORDINALITY AS functions(function, function_position)
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(function->'elements', '[]'::jsonb))
    WITH ORDINALITY AS elements(element, element_position)
  WHERE d.deleted_at IS NULL AND function->>'dir' = 'UP'
    AND COALESCE(element->>'encode', '') <> 'JPEG'
)
SELECT d.id::text, COALESCE(d.protocol_params->>'device_code', ''), d.name,
       configured.element->>'id',
       COALESCE(configured.element->>'name', configured.element->>'id'),
       COALESCE(configured.element->>'unit', '')
FROM device d
LEFT JOIN configured ON configured.device_id = d.id
WHERE d.deleted_at IS NULL
ORDER BY d.id, configured.protocol_order, configured.function_order,
         configured.element_order)sql");
    RealtimeDeviceDefinition* realtimeDevice = nullptr;
    for (const auto& row : realtimeRows) {
        const auto deviceId = cell(row, 0);
        if (!realtimeDevice || realtimeDevice->id != deviceId) {
            RealtimeDeviceDefinition device;
            device.id = deviceId;
            device.code = cell(row, 1);
            device.name = cell(row, 2);
            snapshot.realtimeDevices.push_back(std::move(device));
            realtimeDevice = &snapshot.realtimeDevices.back();
        }
        if (row[3].value().has_value())
            realtimeDevice->points.push_back(
                RealtimePointDefinition{cell(row, 3), cell(row, 4), cell(row, 5)});
    }

    const auto modbusElements = co_await db.query(R"sql(
SELECT d.id::text, element->>'id', element->>'name', COALESCE(element->>'unit', ''),
       element->>'dataType', COALESCE(element->>'byteOrder', p.config->>'byteOrder', 'BIG_ENDIAN'),
       element->>'registerType',
       element->>'address', element->>'quantity',
       COALESCE(element->>'scale', '1'), COALESCE(element->>'decimals', '-1'),
       CASE lower(COALESCE(element->>'writable', 'false'))
         WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE ELSE FALSE END
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]'::jsonb)) element
WHERE d.deleted_at IS NULL AND d.status = 'enabled' AND p.deleted_at IS NULL AND p.enabled = TRUE
ORDER BY d.id,
         CASE WHEN COALESCE(element->>'address', '') ~ '^-?[0-9]{1,18}$'
              THEN (element->>'address')::bigint ELSE 0 END)sql");
    for (const auto& row : modbusElements) {
        auto* device = findDevice(row[0].value().value_or(std::string_view{}));
        if (!device)
            continue;
        ElementDefinition element;
        element.configKey = "element:" + cell(row, 1);
        element.id = cell(row, 1);
        element.name = cell(row, 2);
        element.unit = cell(row, 3);
        element.dataType = cell(row, 4);
        element.byteOrder = cell(row, 5);
        element.registerType = cell(row, 6);
        element.address = cellInt(row, 7);
        element.quantity = cellInt(row, 8);
        element.scale = decimal(row[9].value().value_or(std::string_view{}), "scale");
        element.decimals = cellInt(row, 10);
        element.writable = cellBool(row, 11);
        device->elements.push_back(std::move(element));
    }

    const auto s7Elements = co_await db.query(R"sql(
SELECT d.id::text, element->>'id', element->>'name', COALESCE(element->>'unit', ''),
       COALESCE(element->>'dataType', 'UINT8'), element->>'area',
       COALESCE(element->>'dbNumber', '0'), element->>'start',
       COALESCE(element->>'startBit', '0'), element->>'size',
       COALESCE(element->>'decimals', '-1'),
       CASE lower(COALESCE(element->>'writable', 'false'))
         WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE ELSE FALSE END
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]'::jsonb)) element
WHERE d.deleted_at IS NULL AND d.status = 'enabled' AND p.deleted_at IS NULL AND p.enabled = TRUE
ORDER BY d.id,
         CASE WHEN COALESCE(element->>'start', '') ~ '^-?[0-9]{1,18}$'
              THEN (element->>'start')::bigint ELSE 0 END)sql");
    for (const auto& row : s7Elements) {
        auto* device = findDevice(row[0].value().value_or(std::string_view{}));
        if (!device)
            continue;
        ElementDefinition element;
        element.configKey = "element:" + cell(row, 1);
        element.id = cell(row, 1);
        element.name = cell(row, 2);
        element.unit = cell(row, 3);
        element.dataType = cell(row, 4);
        element.area = cell(row, 5);
        element.dbNumber = cellInt(row, 6);
        element.start = cellInt(row, 7);
        element.startBit = cellInt(row, 8);
        element.size = cellInt(row, 9);
        element.decimals = cellInt(row, 10);
        element.writable = cellBool(row, 11);
        device->elements.push_back(std::move(element));
    }

    const auto sl651Elements = co_await db.query(R"sql(
SELECT d.id::text, configured.element->>'id', configured.element->>'name',
       COALESCE(configured.element->>'unit', ''), func->>'funcCode', func->>'dir',
       configured.element->>'guideHex', configured.element->>'encode',
       configured.element->>'length', COALESCE(configured.element->>'digits', '0'),
       configured.response_element
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]'::jsonb)) func
CROSS JOIN LATERAL (
    SELECT element, FALSE AS response_element
    FROM jsonb_array_elements(COALESCE(func->'elements', '[]'::jsonb)) element
    UNION ALL
    SELECT element, TRUE AS response_element
    FROM jsonb_array_elements(COALESCE(func->'responseElements', '[]'::jsonb)) element
) configured
WHERE d.deleted_at IS NULL AND d.status = 'enabled' AND p.deleted_at IS NULL AND p.enabled = TRUE
ORDER BY d.id, func->>'funcCode', configured.response_element, configured.element->>'id')sql");
    for (const auto& row : sl651Elements) {
        auto* device = findDevice(row[0].value().value_or(std::string_view{}));
        if (!device)
            continue;
        ElementDefinition element;
        element.configKey = (cellBool(row, 10) ? "response:" : "element:") + cell(row, 1);
        element.id = cell(row, 1);
        element.name = cell(row, 2);
        element.unit = cell(row, 3);
        element.functionCode = cell(row, 4);
        element.direction = cell(row, 5);
        element.guideHex = cell(row, 6);
        element.encoding = cell(row, 7);
        element.length = cellInt(row, 8);
        element.digits = cellInt(row, 9);
        element.responseElement = cellBool(row, 10);
        device->elements.push_back(std::move(element));
    }

    co_return snapshot;
}

} // namespace service::runtime::repository
