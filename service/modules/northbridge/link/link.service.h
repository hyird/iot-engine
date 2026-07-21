#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <asio.hpp>
#include <ruvia/web/db/Db.h>

#include "service/modules/northbridge/queue/config_event.publisher.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/modules/northbridge/link/link.types.h"

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
            "SELECT id::text, name, mode, protocol, ip, port, status, created_by::text, "
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

    ruvia::Task<LinkItemDto> detail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT id::text, name, mode, protocol, ip, port, status, created_by::text,
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
            "SELECT id::text, name, mode, protocol FROM iot_link WHERE deleted_at IS NULL AND "
            "status = 'enabled' ORDER BY name");
        ruvia::List<LinkOptionDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.id(row[0].text()).name(row[1].text()).mode(row[2].text()).protocol(row[3].text());
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

    std::string publicIp() {
        std::lock_guard lock(publicIpMutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!cachedPublicIp_.empty() && now - publicIpCachedAt_ < std::chrono::minutes(5))
            return cachedPublicIp_;
        const auto resolved = fetchPublicIp();
        if (!resolved.empty()) {
            cachedPublicIp_ = resolved;
            publicIpCachedAt_ = now;
        }
        return cachedPublicIp_;
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
        validateConfiguration(mode, protocol, ip, port, targets);
        co_await ensureAvailable(c, name, mode, ip, port, std::nullopt);
        const auto targetsJson = serializeTargets(targets);
        const auto id = service::common::nextUuidV7();
        (void)co_await c.db().execute(R"sql(
INSERT INTO iot_link(id, name, mode, protocol, ip, port, targets, status, created_by)
VALUES ($1::uuid, $2, $3, $4, $5, $6, $7::jsonb, $8, $9))sql",
                                      service::common::dbParams(id, name, mode, protocol, ip, port,
                                                                targetsJson, status,
                                                                principal.userId));
        co_await service::bridge::publishConfigEvent(c, "link", "created", id);
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id, const SaveLinkBody& body) {
        const auto rows = co_await c.db().query(
            "SELECT mode, protocol, created_by FROM iot_link WHERE id = $1 AND deleted_at IS "
            "NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(15001, "链路不存在", 404);
        co_await requireOwner(c, rows.rows().front()[2].text());

        const auto name = required(body.name(), "链路名称不能为空");
        const auto mode = required(body.mode(), "链路模式不能为空");
        const auto protocol = required(body.protocol(), "协议不能为空");
        if (mode != rows.rows().front()[0].text() || protocol != rows.rows().front()[1].text())
            service::common::fail(15006, "链路模式和协议创建后不能修改", 400);
        const auto ip = body.ip() ? std::string(body.ip()->view()) : "";
        const auto port = body.port() ? static_cast<std::int64_t>(*body.port()) : 0;
        const auto status = body.status() ? std::string(body.status()->view()) : "enabled";
        const auto& targets = *body.targets();
        validateConfiguration(mode, protocol, ip, port, targets);
        co_await ensureAvailable(c, name, mode, ip, port, std::string(id));
        const auto targetsJson = serializeTargets(targets);
        (void)co_await c.db().execute(
            R"sql(
UPDATE iot_link
SET name = $1, ip = $2, port = $3, targets = $4::jsonb, status = $5, updated_at = NOW()
WHERE id = $6)sql",
            service::common::dbParams(name, ip, port, targetsJson, status, id));
        co_await service::bridge::publishConfigEvent(c, "link", "updated", id);
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT created_by FROM iot_link WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(15001, "链路不存在", 404);
        co_await requireOwner(c, rows.rows().front()[0].text());
        (void)co_await c.db().execute(
            "UPDATE iot_link SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
        co_await service::bridge::publishConfigEvent(c, "link", "deleted", id);
    }

  private:
    struct RuntimeStatus {
        std::map<std::string, std::string> fields;

        [[nodiscard]] std::string text(std::string_view name,
                                       std::string_view fallback = {}) const {
            const auto current = fields.find(std::string(name));
            return current == fields.end() ? std::string(fallback) : current->second;
        }

        [[nodiscard]] std::int64_t integer(std::string_view name) const {
            const auto value = text(name);
            if (value.empty())
                return 0;
            try {
                return std::stoll(value);
            } catch (...) {
                return 0;
            }
        }
    };

    static ruvia::Int64 toInt(std::string_view value) {
        return static_cast<ruvia::Int64>(std::stoll(std::string(value)));
    }

    static std::string fetchPublicIp() {
        asio::ip::tcp::iostream stream;
        stream.expires_after(std::chrono::seconds(3));
        stream.connect("ip.sb", "80");
        if (!stream)
            return {};
        stream << "GET / HTTP/1.1\r\nHost: ip.sb\r\nUser-Agent: curl/8.0\r\n"
                  "Accept: text/plain\r\nConnection: close\r\n\r\n"
               << std::flush;
        std::string statusLine;
        std::getline(stream, statusLine);
        if (statusLine.find(" 200 ") == std::string::npos)
            return {};
        std::string header;
        while (std::getline(stream, header) && header != "\r") {
        }
        std::string value{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const auto last = value.find_last_not_of(" \t\r\n");
        value = value.substr(first, last - first + 1);
        if (value.size() > 64 ||
            !std::all_of(value.begin(), value.end(), [](const unsigned char character) {
                return std::isxdigit(character) || character == '.' || character == ':';
            }))
            return {};
        return value;
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
        const auto id = std::string(row[0].text());
        const auto runtime = co_await loadRuntimeStatus(c, id);
        ruvia::List<ruvia::String> clients(c.resource());
        const auto clientText = runtime.text("clients");
        std::size_t start = 0;
        while (start < clientText.size()) {
            const auto end = clientText.find('\n', start);
            clients.emplace(clientText.substr(start, end == std::string::npos ? std::string::npos
                                                                              : end - start),
                            c.resource());
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        item.id(id)
            .name(row[1].text())
            .mode(row[2].text())
            .protocol(row[3].text())
            .ip(row[4].text())
            .port(toInt(row[5].text()))
            .targets(co_await loadTargets(c, id, runtime))
            .status(row[6].text())
            .connStatus(runtime.text("state", "stopped"))
            .clientCount(runtime.integer("connection_count"))
            .stateReason(runtime.text("state_reason"))
            .clients(std::move(clients))
            .errorMessage(runtime.text("error"))
            .createdBy(row[7].text())
            .createdAt(row[8].text())
            .updatedAt(row[9].text());
    }

    ruvia::Task<ruvia::List<LinkTargetDto>> loadTargets(ruvia::Context& c, std::string_view id,
                                                        const RuntimeStatus& runtime) {
        const auto rows = co_await c.db().query(R"sql(
SELECT target->>'id', target->>'name', target->>'ip', target->>'port', target->>'status'
FROM iot_link, jsonb_array_elements(targets) WITH ORDINALITY AS value(target, position)
WHERE iot_link.id = $1 ORDER BY position)sql",
                                                service::common::dbParams(id));
        ruvia::List<LinkTargetDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& target = result.emplace(c);
            const auto targetId = std::string(row[0].text());
            const auto prefix = "target:" + targetId + ':';
            target.id(targetId)
                .name(row[1].text())
                .ip(row[2].text())
                .port(toInt(row[3].text()))
                .status(row[4].text())
                .connStatus(runtime.text(prefix + "state", "stopped"))
                .stateReason(runtime.text(prefix + "reason"))
                .errorMessage(runtime.text(prefix + "error"))
                .lastActivityAtMs(runtime.integer(prefix + "last_activity_at_ms"));
        }
        co_return result;
    }

    static ruvia::Task<RuntimeStatus> loadRuntimeStatus(ruvia::Context& c, std::string_view id) {
        RuntimeStatus status;
        const auto key = "iot:state:link:" + std::string(id);
        const std::array<std::string_view, 2> args{"HGETALL", key};
        try {
            const auto reply = co_await c.redis().command(args);
            if (reply.kind() != ruvia::RedisValue::Kind::kArray)
                co_return status;
            const auto values = reply.array();
            for (std::size_t index = 0; index + 1 < values.size(); index += 2) {
                if (values[index].kind() != ruvia::RedisValue::Kind::kString ||
                    values[index + 1].kind() != ruvia::RedisValue::Kind::kString)
                    continue;
                status.fields.emplace(std::string(values[index].string()),
                                      std::string(values[index + 1].string()));
            }
        } catch (const std::exception&) {
        }
        co_return status;
    }

    template <typename Targets>
    static void validateConfiguration(std::string_view mode, std::string_view protocol,
                                      std::string_view ip, std::int64_t port,
                                      const Targets& targets) {
        if (protocol == "SL651" && mode != "TCP Server")
            service::common::fail(15003, "SL651 只支持 TCP Server 模式", 400);
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
                                      std::int64_t port, std::optional<std::string> excludedId) {
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

    ruvia::Task<void> requireOwner(ruvia::Context& c, std::string_view ownerId) {
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

    std::mutex publicIpMutex_;
    std::string cachedPublicIp_;
    std::chrono::steady_clock::time_point publicIpCachedAt_{};
};

inline LinkService& linkService() { return LinkService::instance(); }

} // namespace service::link
