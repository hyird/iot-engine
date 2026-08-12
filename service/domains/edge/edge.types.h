#pragma once

#include <ruvia/web/Model.h>

namespace service::edge {

RUVIA_REQUEST_MODEL(EdgeListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20)),
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(EdgeIdParams,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String));

RUVIA_REQUEST_MODEL(EdgePlatformParams,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("platformId", platformId, ruvia::String));

RUVIA_REQUEST_MODEL(EnrollmentBody,
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String));

RUVIA_REQUEST_MODEL(NodeNameBody,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String));

RUVIA_REQUEST_MODEL(NetworkInterfaceBody,
    RUVIA_OPTIONAL_FIELD(operation, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("previousName", previousName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(device, ruvia::String),
    RUVIA_OPTIONAL_FIELD(bridge, ruvia::Bool, RUVIA_DEFAULT(false)),
    RUVIA_OPTIONAL_FIELD_NAME("bridgePorts", bridgePorts, ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("prefixLength", prefixLength, ruvia::Int64, RUVIA_DEFAULT(0)),
    RUVIA_OPTIONAL_FIELD(gateway, ruvia::String));

RUVIA_REQUEST_MODEL(NetworkBody,
    RUVIA_OPTIONAL_FIELD(interfaces, ruvia::Array<NetworkInterfaceBody>),
    RUVIA_OPTIONAL_FIELD_NAME("rollbackTimeoutSec", rollbackTimeoutSec, ruvia::Int64, RUVIA_DEFAULT(60)));

RUVIA_REQUEST_MODEL(PlatformBody,
    RUVIA_OPTIONAL_FIELD_NAME("platformId", platformId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("baseUrl", baseUrl, ruvia::String),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool, RUVIA_DEFAULT(true)),
    RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64, RUVIA_DEFAULT(100)),
    RUVIA_OPTIONAL_FIELD_NAME("reconnectIntervalSec", reconnectIntervalSec, ruvia::Int64, RUVIA_DEFAULT(5)),
    RUVIA_OPTIONAL_FIELD_NAME("outboxMaxBytes", outboxMaxBytes, ruvia::Int64, RUVIA_DEFAULT(262144)));

RUVIA_REQUEST_MODEL(FirmwareDownloadQuery,
    RUVIA_OPTIONAL_FIELD(token, ruvia::String));

RUVIA_REQUEST_MODEL(TerminalTicketQuery,
    RUVIA_OPTIONAL_FIELD(ticket, ruvia::String));

RUVIA_REQUEST_MODEL(LogsQuery,
    RUVIA_OPTIONAL_FIELD(limit, ruvia::Int64, RUVIA_DEFAULT(48)),
    RUVIA_OPTIONAL_FIELD(level, ruvia::String),
    RUVIA_OPTIONAL_FIELD(source, ruvia::String));

RUVIA_REQUEST_MODEL(LogLevelBody,
    RUVIA_OPTIONAL_FIELD(level, ruvia::String));

RUVIA_RESPONSE_MODEL(InterfaceDto,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("displayName", displayName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(mac, ruvia::String),
    RUVIA_OPTIONAL_FIELD(up, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(bridge, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(ipv4, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("prefixLength", prefixLength, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(gateway, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("bridgePorts", bridgePorts, ruvia::BoxedArray<ruvia::String>));

RUVIA_REQUEST_MODEL(ModemControlBody,
    RUVIA_OPTIONAL_FIELD(action, ruvia::String),
    RUVIA_OPTIONAL_FIELD(apn, ruvia::String),
    RUVIA_OPTIONAL_FIELD(automatic, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("pdpType", pdpType, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("authType", authType, ruvia::String),
    RUVIA_OPTIONAL_FIELD(username, ruvia::String),
    RUVIA_OPTIONAL_FIELD(password, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("pinCode", pinCode, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("redialAfterApply", redialAfterApply, ruvia::Bool));

RUVIA_RESPONSE_MODEL(NetworkDto,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(device, ruvia::String),
    RUVIA_OPTIONAL_FIELD(up, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(bridge, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("bridgePorts", bridgePorts, ruvia::BoxedArray<ruvia::String>),
    RUVIA_OPTIONAL_FIELD(ipv4, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("prefixLength", prefixLength, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(gateway, ruvia::String));

RUVIA_RESPONSE_MODEL(SerialDto,
    RUVIA_OPTIONAL_FIELD(path, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("displayName", displayName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(available, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(rs485, ruvia::Bool));

RUVIA_RESPONSE_MODEL(PlatformStatusDto,
    RUVIA_OPTIONAL_FIELD(state, ruvia::String),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String));

RUVIA_RESPONSE_MODEL(PlatformDto,
    RUVIA_OPTIONAL_FIELD_NAME("platformId", platformId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("baseUrl", baseUrl, ruvia::String),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("reconnectIntervalSec", reconnectIntervalSec, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("outboxMaxBytes", outboxMaxBytes, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(status, PlatformStatusDto));

RUVIA_RESPONSE_MODEL(ConfigStatusDto,
    RUVIA_OPTIONAL_FIELD_NAME("activeVersion", activeVersion, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("desiredVersion", desiredVersion, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(state, ruvia::String),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String));

RUVIA_RESPONSE_MODEL(OutboxStatusDto,
    RUVIA_OPTIONAL_FIELD(records, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(bytes, ruvia::Int64));

RUVIA_RESPONSE_MODEL(LogStatusDto,
    RUVIA_OPTIONAL_FIELD(level, ruvia::String));

RUVIA_RESPONSE_MODEL(NodeStatusDto,
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("lastSeenAt", lastSeenAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD(config, ConfigStatusDto),
    RUVIA_OPTIONAL_FIELD(outbox, OutboxStatusDto),
    RUVIA_OPTIONAL_FIELD(log, LogStatusDto));

RUVIA_RESPONSE_MODEL(CapabilityDto,
    RUVIA_OPTIONAL_FIELD_NAME("networkConfig", networkConfig, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("networkConfigVersion", networkConfigVersion, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("firmwareUpdate", firmwareUpdate, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("platformConfig", platformConfig, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("deviceConfig", deviceConfig, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("modemControl", modemControl, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(terminal, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(logs, ruvia::Bool));

RUVIA_RESPONSE_MODEL(SignalDto,
    RUVIA_OPTIONAL_FIELD(csq, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("rssiDbm", rssiDbm, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(percent, ruvia::Int64));

RUVIA_RESPONSE_MODEL(MobileDto,
    RUVIA_OPTIONAL_FIELD(available, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("simState", simState, ruvia::String),
    RUVIA_OPTIONAL_FIELD(iccid, ruvia::String),
    RUVIA_OPTIONAL_FIELD(signal, SignalDto),
    RUVIA_OPTIONAL_FIELD(registered, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("registrationStatus", registrationStatus, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(apn, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("operator", operatorName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(connected, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(ipv4, ruvia::String));

RUVIA_RESPONSE_MODEL(FirmwareStatusDto,
    RUVIA_OPTIONAL_FIELD(state, ruvia::String),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("progressPercent", progressPercent, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("downloadedBytes", downloadedBytes, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("totalBytes", totalBytes, ruvia::Int64));

RUVIA_RESPONSE_MODEL(TaskDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("taskType", taskType, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("progressPercent", progressPercent, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("downloadedBytes", downloadedBytes, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("totalBytes", totalBytes, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("updatedAt", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(EdgeNodeDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(imei, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(model, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("softwareVersion", softwareVersion, ruvia::String),
    RUVIA_OPTIONAL_FIELD(hostname, ruvia::String),
    RUVIA_OPTIONAL_FIELD(architecture, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("openwrtRelease", openwrtRelease, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("enrollmentStatus", enrollmentStatus, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, NodeStatusDto),
    RUVIA_OPTIONAL_FIELD(capability, CapabilityDto),
    RUVIA_OPTIONAL_FIELD(mobile, MobileDto),
    RUVIA_OPTIONAL_FIELD(firmware, FirmwareStatusDto),
    RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD(interfaces, ruvia::BoxedArray<InterfaceDto>),
    RUVIA_OPTIONAL_FIELD(networks, ruvia::BoxedArray<NetworkDto>),
    RUVIA_OPTIONAL_FIELD_NAME("serialPorts", serialPorts, ruvia::BoxedArray<SerialDto>),
    RUVIA_OPTIONAL_FIELD(platforms, ruvia::BoxedArray<PlatformDto>),
    RUVIA_OPTIONAL_FIELD(tasks, ruvia::BoxedArray<TaskDto>));

RUVIA_RESPONSE_MODEL(EdgePageDto,
    RUVIA_OPTIONAL_FIELD(list, ruvia::BoxedArray<EdgeNodeDto>),
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(FirmwareDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(version, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("fileName", fileName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(sha256, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("sizeBytes", sizeBytes, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String));

RUVIA_RESPONSE_MODEL(TerminalTicketDto,
    RUVIA_OPTIONAL_FIELD(ticket, ruvia::String));

RUVIA_RESPONSE_MODEL(LogLineDto,
    RUVIA_OPTIONAL_FIELD(time, ruvia::String),
    RUVIA_OPTIONAL_FIELD(level, ruvia::String),
    RUVIA_OPTIONAL_FIELD(source, ruvia::String),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(detail, ruvia::String));

RUVIA_RESPONSE_MODEL(LogsDto,
    RUVIA_OPTIONAL_FIELD(lines, ruvia::BoxedArray<LogLineDto>));

#define EDGE_RESPONSE(name, dataType)                                                          \
    RUVIA_RESPONSE_MODEL(name,                                                                 \
        RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),                                              \
        RUVIA_OPTIONAL_FIELD(message, ruvia::String),                                          \
        RUVIA_OPTIONAL_FIELD(data, dataType))

EDGE_RESPONSE(EdgePageResponse, EdgePageDto);
EDGE_RESPONSE(EdgeDetailResponse, EdgeNodeDto);
EDGE_RESPONSE(FirmwareListResponse, ruvia::BoxedArray<FirmwareDto>);
EDGE_RESPONSE(TerminalTicketResponse, TerminalTicketDto);
EDGE_RESPONSE(LogsResponse, LogsDto);

#undef EDGE_RESPONSE

} // namespace service::edge
