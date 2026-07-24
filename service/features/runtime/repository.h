#pragma once

// 基于 ruvia DbHandle 的南桥运行时配置加载。
//
// 注意：M2 增量，被协程实例化前不会完整编译。

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbHandle.h>

#include "service/common/message/contract.h"
#include "service/features/collector/types.h"

namespace service::runtime::repository {

using service::collector::DeviceDefinition;
using service::collector::ElementDefinition;
using service::collector::LinkDefinition;
using service::collector::LinkTargetDefinition;
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
template <typename Row> std::string cell(const Row& row, std::size_t column) {
    return std::string(row[column].text());
}
template <typename Row> std::int64_t cellInt(const Row& row, std::size_t column) {
    const auto value = row[column].text();
    return value.empty() ? 0 : std::stoll(std::string(value));
}
template <typename Row> bool cellBool(const Row& row, std::size_t column) {
    const auto value = row[column].text();
    return value == "t" || value == "true" || value == "1";
}
} // namespace detail

template <typename Database> inline ruvia::Task<RuntimeSnapshot> loadRuntimeSnapshot(Database& db) {
    using detail::cell;
    using detail::cellBool;
    using detail::cellInt;
    RuntimeSnapshot snapshot;

    const auto links = co_await db.query(R"sql(
SELECT id::text, name, endpoint->>'mode', protocol, COALESCE(endpoint->>'ip', ''),
       COALESCE((endpoint->>'port')::integer, 0), status
FROM link WHERE deleted_at IS NULL ORDER BY id)sql");
    for (const auto& row : links.rows()) {
        LinkDefinition link;
        link.id = cell(row, 0);
        link.name = cell(row, 1);
        link.mode = cell(row, 2);
        link.protocol = cell(row, 3);
        link.ip = cell(row, 4);
        link.port = static_cast<std::uint16_t>(cellInt(row, 5));
        link.status = cell(row, 6);
        snapshot.links.push_back(std::move(link));
    }

    const auto targets = co_await db.query(R"sql(
SELECT l.id::text, target->>'id', target->>'name', target->>'ip', target->>'port',
       COALESCE(target->>'status', 'enabled')
FROM link l
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(l.endpoint->'targets', '[]'::jsonb)) AS target
WHERE l.deleted_at IS NULL AND l.endpoint->>'mode' = 'TCP Client'
ORDER BY l.id)sql");
    for (const auto& row : targets.rows()) {
        const auto linkId = cell(row, 0);
        const auto link =
            std::find_if(snapshot.links.begin(), snapshot.links.end(),
                         [&linkId](const LinkDefinition& item) { return item.id == linkId; });
        if (link == snapshot.links.end())
            continue;
        LinkTargetDefinition target;
        target.id = cell(row, 1);
        target.name = cell(row, 2);
        target.ip = cell(row, 3);
        target.port = static_cast<std::uint16_t>(cellInt(row, 4));
        target.status = cell(row, 5);
        link->targets.push_back(std::move(target));
    }

    const auto devices = co_await db.query(R"sql(
SELECT d.id::text, d.protocol_params->>'device_code', d.name, d.link_id::text,
       l.endpoint->>'mode',
       COALESCE(d.protocol_params->>'target_id', ''), p.protocol,
       COALESCE(d.protocol_params->>'timezone', '+08:00'),
       COALESCE((d.protocol_params->>'online_timeout')::integer, 300),
       COALESCE(d.protocol_params->'heartbeat'->>'mode', 'OFF'),
       COALESCE(d.protocol_params->'heartbeat'->>'content', ''),
       COALESCE(d.protocol_params->'registration'->>'mode', 'OFF'),
       COALESCE(d.protocol_params->'registration'->>'content', ''),
       COALESCE(d.protocol_params->>'modbus_mode', ''),
       COALESCE((d.protocol_params->>'slave_id')::integer, 1),
       COALESCE(p.config->'connection'->>'mode', 'RACK_SLOT'),
       COALESCE(p.config->'connection'->>'connectionType', 'PG'),
       COALESCE(p.config->'connection'->>'rack', '0'),
       COALESCE(p.config->'connection'->>'slot', '1'),
       COALESCE(p.config->'connection'->>'localTSAP', '0100'),
       COALESCE(p.config->'connection'->>'remoteTSAP', '0101'),
       COALESCE(p.config->'connection'->>'handshakeTimeout', p.config->>'handshakeTimeout', '5000'),
       COALESCE(p.config->'connection'->>'directProbeTimeout', p.config->>'directProbeTimeout', '5000'),
       COALESCE(p.config->'connection'->>'probeMode', p.config->>'probeMode', 'STANDARD'),
       COALESCE(CASE WHEN p.protocol = 'Modbus' THEN p.config->>'readInterval'
                     ELSE p.config->>'pollInterval' END, '5'),
       COALESCE(p.config->>'storageInterval', '1'),
       COALESCE(p.config->>'commandFastReadDuration', '60'),
       COALESCE(p.config->>'commandFastReadInterval', '1'),
       COALESCE(p.config->'packet'->>'mergeGap', '100'),
       COALESCE(p.config->'packet'->>'maxQuantity', '125')
FROM device d
JOIN link l ON l.id = d.link_id AND l.deleted_at IS NULL AND l.status = 'enabled'
JOIN protocol_config p ON p.id = d.protocol_config_id
  AND p.deleted_at IS NULL AND p.enabled = TRUE
WHERE d.deleted_at IS NULL AND d.status = 'enabled'
ORDER BY d.link_id, d.id)sql");
    for (const auto& row : devices.rows()) {
        DeviceDefinition device;
        device.id = cell(row, 0);
        device.code = cell(row, 1);
        device.name = cell(row, 2);
        device.linkId = cell(row, 3);
        device.linkMode = cell(row, 4);
        device.targetId = cell(row, 5);
        device.protocol = cell(row, 6);
        device.timezone = cell(row, 7);
        device.onlineTimeout = cellInt(row, 8);
        device.heartbeatMode = cell(row, 9);
        device.heartbeatBytes = packetBytes(device.heartbeatMode, row[10].text());
        device.registrationMode = cell(row, 11);
        device.registrationBytes = packetBytes(device.registrationMode, row[12].text());
        if (device.linkMode != "TCP Server" || device.protocol == "SL651") {
            device.heartbeatMode = "OFF";
            device.heartbeatBytes.clear();
            device.registrationMode = "OFF";
            device.registrationBytes.clear();
        }
        device.modbusMode = cell(row, 13);
        device.slaveId = static_cast<std::uint8_t>(cellInt(row, 14));
        device.s7ConnectionMode = cell(row, 15);
        device.s7ConnectionType = cell(row, 16);
        device.s7Rack = cellInt(row, 17);
        device.s7Slot = cellInt(row, 18);
        device.s7LocalTsap = cell(row, 19);
        device.s7RemoteTsap = cell(row, 20);
        device.s7HandshakeTimeoutMs = cellInt(row, 21);
        device.s7DirectProbeTimeoutMs = cellInt(row, 22);
        device.s7ProbeMode = cell(row, 23);
        device.pollInterval = cellInt(row, 24);
        device.storageInterval = cellInt(row, 25);
        device.commandFastReadDuration = cellInt(row, 26);
        device.commandFastReadInterval = cellInt(row, 27);
        device.modbusMergeGap = cellInt(row, 28);
        device.modbusMaxQuantity = cellInt(row, 29);
        snapshot.devices.push_back(std::move(device));
    }

    const auto findDevice = [&snapshot](std::string_view id) -> DeviceDefinition* {
        const auto device =
            std::find_if(snapshot.devices.begin(), snapshot.devices.end(),
                         [id](const DeviceDefinition& value) { return value.id == id; });
        return device == snapshot.devices.end() ? nullptr : &*device;
    };

    const auto modbusElements = co_await db.query(R"sql(
SELECT d.id::text, element->>'id', element->>'name', COALESCE(element->>'unit', ''),
       element->>'dataType', COALESCE(element->>'byteOrder', p.config->>'byteOrder', 'BIG_ENDIAN'),
       element->>'registerType',
       element->>'address', element->>'quantity',
       COALESCE(element->>'scale', '1'), COALESCE(element->>'decimals', '-1'),
       COALESCE((element->>'writable')::boolean, FALSE)
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]'::jsonb)) element
WHERE d.deleted_at IS NULL AND d.status = 'enabled' AND p.deleted_at IS NULL AND p.enabled = TRUE
ORDER BY d.id, (element->>'address')::integer)sql");
    for (const auto& row : modbusElements.rows()) {
        auto* device = findDevice(row[0].text());
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
        element.scale = std::stod(cell(row, 9));
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
       COALESCE((element->>'writable')::boolean, FALSE)
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]'::jsonb)) element
WHERE d.deleted_at IS NULL AND d.status = 'enabled' AND p.deleted_at IS NULL AND p.enabled = TRUE
ORDER BY d.id, (element->>'start')::integer)sql");
    for (const auto& row : s7Elements.rows()) {
        auto* device = findDevice(row[0].text());
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
    for (const auto& row : sl651Elements.rows()) {
        auto* device = findDevice(row[0].text());
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
