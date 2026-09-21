#pragma once

#include "service/common/uuid.h"

#include <ruvia/web/Validation.h>

namespace service::link {
RUVIA_MODEL(LinkDebugPacketDto, RUVIA_REQUIRED_FIELD_NAME("acquisition_id", acquisitionId, ruvia::String), RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String), RUVIA_OPTIONAL_FIELD(direction, ruvia::String), RUVIA_OPTIONAL_FIELD(source, ruvia::String), RUVIA_OPTIONAL_FIELD(address, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("edge_node_id", edgeNodeId, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("edge_node_name", edgeNodeName, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("payload_hex", payloadHex, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("time_ms", timeMs, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("transport_status", transportStatus, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("response_status", responseStatus, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("parse_status", parseStatus, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("revision", revision, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("reply_to_packet_id", replyToPacketId, ruvia::String), RUVIA_OPTIONAL_FIELD(reason, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("parsed_json", parsedJson, ruvia::String));
RUVIA_MODEL(LinkDebugAcquisitionDto, RUVIA_REQUIRED_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("started_at_ms", startedAtMs, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("finished_at_ms", finishedAtMs, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("last_packet_at_ms", lastPacketAtMs, ruvia::String), RUVIA_OPTIONAL_FIELD(state, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String), RUVIA_OPTIONAL_FIELD(packets, ruvia::BoxedArray<LinkDebugPacketDto>));

RUVIA_MODEL(LinkDebugBody, RUVIA_REQUIRED_FIELD(enabled, ruvia::Bool));

RUVIA_MODEL(LinkTargetBody, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_MAX(64, "目标 ID 不能超过 64 个字符")), RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MAX(100, "目标名称不能超过 100 个字符")), RUVIA_REQUIRED_FIELD(ip, ruvia::String, RUVIA_MAX(50, "目标 IP 不能超过 50 个字符")), RUVIA_REQUIRED_FIELD(port, ruvia::Int64, RUVIA_MIN(1, "目标端口必须在 1 - 65535 之间"), RUVIA_MAX(65535, "目标端口必须在 1 - 65535 之间")), RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("目标状态无效", "enabled", "disabled")));

RUVIA_MODEL(LinkEndpointBody, RUVIA_OPTIONAL_FIELD(transport, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("interface", interfaceName, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("baud_rate", baudRate, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("data_bits", dataBits, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("stop_bits", stopBits, ruvia::Int64), RUVIA_OPTIONAL_FIELD(parity, ruvia::String), RUVIA_OPTIONAL_FIELD(rs485, ruvia::Bool),

                    RUVIA_OPTIONAL_FIELD(mode, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(ip, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(targets, ruvia::Array<LinkTargetBody>));

RUVIA_MODEL(SaveLinkBody, RUVIA_OPTIONAL_FIELD(execution, ruvia::String, RUVIA_ONE_OF("采集位置无效", "collector", "edge")), RUVIA_OPTIONAL_FIELD_NAME("edge_node_id", edgeNodeId, ruvia::String), RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(1, "链路名称不能为空"), RUVIA_MAX(100, "链路名称不能超过 100 个字符")), RUVIA_REQUIRED_FIELD(protocol, ruvia::String, RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7", "MC", "FINS", "DLT645")), RUVIA_REQUIRED_FIELD(endpoint, LinkEndpointBody), RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")));

RUVIA_MODEL(LinkEventsQuery, RUVIA_OPTIONAL_FIELD_NAME("debugLinkId", debugLinkId, ruvia::String, RUVIA_CUSTOM("调试链路 ID 必须是 UUID", service::common::isUuidField)));

RUVIA_MODEL(LinkListQuery, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")), RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10), RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")), RUVIA_OPTIONAL_FIELD(keyword, ruvia::String), RUVIA_OPTIONAL_FIELD(mode, ruvia::String, RUVIA_ONE_OF("链路模式无效", "TCP Server", "TCP Client")), RUVIA_OPTIONAL_FIELD(protocol, ruvia::String, RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7", "MC", "FINS", "DLT645")), RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")));

RUVIA_MODEL(LinkIdParams, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));

RUVIA_MODEL(RuntimeDto, RUVIA_OPTIONAL_FIELD(state, ruvia::String), RUVIA_OPTIONAL_FIELD(reason, ruvia::String), RUVIA_OPTIONAL_FIELD(error, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("clientCount", clientCount, ruvia::Int64), RUVIA_OPTIONAL_FIELD(clients, ruvia::BoxedArray<ruvia::String>), RUVIA_OPTIONAL_FIELD_NAME("lastActivityAt", lastActivityAt, ruvia::String));

RUVIA_MODEL(LinkTargetDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD(name, ruvia::String), RUVIA_OPTIONAL_FIELD(ip, ruvia::String), RUVIA_OPTIONAL_FIELD(port, ruvia::Int64), RUVIA_OPTIONAL_FIELD(status, ruvia::String), RUVIA_OPTIONAL_FIELD(runtime, RuntimeDto));

RUVIA_MODEL(LinkEndpointDto, RUVIA_OPTIONAL_FIELD(transport, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("interface", interfaceName, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("baud_rate", baudRate, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("data_bits", dataBits, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("stop_bits", stopBits, ruvia::Int64), RUVIA_OPTIONAL_FIELD(parity, ruvia::String), RUVIA_OPTIONAL_FIELD(rs485, ruvia::Bool),

                     RUVIA_OPTIONAL_FIELD(mode, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(ip, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(port, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(targets, ruvia::BoxedArray<LinkTargetDto>));

RUVIA_MODEL(LinkItemDto, RUVIA_OPTIONAL_FIELD_NAME("debug_enabled", debugEnabled, ruvia::Bool), RUVIA_OPTIONAL_FIELD(execution, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("edge_node_id", edgeNodeId, ruvia::String), RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD(name, ruvia::String), RUVIA_OPTIONAL_FIELD(protocol, ruvia::String), RUVIA_OPTIONAL_FIELD(endpoint, LinkEndpointDto), RUVIA_OPTIONAL_FIELD(status, ruvia::String), RUVIA_OPTIONAL_FIELD(runtime, RuntimeDto), RUVIA_OPTIONAL_FIELD_NAME("created_by", createdBy, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_MODEL(LinkOptionDto, RUVIA_OPTIONAL_FIELD(execution, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("edge_node_id", edgeNodeId, ruvia::String), RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD(name, ruvia::String), RUVIA_OPTIONAL_FIELD(protocol, ruvia::String), RUVIA_OPTIONAL_FIELD(endpoint, LinkEndpointDto));

RUVIA_MODEL(LinkEnumsDto, RUVIA_OPTIONAL_FIELD(modes, ruvia::BoxedArray<ruvia::String>), RUVIA_OPTIONAL_FIELD(protocols, ruvia::BoxedArray<ruvia::String>), RUVIA_OPTIONAL_FIELD(statuses, ruvia::BoxedArray<ruvia::String>));

RUVIA_MODEL(LinkPageDataDto, RUVIA_OPTIONAL_FIELD(list, ruvia::BoxedArray<LinkItemDto>), RUVIA_OPTIONAL_FIELD(total, ruvia::Int64), RUVIA_OPTIONAL_FIELD(page, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64), RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_MODEL(PublicIpDto, RUVIA_OPTIONAL_FIELD(ip, ruvia::String));

RUVIA_MODEL(LinkDebugPacketsResponse, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<LinkDebugAcquisitionDto>));
RUVIA_MODEL(LinkPageResponse, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(data, LinkPageDataDto));
RUVIA_MODEL(LinkDetailResponse, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(data, LinkItemDto));
RUVIA_MODEL(LinkOptionsResponse, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<LinkOptionDto>));
RUVIA_MODEL(LinkEnumsResponse, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(data, LinkEnumsDto));
RUVIA_MODEL(PublicIpResponse, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(data, PublicIpDto));
} // namespace service::link
