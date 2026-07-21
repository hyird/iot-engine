#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <libpq-fe.h>

#include "service/common/bridge/message.contract.h"
#include "service/modules/southbridge/queue/redis_stream.h"
#include "service/modules/southbridge/runtime.types.h"

namespace service::southbridge {

struct PostgresEndpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 5432;
    std::string username;
    std::string password;
    std::string database;
    std::chrono::seconds connectTimeout{5};
};

struct SouthbridgeConfig {
    PostgresEndpoint postgres;
    bridge::RedisEndpoint redis;
};

class RuntimeConfigRepository {
  public:
    explicit RuntimeConfigRepository(PostgresEndpoint endpoint) : endpoint_(std::move(endpoint)) {}

    [[nodiscard]] RuntimeSnapshot load() {
        ensureConnected();
        RuntimeSnapshot snapshot;
        const auto links = query(connection_.get(), R"sql(
SELECT id, name, mode, protocol, COALESCE(ip, ''), port, status
FROM iot_link
WHERE deleted_at IS NULL
ORDER BY id)sql");
        for (int row = 0; row < PQntuples(links.get()); ++row) {
            LinkDefinition link;
            link.id = text(links.get(), row, 0);
            link.name = text(links.get(), row, 1);
            link.mode = text(links.get(), row, 2);
            link.protocol = text(links.get(), row, 3);
            link.ip = text(links.get(), row, 4);
            link.port = static_cast<std::uint16_t>(integer(links.get(), row, 5));
            link.status = text(links.get(), row, 6);
            snapshot.links.push_back(std::move(link));
        }

        const auto targets = query(connection_.get(), R"sql(
SELECT l.id, target->>'id', target->>'name', target->>'ip', target->>'port',
       COALESCE(target->>'status', 'enabled')
FROM iot_link l
CROSS JOIN LATERAL jsonb_array_elements(l.targets) AS target
WHERE l.deleted_at IS NULL AND l.mode = 'TCP Client'
ORDER BY l.id)sql");
        for (int row = 0; row < PQntuples(targets.get()); ++row) {
            const auto linkId = text(targets.get(), row, 0);
            const auto link =
                std::find_if(snapshot.links.begin(), snapshot.links.end(),
                             [linkId](const LinkDefinition& item) { return item.id == linkId; });
            if (link == snapshot.links.end())
                continue;
            LinkTargetDefinition target;
            target.id = text(targets.get(), row, 1);
            target.name = text(targets.get(), row, 2);
            target.ip = text(targets.get(), row, 3);
            target.port = static_cast<std::uint16_t>(integer(targets.get(), row, 4));
            target.status = text(targets.get(), row, 5);
            link->targets.push_back(std::move(target));
        }

        const auto devices = query(connection_.get(), R"sql(
SELECT d.id, d.device_code, d.name, d.link_id, l.mode, COALESCE(d.target_id, ''),
       p.protocol, d.timezone, d.online_timeout,
       COALESCE(d.heartbeat->>'mode', 'OFF'), COALESCE(d.heartbeat->>'content', ''),
       COALESCE(d.registration->>'mode', 'OFF'),
       COALESCE(d.registration->>'content', ''), COALESCE(d.modbus_mode, ''),
       COALESCE(d.slave_id, 1)
FROM iot_device d
JOIN iot_link l ON l.id = d.link_id AND l.deleted_at IS NULL AND l.status = 'enabled'
JOIN iot_protocol_config p ON p.id = d.protocol_config_id
  AND p.deleted_at IS NULL AND p.enabled = TRUE
WHERE d.deleted_at IS NULL AND d.status = 'enabled'
ORDER BY d.link_id, d.id)sql");
        for (int row = 0; row < PQntuples(devices.get()); ++row) {
            DeviceDefinition device;
            device.id = text(devices.get(), row, 0);
            device.code = text(devices.get(), row, 1);
            device.name = text(devices.get(), row, 2);
            device.linkId = text(devices.get(), row, 3);
            device.linkMode = text(devices.get(), row, 4);
            device.targetId = text(devices.get(), row, 5);
            device.protocol = text(devices.get(), row, 6);
            device.timezone = text(devices.get(), row, 7);
            device.onlineTimeout = integer(devices.get(), row, 8);
            device.heartbeatMode = text(devices.get(), row, 9);
            device.heartbeatBytes = packetBytes(device.heartbeatMode, text(devices.get(), row, 10));
            device.registrationMode = text(devices.get(), row, 11);
            device.registrationBytes =
                registrationBytes(device.registrationMode, text(devices.get(), row, 12));
            if (device.linkMode != "TCP Server" || device.protocol == "SL651") {
                device.heartbeatMode = "OFF";
                device.heartbeatBytes.clear();
                device.registrationMode = "OFF";
                device.registrationBytes.clear();
            }
            device.modbusMode = text(devices.get(), row, 13);
            device.slaveId = static_cast<std::uint8_t>(integer(devices.get(), row, 14));
            snapshot.devices.push_back(std::move(device));
        }

        const auto findDevice = [&snapshot](std::string_view id) -> DeviceDefinition* {
            const auto device =
                std::find_if(snapshot.devices.begin(), snapshot.devices.end(),
                             [id](const DeviceDefinition& value) { return value.id == id; });
            return device == snapshot.devices.end() ? nullptr : &*device;
        };
        const auto modbusElements = query(connection_.get(), R"sql(
SELECT d.id, element->>'id', element->>'name', COALESCE(element->>'unit', ''),
       element->>'dataType', COALESCE(element->>'byteOrder', p.config->>'byteOrder', 'BIG_ENDIAN'),
       element->>'registerType', element->>'address', element->>'quantity',
       COALESCE(element->>'scale', '1'), COALESCE(element->>'decimals', '-1')
FROM iot_device d
JOIN iot_protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]'::jsonb)) element
WHERE d.deleted_at IS NULL AND d.status = 'enabled' AND p.deleted_at IS NULL AND p.enabled = TRUE
ORDER BY d.id, (element->>'address')::integer)sql");
        for (int row = 0; row < PQntuples(modbusElements.get()); ++row) {
            auto* device = findDevice(text(modbusElements.get(), row, 0));
            if (!device)
                continue;
            ElementDefinition element;
            element.id = text(modbusElements.get(), row, 1);
            element.name = text(modbusElements.get(), row, 2);
            element.unit = text(modbusElements.get(), row, 3);
            element.dataType = text(modbusElements.get(), row, 4);
            element.byteOrder = text(modbusElements.get(), row, 5);
            element.registerType = text(modbusElements.get(), row, 6);
            element.address = integer(modbusElements.get(), row, 7);
            element.quantity = integer(modbusElements.get(), row, 8);
            element.scale = std::stod(text(modbusElements.get(), row, 9));
            element.decimals = integer(modbusElements.get(), row, 10);
            device->elements.push_back(std::move(element));
        }

        const auto s7Elements = query(connection_.get(), R"sql(
SELECT d.id, element->>'id', element->>'name', COALESCE(element->>'unit', ''),
       COALESCE(element->>'dataType', 'UINT8'), element->>'area',
       COALESCE(element->>'dbNumber', '0'), element->>'start',
       COALESCE(element->>'startBit', '0'), element->>'size',
       COALESCE(element->>'decimals', '-1')
FROM iot_device d
JOIN iot_protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]'::jsonb)) element
WHERE d.deleted_at IS NULL AND d.status = 'enabled' AND p.deleted_at IS NULL AND p.enabled = TRUE
ORDER BY d.id, (element->>'start')::integer)sql");
        for (int row = 0; row < PQntuples(s7Elements.get()); ++row) {
            auto* device = findDevice(text(s7Elements.get(), row, 0));
            if (!device)
                continue;
            ElementDefinition element;
            element.id = text(s7Elements.get(), row, 1);
            element.name = text(s7Elements.get(), row, 2);
            element.unit = text(s7Elements.get(), row, 3);
            element.dataType = text(s7Elements.get(), row, 4);
            element.area = text(s7Elements.get(), row, 5);
            element.dbNumber = integer(s7Elements.get(), row, 6);
            element.start = integer(s7Elements.get(), row, 7);
            element.startBit = integer(s7Elements.get(), row, 8);
            element.size = integer(s7Elements.get(), row, 9);
            element.decimals = integer(s7Elements.get(), row, 10);
            device->elements.push_back(std::move(element));
        }

        const auto sl651Elements = query(connection_.get(), R"sql(
SELECT d.id, element->>'id', element->>'name', COALESCE(element->>'unit', ''),
       func->>'funcCode', func->>'dir', element->>'guideHex', element->>'encode',
       element->>'length', COALESCE(element->>'digits', '0')
FROM iot_device d
JOIN iot_protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]'::jsonb)) func
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(func->'elements', '[]'::jsonb)) element
WHERE d.deleted_at IS NULL AND d.status = 'enabled' AND p.deleted_at IS NULL AND p.enabled = TRUE
ORDER BY d.id, func->>'funcCode', element->>'id')sql");
        for (int row = 0; row < PQntuples(sl651Elements.get()); ++row) {
            auto* device = findDevice(text(sl651Elements.get(), row, 0));
            if (!device)
                continue;
            ElementDefinition element;
            element.id = text(sl651Elements.get(), row, 1);
            element.name = text(sl651Elements.get(), row, 2);
            element.unit = text(sl651Elements.get(), row, 3);
            element.functionCode = text(sl651Elements.get(), row, 4);
            element.direction = text(sl651Elements.get(), row, 5);
            element.guideHex = text(sl651Elements.get(), row, 6);
            element.encoding = text(sl651Elements.get(), row, 7);
            element.length = integer(sl651Elements.get(), row, 8);
            element.digits = integer(sl651Elements.get(), row, 9);
            device->elements.push_back(std::move(element));
        }
        return snapshot;
    }

  private:
    struct ConnectionDeleter {
        void operator()(PGconn* connection) const noexcept {
            if (connection)
                PQfinish(connection);
        }
    };
    struct ResultDeleter {
        void operator()(PGresult* result) const noexcept {
            if (result)
                PQclear(result);
        }
    };
    using Connection = std::unique_ptr<PGconn, ConnectionDeleter>;
    using Result = std::unique_ptr<PGresult, ResultDeleter>;

    [[nodiscard]] Connection connect() const {
        const auto port = std::to_string(endpoint_.port);
        const auto timeout = std::to_string(endpoint_.connectTimeout.count());
        const char* keywords[] = {
            "host", "port", "user", "password", "dbname", "connect_timeout", "options", nullptr};
        const char* values[] = {endpoint_.host.c_str(),     port.c_str(),
                                endpoint_.username.c_str(), endpoint_.password.c_str(),
                                endpoint_.database.c_str(), timeout.c_str(),
                                "-c timezone=UTC",          nullptr};
        Connection connection(PQconnectdbParams(keywords, values, 0));
        if (!connection || PQstatus(connection.get()) != CONNECTION_OK)
            throw std::runtime_error(
                "Southbridge database connection failed: " +
                std::string(connection ? PQerrorMessage(connection.get()) : "allocation failed"));
        return connection;
    }

    void ensureConnected() {
        if (connection_ && PQstatus(connection_.get()) == CONNECTION_OK)
            return;
        connection_ = connect();
        execute(connection_.get(), "SET TIME ZONE 'UTC'");
    }

    static Result query(PGconn* connection, std::string_view sql) {
        Result result(PQexec(connection, std::string(sql).c_str()));
        if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
            throw std::runtime_error("Southbridge configuration query failed: " +
                                     std::string(PQerrorMessage(connection)));
        return result;
    }

    static void execute(PGconn* connection, std::string_view sql) {
        Result result(PQexec(connection, std::string(sql).c_str()));
        if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK)
            throw std::runtime_error("Southbridge database command failed: " +
                                     std::string(PQerrorMessage(connection)));
    }

    static std::string text(const PGresult* result, int row, int column) {
        if (PQgetisnull(result, row, column))
            return {};
        return {PQgetvalue(result, row, column),
                static_cast<std::size_t>(PQgetlength(result, row, column))};
    }

    static std::int64_t integer(const PGresult* result, int row, int column) {
        return std::stoll(text(result, row, column));
    }

    static std::vector<std::uint8_t> registrationBytes(std::string_view mode,
                                                       std::string_view content) {
        return packetBytes(mode, content);
    }

    static std::vector<std::uint8_t> packetBytes(std::string_view mode, std::string_view content) {
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
        const auto bytes = bridge::fromHex(normalized);
        if (bytes.empty() && !normalized.empty())
            throw std::runtime_error("Invalid HEX DTU registration content");
        return bytes;
    }

    PostgresEndpoint endpoint_;
    Connection connection_;
};

} // namespace service::southbridge
