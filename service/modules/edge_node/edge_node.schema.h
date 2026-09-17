#pragma once

#include <cctype>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/modules/edge_node/edge_node.types.h"
#include "service/utils/json.h"

namespace service::edge {

class DebugAuthenticationValidator final : public ruvia::Middleware<DebugAuthenticationValidator> {
  public:
    RUVIA_VALIDATE_JSON(DebugAuthenticationBody, RUVIA_RULE(token, RUVIA_MIN(1, "令牌不能为空")))
};

class SerialDebugOpenValidator final : public ruvia::Middleware<SerialDebugOpenValidator> {
  public:
    RUVIA_VALIDATE_JSON(SerialDebugOpenRequest, RUVIA_RULE(path, RUVIA_REQUIRED("请选择串口"), RUVIA_MAX(96, "串口路径过长")));
};

class SerialDebugCommandValidator final : public ruvia::Middleware<SerialDebugCommandValidator> {
  public:
    RUVIA_VALIDATE_JSON(SerialDebugCommandRequest, RUVIA_RULE(action, RUVIA_REQUIRED("请选择串口操作"), RUVIA_ONE_OF("串口操作无效", "manual", "monitor", "write", "keepalive")), RUVIA_RULE(requestId, RUVIA_REQUIRED("缺少指令序号"), RUVIA_MIN(2, "指令序号无效"), RUVIA_MAX(9007199254740991LL, "指令序号无效")), RUVIA_RULE(hex, RUVIA_MAX(2048, "每次最多发送 1024 字节")));
};

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

class EdgeEventsValidator final : public ruvia::Middleware<EdgeEventsValidator> {
  public:
    RUVIA_VALIDATE_QUERY(EdgeEventsQuery, RUVIA_RULE(nodeId, RUVIA_CUSTOM("nodeId 必须是 UUID", service::common::isUuidField)));
};

class EdgeListValidator final : public ruvia::Middleware<EdgeListValidator> {
  public:
    RUVIA_VALIDATE_QUERY(
        EdgeListQuery,
        RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
        RUVIA_RULE_NAME("pageSize", pageSize, RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
        RUVIA_RULE(status, RUVIA_ONE_OF("注册状态无效", "pending", "approved")),
        RUVIA_RULE_NAME("groupId", groupId, RUVIA_CUSTOM("节点分组筛选无效", isEdgeGroupFilter))
    );
};

class EdgeIdValidator final : public ruvia::Middleware<EdgeIdValidator> {
  public:
    RUVIA_VALIDATE_PARAM(EdgeIdParams, RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"), RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));
};

class EnrollmentValidator final : public ruvia::Middleware<EnrollmentValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        EnrollmentBody,
        RUVIA_RULE(status, RUVIA_REQUIRED("注册状态不能为空"), RUVIA_ONE_OF("注册状态无效", "approved")),
        RUVIA_RULE(name, RUVIA_MAX(100, "节点名称不能超过 100 个字符"))
    );
};

class NodeNameValidator final : public ruvia::Middleware<NodeNameValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        NodeNameBody,
        RUVIA_RULE(name, RUVIA_REQUIRED("节点名称不能为空"), RUVIA_MAX(100, "节点名称不能超过 100 个字符"))
    );
};

class NodeGroupValidator final : public ruvia::Middleware<NodeGroupValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        NodeGroupBody,
        RUVIA_RULE_NAME("groupId", groupId, RUVIA_CUSTOM("节点分组 ID 无效", service::common::isOptionalUuidField))
    );
};

class EdgeGroupValidator final : public ruvia::Middleware<EdgeGroupValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        EdgeGroupBody,
        RUVIA_RULE(name, RUVIA_REQUIRED("分组名称不能为空"), RUVIA_MIN(1, "分组名称不能为空"), RUVIA_MAX(100, "分组名称不能超过 100 个字符")),
        RUVIA_RULE_NAME("parentId", parentId, RUVIA_CUSTOM("上级分组 ID 无效", service::common::isOptionalUuidField)),
        RUVIA_RULE(status, RUVIA_ONE_OF("分组状态无效", "enabled", "disabled")),
        RUVIA_RULE_NAME("sortOrder", sortOrder, RUVIA_MIN(0, "分组排序不能小于 0")),
        RUVIA_RULE(remark, RUVIA_MAX(500, "分组备注不能超过 500 个字符"))
    );
};

class NetworkInterfaceValidator final : public ruvia::Middleware<NetworkInterfaceValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        NetworkInterfaceBody,
        RUVIA_RULE(operation, RUVIA_REQUIRED("网络操作不能为空"), RUVIA_ONE_OF("网络操作无效", "upsert", "delete")),
        RUVIA_RULE(name, RUVIA_REQUIRED("逻辑接口名称不能为空"), RUVIA_CUSTOM("逻辑接口名称只能包含字母、数字和下划线", isUciSectionName)),
        RUVIA_RULE_NAME("previousName", previousName, RUVIA_CUSTOM("原逻辑接口名称只能包含字母、数字和下划线", isUciSectionName)),
        RUVIA_RULE(mode, RUVIA_ONE_OF("地址协议无效", "dhcp", "static")),
        RUVIA_RULE(device, RUVIA_CUSTOM("设备名称包含非法字符", isOptionalNetworkDevice)),
        RUVIA_RULE_NAME("bridgePorts", bridgePorts, RUVIA_MAX(8, "网桥最多包含 8 个成员")),
        RUVIA_RULE(ip, RUVIA_MAX(15, "IPv4 地址格式无效")),
        RUVIA_RULE_NAME("prefixLength", prefixLength, RUVIA_MIN(0, "IPv4 前缀必须在 0 - 30 之间"), RUVIA_MAX(30, "IPv4 前缀必须在 0 - 30 之间")),
        RUVIA_RULE(gateway, RUVIA_MAX(15, "网关格式无效"))
    );
};

class NetworkValidator final : public ruvia::Middleware<NetworkValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        NetworkBody,
        RUVIA_RULE(interfaces, RUVIA_REQUIRED("网络配置不能为空"), RUVIA_MIN(1, "请至少配置一个逻辑接口"), RUVIA_MAX(8, "单次最多配置 8 个逻辑接口"), RUVIA_EACH(NetworkInterfaceValidator)),
        RUVIA_RULE_NAME("rollbackTimeoutSec", rollbackTimeoutSec, RUVIA_MIN(30, "回滚等待时间必须在 30 - 300 秒之间"), RUVIA_MAX(300, "回滚等待时间必须在 30 - 300 秒之间"))
    );
};

class FirmwareUploadValidator final : public ruvia::Middleware<FirmwareUploadValidator> {
  public:
    RUVIA_VALIDATE_QUERY(FirmwareUploadBody, RUVIA_RULE_NAME("fileName", fileName, RUVIA_REQUIRED("固件文件名不能为空"), RUVIA_MIN(1, "固件文件名不能为空"), RUVIA_MAX(255, "固件文件名过长")), RUVIA_RULE_NAME("sizeBytes", sizeBytes, RUVIA_REQUIRED("固件大小不能为空"), RUVIA_MIN(1, "固件文件不能为空"), RUVIA_MAX(134217728, "固件不能超过 128 MiB")));
};

class FirmwareDownloadValidator final : public ruvia::Middleware<FirmwareDownloadValidator> {
  public:
    RUVIA_VALIDATE_QUERY(FirmwareDownloadQuery, RUVIA_RULE(token, RUVIA_REQUIRED("下载凭据不能为空"), RUVIA_MAX(64, "下载凭据无效")));
};

class LogsValidator final : public ruvia::Middleware<LogsValidator> {
  public:
    RUVIA_VALIDATE_QUERY(
        LogsQuery,
        RUVIA_RULE(limit, RUVIA_MIN(1, "日志条数必须在 1 - 48 之间"), RUVIA_MAX(48, "日志条数必须在 1 - 48 之间")),
        RUVIA_RULE(level, RUVIA_ONE_OF("日志级别无效", "debug", "info", "warn", "error")),
        RUVIA_RULE(source, RUVIA_MAX(16, "日志来源不能超过 16 个字符"))
    );
};

class LogLevelValidator final : public ruvia::Middleware<LogLevelValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        LogLevelBody,
        RUVIA_RULE(level, RUVIA_REQUIRED("日志级别不能为空"), RUVIA_ONE_OF("日志级别无效", "debug", "info", "warn", "error"))
    );
};

// The node ID belongs to the event envelope data, independently of its typed
// business configuration. Never construct an HTTP request to validate it.
template <typename Input, typename Configuration>
std::optional<Input> parseNodeInput(std::string_view raw, std::pmr::memory_resource* resource, bool nested) {
    const auto object = ruvia::JsonValue::parse(raw);
    if (!object || !object->isObject()) {
        return std::nullopt;
    }
    const auto id = object->get<ruvia::String>("id");
    if (!id || !service::common::isUuid(id->view())) {
        return std::nullopt;
    }
    auto configuration = raw;
    std::optional<ruvia::JsonValue> nestedValue;
    if (nested) {
        nestedValue = service::utils::jsonField(*object, "configuration");
        if (!nestedValue || !nestedValue->isObject()) {
            return std::nullopt;
        }
        configuration = nestedValue->view();
    }
    auto parsed = ruvia::fromJson<Configuration>(configuration, { .resource = resource });
    if (!parsed) {
        return std::nullopt;
    }
    return Input{ std::string(id->view()), std::move(*parsed) };
}

class NodeSerialOpenValidator final {
  public:
    using RuviaValidationBody = NodeSerialOpenInput;

    static std::optional<NodeSerialOpenInput> parse(std::string_view raw, std::pmr::memory_resource* resource) {
        return parseNodeInput<NodeSerialOpenInput, SerialDebugOpenRequest>(raw, resource, false);
    }

    void validate(const NodeSerialOpenInput& body, ruvia::Validator& validation) const {
        SerialDebugOpenValidator validator;
        validator.validate(body.configuration, validation);
    }
};

class NodeSerialSessionValidator final : public ruvia::Middleware<NodeSerialSessionValidator> {
  public:
    RUVIA_VALIDATE_JSON(NodeSerialSessionInput, RUVIA_RULE(id, RUVIA_REQUIRED("缺少节点编号"), RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_RULE(sessionId, RUVIA_REQUIRED("缺少会话编号"), RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)));
};

class NodeSerialCommandValidator final {
  public:
    using RuviaValidationBody = NodeSerialCommandInput;

    static std::optional<NodeSerialCommandInput> parse(std::string_view raw, std::pmr::memory_resource* resource) {
        const auto object = ruvia::JsonValue::parse(raw);
        if (!object || !object->isObject()) {
            return std::nullopt;
        }
        const auto id = object->get<ruvia::String>("id");
        const auto session = object->get<ruvia::String>("sessionId");
        const auto command = service::utils::jsonField(*object, "command");
        if (!id || !session || !service::common::isUuid(id->view()) || !service::common::isUuid(session->view()) || !command || command->view().size() > 4096) {
            return std::nullopt;
        }
        auto parsed = ruvia::fromJson<SerialDebugCommandRequest>(command->view(), { .resource = resource });
        if (!parsed) {
            return std::nullopt;
        }
        return NodeSerialCommandInput{ std::string(id->view()), std::string(session->view()), std::move(*parsed), std::string(command->view()) };
    }

    void validate(const NodeSerialCommandInput& body, ruvia::Validator& validation) const {
        SerialDebugCommandValidator validator;
        validator.validate(body.command, validation);
    }
};

class TerminalOpenValidator final : public ruvia::Middleware<TerminalOpenValidator> {
  public:
    RUVIA_VALIDATE_JSON(TerminalOpenInput, RUVIA_RULE(id, RUVIA_REQUIRED("缺少节点编号"), RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_RULE(columns, RUVIA_REQUIRED("缺少终端参数"), RUVIA_MIN(1, "终端参数无效"), RUVIA_MAX(1000LL, "终端参数过大")), RUVIA_RULE(rows, RUVIA_REQUIRED("缺少终端参数"), RUVIA_MIN(1, "终端参数无效"), RUVIA_MAX(1000LL, "终端参数过大")));
};

class TerminalSessionValidator final : public ruvia::Middleware<TerminalSessionValidator> {
  public:
    RUVIA_VALIDATE_JSON(TerminalSessionInput, RUVIA_RULE(id, RUVIA_REQUIRED("缺少节点编号"), RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_RULE(sessionId, RUVIA_REQUIRED("缺少终端参数"), RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)));
};

class TerminalResizeValidator final : public ruvia::Middleware<TerminalResizeValidator> {
  public:
    RUVIA_VALIDATE_JSON(TerminalResizeInput, RUVIA_RULE(id, RUVIA_REQUIRED("缺少节点编号"), RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_RULE(sessionId, RUVIA_REQUIRED("缺少终端参数"), RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)), RUVIA_RULE(columns, RUVIA_REQUIRED("缺少终端参数"), RUVIA_MIN(1, "终端参数无效"), RUVIA_MAX(1000LL, "终端参数过大")), RUVIA_RULE(rows, RUVIA_REQUIRED("缺少终端参数"), RUVIA_MIN(1, "终端参数无效"), RUVIA_MAX(1000LL, "终端参数过大")));
};

class TerminalWriteValidator final : public ruvia::Middleware<TerminalWriteValidator> {
  public:
    RUVIA_VALIDATE_JSON(TerminalWriteInput, RUVIA_RULE(id, RUVIA_REQUIRED("缺少节点编号"), RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_RULE(sessionId, RUVIA_REQUIRED("缺少终端参数"), RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)), RUVIA_RULE(content, RUVIA_REQUIRED("缺少终端参数"), RUVIA_MIN(4, "终端内容为空"), RUVIA_MAX(21848, "终端输入过大")));
};

class TerminalAckValidator final : public ruvia::Middleware<TerminalAckValidator> {
  public:
    RUVIA_VALIDATE_JSON(TerminalAckInput, RUVIA_RULE(id, RUVIA_REQUIRED("缺少节点编号"), RUVIA_CUSTOM("节点编号必须是 UUID", service::common::isUuidField)), RUVIA_RULE(sessionId, RUVIA_REQUIRED("缺少终端参数"), RUVIA_CUSTOM("会话编号必须是 UUID", service::common::isUuidField)), RUVIA_RULE(sequence, RUVIA_REQUIRED("缺少终端参数"), RUVIA_MIN(1, "终端参数无效"), RUVIA_MAX(9007199254740991LL, "终端参数过大")));
};

} // namespace service::edge
