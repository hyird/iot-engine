#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <system_error>
#include <asio/ip/address_v4.hpp>
#include <ruvia/web/Controller.h>

#include "service/modules/link/link.types.h"
#include "service/common/http.h"

namespace service::link {

class LinkDebugValidator final : public ruvia::Middleware<LinkDebugValidator> {
  public:
    RUVIA_VALIDATE_JSON(LinkDebugBody, RUVIA_RULE(enabled, RUVIA_REQUIRED("必须指定调试开关")))
};

class LinkPayloadValidator final {
  public:
    static std::string required(const std::optional<ruvia::String>& value,
                                std::string_view message) {
        if (!value || value->view().empty())
            service::common::fail(15002, std::string(message), 400);
        return std::string(value->view());
    }

    static const LinkEndpointBody& requiredEndpoint(const SaveLinkBody& body) {
        if (!body.get<"endpoint">())
            service::common::fail(15002, "链路端点不能为空", 400);
        return *body.get<"endpoint">();
    }

    static const ruvia::Array<LinkTargetBody>& requiredTargets(const LinkEndpointBody& endpoint) {
        if (!endpoint.get<"targets">())
            service::common::fail(15002, "目标列表不能为空", 400);
        return *endpoint.get<"targets">();
    }

    static void validateStatus(std::string_view status) {
        if (status != "enabled" && status != "disabled")
            service::common::fail(15002, "状态无效", 400);
    }

    static void validateNodeId(std::string_view nodeId) {
        if (!service::common::isUuid(nodeId)) service::common::fail(15002, "节点 ID 无效", 400);
    }

    static void validateEdgeEndpoint(const LinkEndpointBody& endpoint, std::string_view protocol,
                                     std::string_view transport, std::string_view interfaceName) {
        if (interfaceName.size() > 96) service::common::fail(15002, "接口名称过长", 400);
        if (transport == "serial") {
            if (protocol == "S7") service::common::fail(15002, "S7 不支持串口", 400);
            const auto baud = endpoint.get<"baudRate">().value_or(9600);
            const auto bits = endpoint.get<"dataBits">().value_or(8);
            const auto stops = endpoint.get<"stopBits">().value_or(1);
            const auto parity = endpoint.get<"parity">() ? endpoint.get<"parity">()->view() : std::string_view("none");
            if (baud < 300 || baud > 4000000 || bits < 5 || bits > 8 || stops < 1 || stops > 2 ||
                (parity != "none" && parity != "odd" && parity != "even"))
                service::common::fail(15002, "串口参数无效", 400);
        } else if (transport == "tcp") {
            const auto mode = required(endpoint.get<"mode">(), "请选择 TCP 模式");
            const auto ip = required(endpoint.get<"ip">(), "请输入 IP 地址");
            std::error_code error;
            (void)asio::ip::make_address_v4(ip, error);
            const auto port = endpoint.get<"port">().value_or(0);
            if (error || port < 1 || port > 65535 || (mode != "TCP Client" && mode != "TCP Server"))
                service::common::fail(15002, "TCP 参数无效", 400);
        } else service::common::fail(15002, "传输类型无效", 400);
    }

    template <typename Targets>
    static void validateConfiguration(std::string_view mode, std::string_view protocol,
                                      std::string_view ip, std::int64_t port,
                                      const Targets& targets) {
        if (mode != "TCP Server" && mode != "TCP Client")
            service::common::fail(15003, "链路模式无效", 400);
        if (protocol != "SL651" && protocol != "Modbus" && protocol != "S7")
            service::common::fail(15003, "协议无效", 400);
        if (protocol == "SL651" && mode != "TCP Server")
            service::common::fail(15003, "SL651 只支持 TCP Server 模式", 400);
        if (mode == "TCP Server") {
            if (ip != "0.0.0.0")
                service::common::fail(15003, "TCP Server 监听 IP 必须是 0.0.0.0", 400);
            if (port < 1 || port > 65535)
                service::common::fail(15003, "TCP Server 必须配置有效的监听端口", 400);
            if (!targets.empty())
                service::common::fail(15003, "TCP Server 不能配置目标地址", 400);
            return;
        }
        if (!ip.empty() || port != 0)
            service::common::fail(15003, "TCP Client 不能配置监听地址", 400);
        if (targets.empty())
            service::common::fail(15003, "TCP Client 至少需要一个目标地址", 400);
        std::set<std::string> ids;
        std::set<std::string> endpoints;
        for (const auto& target : targets) {
            const auto id = required(target.template get<"id">(), "目标 ID 不能为空");
            const auto name = required(target.template get<"name">(), "目标名称不能为空");
            const auto targetIp = required(target.template get<"ip">(), "目标 IP 不能为空");
            const auto targetPort = target.template get<"port">()
                                        ? static_cast<std::int64_t>(*target.template get<"port">())
                                        : 0;
            const auto targetStatus = target.template get<"status">()
                                          ? std::string(target.template get<"status">()->view())
                                          : "enabled";
            if (targetStatus != "enabled" && targetStatus != "disabled")
                service::common::fail(15003, "目标状态无效", 400);
            if (name.empty() || !isIpv4(targetIp) || targetPort < 1 || targetPort > 65535)
                service::common::fail(15003, "目标地址配置无效", 400);
            if (!ids.emplace(id).second)
                service::common::fail(15004, "同一链路内目标 ID 不能重复", 409);
            if (!endpoints.emplace(targetIp + ":" + std::to_string(targetPort)).second)
                service::common::fail(15004, "同一链路内目标地址不能重复", 409);
        }
    }

  private:
    static bool isIpv4(std::string_view value) {
        int parts = 0;
        std::size_t start = 0;
        while (start < value.size()) {
            const auto end = value.find('.', start);
            const auto part = value.substr(
                start, end == std::string_view::npos ? value.size() - start : end - start);
            if (part.empty() || part.size() > 3)
                return false;
            int number = 0;
            for (const char ch : part) {
                if (!std::isdigit(static_cast<unsigned char>(ch)))
                    return false;
                number = number * 10 + (ch - '0');
            }
            if (number > 255)
                return false;
            ++parts;
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        return parts == 4;
    }

};

class LinkTargetValidator final : public ruvia::Middleware<LinkTargetValidator> {
  public:
    RUVIA_VALIDATE_JSON(LinkTargetBody,
                        RUVIA_RULE(id, RUVIA_REQUIRED("目标 ID 不能为空"),
                                   RUVIA_MAX(64, "目标 ID 不能超过 64 个字符")),
                        RUVIA_RULE(name, RUVIA_REQUIRED("目标名称不能为空"),
                                   RUVIA_MAX(100, "目标名称不能超过 100 个字符")),
                        RUVIA_RULE(ip, RUVIA_REQUIRED("目标 IP 不能为空"),
                                   RUVIA_MAX(50, "目标 IP 不能超过 50 个字符")),
                        RUVIA_RULE(port, RUVIA_REQUIRED("目标端口不能为空"),
                                   RUVIA_MIN(1, "目标端口必须在 1 - 65535 之间"),
                                   RUVIA_MAX(65535, "目标端口必须在 1 - 65535 之间")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("目标状态无效", "enabled", "disabled")))
};

class SaveLinkValidator final : public ruvia::Middleware<SaveLinkValidator> {
  public:
    RUVIA_VALIDATE_JSON(SaveLinkBody,
                        RUVIA_RULE(name, RUVIA_REQUIRED("链路名称不能为空"),
                                   RUVIA_MAX(100, "链路名称不能超过 100 个字符")),
                        RUVIA_RULE(protocol, RUVIA_REQUIRED("协议不能为空"),
                                   RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7")),
                        RUVIA_RULE(endpoint, RUVIA_REQUIRED("链路端点不能为空")),
                        RUVIA_RULE(execution, RUVIA_ONE_OF("采集位置无效", "collector", "edge")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")))
};

class LinkListQueryValidator final : public ruvia::Middleware<LinkListQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(LinkListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
                         RUVIA_RULE_NAME("pageSize", pageSize,
                                         RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"),
                                         RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
                         RUVIA_RULE(mode, RUVIA_ONE_OF("链路模式无效", "TCP Server", "TCP Client")),
                         RUVIA_RULE(protocol, RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7")),
                         RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")))
};

class LinkIdParamsValidator final : public ruvia::Middleware<LinkIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(LinkIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

} // namespace service::link
