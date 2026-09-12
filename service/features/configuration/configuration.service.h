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
#include <ruvia/web/db/DbQuery.h>

#include "service/common/message.h"
#include "service/features/collector/collector.types.h"
#include "service/features/collector/collector.service.h"
#include "service/features/live/live.service.h"
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

template <typename Database> ruvia::Task<RuntimeSnapshot> loadRuntimeSnapshot(Database& db) {
    using detail::cell;
    using detail::cellBool;
    using detail::cellInt;
    using detail::decimal;
    RuntimeSnapshot snapshot;

    using Op = ruvia::DbBinaryOperator;
    using Type = ruvia::DbDataType;
    ruvia::DbQuery linkQuery;
    const auto endpointText = [&](std::string_view key) {
        return linkQuery.binary(linkQuery.column("endpoint"), Op::kJsonGetText, linkQuery.value(key));
    };
    linkQuery.select({ linkQuery.cast(linkQuery.column("id"), Type::kText), linkQuery.column("name"),
            endpointText("mode"), linkQuery.column("protocol"),
            linkQuery.coalesce({ endpointText("ip"), linkQuery.value("") }),
            linkQuery.coalesce({ linkQuery.nullIf(endpointText("port"), linkQuery.value("")), linkQuery.value("0") }),
            linkQuery.column("status") })
        .from("link")
        .andWhere(linkQuery.unary(ruvia::DbUnaryOperator::kIsNull, linkQuery.column("deleted_at")))
        .andWhere(linkQuery.binary(linkQuery.column("execution"), Op::kEqual, linkQuery.value("collector")))
        .addOrderBy(linkQuery.column("id"));
    const auto links = co_await db.query(linkQuery);
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

    ruvia::DbQuery targetQuery;
    const auto targetText = [&](std::string_view key) {
        return targetQuery.binary(targetQuery.column("target"), Op::kJsonGetText, targetQuery.value(key));
    };
    targetQuery.select({ targetQuery.cast(targetQuery.column("id", "l"), Type::kText),
            targetText("id"), targetText("name"), targetText("ip"),
            targetQuery.coalesce({ targetQuery.nullIf(targetText("port"), targetQuery.value("")), targetQuery.value("0") }),
            targetQuery.coalesce({ targetText("status"), targetQuery.value("enabled") }) })
        .from("link", "l")
        .joinFunction(ruvia::DbJoinType::kCross, targetQuery.call("jsonb_array_elements", {
            targetQuery.coalesce({ targetQuery.binary(targetQuery.column("endpoint", "l"), Op::kJsonGet, targetQuery.value("targets")),
                targetQuery.cast(targetQuery.value("[]"), Type::kJsonb) }) }), {}, "target", { .lateral = true })
        .andWhere(targetQuery.unary(ruvia::DbUnaryOperator::kIsNull, targetQuery.column("deleted_at", "l")))
        .andWhere(targetQuery.binary(targetQuery.column("execution", "l"), Op::kEqual, targetQuery.value("collector")))
        .andWhere(targetQuery.binary(targetQuery.binary(targetQuery.column("endpoint", "l"), Op::kJsonGetText, targetQuery.value("mode")),
            Op::kEqual, targetQuery.value("TCP Client")))
        .addOrderBy(targetQuery.column("id", "l"));
    const auto targets = co_await db.query(targetQuery);
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

    ruvia::DbQuery deviceQuery;
    const auto parameters = deviceQuery.column("protocol_params", "d");
    const auto config = deviceQuery.column("config", "p");
    const auto jsonText = [&](ruvia::DbExpression object, std::string_view key) {
        return deviceQuery.binary(object, Op::kJsonGetText, deviceQuery.value(key));
    };
    const auto defaultText = [&](ruvia::DbExpression object, std::string_view key, std::string_view fallback) {
        return deviceQuery.coalesce({ jsonText(object, key), deviceQuery.value(fallback) });
    };
    const auto heartbeat = deviceQuery.binary(parameters, Op::kJsonGet, deviceQuery.value("heartbeat"));
    const auto registration = deviceQuery.binary(parameters, Op::kJsonGet, deviceQuery.value("registration"));
    const auto connection = deviceQuery.binary(config, Op::kJsonGet, deviceQuery.value("connection"));
    const auto packet = deviceQuery.binary(config, Op::kJsonGet, deviceQuery.value("packet"));
    deviceQuery.select({ deviceQuery.cast(deviceQuery.column("id", "d"), Type::kText),
            jsonText(parameters, "device_code"), deviceQuery.column("name", "d"),
            deviceQuery.cast(deviceQuery.column("link_id", "d"), Type::kText),
            jsonText(deviceQuery.column("endpoint", "l"), "mode"),
            defaultText(parameters, "target_id", ""), deviceQuery.column("protocol", "p"),
            defaultText(parameters, "timezone", "+08:00"),
            deviceQuery.coalesce({ deviceQuery.nullIf(jsonText(parameters, "online_timeout"), deviceQuery.value("")), deviceQuery.value("300") }),
            defaultText(heartbeat, "mode", "OFF"), defaultText(heartbeat, "content", ""),
            defaultText(registration, "mode", "OFF"), defaultText(registration, "content", ""),
            defaultText(parameters, "modbus_mode", ""),
            deviceQuery.coalesce({ deviceQuery.nullIf(jsonText(parameters, "slave_id"), deviceQuery.value("")), deviceQuery.value("1") }),
            defaultText(connection, "mode", "RACK_SLOT"), defaultText(connection, "connectionType", "PG"),
            defaultText(connection, "rack", "0"), defaultText(connection, "slot", "1"),
            defaultText(connection, "localTSAP", "0100"), defaultText(connection, "remoteTSAP", "0101"),
            deviceQuery.coalesce({ jsonText(connection, "handshakeTimeout"), jsonText(config, "handshakeTimeout"), deviceQuery.value("5000") }),
            deviceQuery.coalesce({ jsonText(connection, "directProbeTimeout"), jsonText(config, "directProbeTimeout"), deviceQuery.value("5000") }),
            deviceQuery.coalesce({ jsonText(connection, "probeMode"), jsonText(config, "probeMode"), deviceQuery.value("STANDARD") }),
            deviceQuery.coalesce({ deviceQuery.nullIf(jsonText(config, "readInterval"), deviceQuery.value("")), deviceQuery.value("1") }),
            defaultText(config, "storagePolicy", "report"), defaultText(config, "commandFastReadDuration", "60"),
            defaultText(config, "commandFastReadInterval", "1"), defaultText(packet, "mergeGap", "100"),
            defaultText(packet, "maxQuantity", "125"), deviceQuery.cast(deviceQuery.column("id", "p"), Type::kText), deviceQuery.column("revision", "p") })
        .from("device", "d")
        .join(ruvia::DbJoinType::kInner, "link",
            deviceQuery.binary(deviceQuery.column("id", "l"), Op::kEqual, deviceQuery.column("link_id", "d")), "l")
        .join(ruvia::DbJoinType::kInner, "device_model",
            deviceQuery.binary(deviceQuery.column("device_id", "p"), Op::kEqual, deviceQuery.column("id", "d")), "p")
        .andWhere(deviceQuery.unary(ruvia::DbUnaryOperator::kIsNull, deviceQuery.column("deleted_at", "l")))
        .andWhere(deviceQuery.binary(deviceQuery.column("status", "l"), Op::kEqual, deviceQuery.value("enabled")))
        .andWhere(deviceQuery.binary(deviceQuery.column("execution", "l"), Op::kEqual, deviceQuery.value("collector")))
        .andWhere(deviceQuery.unary(ruvia::DbUnaryOperator::kIsNull, deviceQuery.column("deleted_at", "p")))
        .andWhere(deviceQuery.binary(deviceQuery.column("enabled", "p"), Op::kEqual, deviceQuery.value(true)))
        .andWhere(deviceQuery.unary(ruvia::DbUnaryOperator::kIsNull, deviceQuery.column("deleted_at", "d")))
        .andWhere(deviceQuery.binary(deviceQuery.column("status", "d"), Op::kEqual, deviceQuery.value("enabled")))
        .addOrderBy(deviceQuery.column("link_id", "d")).addOrderBy(deviceQuery.column("id", "d"));
    const auto devices = co_await db.query(deviceQuery);
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
        device.storagePolicy = cell(row, 25);
        device.commandFastReadDuration = cellInt(row, 26);
        device.commandFastReadInterval = cellInt(row, 27);
        device.modbusMergeGap = cellInt(row, 28);
        device.modbusMaxQuantity = cellInt(row, 29);
        device.modelId = cell(row, 30);
        device.modelRevision = cellInt(row, 31);
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
    const auto configuredProtocol = [](std::string_view protocol, std::string_view arrayKey, int order) {
        ruvia::DbQuery query;
        query.from("device", "d")
            .join(ruvia::DbJoinType::kInner, "device_model",
                query.binary(query.column("device_id", "p"), Op::kEqual, query.column("id", "d")), "p")
            .andWhere(query.binary(query.column("protocol", "p"), Op::kEqual, query.value(protocol)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at", "d")));
        const auto entries = query.call("jsonb_array_elements", { query.coalesce({
            query.binary(query.column("config", "p"), Op::kJsonGet, query.value(arrayKey)), query.cast(query.value("[]"), Type::kJsonb) }) });
        if (protocol == "SL651") {
            query.joinFunction(ruvia::DbJoinType::kCross, entries, {}, "functions",
                { .lateral = true, .withOrdinality = true, .columns = { { .name = "function" }, { .name = "function_position" } } })
                .joinFunction(ruvia::DbJoinType::kCross, query.call("jsonb_array_elements", { query.coalesce({
                    query.binary(query.column("function"), Op::kJsonGet, query.value("elements")), query.cast(query.value("[]"), Type::kJsonb) }) }),
                    {}, "elements", { .lateral = true, .withOrdinality = true, .columns = { { .name = "element" }, { .name = "element_position" } } })
                .andWhere(query.binary(query.binary(query.column("function"), Op::kJsonGetText, query.value("dir")), Op::kEqual, query.value("UP")))
                .select({ query.column("id", "d"), query.column("element"), query.cast(query.value(order), Type::kInteger), query.column("function_position"), query.column("element_position") });
        } else {
            query.joinFunction(ruvia::DbJoinType::kCross, entries, {}, "entry",
                { .lateral = true, .withOrdinality = true, .columns = { { .name = "element" }, { .name = "position" } } })
                .select({ query.column("id", "d"), query.column("element"), query.cast(query.value(order), Type::kInteger), query.column("position"), query.cast(query.value(0), Type::kBigInt) });
        }
        query.andWhere(query.binary(query.coalesce({ query.binary(query.column("element"), Op::kJsonGetText, query.value("encode")), query.value("") }),
            Op::kNotEqual, query.value("JPEG")));
        return query;
    };
    auto configured = configuredProtocol("Modbus", "registers", 1);
    const auto s7Configured = configuredProtocol("S7", "areas", 2);
    const auto sl651Configured = configuredProtocol("SL651", "funcs", 3);
    configured.combine(ruvia::DbSetOperation::kUnionAll, s7Configured).combine(ruvia::DbSetOperation::kUnionAll, sl651Configured);
    ruvia::DbQuery realtime;
    const auto realtimeText = [&](std::string_view key) {
        return realtime.binary(realtime.column("element", "configured"), Op::kJsonGetText, realtime.value(key));
    };
    realtime.with("configured", configured, { .columns = { "device_id", "element", "protocol_order", "function_order", "element_order" } })
        .select({ realtime.cast(realtime.column("id", "d"), Type::kText),
            realtime.coalesce({ realtime.binary(realtime.column("protocol_params", "d"), Op::kJsonGetText, realtime.value("device_code")), realtime.value("") }),
            realtime.column("name", "d"), realtimeText("id"), realtime.coalesce({ realtimeText("name"), realtimeText("id") }),
            realtime.coalesce({ realtimeText("unit"), realtime.value("") }) })
        .from("device", "d")
        .join(ruvia::DbJoinType::kLeft, "configured", realtime.binary(realtime.column("device_id", "configured"), Op::kEqual, realtime.column("id", "d")))
        .andWhere(realtime.unary(ruvia::DbUnaryOperator::kIsNull, realtime.column("deleted_at", "d")))
        .addOrderBy(realtime.column("id", "d")).addOrderBy(realtime.column("protocol_order", "configured"))
        .addOrderBy(realtime.column("function_order", "configured")).addOrderBy(realtime.column("element_order", "configured"));
    const auto realtimeRows = co_await db.query(realtime);
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

    const auto protocolElements = [](std::string_view protocol, std::string_view arrayKey, std::string_view alias) {
        ruvia::DbQuery query;
        query.from("device", "d")
            .join(ruvia::DbJoinType::kInner, "device_model", query.binary(query.column("device_id", "p"), Op::kEqual, query.column("id", "d")), "p")
            .joinFunction(ruvia::DbJoinType::kCross, query.call("jsonb_array_elements", { query.coalesce({
                query.binary(query.column("config", "p"), Op::kJsonGet, query.value(arrayKey)), query.cast(query.value("[]"), Type::kJsonb) }) }),
                {}, alias, { .lateral = true })
            .andWhere(query.binary(query.column("protocol", "p"), Op::kEqual, query.value(protocol)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at", "d")))
            .andWhere(query.binary(query.column("status", "d"), Op::kEqual, query.value("enabled")))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at", "p")))
            .andWhere(query.binary(query.column("enabled", "p"), Op::kEqual, query.value(true)));
        return query;
    };
    const auto elementText = [](ruvia::DbQuery& query, std::string_view key) {
        return query.binary(query.column("element"), Op::kJsonGetText, query.value(key));
    };
    const auto elementDefault = [&](ruvia::DbQuery& query, std::string_view key, std::string_view fallback) {
        return query.coalesce({ elementText(query, key), query.value(fallback) });
    };
    const auto writableElement = [&](ruvia::DbQuery& query) {
        return query.binary(query.call("lower", { elementDefault(query, "writable", "false") }), Op::kIn,
            query.list({ query.value("true"), query.value("t"), query.value("1") }));
    };
    const auto orderElementAddress = [&](ruvia::DbQuery& query, std::string_view key) {
        query.addOrderBy(query.column("id", "d")).addOrderBy(query.caseWhen({ {
            query.binary(elementDefault(query, key, ""), Op::kRegex, query.value("^-?[0-9]{1,18}$")),
            query.cast(elementText(query, key), Type::kBigInt) } }, query.value(0)));
    };
    auto modbusQuery = protocolElements("Modbus", "registers", "element");
    modbusQuery.select({ modbusQuery.cast(modbusQuery.column("id", "d"), Type::kText),
        elementText(modbusQuery, "id"), elementText(modbusQuery, "name"), elementDefault(modbusQuery, "unit", ""),
        elementText(modbusQuery, "dataType"), modbusQuery.coalesce({ elementText(modbusQuery, "byteOrder"),
            modbusQuery.binary(modbusQuery.column("config", "p"), Op::kJsonGetText, modbusQuery.value("byteOrder")), modbusQuery.value("BIG_ENDIAN") }),
        elementText(modbusQuery, "registerType"), elementText(modbusQuery, "address"), elementText(modbusQuery, "quantity"),
        elementDefault(modbusQuery, "scale", "1"), elementDefault(modbusQuery, "decimals", "-1"), writableElement(modbusQuery) });
    orderElementAddress(modbusQuery, "address");
    const auto modbusElements = co_await db.query(modbusQuery);
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

    auto s7Query = protocolElements("S7", "areas", "element");
    s7Query.select({ s7Query.cast(s7Query.column("id", "d"), Type::kText), elementText(s7Query, "id"), elementText(s7Query, "name"),
        elementDefault(s7Query, "unit", ""), elementDefault(s7Query, "dataType", "UINT8"), elementText(s7Query, "area"),
        elementDefault(s7Query, "dbNumber", "0"), elementText(s7Query, "start"), elementDefault(s7Query, "startBit", "0"),
        elementText(s7Query, "size"), elementDefault(s7Query, "decimals", "-1"), writableElement(s7Query) });
    orderElementAddress(s7Query, "start");
    const auto s7Elements = co_await db.query(s7Query);
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

    const auto sl651Fields = [](std::string_view key, bool response) {
        ruvia::DbQuery query;
        query.select({ query.column("element"), query.alias(query.cast(query.value(response), Type::kBoolean), "response_element") })
            .fromFunction(query.call("jsonb_array_elements", { query.coalesce({
                query.binary(query.column("func"), Op::kJsonGet, query.value(key)), query.cast(query.value("[]"), Type::kJsonb) }) }), "element");
        return query;
    };
    auto fields = sl651Fields("elements", false);
    const auto responseFields = sl651Fields("responseElements", true);
    fields.combine(ruvia::DbSetOperation::kUnionAll, responseFields);
    auto sl651Query = protocolElements("SL651", "funcs", "func");
    const auto sl651Text = [&](std::string_view key) {
        return sl651Query.binary(sl651Query.column("element", "configured"), Op::kJsonGetText, sl651Query.value(key));
    };
    const auto functionCode = sl651Query.binary(sl651Query.column("func"), Op::kJsonGetText, sl651Query.value("funcCode"));
    sl651Query.join(ruvia::DbJoinType::kCross, fields, {}, "configured", { .lateral = true })
        .select({ sl651Query.cast(sl651Query.column("id", "d"), Type::kText), sl651Text("id"), sl651Text("name"),
            sl651Query.coalesce({ sl651Text("unit"), sl651Query.value("") }), functionCode,
            sl651Query.binary(sl651Query.column("func"), Op::kJsonGetText, sl651Query.value("dir")),
            sl651Text("guideHex"), sl651Text("encode"), sl651Text("length"),
            sl651Query.coalesce({ sl651Text("digits"), sl651Query.value("0") }), sl651Query.column("response_element", "configured") })
        .addOrderBy(sl651Query.column("id", "d")).addOrderBy(functionCode)
        .addOrderBy(sl651Query.column("response_element", "configured")).addOrderBy(sl651Text("id"));
    const auto sl651Elements = co_await db.query(sl651Query);
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

namespace service::runtime {

class ConfigurationService final {
  public:
    template <typename Context>
    static ruvia::Task<std::string> project(Context& context, bool notify = false) {
        auto transaction = co_await context.db().beginTransaction();
        // Every Service Worker shares this transaction-scoped lock. The snapshot is loaded only
        // after earlier projections finish, so a slower request can never overwrite a newer DB
        // state.
        ruvia::DbQuery lock;
        lock.select(lock.call("pg_advisory_xact_lock", {
            lock.cast(lock.value(std::int64_t{5282804697543808067}), ruvia::DbDataType::kBigInt) }));
        (void)co_await transaction.query(lock);
        auto snapshot =
            co_await service::runtime::repository::loadRuntimeSnapshot(transaction);
        auto version =
            co_await service::collector::config::project(context.redis(), snapshot);
        if (notify)
            co_await service::live::publish(context.redis(), "runtime-config");
        co_await transaction.commit();
        co_return version;
    }
};

} // namespace service::runtime
