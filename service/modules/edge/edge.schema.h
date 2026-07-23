#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/id.validation.h"
#include "service/modules/edge/edge.types.h"

namespace service::edge {

class EdgeListValidator final : public ruvia::Middleware<EdgeListValidator> {
  public:
    RUVIA_VALIDATE_QUERY(
        EdgeListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
        RUVIA_RULE_NAME("pageSize", pageSize, RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"),
                        RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
        RUVIA_RULE(status, RUVIA_ONE_OF("注册状态无效", "pending", "approved", "rejected")));
};

class EdgeIdValidator final : public ruvia::Middleware<EdgeIdValidator> {
  public:
    RUVIA_VALIDATE_PARAM(EdgeIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));
};

class EdgePlatformParamsValidator final : public ruvia::Middleware<EdgePlatformParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(
        EdgePlatformParams,
        RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                   RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)),
        RUVIA_RULE_NAME("platformId", platformId, RUVIA_REQUIRED("platformId 不能为空"),
                        RUVIA_CUSTOM("platformId 必须是 UUID", service::common::isUuidField)));
};

class EnrollmentValidator final : public ruvia::Middleware<EnrollmentValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        EnrollmentBody,
        RUVIA_RULE(status, RUVIA_REQUIRED("注册状态不能为空"),
                   RUVIA_ONE_OF("注册状态无效", "approved", "rejected")),
        RUVIA_RULE(name, RUVIA_MAX(100, "节点名称不能超过 100 个字符")));
};

class NodeNameValidator final : public ruvia::Middleware<NodeNameValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        NodeNameBody, RUVIA_RULE(name, RUVIA_REQUIRED("节点名称不能为空"),
                                 RUVIA_MAX(100, "节点名称不能超过 100 个字符")));
};

class NetworkValidator final : public ruvia::Middleware<NetworkValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        NetworkBody, RUVIA_RULE(ip, RUVIA_REQUIRED("br-lan IP 不能为空"),
                                RUVIA_MAX(15, "br-lan IP 格式无效")),
        RUVIA_RULE(netmask, RUVIA_REQUIRED("br-lan 掩码不能为空"),
                   RUVIA_MAX(15, "br-lan 掩码格式无效")),
        RUVIA_RULE(gateway, RUVIA_MAX(15, "网关格式无效")),
        RUVIA_RULE_NAME("rollbackTimeoutSec", rollbackTimeoutSec,
                        RUVIA_MIN(30, "回滚等待时间必须在 30 - 300 秒之间"),
                        RUVIA_MAX(300, "回滚等待时间必须在 30 - 300 秒之间")));
};

class PlatformValidator final : public ruvia::Middleware<PlatformValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        PlatformBody,
        RUVIA_RULE_NAME("platformId", platformId,
                        RUVIA_CUSTOM("平台 ID 必须是 UUID", service::common::isUuidField)),
        RUVIA_RULE(name, RUVIA_REQUIRED("平台名称不能为空"),
                   RUVIA_MAX(32, "平台名称不能超过 32 个字符")),
        RUVIA_RULE_NAME("baseUrl", baseUrl, RUVIA_REQUIRED("平台地址不能为空"),
                        RUVIA_MAX(255, "平台地址不能超过 255 个字符")),
        RUVIA_RULE_NAME("enrollmentToken", enrollmentToken,
                        RUVIA_MAX(192, "注册凭据不能超过 192 个字符")),
        RUVIA_RULE(priority, RUVIA_MIN(0, "优先级必须在 0 - 65535 之间"),
                   RUVIA_MAX(65535, "优先级必须在 0 - 65535 之间")),
        RUVIA_RULE_NAME("reconnectIntervalSec", reconnectIntervalSec,
                        RUVIA_MIN(1, "重连间隔必须在 1 - 3600 秒之间"),
                        RUVIA_MAX(3600, "重连间隔必须在 1 - 3600 秒之间")),
        RUVIA_RULE_NAME("outboxMaxBytes", outboxMaxBytes,
                        RUVIA_MIN(16384, "缓存上限必须在 16 KiB - 8 MiB 之间"),
                        RUVIA_MAX(8388608, "缓存上限必须在 16 KiB - 8 MiB 之间")));
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

} // namespace service::edge
