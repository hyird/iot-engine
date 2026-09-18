#pragma once

#include <cctype>

#include "service/common/uuid.h"

#include <string>

#include <ruvia/web/Validation.h>

namespace service::edge {

inline bool isUciSectionName(const ruvia::String& value) {
    const auto text = value.view();
    if (text.empty() || text.size() > 15) {
        return false;
    }
    for (const unsigned char character : text) {
        if (!std::isalnum(character) && character != '_') {
            return false;
        }
    }
    return true;
}

inline bool isOptionalNetworkDevice(const ruvia::String& value) {
    const auto text = value.view();
    if (text.size() > 32) {
        return false;
    }
    for (const unsigned char character : text) {
        if (!std::isalnum(character) && character != '_' && character != '-' &&
            character != '.' && character != ':') {
            return false;
        }
    }
    return true;
}

inline bool isEdgeGroupFilter(const ruvia::String& value) {
    return value.view() == "ungrouped" || service::common::isOptionalUuidField(value);
}

RUVIA_REQUEST_MODEL(DebugAuthenticationBody, RUVIA_REQUIRED_FIELD(token, ruvia::String, RUVIA_MIN(1, "令牌不能为空")));

RUVIA_REQUEST_MODEL(EdgeEventsQuery, RUVIA_OPTIONAL_FIELD(dtu, ruvia::Bool, RUVIA_DEFAULT(false)), RUVIA_OPTIONAL_FIELD(nodeId, ruvia::String, RUVIA_CUSTOM("nodeId 必须是 UUID", service::common::isUuidField)), RUVIA_OPTIONAL_FIELD(logs, ruvia::Bool, RUVIA_DEFAULT(false)), RUVIA_OPTIONAL_FIELD(vpn, ruvia::Bool, RUVIA_DEFAULT(false)));

RUVIA_REQUEST_MODEL(EdgeListQuery, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")), RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20), RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")), RUVIA_OPTIONAL_FIELD(keyword, ruvia::String), RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("注册状态无效", "pending", "approved")), RUVIA_OPTIONAL_FIELD_NAME("groupId", groupId, ruvia::String, RUVIA_CUSTOM("节点分组筛选无效", isEdgeGroupFilter)));

RUVIA_REQUEST_MODEL(EdgeIdParams, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));

RUVIA_REQUEST_MODEL(EnrollmentBody, RUVIA_REQUIRED_FIELD(status, ruvia::String, RUVIA_ONE_OF("注册状态无效", "approved")), RUVIA_OPTIONAL_FIELD(name, ruvia::String, RUVIA_MAX(100, "节点名称不能超过 100 个字符")));

RUVIA_REQUEST_MODEL(NodeNameBody, RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MAX(100, "节点名称不能超过 100 个字符")));

RUVIA_REQUEST_MODEL(NodeGroupBody, RUVIA_OPTIONAL_FIELD_NAME("groupId", groupId, ruvia::String, RUVIA_CUSTOM("节点分组 ID 无效", service::common::isOptionalUuidField)));

RUVIA_REQUEST_MODEL(EdgeGroupBody, RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(1, "分组名称不能为空"), RUVIA_MAX(100, "分组名称不能超过 100 个字符")), RUVIA_OPTIONAL_FIELD_NAME("parentId", parentId, ruvia::String, RUVIA_CUSTOM("上级分组 ID 无效", service::common::isOptionalUuidField)), RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("分组状态无效", "enabled", "disabled")), RUVIA_OPTIONAL_FIELD_NAME("sortOrder", sortOrder, ruvia::Int64, RUVIA_DEFAULT(0), RUVIA_MIN(0, "分组排序不能小于 0")), RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_MAX(500, "分组备注不能超过 500 个字符")));

RUVIA_REQUEST_MODEL(NetworkInterfaceBody, RUVIA_REQUIRED_FIELD(operation, ruvia::String, RUVIA_ONE_OF("网络操作无效", "upsert", "delete")), RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_CUSTOM("逻辑接口名称只能包含字母、数字和下划线", isUciSectionName)), RUVIA_OPTIONAL_FIELD_NAME("previousName", previousName, ruvia::String, RUVIA_CUSTOM("原逻辑接口名称只能包含字母、数字和下划线", isUciSectionName)), RUVIA_OPTIONAL_FIELD(mode, ruvia::String, RUVIA_ONE_OF("地址协议无效", "dhcp", "static")), RUVIA_OPTIONAL_FIELD(device, ruvia::String, RUVIA_CUSTOM("设备名称包含非法字符", isOptionalNetworkDevice)), RUVIA_OPTIONAL_FIELD(bridge, ruvia::Bool, RUVIA_DEFAULT(false)), RUVIA_OPTIONAL_FIELD_NAME("bridgePorts", bridgePorts, ruvia::Array<ruvia::String>, RUVIA_MAX(8, "网桥最多包含 8 个成员")), RUVIA_OPTIONAL_FIELD(ip, ruvia::String, RUVIA_MAX(15, "IPv4 地址格式无效")), RUVIA_OPTIONAL_FIELD_NAME("prefixLength", prefixLength, ruvia::Int64, RUVIA_DEFAULT(0), RUVIA_MIN(0, "IPv4 前缀必须在 0 - 30 之间"), RUVIA_MAX(30, "IPv4 前缀必须在 0 - 30 之间")), RUVIA_OPTIONAL_FIELD(gateway, ruvia::String, RUVIA_MAX(15, "网关格式无效")));

RUVIA_REQUEST_MODEL(NetworkBody, RUVIA_REQUIRED_FIELD(interfaces, ruvia::Array<NetworkInterfaceBody>, RUVIA_MIN(1, "请至少配置一个逻辑接口"), RUVIA_MAX(8, "单次最多配置 8 个逻辑接口")), RUVIA_OPTIONAL_FIELD_NAME("rollbackTimeoutSec", rollbackTimeoutSec, ruvia::Int64, RUVIA_DEFAULT(60), RUVIA_MIN(30, "回滚等待时间必须在 30 - 300 秒之间"), RUVIA_MAX(300, "回滚等待时间必须在 30 - 300 秒之间")));

RUVIA_REQUEST_MODEL(FirmwareUploadBody, RUVIA_REQUIRED_FIELD_NAME("fileName", fileName, ruvia::String, RUVIA_MIN(1, "固件文件名不能为空"), RUVIA_MAX(255, "固件文件名过长")), RUVIA_REQUIRED_FIELD_NAME("sizeBytes", sizeBytes, ruvia::Int64, RUVIA_MIN(1, "固件文件不能为空"), RUVIA_MAX(134217728, "固件不能超过 128 MiB")), RUVIA_OPTIONAL_FIELD_NAME("keepSettings", keepSettings, ruvia::Bool, RUVIA_DEFAULT(true)));
RUVIA_REQUEST_MODEL(FirmwareReuseBody, RUVIA_REQUIRED_FIELD(sha256, ruvia::String, RUVIA_MIN(64, "固件摘要无效"), RUVIA_MAX(64, "固件摘要无效")), RUVIA_REQUIRED_FIELD_NAME("sizeBytes", sizeBytes, ruvia::Int64, RUVIA_MIN(1, "固件文件不能为空"), RUVIA_MAX(134217728, "固件不能超过 128 MiB")), RUVIA_OPTIONAL_FIELD_NAME("keepSettings", keepSettings, ruvia::Bool, RUVIA_DEFAULT(true)));
RUVIA_RESPONSE_MODEL(FirmwareReuseResult, RUVIA_OPTIONAL_FIELD(reused, ruvia::Bool));

RUVIA_REQUEST_MODEL(FirmwareDownloadQuery, RUVIA_REQUIRED_FIELD(token, ruvia::String, RUVIA_MAX(64, "下载凭据无效")));

RUVIA_REQUEST_MODEL(LogsQuery, RUVIA_OPTIONAL_FIELD(limit, ruvia::Int64, RUVIA_DEFAULT(48), RUVIA_MIN(1, "日志条数必须在 1 - 48 之间"), RUVIA_MAX(48, "日志条数必须在 1 - 48 之间")), RUVIA_OPTIONAL_FIELD(level, ruvia::String, RUVIA_ONE_OF("日志级别无效", "debug", "info", "warn", "error")), RUVIA_OPTIONAL_FIELD(source, ruvia::String, RUVIA_MAX(16, "日志来源不能超过 16 个字符")));

RUVIA_REQUEST_MODEL(DtuChannelBody,
    RUVIA_REQUIRED_FIELD(channelId, ruvia::String, RUVIA_CUSTOM("通道 ID 无效", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(1, "请输入通道名称"), RUVIA_MAX(100, "通道名称过长")),
    RUVIA_REQUIRED_FIELD(enabled, ruvia::Bool), RUVIA_REQUIRED_FIELD(southMode, ruvia::String, RUVIA_ONE_OF("南向模式无效", "serial", "tcp_client", "tcp_server")),
    RUVIA_OPTIONAL_FIELD(southHost, ruvia::String, RUVIA_MAX(253, "南向地址过长")), RUVIA_OPTIONAL_FIELD(southPort, ruvia::Int64, RUVIA_MIN(1, "端口无效"), RUVIA_MAX(65535, "端口无效")),
    RUVIA_REQUIRED_FIELD(northHost, ruvia::String, RUVIA_MIN(1, "请输入北向服务器"), RUVIA_MAX(253, "服务器地址过长")), RUVIA_REQUIRED_FIELD(northPort, ruvia::Int64, RUVIA_MIN(1, "端口无效"), RUVIA_MAX(65535, "端口无效")),
    RUVIA_OPTIONAL_FIELD(serialPath, ruvia::String, RUVIA_MAX(96, "串口路径过长")), RUVIA_OPTIONAL_FIELD(baudRate, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(dataBits, ruvia::Int64), RUVIA_OPTIONAL_FIELD(stopBits, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(parity, ruvia::String), RUVIA_OPTIONAL_FIELD(rs485, ruvia::Bool),
    RUVIA_REQUIRED_FIELD(maxClients, ruvia::Int64, RUVIA_MIN(1, "客户端数至少为 1"), RUVIA_MAX(16, "最多 16 个客户端")), RUVIA_REQUIRED_FIELD(queueBytes, ruvia::Int64, RUVIA_MIN(4096, "缓冲至少 4096 字节"), RUVIA_MAX(65536, "缓冲最多 65536 字节")),
    RUVIA_REQUIRED_FIELD(serialFrameMs, ruvia::Int64, RUVIA_MIN(0, "组帧间隔无效"), RUVIA_MAX(1000, "组帧间隔最多 1000 ms")), RUVIA_OPTIONAL_FIELD(uplinkOnly, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(registrationHex, ruvia::String, RUVIA_MAX(512, "注册包最多 256 字节")), RUVIA_OPTIONAL_FIELD(debugEnabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(heartbeatHex, ruvia::String, RUVIA_MAX(512, "心跳包最多 256 字节")), RUVIA_OPTIONAL_FIELD(heartbeatIntervalSec, ruvia::Int64, RUVIA_MIN(0, "心跳间隔无效"), RUVIA_MAX(86400, "心跳间隔最多 86400 秒")));
RUVIA_REQUEST_MODEL(DtuChannelParams, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("节点 ID 无效", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(channelId, ruvia::String, RUVIA_CUSTOM("通道 ID 无效", service::common::isUuidField)));

RUVIA_REQUEST_MODEL(LogLevelBody, RUVIA_REQUIRED_FIELD(level, ruvia::String, RUVIA_ONE_OF("日志级别无效", "debug", "info", "warn", "error")));

RUVIA_RESPONSE_MODEL(InterfaceDto, RUVIA_OPTIONAL_FIELD(name, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("displayName", displayName, ruvia::String), RUVIA_OPTIONAL_FIELD(mac, ruvia::String), RUVIA_OPTIONAL_FIELD(up, ruvia::Bool), RUVIA_OPTIONAL_FIELD(bridge, ruvia::Bool), RUVIA_OPTIONAL_FIELD(ipv4, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("prefixLength", prefixLength, ruvia::Int64), RUVIA_OPTIONAL_FIELD(gateway, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("bridgePorts", bridgePorts, ruvia::BoxedArray<ruvia::String>));

RUVIA_RESPONSE_MODEL(NetworkDto, RUVIA_OPTIONAL_FIELD(name, ruvia::String), RUVIA_OPTIONAL_FIELD(mode, ruvia::String), RUVIA_OPTIONAL_FIELD(device, ruvia::String), RUVIA_OPTIONAL_FIELD(up, ruvia::Bool), RUVIA_OPTIONAL_FIELD(bridge, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("bridgePorts", bridgePorts, ruvia::BoxedArray<ruvia::String>), RUVIA_OPTIONAL_FIELD(ipv4, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("prefixLength", prefixLength, ruvia::Int64), RUVIA_OPTIONAL_FIELD(gateway, ruvia::String));

RUVIA_RESPONSE_MODEL(SerialDto, RUVIA_OPTIONAL_FIELD(path, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("displayName", displayName, ruvia::String), RUVIA_OPTIONAL_FIELD(available, ruvia::Bool), RUVIA_OPTIONAL_FIELD(rs485, ruvia::Bool));

RUVIA_RESPONSE_MODEL(ConfigStatusDto, RUVIA_OPTIONAL_FIELD_NAME("activeVersion", activeVersion, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("desiredVersion", desiredVersion, ruvia::Int64), RUVIA_OPTIONAL_FIELD(state, ruvia::String), RUVIA_OPTIONAL_FIELD(message, ruvia::String));

RUVIA_RESPONSE_MODEL(OutboxStatusDto, RUVIA_OPTIONAL_FIELD(records, ruvia::Int64), RUVIA_OPTIONAL_FIELD(bytes, ruvia::Int64));

RUVIA_RESPONSE_MODEL(LogStatusDto, RUVIA_OPTIONAL_FIELD(level, ruvia::String));

RUVIA_RESPONSE_MODEL(NodeStatusDto, RUVIA_OPTIONAL_FIELD(online, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("lastSeenAt", lastSeenAt, ruvia::String), RUVIA_OPTIONAL_FIELD(config, ConfigStatusDto), RUVIA_OPTIONAL_FIELD(outbox, OutboxStatusDto), RUVIA_OPTIONAL_FIELD(log, LogStatusDto));

RUVIA_RESPONSE_MODEL(VpnCapabilityDto, RUVIA_OPTIONAL_FIELD_NAME("supportsVpn", supportsVpn, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("wireguardVersion", wireguardVersion, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("agentVersion", agentVersion, ruvia::String), RUVIA_OPTIONAL_FIELD(publicKey, ruvia::String));

RUVIA_RESPONSE_MODEL(CapabilityDto, RUVIA_OPTIONAL_FIELD(serialDebug, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("networkConfig", networkConfig, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("networkConfigVersion", networkConfigVersion, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("firmwareUpdate", firmwareUpdate, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("deviceConfig", deviceConfig, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("modemControl", modemControl, ruvia::Bool), RUVIA_OPTIONAL_FIELD(terminal, ruvia::Bool), RUVIA_OPTIONAL_FIELD(logs, ruvia::Bool), RUVIA_OPTIONAL_FIELD(vpn, VpnCapabilityDto));

RUVIA_REQUEST_MODEL(SerialDebugOpenRequest, RUVIA_REQUIRED_FIELD(path, ruvia::String, RUVIA_MAX(96, "串口路径过长")));

RUVIA_REQUEST_MODEL(SerialDebugCommandRequest, RUVIA_REQUIRED_FIELD(action, ruvia::String, RUVIA_ONE_OF("串口操作无效", "manual", "monitor", "write", "keepalive")), RUVIA_REQUIRED_FIELD(requestId, ruvia::Int64, RUVIA_MIN(2, "指令序号无效"), RUVIA_MAX(9007199254740991LL, "指令序号无效")), RUVIA_OPTIONAL_FIELD(baudRate, ruvia::Int64), RUVIA_OPTIONAL_FIELD(dataBits, ruvia::Int64), RUVIA_OPTIONAL_FIELD(stopBits, ruvia::Int64), RUVIA_OPTIONAL_FIELD(parity, ruvia::String), RUVIA_OPTIONAL_FIELD(rs485, ruvia::Bool), RUVIA_OPTIONAL_FIELD(hex, ruvia::String, RUVIA_MAX(2048, "每次最多发送 1024 字节")));

RUVIA_REQUEST_MODEL(NodeSerialOpenInput, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(path, ruvia::String, RUVIA_MAX(96, "串口路径过长")));

RUVIA_REQUEST_MODEL(TerminalOpenInput, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(columns, ruvia::Int64, RUVIA_MIN(1, "终端参数无效"), RUVIA_MAX(1000LL, "终端参数过大")), RUVIA_REQUIRED_FIELD(rows, ruvia::Int64, RUVIA_MIN(1, "终端参数无效"), RUVIA_MAX(1000LL, "终端参数过大")));
RUVIA_REQUEST_MODEL(TerminalSessionInput, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(sessionId, ruvia::String, RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)));
RUVIA_REQUEST_MODEL(TerminalResizeInput, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(sessionId, ruvia::String, RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(columns, ruvia::Int64, RUVIA_MIN(1, "终端参数无效"), RUVIA_MAX(1000LL, "终端参数过大")), RUVIA_REQUIRED_FIELD(rows, ruvia::Int64, RUVIA_MIN(1, "终端参数无效"), RUVIA_MAX(1000LL, "终端参数过大")));
RUVIA_REQUEST_MODEL(TerminalWriteInput, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(sessionId, ruvia::String, RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(content, ruvia::String, RUVIA_MIN(4, "终端内容为空"), RUVIA_MAX(21848, "终端输入过大")));
RUVIA_REQUEST_MODEL(TerminalAckInput, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(sessionId, ruvia::String, RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(sequence, ruvia::Int64, RUVIA_MIN(1, "终端参数无效"), RUVIA_MAX(9007199254740991LL, "终端参数过大")));

RUVIA_REQUEST_MODEL(NodeSerialSessionInput, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(sessionId, ruvia::String, RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)));

RUVIA_REQUEST_MODEL(NodeSerialCommandInput, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(sessionId, ruvia::String, RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)), RUVIA_REQUIRED_FIELD(command, SerialDebugCommandRequest));

RUVIA_RESPONSE_MODEL(SignalDto, RUVIA_OPTIONAL_FIELD(csq, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("rssiDbm", rssiDbm, ruvia::Int64), RUVIA_OPTIONAL_FIELD(percent, ruvia::Int64));

RUVIA_RESPONSE_MODEL(MobileDto, RUVIA_OPTIONAL_FIELD(available, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("simState", simState, ruvia::String), RUVIA_OPTIONAL_FIELD(iccid, ruvia::String), RUVIA_OPTIONAL_FIELD(signal, SignalDto), RUVIA_OPTIONAL_FIELD(registered, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("registrationStatus", registrationStatus, ruvia::Int64), RUVIA_OPTIONAL_FIELD(apn, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("operator", operatorName, ruvia::String), RUVIA_OPTIONAL_FIELD(connected, ruvia::Bool), RUVIA_OPTIONAL_FIELD(ipv4, ruvia::String));

RUVIA_RESPONSE_MODEL(FirmwareStatusDto, RUVIA_OPTIONAL_FIELD(state, ruvia::String), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("progressPercent", progressPercent, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("downloadedBytes", downloadedBytes, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("totalBytes", totalBytes, ruvia::Int64));

RUVIA_RESPONSE_MODEL(TaskDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("taskType", taskType, ruvia::String), RUVIA_OPTIONAL_FIELD(status, ruvia::String), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("progressPercent", progressPercent, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("downloadedBytes", downloadedBytes, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("totalBytes", totalBytes, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("updatedAt", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(EdgeNodeDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD(imei, ruvia::String), RUVIA_OPTIONAL_FIELD(name, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("groupId", groupId, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("groupName", groupName, ruvia::String), RUVIA_OPTIONAL_FIELD(model, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("softwareVersion", softwareVersion, ruvia::String), RUVIA_OPTIONAL_FIELD(hostname, ruvia::String), RUVIA_OPTIONAL_FIELD(architecture, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("openwrtRelease", openwrtRelease, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("enrollmentStatus", enrollmentStatus, ruvia::String), RUVIA_OPTIONAL_FIELD(status, NodeStatusDto), RUVIA_OPTIONAL_FIELD(capability, CapabilityDto), RUVIA_OPTIONAL_FIELD(mobile, MobileDto), RUVIA_OPTIONAL_FIELD(firmware, FirmwareStatusDto), RUVIA_OPTIONAL_FIELD_NAME("vpnVirtualCidrs", vpnVirtualCidrs, ruvia::BoxedArray<ruvia::String>), RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String), RUVIA_OPTIONAL_FIELD(interfaces, ruvia::BoxedArray<InterfaceDto>), RUVIA_OPTIONAL_FIELD(networks, ruvia::BoxedArray<NetworkDto>), RUVIA_OPTIONAL_FIELD_NAME("serialPorts", serialPorts, ruvia::BoxedArray<SerialDto>), RUVIA_OPTIONAL_FIELD(tasks, ruvia::BoxedArray<TaskDto>));

RUVIA_RESPONSE_MODEL(EdgeGroupDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD(name, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("parentId", parentId, ruvia::String), RUVIA_OPTIONAL_FIELD(status, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("sortOrder", sortOrder, ruvia::Int64), RUVIA_OPTIONAL_FIELD(remark, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("nodeCount", nodeCount, ruvia::Int64));

RUVIA_RESPONSE_MODEL(EdgePageDto, RUVIA_OPTIONAL_FIELD(list, ruvia::BoxedArray<EdgeNodeDto>), RUVIA_OPTIONAL_FIELD(total, ruvia::Int64), RUVIA_OPTIONAL_FIELD(page, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(FirmwareDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD(version, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("fileName", fileName, ruvia::String), RUVIA_OPTIONAL_FIELD(sha256, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("sizeBytes", sizeBytes, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("createdAt", createdAt, ruvia::String));

RUVIA_RESPONSE_MODEL(LogLineDto, RUVIA_OPTIONAL_FIELD(time, ruvia::String), RUVIA_OPTIONAL_FIELD(level, ruvia::String), RUVIA_OPTIONAL_FIELD(source, ruvia::String), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(detail, ruvia::String));

RUVIA_RESPONSE_MODEL(LogsDto, RUVIA_OPTIONAL_FIELD(lines, ruvia::BoxedArray<LogLineDto>));

RUVIA_RESPONSE_MODEL(LogsResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, LogsDto));

RUVIA_RESPONSE_MODEL(FirmwareListResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, ruvia::BoxedArray<FirmwareDto>));

RUVIA_RESPONSE_MODEL(EdgeGroupsResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, ruvia::BoxedArray<EdgeGroupDto>));

RUVIA_RESPONSE_MODEL(EdgeNodeResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, EdgeNodeDto));

RUVIA_RESPONSE_MODEL(EdgePageResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, EdgePageDto));

} // namespace service::edge
