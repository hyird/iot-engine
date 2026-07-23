#pragma once

#include <ruvia/web/Model.h>

namespace service::edge {

struct EdgeListQuery final {
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1));
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20));
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_MODEL(EdgeListQuery, page, pageSize, keyword, status);
};

struct EdgeIdParams final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_MODEL(EdgeIdParams, id);
};

struct EdgePlatformParams final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("platformId", platformId, ruvia::String);
    RUVIA_MODEL(EdgePlatformParams, id, platformId);
};

struct EnrollmentBody final {
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_MODEL(EnrollmentBody, status, name);
};

struct NetworkBody final {
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String);
    RUVIA_OPTIONAL_FIELD(netmask, ruvia::String);
    RUVIA_OPTIONAL_FIELD(gateway, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("rollbackTimeoutSec", rollbackTimeoutSec, ruvia::Int64,
                              RUVIA_DEFAULT(60));
    RUVIA_MODEL(NetworkBody, ip, netmask, gateway, rollbackTimeoutSec);
};

struct PlatformBody final {
    RUVIA_OPTIONAL_FIELD_NAME("platformId", platformId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("baseUrl", baseUrl, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("enrollmentToken", enrollmentToken, ruvia::String);
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool, RUVIA_DEFAULT(true));
    RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64, RUVIA_DEFAULT(100));
    RUVIA_OPTIONAL_FIELD_NAME("reconnectIntervalSec", reconnectIntervalSec, ruvia::Int64,
                              RUVIA_DEFAULT(5));
    RUVIA_OPTIONAL_FIELD_NAME("outboxMaxBytes", outboxMaxBytes, ruvia::Int64,
                              RUVIA_DEFAULT(262144));
    RUVIA_MODEL(PlatformBody, platformId, name, baseUrl, enrollmentToken, enabled, priority,
                reconnectIntervalSec, outboxMaxBytes);
};

struct FirmwareDownloadQuery final {
    RUVIA_OPTIONAL_FIELD(token, ruvia::String);
    RUVIA_MODEL(FirmwareDownloadQuery, token);
};

struct TerminalTicketQuery final {
    RUVIA_OPTIONAL_FIELD(ticket, ruvia::String);
    RUVIA_MODEL(TerminalTicketQuery, ticket);
};

struct InterfaceDto final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("displayName", displayName, ruvia::String);
    RUVIA_OPTIONAL_FIELD(mac, ruvia::String);
    RUVIA_OPTIONAL_FIELD(up, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(bridge, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(ipv4, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("prefixLength", prefixLength, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(gateway, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("bridgePorts", bridgePorts, ruvia::List<ruvia::String>);
    RUVIA_MODEL(InterfaceDto, name, displayName, mac, up, bridge, ipv4, prefixLength, gateway,
                bridgePorts);
};

struct SerialDto final {
    RUVIA_OPTIONAL_FIELD(path, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("displayName", displayName, ruvia::String);
    RUVIA_OPTIONAL_FIELD(available, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(rs485, ruvia::Bool);
    RUVIA_MODEL(SerialDto, path, displayName, available, rs485);
};

struct PlatformDto final {
    RUVIA_OPTIONAL_FIELD_NAME("platformId", platformId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("baseUrl", baseUrl, ruvia::String);
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("reconnectIntervalSec", reconnectIntervalSec, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("outboxMaxBytes", outboxMaxBytes, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("applyStatus", applyStatus, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("lastMessage", lastMessage, ruvia::String);
    RUVIA_MODEL(PlatformDto, platformId, name, baseUrl, enabled, priority, reconnectIntervalSec,
                outboxMaxBytes, applyStatus, lastMessage);
};

struct TaskDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("taskType", taskType, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("updatedAt", updatedAt, ruvia::String);
    RUVIA_MODEL(TaskDto, id, taskType, status, message, createdAt, updatedAt);
};

struct EdgeNodeDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(imei, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(model, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("softwareVersion", softwareVersion, ruvia::String);
    RUVIA_OPTIONAL_FIELD(hostname, ruvia::String);
    RUVIA_OPTIONAL_FIELD(architecture, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("openwrtRelease", openwrtRelease, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("enrollmentStatus", enrollmentStatus, ruvia::String);
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("supportsNetworkConfig", supportsNetworkConfig, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("supportsFirmwareUpdate", supportsFirmwareUpdate, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("supportsPlatformConfig", supportsPlatformConfig, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("supportsDeviceConfig", supportsDeviceConfig, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("ttydAvailable", ttydAvailable, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("activeConfigVersion", activeConfigVersion, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("desiredConfigVersion", desiredConfigVersion, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("configStatus", configStatus, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("configMessage", configMessage, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("outboxRecords", outboxRecords, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("outboxBytes", outboxBytes, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("lastSeenAt", lastSeenAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD(interfaces, ruvia::List<InterfaceDto>);
    RUVIA_OPTIONAL_FIELD_NAME("serialPorts", serialPorts, ruvia::List<SerialDto>);
    RUVIA_OPTIONAL_FIELD(platforms, ruvia::List<PlatformDto>);
    RUVIA_OPTIONAL_FIELD(tasks, ruvia::List<TaskDto>);
    RUVIA_MODEL(EdgeNodeDto, id, imei, name, model, softwareVersion, hostname, architecture,
                openwrtRelease, enrollmentStatus, online, supportsNetworkConfig,
                supportsFirmwareUpdate, supportsPlatformConfig, supportsDeviceConfig,
                ttydAvailable, activeConfigVersion, desiredConfigVersion, configStatus,
                configMessage, outboxRecords, outboxBytes, lastSeenAt, createdAt, interfaces,
                serialPorts, platforms, tasks);
};

struct EdgePageDto final {
    RUVIA_OPTIONAL_FIELD(list, ruvia::List<EdgeNodeDto>);
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64);
    RUVIA_MODEL(EdgePageDto, list, total, page, pageSize, totalPages);
};

struct FirmwareDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(version, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("fileName", fileName, ruvia::String);
    RUVIA_OPTIONAL_FIELD(sha256, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("sizeBytes", sizeBytes, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String);
    RUVIA_MODEL(FirmwareDto, id, version, fileName, sha256, sizeBytes, createdAt);
};

struct TerminalTicketDto final {
    RUVIA_OPTIONAL_FIELD(ticket, ruvia::String);
    RUVIA_MODEL(TerminalTicketDto, ticket);
};

#define EDGE_RESPONSE(name, dataType)                                                          \
    struct name final {                                                                        \
        RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);                                               \
        RUVIA_OPTIONAL_FIELD(message, ruvia::String);                                           \
        RUVIA_OPTIONAL_FIELD(data, dataType);                                                   \
        RUVIA_MODEL(name, code, message, data);                                                 \
    }

EDGE_RESPONSE(EdgePageResponse, EdgePageDto);
EDGE_RESPONSE(EdgeDetailResponse, EdgeNodeDto);
EDGE_RESPONSE(FirmwareListResponse, ruvia::List<FirmwareDto>);
EDGE_RESPONSE(TerminalTicketResponse, TerminalTicketDto);

#undef EDGE_RESPONSE

} // namespace service::edge
