#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/modules/iot/link/link.types.h"

namespace service::link {

class LinkService {
  public:
    static LinkService& instance() {
        static LinkService service;
        return service;
    }

    ruvia::Task<LinkPageDataDto> list(ruvia::Context& c, std::int64_t page, std::int64_t pageSize,
                                      std::optional<std::string> keyword,
                                      std::optional<std::string> mode,
                                      std::optional<std::string> protocol,
                                      std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        std::string where = " WHERE deleted_at IS NULL";
        std::vector<ruvia::DbValue> params;
        std::optional<std::string> keywordPattern;
        if (keyword && !keyword->empty()) {
            keywordPattern = "%" + *keyword + "%";
            params.emplace_back(*keywordPattern);
            where += " AND name ILIKE $" + std::to_string(params.size());
        }
        appendFilter(where, params, "mode", mode);
        appendFilter(where, params, "protocol", protocol);
        appendFilter(where, params, "status", status);

        const auto countRows =
            co_await c.db().query("SELECT COUNT(*) FROM iot_link" + where, params);
        const auto total = toInt(countRows.rows().front()[0].text());
        auto listParams = params;
        listParams.emplace_back(pageSize);
        const auto limitIndex = listParams.size();
        listParams.emplace_back((page - 1) * pageSize);
        const auto offsetIndex = listParams.size();
        const auto rows = co_await c.db().query(
            "SELECT id, name, mode, protocol, ip, port, status, created_by, "
            "created_at::text, updated_at::text FROM iot_link" +
                where + " ORDER BY id DESC LIMIT $" + std::to_string(limitIndex) + " OFFSET $" +
                std::to_string(offsetIndex),
            listParams);

        ruvia::List<LinkItemDto> links(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = links.emplace(c);
            co_await fill(c, item, row);
        }
        LinkPageDataDto result(c);
        result.list(std::move(links))
            .total(total)
            .page(page)
            .pageSize(pageSize)
            .totalPages(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<LinkItemDto> detail(ruvia::Context& c, std::int64_t id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT id, name, mode, protocol, ip, port, status, created_by,
       created_at::text, updated_at::text
FROM iot_link WHERE id = $1 AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(15001, "链路不存在", 404);
        LinkItemDto item(c);
        co_await fill(c, item, rows.rows().front());
        co_return item;
    }

    ruvia::Task<ruvia::List<LinkOptionDto>> options(ruvia::Context& c) {
        const auto rows = co_await c.db().query(
            "SELECT id, name, mode, protocol FROM iot_link WHERE deleted_at IS NULL AND "
            "status = 'enabled' ORDER BY name");
        ruvia::List<LinkOptionDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.id(toInt(row[0].text()))
                .name(row[1].text())
                .mode(row[2].text())
                .protocol(row[3].text());
        }
        co_return result;
    }

    LinkEnumsDto enums(ruvia::Context& c) {
        LinkEnumsDto result(c);
        ruvia::List<ruvia::String> modes(c.resource());
        modes.emplace("TCP Server", c.resource());
        modes.emplace("TCP Client", c.resource());
        ruvia::List<ruvia::String> protocols(c.resource());
        protocols.emplace("SL651", c.resource());
        protocols.emplace("Modbus", c.resource());
        protocols.emplace("S7", c.resource());
        ruvia::List<ruvia::String> statuses(c.resource());
        statuses.emplace("enabled", c.resource());
        statuses.emplace("disabled", c.resource());
        result.modes(std::move(modes))
            .protocols(std::move(protocols))
            .statuses(std::move(statuses));
        return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const SaveLinkBody& body) {
        const auto principal = service::middleware::requireAuth(c);
        const auto name = required(body.name(), "链路名称不能为空");
        const auto mode = required(body.mode(), "链路模式不能为空");
        const auto protocol = required(body.protocol(), "协议不能为空");
        const auto ip = body.ip() ? std::string(body.ip()->view()) : "";
        const auto port = body.port() ? static_cast<std::int64_t>(*body.port()) : 0;
        const auto status = body.status() ? std::string(body.status()->view()) : "enabled";
        const auto& targets = *body.targets();
        validateConfiguration(mode, ip, port, targets);
        co_await ensureAvailable(c, name, mode, ip, port, std::nullopt);
        const auto targetsJson = serializeTargets(targets);
        (void)co_await c.db().execute(R"sql(
INSERT INTO iot_link(name, mode, protocol, ip, port, targets, status, created_by)
VALUES ($1, $2, $3, $4, $5, $6::jsonb, $7, $8))sql",
                                      service::common::dbParams(name, mode, protocol, ip, port,
                                                                targetsJson, status,
                                                                principal.userId));
    }

    ruvia::Task<void> update(ruvia::Context& c, std::int64_t id, const SaveLinkBody& body) {
        const auto rows = co_await c.db().query(
            "SELECT mode, protocol, created_by FROM iot_link WHERE id = $1 AND deleted_at IS "
            "NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(15001, "链路不存在", 404);
        co_await requireOwner(c, toInt(rows.rows().front()[2].text()));

        const auto name = required(body.name(), "链路名称不能为空");
        const auto mode = required(body.mode(), "链路模式不能为空");
        const auto protocol = required(body.protocol(), "协议不能为空");
        if (mode != rows.rows().front()[0].text() || protocol != rows.rows().front()[1].text())
            service::common::fail(15006, "链路模式和协议创建后不能修改", 400);
        const auto ip = body.ip() ? std::string(body.ip()->view()) : "";
        const auto port = body.port() ? static_cast<std::int64_t>(*body.port()) : 0;
        const auto status = body.status() ? std::string(body.status()->view()) : "enabled";
        const auto& targets = *body.targets();
        validateConfiguration(mode, ip, port, targets);
        co_await ensureAvailable(c, name, mode, ip, port, id);
        const auto targetsJson = serializeTargets(targets);
        (void)co_await c.db().execute(
            R"sql(
UPDATE iot_link
SET name = $1, ip = $2, port = $3, targets = $4::jsonb, status = $5, updated_at = NOW()
WHERE id = $6)sql",
            service::common::dbParams(name, ip, port, targetsJson, status, id));
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::int64_t id) {
        const auto rows = co_await c.db().query(
            "SELECT created_by FROM iot_link WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(15001, "链路不存在", 404);
        co_await requireOwner(c, toInt(rows.rows().front()[0].text()));
        (void)co_await c.db().execute(
            "UPDATE iot_link SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
    }

  private:
    static ruvia::Int64 toInt(std::string_view value) {
        return static_cast<ruvia::Int64>(std::stoll(std::string(value)));
    }

    static std::string required(const std::optional<ruvia::String>& value,
                                std::string_view message) {
        if (!value)
            service::common::fail(15002, std::string(message), 400);
        return std::string(value->view());
    }

    static void appendFilter(std::string& where, std::vector<ruvia::DbValue>& params,
                             std::string_view column, const std::optional<std::string>& value) {
        if (!value || value->empty())
            return;
        params.emplace_back(*value);
        where += " AND " + std::string(column) + " = $" + std::to_string(params.size());
    }

    template <typename Row>
    ruvia::Task<void> fill(ruvia::Context& c, LinkItemDto& item, const Row& row) {
        const auto id = toInt(row[0].text());
        item.id(id)
            .name(row[1].text())
            .mode(row[2].text())
            .protocol(row[3].text())
            .ip(row[4].text())
            .port(toInt(row[5].text()))
            .targets(co_await loadTargets(c, id))
            .status(row[6].text())
            .connStatus("stopped")
            .clientCount(0)
            .createdBy(toInt(row[7].text()))
            .createdAt(row[8].text())
            .updatedAt(row[9].text());
    }

    ruvia::Task<ruvia::List<LinkTargetDto>> loadTargets(ruvia::Context& c, std::int64_t id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT target->>'id', target->>'name', target->>'ip', target->>'port', target->>'status'
FROM iot_link, jsonb_array_elements(targets) WITH ORDINALITY AS value(target, position)
WHERE iot_link.id = $1 ORDER BY position)sql",
                                                service::common::dbParams(id));
        ruvia::List<LinkTargetDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& target = result.emplace(c);
            target.id(row[0].text())
                .name(row[1].text())
                .ip(row[2].text())
                .port(toInt(row[3].text()))
                .status(row[4].text());
        }
        co_return result;
    }

    template <typename Targets>
    static void validateConfiguration(std::string_view mode, std::string_view ip, std::int64_t port,
                                      const Targets& targets) {
        if (mode == "TCP Server") {
            if (!isIpv4(ip) || port < 1 || port > 65535)
                service::common::fail(15003, "TCP Server 必须配置有效的监听 IP 和端口", 400);
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
            const auto id = required(target.id(), "目标 ID 不能为空");
            const auto name = required(target.name(), "目标名称不能为空");
            const auto targetIp = required(target.ip(), "目标 IP 不能为空");
            const auto targetPort = target.port() ? static_cast<std::int64_t>(*target.port()) : 0;
            if (name.empty() || !isIpv4(targetIp) || targetPort < 1 || targetPort > 65535)
                service::common::fail(15003, "目标地址配置无效", 400);
            if (!ids.emplace(id).second)
                service::common::fail(15004, "同一链路内目标 ID 不能重复", 409);
            if (!endpoints.emplace(targetIp + ":" + std::to_string(targetPort)).second)
                service::common::fail(15004, "同一链路内目标地址不能重复", 409);
        }
    }

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

    static void appendJsonString(std::string& output, std::string_view value) {
        static constexpr char hex[] = "0123456789abcdef";
        output.push_back('"');
        for (const unsigned char ch : value) {
            switch (ch) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    output += "\\u00";
                    output.push_back(hex[ch >> 4]);
                    output.push_back(hex[ch & 0x0f]);
                } else {
                    output.push_back(static_cast<char>(ch));
                }
            }
        }
        output.push_back('"');
    }

    template <typename Targets> static std::string serializeTargets(const Targets& targets) {
        std::string result = "[";
        for (const auto& target : targets) {
            if (result.size() > 1)
                result.push_back(',');
            result += "{\"id\":";
            appendJsonString(result, target.id()->view());
            result += ",\"name\":";
            appendJsonString(result, target.name()->view());
            result += ",\"ip\":";
            appendJsonString(result, target.ip()->view());
            result += ",\"port\":" + std::to_string(static_cast<std::int64_t>(*target.port()));
            result += ",\"status\":";
            appendJsonString(result, target.status() ? target.status()->view() : "enabled");
            result.push_back('}');
        }
        result.push_back(']');
        return result;
    }

    ruvia::Task<void> ensureAvailable(ruvia::Context& c, const std::string& name,
                                      const std::string& mode, const std::string& ip,
                                      std::int64_t port, std::optional<std::int64_t> excludedId) {
        std::string sql = "SELECT 1 FROM iot_link WHERE deleted_at IS NULL AND (name = $1 OR "
                          "($2 = 'TCP Server' AND mode = $2 AND ip = $3 AND port = $4))";
        auto params = service::common::dbParams(name, mode, ip, port);
        if (excludedId) {
            params.emplace_back(*excludedId);
            sql += " AND id <> $" + std::to_string(params.size());
        }
        sql += " LIMIT 1";
        const auto rows = co_await c.db().query(sql, params);
        if (!rows.rows().empty())
            service::common::fail(15005, "链路名称或监听地址已存在", 409);
    }

    ruvia::Task<void> requireOwner(ruvia::Context& c, std::int64_t ownerId) {
        const auto principal = service::middleware::requireAuth(c);
        if (principal.userId == ownerId)
            co_return;
        const auto rows = co_await c.db().query(R"sql(
SELECT EXISTS (
    SELECT 1 FROM sys_user_role ur JOIN sys_role r ON r.id = ur.role_id
    WHERE ur.user_id = $1 AND r.code = 'superadmin'
      AND r.status = 'enabled' AND r.deleted_at IS NULL
))sql",
                                                service::common::dbParams(principal.userId));
        if (rows.rows().empty() || rows.rows().front()[0].text() != "t")
            service::common::fail(15007, "只能修改或删除自己创建的链路", 403);
    }
};

inline LinkService& linkService() { return LinkService::instance(); }

} // namespace service::link
