#pragma once

#include <cctype>

#include <ruvia/web/Controller.h>

#include "service/common/id.validation.h"
#include "service/domains/edge/edge.types.h"

namespace service::edge {

inline bool isUciSectionName(const ruvia::String& value) {
    const auto text = value.view();
    if (text.empty() || text.size() > 15)
        return false;
    for (const unsigned char character : text)
        if (!std::isalnum(character) && character != '_')
            return false;
    return true;
}

inline bool isOptionalNetworkDevice(const ruvia::String& value) {
    const auto text = value.view();
    if (text.size() > 32)
        return false;
    for (const unsigned char character : text)
        if (!std::isalnum(character) && character != '_' && character != '-' &&
            character != '.' && character != ':')
            return false;
    return true;
}

class EdgeListValidator final : public ruvia::Middleware<EdgeListValidator> {
  public:
    RUVIA_VALIDATE_QUERY(
        EdgeListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
        RUVIA_RULE_NAME("pageSize", pageSize, RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"),
                        RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
        RUVIA_RULE(status, RUVIA_ONE_OF("注册状态无效", "pending", "approved")));
};

class EdgeIdValidator final : public ruvia::Middleware<EdgeIdValidator> {
  public:
    RUVIA_VALIDATE_PARAM(EdgeIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));
};

class EnrollmentValidator final : public ruvia::Middleware<EnrollmentValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        EnrollmentBody,
        RUVIA_RULE(status, RUVIA_REQUIRED("注册状态不能为空"),
                   RUVIA_ONE_OF("注册状态无效", "approved")),
        RUVIA_RULE(name, RUVIA_MAX(100, "节点名称不能超过 100 个字符")));
};

class NodeNameValidator final : public ruvia::Middleware<NodeNameValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        NodeNameBody, RUVIA_RULE(name, RUVIA_REQUIRED("节点名称不能为空"),
                                 RUVIA_MAX(100, "节点名称不能超过 100 个字符")));
};

class NetworkInterfaceValidator final : public ruvia::Middleware<NetworkInterfaceValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        NetworkInterfaceBody,
        RUVIA_RULE(operation, RUVIA_REQUIRED("网络操作不能为空"),
                   RUVIA_ONE_OF("网络操作无效", "upsert", "delete")),
        RUVIA_RULE(name, RUVIA_REQUIRED("逻辑接口名称不能为空"),
                   RUVIA_CUSTOM("逻辑接口名称只能包含字母、数字和下划线",
                                isUciSectionName)),
        RUVIA_RULE_NAME("previousName", previousName,
                        RUVIA_CUSTOM("原逻辑接口名称只能包含字母、数字和下划线",
                                     isUciSectionName)),
        RUVIA_RULE(mode, RUVIA_ONE_OF("地址协议无效", "dhcp", "static")),
        RUVIA_RULE(device, RUVIA_CUSTOM("设备名称包含非法字符", isOptionalNetworkDevice)),
        RUVIA_RULE_NAME("bridgePorts", bridgePorts, RUVIA_MAX(8, "网桥最多包含 8 个成员")),
        RUVIA_RULE(ip, RUVIA_MAX(15, "IPv4 地址格式无效")),
        RUVIA_RULE_NAME("prefixLength", prefixLength,
                        RUVIA_MIN(0, "IPv4 前缀必须在 0 - 30 之间"),
                        RUVIA_MAX(30, "IPv4 前缀必须在 0 - 30 之间")),
        RUVIA_RULE(gateway, RUVIA_MAX(15, "网关格式无效")));
};

class NetworkValidator final : public ruvia::Middleware<NetworkValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        NetworkBody,
        RUVIA_RULE(interfaces, RUVIA_REQUIRED("网络配置不能为空"),
                   RUVIA_MIN(1, "请至少配置一个逻辑接口"),
                   RUVIA_MAX(8, "单次最多配置 8 个逻辑接口"),
                   RUVIA_EACH(NetworkInterfaceValidator)),
        RUVIA_RULE_NAME("rollbackTimeoutSec", rollbackTimeoutSec,
                        RUVIA_MIN(30, "回滚等待时间必须在 30 - 300 秒之间"),
                        RUVIA_MAX(300, "回滚等待时间必须在 30 - 300 秒之间")));
};

class FirmwareDownloadValidator final : public ruvia::Middleware<FirmwareDownloadValidator> {
  public:
    RUVIA_VALIDATE_QUERY(FirmwareDownloadQuery,
                         RUVIA_RULE(token, RUVIA_REQUIRED("下载凭据不能为空"),
                                    RUVIA_MAX(64, "下载凭据无效")));
};

class TerminalTicketValidator final : public ruvia::Middleware<TerminalTicketValidator> {
  public:
    RUVIA_VALIDATE_QUERY(TerminalTicketQuery,
                         RUVIA_RULE(ticket, RUVIA_REQUIRED("终端票据不能为空"),
                                    RUVIA_CUSTOM("终端票据无效", service::common::isUuidField)));
};

class LogsValidator final : public ruvia::Middleware<LogsValidator> {
  public:
    RUVIA_VALIDATE_QUERY(
        LogsQuery,
        RUVIA_RULE(limit, RUVIA_MIN(1, "日志条数必须在 1 - 48 之间"),
                   RUVIA_MAX(48, "日志条数必须在 1 - 48 之间")),
        RUVIA_RULE(level, RUVIA_ONE_OF("日志级别无效", "debug", "info", "warn", "error")),
        RUVIA_RULE(source, RUVIA_MAX(16, "日志来源不能超过 16 个字符")));
};

class LogLevelValidator final : public ruvia::Middleware<LogLevelValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        LogLevelBody,
        RUVIA_RULE(level, RUVIA_REQUIRED("日志级别不能为空"),
                   RUVIA_ONE_OF("日志级别无效", "debug", "info", "warn", "error")));
};

} // namespace service::edge
