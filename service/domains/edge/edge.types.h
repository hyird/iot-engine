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

struct NodeNameBody final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_MODEL(NodeNameBody, name);
};

struct NetworkInterfaceBody final {
    RUVIA_OPTIONAL_FIELD(operation, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("previousName", previousName, ruvia::String);
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String);
    RUVIA_OPTIONAL_FIELD(device, ruvia::String);
    RUVIA_OPTIONAL_FIELD(bridge, ruvia::Bool, RUVIA_DEFAULT(false));
    RUVIA_OPTIONAL_FIELD_NAME("bridgePorts", bridgePorts, ruvia::Array<ruvia::String>);
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("prefixLength", prefixLength, ruvia::Int64,
                              RUVIA_DEFAULT(0));
    RUVIA_OPTIONAL_FIELD(gateway, ruvia::String);
    RUVIA_MODEL(NetworkInterfaceBody, operation, name, previousName, mode, device, bridge,
                bridgePorts, ip, prefixLength, gateway);
};

struct NetworkBody final {
    RUVIA_OPTIONAL_FIELD(interfaces, ruvia::Array<NetworkInterfaceBody>);
    RUVIA_OPTIONAL_FIELD_NAME("rollbackTimeoutSec", rollbackTimeoutSec, ruvia::Int64,
                              RUVIA_DEFAULT(60));
    RUVIA_MODEL(NetworkBody, interfaces, rollbackTimeoutSec);
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

struct ModemControlBody final {
    RUVIA_OPTIONAL_FIELD(action, ruvia::String);
    RUVIA_OPTIONAL_FIELD(apn, ruvia::String);
    RUVIA_MODEL(ModemControlBody, action, apn);
};

struct NetworkDto final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String);
    RUVIA_OPTIONAL_FIELD(device, ruvia::String);
    RUVIA_OPTIONAL_FIELD(up, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(bridge, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("bridgePorts", bridgePorts, ruvia::List<ruvia::String>);
    RUVIA_OPTIONAL_FIELD(ipv4, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("prefixLength", prefixLength, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(gateway, ruvia::String);
    RUVIA_MODEL(NetworkDto, name, mode, device, up, bridge, bridgePorts, ipv4, prefixLength,
                gateway);
};

struct SerialDto final {
    RUVIA_OPTIONAL_FIELD(path, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("displayName", displayName, ruvia::String);
    RUVIA_OPTIONAL_FIELD(available, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(rs485, ruvia::Bool);
    RUVIA_MODEL(SerialDto, path, displayName, available, rs485);
};

struct PlatformStatusDto final {
    RUVIA_OPTIONAL_FIELD(state, ruvia::String);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_MODEL(PlatformStatusDto, state, message);
};

struct PlatformDto final {
    RUVIA_OPTIONAL_FIELD_NAME("platformId", platformId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("baseUrl", baseUrl, ruvia::String);
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("reconnectIntervalSec", reconnectIntervalSec, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("outboxMaxBytes", outboxMaxBytes, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(status, PlatformStatusDto);
    RUVIA_MODEL(PlatformDto, platformId, name, baseUrl, enabled, priority, reconnectIntervalSec,
                outboxMaxBytes, status);
};

struct ConfigStatusDto final {
    RUVIA_OPTIONAL_FIELD_NAME("activeVersion", activeVersion, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("desiredVersion", desiredVersion, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(state, ruvia::String);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_MODEL(ConfigStatusDto, activeVersion, desiredVersion, state, message);
};

struct OutboxStatusDto final {
    RUVIA_OPTIONAL_FIELD(records, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(bytes, ruvia::Int64);
    RUVIA_MODEL(OutboxStatusDto, records, bytes);
};

struct NodeStatusDto final {
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("lastSeenAt", lastSeenAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD(config, ConfigStatusDto);
    RUVIA_OPTIONAL_FIELD(outbox, OutboxStatusDto);
    RUVIA_MODEL(NodeStatusDto, online, lastSeenAt, config, outbox);
};

struct CapabilityDto final {
    RUVIA_OPTIONAL_FIELD_NAME("networkConfig", networkConfig, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("networkConfigVersion", networkConfigVersion, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("firmwareUpdate", firmwareUpdate, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("platformConfig", platformConfig, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("deviceConfig", deviceConfig, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("modemControl", modemControl, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(terminal, ruvia::Bool);
    RUVIA_MODEL(CapabilityDto, networkConfig, networkConfigVersion, firmwareUpdate,
                platformConfig, deviceConfig, modemControl, terminal);
};

struct SignalDto final {
    RUVIA_OPTIONAL_FIELD(csq, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("rssiDbm", rssiDbm, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(percent, ruvia::Int64);
    RUVIA_MODEL(SignalDto, csq, rssiDbm, percent);
};

struct MobileDto final {
    RUVIA_OPTIONAL_FIELD(available, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("simState", simState, ruvia::String);
    RUVIA_OPTIONAL_FIELD(iccid, ruvia::String);
    RUVIA_OPTIONAL_FIELD(signal, SignalDto);
    RUVIA_OPTIONAL_FIELD(registered, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("registrationStatus", registrationStatus, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(apn, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("operator", operatorName, ruvia::String);
    RUVIA_OPTIONAL_FIELD(connected, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(ipv4, ruvia::String);
    RUVIA_MODEL(MobileDto, available, simState, iccid, signal, registered, registrationStatus, apn,
                operatorName, connected, ipv4);
};

struct FirmwareStatusDto final {
    RUVIA_OPTIONAL_FIELD(state, ruvia::String);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("progressPercent", progressPercent, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("downloadedBytes", downloadedBytes, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("totalBytes", totalBytes, ruvia::Int64);
    RUVIA_MODEL(FirmwareStatusDto, state, message, progressPercent, downloadedBytes, totalBytes);
};

struct TaskDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("taskType", taskType, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("progressPercent", progressPercent, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("downloadedBytes", downloadedBytes, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("totalBytes", totalBytes, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("updatedAt", updatedAt, ruvia::String);
    RUVIA_MODEL(TaskDto, id, taskType, status, message, progressPercent, downloadedBytes,
                totalBytes, createdAt, updatedAt);
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
    RUVIA_OPTIONAL_FIELD(status, NodeStatusDto);
    RUVIA_OPTIONAL_FIELD(capability, CapabilityDto);
    RUVIA_OPTIONAL_FIELD(mobile, MobileDto);
    RUVIA_OPTIONAL_FIELD(firmware, FirmwareStatusDto);
    RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD(interfaces, ruvia::List<InterfaceDto>);
    RUVIA_OPTIONAL_FIELD(networks, ruvia::List<NetworkDto>);
    RUVIA_OPTIONAL_FIELD_NAME("serialPorts", serialPorts, ruvia::List<SerialDto>);
    RUVIA_OPTIONAL_FIELD(platforms, ruvia::List<PlatformDto>);
    RUVIA_OPTIONAL_FIELD(tasks, ruvia::List<TaskDto>);
    RUVIA_MODEL(EdgeNodeDto, id, imei, name, model, softwareVersion, hostname, architecture,
                openwrtRelease, enrollmentStatus, status, capability, mobile, firmware, createdAt,
                interfaces, networks, serialPorts, platforms, tasks);
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
