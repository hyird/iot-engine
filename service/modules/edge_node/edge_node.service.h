#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <edge.pb.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <openssl/rand.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ruvia/web/Context.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/http.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/middleware/live.h"
#include "service/middleware/rpc.h"
#include "service/modules/edge_node/edge_node.entity.h"
#include "service/modules/edge_node/edge_node.types.h"
#include "service/modules/link/link.entity.h"
#include "service/modules/system/auth/auth.service.h"
#include "service/modules/vpn/vpn.service.h"
#include "service/utils/base64.h"
#include "service/utils/number.h"

namespace service::edge {

namespace module_pb = ::iot::edge::v1;

namespace module_wire {

using service::message::edge::authKey;

using service::common::hexDigit;
using service::common::uuidBytes;

inline std::string bytes(const std::uint8_t* data, std::size_t size) {
    return { reinterpret_cast<const char*>(data), size };
}

template <typename Context, typename Request>
ruvia::Task<void> queueControl(Context& context, std::string_view nodeId, std::string_view operation, const Request& request) {
    std::string requestPayload;
    if (!request.SerializeToString(&requestPayload)) {
        service::common::fail(17005, "边缘命令编码失败", 500);
    }
    (void)co_await service::rpc::call(
        context,
        "edge",
        operation,
        std::string(nodeId) + "\n" + std::move(requestPayload)
    );
}

} // namespace module_wire

class EdgeService {
  public:
    template <typename Context>
    ruvia::Task<std::string> dtuChannels(Context& c, std::string_view nodeId) {
        ruvia::DbQuery node(c.pool());
        node.select(booleanText(node, jsonText(node, node.column(EdgeNodeEntity::columnName<"capability">()), "dtu")))
            .from(EdgeNodeEntity::tableName()).where(node.binary(node.column(EdgeNodeEntity::columnName<"id">()), Op::kEqual, node.cast(node.value(nodeId), Type::kUuid)));
        const auto nodes = co_await c.db().query(node);
        if (nodes.empty()) service::common::fail(17001, "边缘节点不存在", 404);
        std::string result = nodes.front()[0].template as<bool>().value_or(false) ? "{\"supported\":true,\"channels\":[" : "{\"supported\":false,\"channels\":[";
        ruvia::DbQuery query(c.pool());
        query.select({query.column(EdgeDtuEntity::columnName<"config">()), query.column(EdgeDtuEntity::columnName<"status">())})
            .from(EdgeDtuEntity::tableName()).where(query.binary(query.column(EdgeDtuEntity::columnName<"node_id">()), Op::kEqual, query.cast(query.value(nodeId), Type::kUuid)))
            .orderBy(query.column(EdgeDtuEntity::columnName<"channel_id">()));
        const auto rows = co_await c.db().query(query);
        bool first = true;
        for (const auto& row : rows) {
            if (!first) result += ',';
            first = false;
            std::string config(row[0].value().value_or("{}"));
            config.pop_back();
            result += config + ",\"status\":" + std::string(row[1].value().value_or("{}")) + "}";
        }
        co_return result + "]}";
    }

    template <typename Context>
    ruvia::Task<void> saveDtuChannel(Context& c, std::string_view nodeId, const DtuChannelBody& body, std::string_view json) {
        co_await requireNodeCapability(c, nodeId, "dtu", "DTU 透传");
        const auto stringValue = [](const auto& value, std::string_view fallback) {
            return value ? std::string(value->view()) : std::string(fallback);
        };
        module_pb::DtuConfig config;
        std::array<std::uint8_t,16> id{};
        const auto channelId = body.get<"channelId">().view();
        if (!module_wire::uuidBytes(channelId, id.data())) service::common::fail(17003, "通道 ID 无效", 400);
        config.set_channel_id(module_wire::bytes(id.data(), id.size()));
        config.set_name(body.get<"name">().view());
        config.set_enabled(body.get<"enabled">().value);
        config.set_debug_enabled(body.get<"debugEnabled">() && body.get<"debugEnabled">()->value);
        config.set_north_host(body.get<"northHost">().view());
        config.set_north_port(static_cast<std::uint32_t>(body.get<"northPort">().value));
        config.set_max_clients(static_cast<std::uint32_t>(body.get<"maxClients">().value));
        config.set_queue_bytes(static_cast<std::uint32_t>(body.get<"queueBytes">().value));
        config.set_serial_frame_ms(static_cast<std::uint32_t>(body.get<"serialFrameMs">().value));
        config.set_uplink_only(body.get<"uplinkOnly">() && body.get<"uplinkOnly">()->value);
        const auto mode = body.get<"southMode">().view();
        if (mode == "serial") {
            const auto path = stringValue(body.get<"serialPath">(), "");
            const auto baud = body.get<"baudRate">() ? body.get<"baudRate">()->value : 9600;
            const auto bits = body.get<"dataBits">() ? body.get<"dataBits">()->value : 8;
            const auto stops = body.get<"stopBits">() ? body.get<"stopBits">()->value : 1;
            const auto parity = stringValue(body.get<"parity">(), "none");
            constexpr std::array<std::int64_t,11> rates{300,600,1200,2400,4800,9600,19200,38400,57600,115200,230400};
            if (!path.starts_with("/dev/") || std::ranges::find(rates, baud) == rates.end() || bits < 5 || bits > 8 || stops < 1 || stops > 2 ||
                (parity != "none" && parity != "even" && parity != "odd")) service::common::fail(17003, "串口参数无效", 400);
            config.set_south_mode(module_pb::LINK_MODE_SERIAL);
            auto* serial = config.mutable_serial();
            serial->set_channel(path); serial->set_baud_rate(static_cast<std::uint32_t>(baud));
            serial->set_data_bits(static_cast<std::uint32_t>(bits)); serial->set_stop_bits(static_cast<std::uint32_t>(stops));
            serial->set_parity(parity); serial->set_rs485(body.get<"rs485">() && body.get<"rs485">()->value);
        } else {
            const auto host = stringValue(body.get<"southHost">(), "");
            if (host.empty() || !body.get<"southPort">()) service::common::fail(17003, "请输入南向地址和端口", 400);
            config.set_south_mode(mode == "tcp_server" ? module_pb::LINK_MODE_TCP_SERVER : module_pb::LINK_MODE_TCP_CLIENT);
            config.set_south_host(host); config.set_south_port(static_cast<std::uint32_t>(body.get<"southPort">()->value));
        }
        const auto decodePacket = [](std::string_view hex) {
            std::string packet;
            if (hex.size() > 512 || hex.size() % 2) service::common::fail(17003, "报文 HEX 必须为完整字节且不超过 256 字节", 400);
            for (std::size_t i = 0; i < hex.size(); i += 2) {
                const int high = module_wire::hexDigit(hex[i]), low = module_wire::hexDigit(hex[i+1]);
                if (high < 0 || low < 0) service::common::fail(17003, "报文 HEX 无效", 400);
                packet.push_back(static_cast<char>((high << 4) | low));
            }
            return packet;
        };
        config.set_registration(decodePacket(stringValue(body.get<"registrationHex">(), "")));
        config.set_heartbeat(decodePacket(stringValue(body.get<"heartbeatHex">(), "")));
        const auto heartbeatInterval = body.get<"heartbeatIntervalSec">() ? body.get<"heartbeatIntervalSec">()->value : 0;
        if (heartbeatInterval < 0 || heartbeatInterval > 86400 || (heartbeatInterval && config.heartbeat().empty()))
            service::common::fail(17003, "心跳间隔应为 0～86400 秒，启用时须填写心跳包", 400);
        config.set_heartbeat_interval_sec(static_cast<std::uint32_t>(heartbeatInterval));
        const auto validHost = [](std::string_view host) {
            return !host.empty() && std::ranges::all_of(host, [](unsigned char value) {
                return std::isalnum(value) || value == '.' || value == '-' || value == '_' || value == ':' || value == '%';
            });
        };
        if (!validHost(config.north_host()) || (mode != "serial" && !validHost(config.south_host())))
            service::common::fail(17003, "地址只能填写域名或 IP，不能包含空白、URL 或控制字符", 400);
        std::string wire; config.SerializeToString(&wire);
        std::string encoded;
        constexpr char digits[] = "0123456789abcdef";
        for (unsigned char byte : wire) { encoded += digits[byte >> 4]; encoded += digits[byte & 15]; }
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery lock(c.pool());
        lock.select(lock.column(EdgeNodeEntity::columnName<"id">())).from(EdgeNodeEntity::tableName())
            .where(lock.binary(lock.column(EdgeNodeEntity::columnName<"id">()), Op::kEqual, lock.cast(lock.value(nodeId), Type::kUuid))).lock({.mode=ruvia::DbRowLock::kUpdate});
        (void)co_await transaction.query(lock);
        ruvia::DbQuery existing(c.pool());
        existing.select({existing.column(EdgeDtuEntity::columnName<"channel_id">()), existing.column(EdgeDtuEntity::columnName<"wire_hex">())}).from(EdgeDtuEntity::tableName())
            .where(existing.binary(existing.column(EdgeDtuEntity::columnName<"node_id">()), Op::kEqual, existing.cast(existing.value(nodeId), Type::kUuid)));
        const auto rows = co_await transaction.query(existing);
        bool found = false;
        for (const auto& row : rows) {
            if (row[0].value().value_or("") == channelId) { found = true; continue; }
            if (!config.enabled()) continue;
            const auto encodedOther = row[1].value().value_or("");
            std::string wireOther;
            for (std::size_t i = 0; i + 1 < encodedOther.size(); i += 2)
                wireOther.push_back(static_cast<char>((module_wire::hexDigit(encodedOther[i]) << 4) | module_wire::hexDigit(encodedOther[i+1])));
            module_pb::DtuConfig other;
            if (!other.ParseFromString(wireOther)) service::common::fail(17005, "已存透传配置无法解析", 500);
            if (!other.enabled() || other.south_mode() != config.south_mode()) continue;
            if ((mode == "serial" && other.serial().channel() == config.serial().channel()) ||
                (mode == "tcp_server" && other.south_port() == config.south_port()))
                service::common::fail(17003, "该南向串口或监听端口已被另一个透传通道占用", 409);
        }
        if (!found && rows.size() >= 8) service::common::fail(17003, "每个节点最多 8 个透传通道", 409);
        ruvia::DbQuery save(c.pool());
        save.insertInto(EdgeDtuEntity::tableName(), {EdgeDtuEntity::columnName<"node_id">(), EdgeDtuEntity::columnName<"channel_id">(), EdgeDtuEntity::columnName<"config">(), EdgeDtuEntity::columnName<"wire_hex">()})
            .values({save.cast(save.value(nodeId), Type::kUuid), save.cast(save.value(channelId), Type::kUuid), save.cast(save.value(json), Type::kJsonb), save.value(encoded)});
        ruvia::DbConflictOptions conflict;
        conflict.columns = {std::string(EdgeDtuEntity::columnName<"node_id">()), std::string(EdgeDtuEntity::columnName<"channel_id">())};
        conflict.update = {{std::string(EdgeDtuEntity::columnName<"config">()), save.excluded(EdgeDtuEntity::columnName<"config">())},
                           {std::string(EdgeDtuEntity::columnName<"wire_hex">()), save.excluded(EdgeDtuEntity::columnName<"wire_hex">())},
                           {std::string(EdgeDtuEntity::columnName<"status">()), save.cast(save.value("{}"), Type::kJsonb)}};
        save.onConflict(conflict);
        (void)co_await transaction.execute(save);
        co_await transaction.commit();
        try { (void)co_await queueSnapshot(c, nodeId, c.userId); }
        catch (...) { service::common::fail(17005, "配置已保存，下发未成功，请点击同步配置重试", 502); }
    }

    template <typename Context>
    ruvia::Task<void> deleteDtuChannel(Context& c, std::string_view nodeId, std::string_view channelId) {
        ruvia::DbQuery query(c.pool());
        query.deleteFrom(EdgeDtuEntity::tableName()).where(query.binary(query.column(EdgeDtuEntity::columnName<"node_id">()), Op::kEqual, query.cast(query.value(nodeId), Type::kUuid)))
            .andWhere(query.binary(query.column(EdgeDtuEntity::columnName<"channel_id">()), Op::kEqual, query.cast(query.value(channelId), Type::kUuid)));
        (void)co_await c.db().execute(query);
        try { (void)co_await queueSnapshot(c, nodeId, c.userId); }
        catch (...) { service::common::fail(17005, "配置已删除，下发未成功，请点击同步配置重试", 502); }
    }

    template <typename Context>
    ruvia::Task<void> authenticateDebugConnection(Context& c, std::string_view token) {
        const auto principal = service::auth::AuthTokenService::verifyAccessToken(c, token);
        (void)co_await service::auth::authService().current(c, principal.userId);
    }

    template <typename Context>
    static ruvia::Task<std::uint64_t> queueSnapshot(Context& context, std::string_view nodeId, std::string_view userId) {
        if (!service::common::isUuid(nodeId)) {
            service::common::fail(17001, "边缘节点不存在", 404);
        }
        const auto revision = co_await service::rpc::call(
            context,
            "edge",
            "queue-snapshot",
            std::string(nodeId) + "\n" + std::string(userId)
        );
        const auto parsed = service::utils::parseInt64(
            std::optional<std::string_view>(revision)
        );
        if (!parsed || *parsed < 0) {
            service::common::fail(17005, "边缘配置生成失败", 502);
        }
        co_return static_cast<std::uint64_t>(*parsed);
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<EdgeNodeDto>> inventory(Context& c) {
        auto query = nodeSelect(c.pool());
        query.orderBy(query.column("created_at", "node"), ruvia::DbOrderDirection::kDesc)
            .addOrderBy(query.column("id", "node"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<EdgeNodeDto> nodes(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& node = nodes.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            co_await fillNode(c, node, row);
        }
        co_return nodes;
    }

    template <typename Context>
    ruvia::Task<std::string> vpnState(Context& c, std::string_view nodeId) {
        co_await service::auth::AuthService::requirePermission(c, c.userId, "iot:vpn:query");
        const auto peers = co_await service::vpn::vpnService().peers(c, std::nullopt, std::string(nodeId));
        const auto routes = co_await service::vpn::vpnService().routes(c, std::nullopt, std::string(nodeId));
        co_return "{\"peers\":" + peers + ",\"routes\":" + routes + "}";
    }

    template <typename Context>
    ruvia::Task<EdgePageDto> list(Context& c, std::int64_t page, std::int64_t pageSize, std::optional<std::string> keyword, std::optional<std::string> status, std::optional<std::string> groupId) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        if (page - 1 > std::numeric_limits<std::int64_t>::max() / pageSize) {
            service::common::fail(17003, "分页范围无效", 400);
        }
        ruvia::DbQuery count(c.pool());
        count.select(count.aggregate("count", { count.star() }))
            .from(EdgeNodeEntity::tableName(), "node")
            .join(ruvia::DbJoinType::kLeft, EdgeNodeGroupEntity::tableName(), count.binary(count.column("group_id", "node"), ruvia::DbBinaryOperator::kEqual, count.column("id", "group_item")), "group_item");
        applyNodeFilters(count, keyword, status, groupId);
        const auto countRows = co_await c.db().query(count);
        const auto total = countRows.empty()
            ? std::int64_t{ 0 }
            : countRows.front()[0].template as<std::int64_t>().value_or(0);

        ruvia::DbQuery query = nodeSelect(c.pool());
        applyNodeFilters(query, keyword, status, groupId);
        query.orderBy(query.column("created_at", "node"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>((page - 1) * pageSize));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<EdgeNodeDto> nodes(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& node = nodes.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            co_await fillNode(c, node, row);
        }
        EdgePageDto result(ruvia::ModelOptions{ .resource = c.arena() });
        result.template set<"list">(std::move(nodes))
            .template set<"total">(total)
            .template set<"page">(page)
            .template set<"pageSize">(pageSize)
            .template set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<EdgeGroupDto>> groups(Context& c) {
        ruvia::DbQuery query(c.pool());
        const auto groupId = query.column("id", "group_item");
        const auto parentId = query.column("parent_id", "group_item");
        query.select({ query.cast(groupId, ruvia::DbDataType::kText), query.column("name", "group_item"), query.coalesce({ query.cast(parentId, ruvia::DbDataType::kText), query.cast(query.value(""), ruvia::DbDataType::kText) }), query.column("status", "group_item"), query.column("sort_order", "group_item"), query.coalesce({ query.column("remark", "group_item"), query.cast(query.value(""), ruvia::DbDataType::kText) }), query.aggregate("count", { query.column("id", "node") }) })
            .from(EdgeNodeGroupEntity::tableName(), "group_item")
            .join(ruvia::DbJoinType::kLeft, EdgeNodeEntity::tableName(), query.binary(query.column("group_id", "node"), ruvia::DbBinaryOperator::kEqual, groupId), "node")
            .where(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at", "group_item")))
            .groupBy({ groupId, query.column("name", "group_item"), parentId, query.column("status", "group_item"), query.column("sort_order", "group_item"), query.column("remark", "group_item") })
            .orderBy(query.column("sort_order", "group_item"))
            .addOrderBy(groupId);
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<EdgeGroupDto> result(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.template set<"id">(row[0].value().value_or(std::string_view{}));
            item.template set<"name">(row[1].value().value_or(std::string_view{}));
            item.template set<"parentId">(row[2].value().value_or(std::string_view{}));
            item.template set<"status">(row[3].value().value_or(std::string_view{}));
            item.template set<"sortOrder">(integer(row[4].value().value_or(std::string_view{})));
            item.template set<"remark">(row[5].value().value_or(std::string_view{}));
            item.template set<"nodeCount">(integer(row[6].value().value_or(std::string_view{})));
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<void> createGroup(Context& c, const EdgeGroupBody& body) {
        const auto name = std::string(body.get<"name">().view());
        const auto parentId = body.get<"parentId">()
            ? std::string(body.get<"parentId">()->view())
            : std::string{};
        co_await validateGroupParent(c, parentId, {});
        ruvia::DbQuery duplicate(c.pool());
        duplicate.select(duplicate.cast(duplicate.value(1), ruvia::DbDataType::kInteger))
            .from(EdgeNodeGroupEntity::tableName())
            .where(duplicate.binary(duplicate.column("name"), ruvia::DbBinaryOperator::kEqual, duplicate.value(name)))
            .andWhere(duplicate.unary(ruvia::DbUnaryOperator::kIsNull, duplicate.column("deleted_at")))
            .limit(1);
        const auto duplicateRows = co_await c.db().query(duplicate);
        if (!duplicateRows.empty()) {
            service::common::fail(17002, "边缘节点分组名称已存在", 409);
        }
        const auto status = body.get<"status">()
            ? std::string(body.get<"status">()->view())
            : std::string{ "enabled" };
        const auto sortOrder = body.get<"sortOrder">()
            ? static_cast<std::int64_t>(*body.get<"sortOrder">())
            : 0;
        const auto remark = body.get<"remark">()
            ? std::string(body.get<"remark">()->view())
            : std::string{};
        const auto groupId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        ruvia::DbQuery query(c.pool());
        query.insertInto(EdgeNodeGroupEntity::tableName(), { "id", "name", "parent_id", "status", "sort_order", "remark", "created_by" })
            .values({ query.cast(query.value(groupId), ruvia::DbDataType::kUuid), query.value(name), nullableUuid(query, parentId), query.cast(query.value(status), { .customName = "status_enum" }), query.cast(query.value(sortOrder), ruvia::DbDataType::kInteger), nullableText(query, remark), query.cast(query.value(c.userId), ruvia::DbDataType::kUuid) });
        (void)co_await c.db().execute(query);
    }

    template <typename Context>
    ruvia::Task<void> updateGroup(Context& c, std::string_view id, const EdgeGroupBody& body) {
        ruvia::DbQuery current(c.pool());
        current.select(current.cast(current.value(1), ruvia::DbDataType::kInteger))
            .from(EdgeNodeGroupEntity::tableName())
            .where(current.binary(current.column("id"), ruvia::DbBinaryOperator::kEqual, current.cast(current.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(current.unary(ruvia::DbUnaryOperator::kIsNull, current.column("deleted_at")))
            .limit(1);
        if ((co_await c.db().query(current)).empty()) {
            service::common::fail(17001, "边缘节点分组不存在", 404);
        }
        const auto name = std::string(body.get<"name">().view());
        const auto parentId = body.get<"parentId">()
            ? std::string(body.get<"parentId">()->view())
            : std::string{};
        co_await validateGroupParent(c, parentId, id);
        ruvia::DbQuery duplicate(c.pool());
        duplicate.select(duplicate.cast(duplicate.value(1), ruvia::DbDataType::kInteger))
            .from(EdgeNodeGroupEntity::tableName())
            .where(duplicate.binary(duplicate.column("name"), ruvia::DbBinaryOperator::kEqual, duplicate.value(name)))
            .andWhere(duplicate.binary(
                duplicate.column("id"),
                ruvia::DbBinaryOperator::kNotEqual,
                duplicate.cast(duplicate.value(id), ruvia::DbDataType::kUuid)
            ))
            .andWhere(duplicate.unary(ruvia::DbUnaryOperator::kIsNull, duplicate.column("deleted_at")))
            .limit(1);
        if (!(co_await c.db().query(duplicate)).empty()) {
            service::common::fail(17002, "边缘节点分组名称已存在", 409);
        }
        const auto status = body.get<"status">()
            ? std::string(body.get<"status">()->view())
            : std::string{ "enabled" };
        const auto sortOrder = body.get<"sortOrder">()
            ? static_cast<std::int64_t>(*body.get<"sortOrder">())
            : 0;
        const auto remark = body.get<"remark">()
            ? std::string(body.get<"remark">()->view())
            : std::string{};
        ruvia::DbQuery query(c.pool());
        query.update(EdgeNodeGroupEntity::tableName())
            .set("name", query.value(name))
            .set("parent_id", nullableUuid(query, parentId))
            .set("status", query.cast(query.value(status), { .customName = "status_enum" }))
            .set("sort_order", query.cast(query.value(sortOrder), ruvia::DbDataType::kInteger))
            .set("remark", nullableText(query, remark))
            .set("updated_at", query.call("now"))
            .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual, query.cast(query.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        (void)co_await c.db().execute(query);
    }

    template <typename Context>
    ruvia::Task<void> removeGroup(Context& c, std::string_view id) {
        ruvia::DbQuery current(c.pool());
        current.select(current.cast(current.value(1), ruvia::DbDataType::kInteger))
            .from(EdgeNodeGroupEntity::tableName())
            .where(current.binary(current.column("id"), ruvia::DbBinaryOperator::kEqual, current.cast(current.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(current.unary(ruvia::DbUnaryOperator::kIsNull, current.column("deleted_at")))
            .limit(1);
        if ((co_await c.db().query(current)).empty()) {
            service::common::fail(17001, "边缘节点分组不存在", 404);
        }

        ruvia::DbQuery removal(c.pool());
        const auto targetId = removal.column("id", "target");
        ruvia::DbQuery child(c.pool());
        child.select(child.cast(child.value(1), ruvia::DbDataType::kInteger))
            .from(EdgeNodeGroupEntity::tableName(), "child")
            .where(child.binary(
                child.column("parent_id", "child"),
                ruvia::DbBinaryOperator::kEqual,
                child.importExpression(targetId, "target", "target")
            ))
            .andWhere(child.unary(ruvia::DbUnaryOperator::kIsNull, child.column("deleted_at", "child")))
            .limit(1);
        ruvia::DbQuery node(c.pool());
        node.select(node.cast(node.value(1), ruvia::DbDataType::kInteger))
            .from(EdgeNodeEntity::tableName(), "node")
            .where(node.binary(
                node.column("group_id", "node"),
                ruvia::DbBinaryOperator::kEqual,
                node.importExpression(targetId, "target", "target")
            ))
            .limit(1);
        removal.update(EdgeNodeGroupEntity::tableName(), "target")
            .set("deleted_at", removal.call("now"))
            .set("updated_at", removal.call("now"))
            .where(removal.binary(targetId, ruvia::DbBinaryOperator::kEqual, removal.cast(removal.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(removal.unary(ruvia::DbUnaryOperator::kIsNull, removal.column("deleted_at", "target")))
            .andWhere(removal.unary(ruvia::DbUnaryOperator::kNot, removal.exists(child)))
            .andWhere(removal.unary(ruvia::DbUnaryOperator::kNot, removal.exists(node)));
        const auto removed = co_await c.db().execute(removal);
        if (removed.affectedRows() == 0) {
            service::common::fail(17004, "请先移除子分组和边缘节点", 409);
        }
    }

    template <typename Context>
    ruvia::Task<EdgeNodeDto> detail(Context& c, std::string_view id) {
        ruvia::DbQuery query = nodeSelect(c.pool());
        query.where(query.binary(query.column("id", "node"), ruvia::DbBinaryOperator::kEqual, query.cast(query.value(id), ruvia::DbDataType::kUuid)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17001, "边缘节点不存在", 404);
        }
        EdgeNodeDto node(ruvia::ModelOptions{ .resource = c.arena() });
        co_await fillNode(c, node, rows.front());
        node.template set<"interfaces">(co_await interfaces(c, id));
        node.template set<"networks">(co_await networks(c, id));
        node.template set<"serialPorts">(co_await serialPorts(c, id));
        node.template set<"tasks">(co_await tasks(c, id));
        co_return node;
    }

    template <typename Context>
    ruvia::Task<void> setEnrollment(Context& c, std::string_view id, const EnrollmentBody& body) {
        const auto& statusValue = body.get<"status">();
        if (statusValue.view().empty()) {
            service::common::fail(17003, "注册状态不能为空", 400);
        }
        const std::string status(statusValue.view());
        if (status != "approved") {
            service::common::fail(17003, "注册状态无效", 400);
        }
        const std::string name = body.get<"name">() ? std::string(body.get<"name">()->view()) : std::string{};
        std::string_view stage = "database";
        try {
            ruvia::DbQuery update(c.pool());
            update.update(EdgeNodeEntity::tableName())
                .set("enrollment_status", update.value(status))
                .set("name", nullableText(update, name))
                .set("approved_by", update.cast(update.value(c.userId), ruvia::DbDataType::kUuid))
                .set("approved_at", update.call("now"))
                .set("updated_at", update.call("now"))
                .where(update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual, update.cast(update.value(id), ruvia::DbDataType::kUuid)))
                .returning({ update.column("imei") });
            const auto updated = co_await c.db().query(update);
            if (updated.empty()) {
                service::common::fail(17001, "边缘节点不存在", 404);
            }
            const auto key = module_wire::authKey(
                updated.front()[0].value().value_or(std::string_view{})
            );
            const auto value = std::string(id) + "|" + status;
            stage = "redis";
            co_await c.redis().set(key, value);
            if (status == "approved") {
                try {
                    // Approval must be useful without a second manual click. Queue the
                    // current device snapshot immediately; the gateway will deliver it
                    // as soon as the pending WebSocket becomes an enrolled session.
                    (void)co_await queueSnapshot(c, id, c.userId);
                } catch (const std::exception& error) {
                    // A node may be approved before its first capability projection or
                    // before it has any configurable devices. Approval itself must not
                    // fail in those compatible/empty cases; the existing heartbeat retry
                    // and manual sync endpoint remain available.
                    std::cerr << "edge enrollment initial config sync skipped: node_id=" << id
                              << " error=" << error.what() << '\n';
                } catch (...) {
                    std::cerr << "edge enrollment initial config sync skipped: node_id=" << id
                              << " error=unknown exception\n";
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "edge enrollment update failed: stage=" << stage << " node_id=" << id
                      << " status=" << status << " error=" << error.what() << '\n';
            throw;
        }
    }

    template <typename Context>
    ruvia::Task<void> removeEnrollment(Context& c, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        const auto nodeId = query.column("id", "node");
        ruvia::DbQuery link(c.pool());
        link.select(link.cast(link.value(1), ruvia::DbDataType::kInteger))
            .from(service::link::LinkEntity::tableName(), "link")
            .where(link.binary(link.column("edge_node_id", "link"), ruvia::DbBinaryOperator::kEqual, link.importExpression(nodeId, "node", "node")))
            .limit(1);
        query.select({ query.column("imei", "node"), query.column("enrollment_status", "node"), query.exists(link) })
            .from(EdgeNodeEntity::tableName(), "node")
            .where(query.binary(nodeId, ruvia::DbBinaryOperator::kEqual, query.cast(query.value(id), ruvia::DbDataType::kUuid)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17001, "边缘节点不存在", 404);
        }
        const auto imei = std::string(rows.front()[0].value().value_or(std::string_view{}));
        const auto status = rows.front()[1].value().value_or(std::string_view{});
        if (status != "pending") {
            service::common::fail(17021, "只能删除待处理的注册申请", 409);
        }
        if (rows.front()[2].template as<bool>().value_or(false)) {
            service::common::fail(17021, "注册申请仍被采集链路引用，无法删除", 409);
        }

        const std::array<std::string, 5> keys{
            "iot:edge:session:" + std::string(id),
            "iot:edge:metadata:" + std::string(id),
            "iot:edge:egress:" + std::string(id),
            "iot:edge:config:" + std::string(id),
            "iot:edge:config-revision:" + std::string(id),
        };
        co_await c.redis().set(module_wire::authKey(imei), std::string(id) + "|pending");
        for (const auto& key : keys) {
            (void)co_await c.redis().del(key);
        }
        ruvia::DbQuery removal(c.pool());
        removal.deleteFrom(EdgeNodeEntity::tableName())
            .where(removal.binary(removal.column("id"), ruvia::DbBinaryOperator::kEqual, removal.cast(removal.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(removal.binary(removal.column("enrollment_status"), ruvia::DbBinaryOperator::kEqual, removal.value("pending")))
            .returning({ removal.column("id") });
        const auto removed = co_await c.db().query(removal);
        if (removed.empty()) {
            service::common::fail(17021, "注册状态已变化，请刷新后重试", 409);
        }
        (void)co_await c.redis().del(module_wire::authKey(imei));
    }

    template <typename Context>
    ruvia::Task<void> renameNode(Context& c, std::string_view id, const NodeNameBody& body) {
        const auto& nameValue = body.get<"name">();
        if (nameValue.view().empty()) {
            service::common::fail(17003, "节点名称不能为空", 400);
        }
        const std::string name(nameValue.view());
        ruvia::DbQuery update(c.pool());
        update.update(EdgeNodeEntity::tableName())
            .set("name", update.value(name))
            .set("updated_at", update.call("now"))
            .where(update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual, update.cast(update.value(id), ruvia::DbDataType::kUuid)))
            .returning({ update.column("id") });
        const auto updated = co_await c.db().query(update);
        if (updated.empty()) {
            service::common::fail(17001, "边缘节点不存在", 404);
        }
    }

    template <typename Context>
    ruvia::Task<void> setNodeGroup(Context& c, std::string_view id, const NodeGroupBody& body) {
        const auto& maybeGroup = body.get<"groupId">();
        if (!maybeGroup) {
            service::common::fail(17003, "节点分组参数不能为空", 400);
        }
        const std::string groupId(maybeGroup->view());
        if (!groupId.empty()) {
            ruvia::DbQuery group(c.pool());
            group.select(group.cast(group.value(1), ruvia::DbDataType::kInteger))
                .from(EdgeNodeGroupEntity::tableName())
                .where(group.binary(group.column("id"), ruvia::DbBinaryOperator::kEqual, group.cast(group.value(groupId), ruvia::DbDataType::kUuid)))
                .andWhere(group.binary(group.column("status"), ruvia::DbBinaryOperator::kEqual, group.cast(group.value("enabled"), { .customName = "status_enum" })))
                .andWhere(group.unary(ruvia::DbUnaryOperator::kIsNull, group.column("deleted_at")))
                .limit(1);
            if ((co_await c.db().query(group)).empty()) {
                service::common::fail(17001, "边缘节点分组不存在或已停用", 404);
            }
        }
        ruvia::DbQuery update(c.pool());
        update.update(EdgeNodeEntity::tableName())
            .set("group_id", nullableUuid(update, groupId))
            .set("updated_at", update.call("now"))
            .where(update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual, update.cast(update.value(id), ruvia::DbDataType::kUuid)))
            .returning({ update.column("id") });
        const auto updated = co_await c.db().query(update);
        if (updated.empty()) {
            service::common::fail(17001, "边缘节点不存在", 404);
        }
    }

    template <typename Context>
    ruvia::Task<void> queueNetwork(Context& c, std::string_view nodeId, const NetworkBody& body) {
        const auto networkConfigVersion = co_await requireNetworkManagement(c, nodeId);
        const auto& configs = body.get<"interfaces">();
        if (configs.empty()) {
            service::common::fail(17003, "至少配置一个网络接口", 400);
        }
        const auto available = co_await manageableInterfaces(c, nodeId);
        std::unordered_set<std::string> names;
        std::unordered_set<std::string> previousNames;
        std::unordered_set<std::string> devices;
        const auto taskId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        module_pb::NetworkConfigRequest request;
        std::uint8_t requestId[16]{};
        if (!module_wire::uuidBytes(taskId, requestId)) {
            service::common::fail(17005, "边缘命令编码失败", 500);
        }
        request.set_request_id(module_wire::bytes(requestId, 16));
        for (const auto& config : configs) {
            if (config.get<"operation">().view().empty()) {
                service::common::fail(17003, "网络接口操作不能为空", 400);
            }
            if (config.get<"name">().view().empty()) {
                service::common::fail(17003, "逻辑接口名称不能为空", 400);
            }
            const std::string operation(config.get<"operation">().view());
            const std::string name(config.get<"name">().view());
            if (operation != "upsert" && operation != "delete") {
                service::common::fail(17003, "网络接口操作只支持 upsert 或 delete", 400);
            }
            const std::string previousName =
                config.get<"previousName">() ? std::string(config.get<"previousName">()->view())
                                             : std::string{};
            if (!names.emplace(name).second) {
                service::common::fail(17003, "同一请求不能重复配置逻辑接口 " + name, 400);
            }
            if (name == "loopback") {
                service::common::fail(17003, "loopback 接口不允许远程修改", 400);
            }
            if (!previousName.empty()) {
                if (operation != "upsert" || previousName == name ||
                    previousName == "loopback") {
                    service::common::fail(17003, "原逻辑接口名称无效", 400);
                }
                if (networkConfigVersion < 3) {
                    service::common::fail(
                        17004,
                        "节点代理版本过旧，请先升级后再修改逻辑接口名称",
                        409
                    );
                }
                if (!previousNames.emplace(previousName).second) {
                    service::common::fail(
                        17003,
                        "同一请求不能重复修改原逻辑接口 " + previousName,
                        400
                    );
                }
            }

            auto* item = request.add_interfaces();
            item->set_logical_name(name);
            if (operation == "delete") {
                item->set_operation(module_pb::NETWORK_CONFIG_DELETE);
            } else {
                item->set_operation(module_pb::NETWORK_CONFIG_UPSERT);
                item->set_previous_logical_name(previousName);
                const std::string mode =
                    config.get<"mode">() ? std::string(config.get<"mode">()->view()) : std::string{};
                const std::string device = config.get<"device">()
                    ? std::string(config.get<"device">()->view())
                    : std::string{};
                const std::string ip =
                    config.get<"ip">() ? std::string(config.get<"ip">()->view()) : std::string{};
                const std::string gateway =
                    config.get<"gateway">() ? std::string(config.get<"gateway">()->view()) : std::string{};
                const bool bridge = config.get<"bridge">() && *config.get<"bridge">();
                const auto prefix =
                    config.get<"prefixLength">() ? static_cast<std::uint32_t>(*config.get<"prefixLength">()) : 0U;
                const auto& ports = config.get<"bridgePorts">();
                validateNetworkConfig(name, mode, device, bridge, ports, ip, prefix, gateway, available, devices);
                item->set_mode(mode == "static" ? module_pb::NETWORK_ADDRESS_STATIC : module_pb::NETWORK_ADDRESS_DHCP);
                item->set_bridge(bridge);
                item->set_device(device);
                item->set_name(bridge ? "br-" + name : device);
                if (ports) {
                    for (const auto& port : *ports) {
                        item->add_bridge_ports(port.view());
                    }
                }
                item->set_ip(ip);
                item->set_prefix_length(prefix);
                item->set_gateway(gateway);
            }
        }
        const auto rollbackTimeoutSec = body.get<"rollbackTimeoutSec">().value_or(60);
        if (rollbackTimeoutSec < 30 || rollbackTimeoutSec > 300) {
            service::common::fail(17003, "回滚等待时间必须在 30 - 300 秒之间", 400);
        }
        request.set_rollback_timeout_sec(static_cast<std::uint32_t>(rollbackTimeoutSec));
        co_await createNetworkTaskAndQueue(c, nodeId, taskId, configs.size(), request);
    }

    template <typename Context>
    ruvia::Task<void> validateFirmwareTarget(Context& c, std::string_view nodeId) {
        co_await requireNodeCapability(c, nodeId, "firmwareUpdate", "远程刷写");
    }

    template <typename Context, typename Transaction>
    ruvia::Task<module_pb::FirmwareUpdateRequest> createFirmwareTask(Context& c, Transaction& transaction, std::string_view nodeId, std::string_view firmwareId, bool keepSettings) {
        const std::string firmwareIdText(firmwareId);
        ruvia::DbQuery query(c.pool());
        query.select({ query.column("sha256", "firmware"), query.column("size_bytes", "firmware"), query.column("download_token", "firmware"), booleanText(query, jsonText(query, query.column("capability", "node"), "firmwareStream")) })
            .from(EdgeFirmwareEntity::tableName(), "firmware")
            .join(ruvia::DbJoinType::kInner, EdgeNodeEntity::tableName(), query.binary(query.column("id", "node"), ruvia::DbBinaryOperator::kEqual, query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)), "node")
            .where(query.binary(query.column("id", "firmware"), ruvia::DbBinaryOperator::kEqual, query.cast(query.value(firmwareIdText), ruvia::DbDataType::kUuid)))
            .limit(1);
        const auto rows = co_await transaction.query(query);
        if (rows.empty()) {
            service::common::fail(17009, "固件不存在", 404);
        }
        const auto& row = rows.front();
        const auto taskId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        module_pb::FirmwareUpdateRequest request;
        std::uint8_t bytes[32]{};
        if (!module_wire::uuidBytes(taskId, bytes)) {
            service::common::fail(17010, "固件请求 ID 无效", 500);
        }
        const auto requestId = module_wire::bytes(bytes, 16);
        std::string download;
        if (row[3].value().value_or(std::string_view{}) != "t") {
            auto baseUrl = std::string(c.env().get("EDGE_PUBLIC_BASE_URL").value_or(service::message::edge::kDefaultPublicBaseUrl));
            while (baseUrl.ends_with('/')) {
                baseUrl.pop_back();
            }
            download = baseUrl +
                "/edge/v1/firmware/" + firmwareIdText + "/download?token=" +
                std::string(row[2].value().value_or(std::string_view{}));
        }
        if (!hex(row[0].value().value_or(std::string_view{}), bytes, 32)) {
            service::common::fail(17010, "固件摘要无效", 500);
        }
        const auto size = static_cast<std::uint64_t>(
            integer(row[1].value().value_or(std::string_view{}))
        );
        if (download.size() > 512 || size == 0) {
            service::common::fail(17010, "固件请求元数据无效", 500);
        }
        request.set_request_id(requestId);
        request.set_download_url(download);
        request.set_sha256(module_wire::bytes(bytes, 32));
        request.set_size_bytes(size);
        request.set_keep_settings(keepSettings);
        const std::string json = "{\"firmware_id\":\"" + firmwareIdText + "\"}";
        ruvia::DbQuery task(c.pool());
        task.insertInto(EdgeTaskEntity::tableName(), { "id", "node_id", "task_type", "request", "created_by" })
            .values({ task.cast(task.value(taskId), Type::kUuid), task.cast(task.value(nodeId), Type::kUuid), task.value("firmware"), task.cast(task.value(json), Type::kJsonb), task.cast(task.value(c.userId), Type::kUuid) });
        (void)co_await transaction.execute(task);
        co_return request;
    }

    template <typename Transaction>
    static ruvia::Task<void> lockFirmwareStorage(Transaction& transaction) {
        // 与后台回收使用同一事务锁；同一 Worker 只使用此事务的连接。
        ruvia::DbQuery lock;
        lock.select(lock.call("pg_advisory_xact_lock", {
            lock.cast(lock.value(17011), Type::kInteger), lock.cast(lock.value(1), Type::kInteger)}));
        (void)co_await transaction.query(lock);
    }

    template <typename Transaction>
    static ruvia::Task<std::string> findReusableFirmware(Transaction& transaction, std::string_view hash, std::int64_t bytes) {
        ruvia::DbQuery query;
        query.select({query.cast(query.column("id"), Type::kText), query.column("storage_path")})
            .from(EdgeFirmwareEntity::tableName())
            .where(query.binary(query.column("sha256"), Op::kEqual, query.value(hash)))
            .andWhere(query.binary(query.column("size_bytes"), Op::kEqual, query.value(bytes)))
            .orderBy(query.column("created_at"), ruvia::DbOrderDirection::kDesc);
        const auto rows = co_await transaction.query(query);
        for (const auto& row : rows) {
            const std::filesystem::path path(std::string(row[1].value().value_or(std::string_view{})));
            std::error_code error;
            if (std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) && !error &&
                std::filesystem::file_size(path, error) == static_cast<std::uint64_t>(bytes) && !error)
                co_return std::string(row[0].value().value_or(std::string_view{}));
        }
        co_return std::string{};
    }

    template <typename Context>
    ruvia::Task<bool> reuseFirmware(Context& c, std::string_view nodeId, const FirmwareReuseBody& body) {
        co_await validateFirmwareTarget(c, nodeId);
        const auto hash = body.get<"sha256">().view();
        std::uint8_t digest[32]{};
        if (hash.size() != 64 || !hex(hash, digest, 32) ||
            hash.find_first_not_of("0123456789abcdef") != std::string_view::npos)
            service::common::fail(17017, "固件摘要无效", 400);
        auto transaction = co_await c.db().beginTransaction();
        co_await lockFirmwareStorage(transaction);
        const auto id = co_await findReusableFirmware(transaction, hash, body.get<"sizeBytes">().value);
        if (id.empty())
            co_return false;
        auto request = co_await createFirmwareTask(c, transaction, nodeId, id, body.get<"keepSettings">()->value);
        co_await transaction.commit();
        co_await module_wire::queueControl(c, nodeId, "queue-firmware", request);
        co_return true;
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<FirmwareDto>> firmwares(Context& c) {
        ruvia::DbQuery query(c.pool());
        query.select({ query.cast(query.column("id"), ruvia::DbDataType::kText), query.column("version"), query.column("file_name"), query.column("sha256"), query.column("size_bytes"), query.call("iot_utc_timestamp", { query.column("created_at") }) })
            .from(EdgeFirmwareEntity::tableName())
            .orderBy(query.column("created_at"), ruvia::DbOrderDirection::kDesc)
            .limit(100);
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<FirmwareDto> result(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.template set<"id">(row[0].value().value_or(std::string_view{}))
                .template set<"version">(row[1].value().value_or(std::string_view{}))
                .template set<"fileName">(row[2].value().value_or(std::string_view{}))
                .template set<"sha256">(row[3].value().value_or(std::string_view{}))
                .template set<"sizeBytes">(integer(row[4].value().value_or(std::string_view{})))
                .template set<"createdAt">(row[5].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<std::filesystem::path> prepareFirmwareUpload(Context& c, std::string_view nodeId) {
        co_await validateFirmwareTarget(c, nodeId);
        std::error_code error;
        auto directory = std::filesystem::absolute(std::filesystem::path(c.env().get("EDGE_FIRMWARE_DIR").value_or("firmware")), error);
        if (error || (!std::filesystem::create_directories(directory, error) && error)) {
            service::common::fail(17011, "无法创建固件目录", 500);
        }
        co_return directory.lexically_normal();
    }

    template <typename Context>
    ruvia::Task<void> finishFirmwareUpload(Context& c, std::string_view nodeId, const FirmwareUploadBody& body, const std::filesystem::path& uploadedPath, std::string_view hash, std::int64_t bytes) {
        const auto directory = co_await prepareFirmwareUpload(c, nodeId);
        const auto source = uploadedPath.lexically_normal();
        if (source.empty() || source.parent_path() != directory ||
            source.extension() != ".upload" || !service::common::isUuid(source.stem().string()) ||
            hash.size() != 64 || bytes != body.get<"sizeBytes">().value) {
            service::common::fail(17017, "固件上传尚未完成", 400);
        }
        std::error_code error;
        if (std::filesystem::file_size(source, error) != static_cast<std::uint64_t>(bytes) || error) {
            service::common::fail(17017, "固件大小校验失败", 400);
        }
        auto transaction = co_await c.db().beginTransaction();
        co_await lockFirmwareStorage(transaction);
        auto storageId = co_await findReusableFirmware(transaction, hash, bytes);
        if (storageId.empty()) {
            storageId = source.stem().string();
            const auto destination = directory / (storageId + ".bin");
            std::filesystem::rename(source, destination, error);
            if (error)
                service::common::fail(17014, "无法保存固件文件", 500);
            // 提交结果不确定时保留文件，由后台在确认无引用且超过保留期后回收。
            const auto fileName = std::filesystem::path(std::string(body.get<"fileName">().view())).filename().string();
            const auto token = randomToken();
            const auto storagePath = destination.string();
            ruvia::DbQuery query(c.pool());
            query.insertInto(EdgeFirmwareEntity::tableName(), { "id", "version", "file_name", "storage_path", "sha256", "size_bytes", "download_token", "created_by" })
                .values({ query.cast(query.value(storageId), Type::kUuid), query.value(""), query.value(fileName), query.value(storagePath), query.value(hash), query.cast(query.value(bytes), Type::kBigInt), query.value(token), query.cast(query.value(c.userId), Type::kUuid) });
            (void)co_await transaction.execute(query);
        }
        auto request = co_await createFirmwareTask(c, transaction, nodeId, storageId, body.get<"keepSettings">()->value);
        co_await transaction.commit();
        co_await module_wire::queueControl(c, nodeId, "queue-firmware", request);
    }

    template <typename Context>
    ruvia::Task<std::pair<std::filesystem::path, std::string>> firmwareDownload(
        Context& c,
        std::string_view id,
        std::string_view token
    ) {
        ruvia::DbQuery query(c.pool());
        query.select({ query.column("storage_path"), query.column("file_name") })
            .from(EdgeFirmwareEntity::tableName())
            .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual, query.cast(query.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(query.binary(query.column("download_token"), ruvia::DbBinaryOperator::kEqual, query.value(token)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17009, "固件不存在或下载凭据无效", 404);
        }
        co_return std::pair<std::filesystem::path, std::string>{
            std::filesystem::path(std::string(rows.front()[0].value().value_or(std::string_view{}))),
            std::string(rows.front()[1].value().value_or(std::string_view{}))
        };
    }

    template <typename Context>
    ruvia::Task<std::string> openSerialDebug(
        Context& c,
        std::string_view nodeId,
        const SerialDebugOpenRequest& body,
        std::string_view connectionId,
        std::string_view requestedSessionId
    ) {
        co_await requireNodeCapability(c, nodeId, "serialDebug", "串口调试");
        const std::string path(body.get<"path">().view());
        if (!path.starts_with("/dev/") || path.find_first_of("\r\n") != std::string::npos) {
            service::common::fail(17021, "串口路径无效", 400);
        }
        ruvia::DbQuery port(c.pool());
        port.select(port.column("available"))
            .from(EdgeNodeSerialEntity::tableName())
            .where(port.binary(port.column("node_id"), Op::kEqual, port.cast(port.value(nodeId), Type::kUuid)))
            .andWhere(port.binary(port.column("path"), Op::kEqual, port.value(path)));
        const auto rows = co_await c.db().query(port);
        if (rows.empty() || !rows.front()[0].template as<bool>().value_or(false)) {
            service::common::fail(17021, "节点串口不存在或不可用", 409);
        }
        const auto result = co_await serialOperation(c, nodeId, requestedSessionId, connectionId, "serial-open", ",\"path\":" + service::utils::jsonQuoted(path));
        const auto parsed = ruvia::JsonValue::parse(result);
        const auto sessionId = parsed ? parsed->template get<ruvia::String>("id") : std::nullopt;
        if (!sessionId || sessionId->view() != requestedSessionId) {
            service::common::fail(17021, "串口会话响应无效", 502);
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<std::string> serialOperation(Context& c, std::string_view nodeId, std::string_view sessionId, std::string_view connectionId, std::string_view operation, std::string fields = {}) {
        if (!service::common::isUuid(connectionId) ||
            (!sessionId.empty() && !service::common::isUuid(sessionId))) {
            service::common::fail(17021, "串口会话身份无效", 400);
        }
        const auto quote = service::utils::jsonQuoted;
        co_return co_await service::rpc::call(c, "edge-serial", operation, "{\"nodeId\":" + quote(nodeId) + ",\"userId\":" + quote(c.userId) + ",\"connectionId\":" + quote(connectionId) + ",\"sessionId\":" + quote(sessionId) + fields + "}");
    }

    template <typename Context>
    ruvia::Task<std::string> serialEvents(Context& c, std::string_view nodeId, std::string_view sessionId, std::string_view connectionId, std::int64_t& cursor) {
        const auto result = co_await serialOperation(c, nodeId, sessionId, connectionId, "serial-events", ",\"after\":" + std::to_string(cursor));
        const auto parsed = ruvia::JsonValue::parse(result);
        const auto next = parsed ? parsed->template get<ruvia::Int64>("cursor") : std::nullopt;
        if (!next || next->value < cursor) {
            service::common::fail(17021, "串口事件响应无效", 502);
        }
        cursor = next->value;
        co_return result;
    }

    template <typename Context>
    ruvia::Task<std::string> openTerminal(Context& c, std::string_view nodeId, std::string_view sessionId, std::string_view connectionId, unsigned columns, unsigned rows) {
        ruvia::DbQuery query(c.pool());
        query.select({ query.column("enrollment_status"), booleanText(query, jsonText(query, query.column("capability"), "terminal")) })
            .from(EdgeNodeEntity::tableName())
            .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual, query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)))
            .limit(1);
        const auto nodes = co_await c.db().query(query);
        if (nodes.empty()) {
            service::common::fail(17001, "边缘节点不存在", 404);
        }
        if (nodes.front()[0].value().value_or(std::string_view{}) != "approved" ||
            !nodes.front()[1].template as<bool>().value_or(false)) {
            service::common::fail(17018, "节点未检测到 ttyd", 409);
        }
        const auto sessionKey = "iot:edge:session:" + std::string(nodeId);
        const auto session = co_await c.redis().get(sessionKey);
        if (!session) {
            service::common::fail(17019, "节点当前离线", 409);
        }
        co_return co_await terminalOperation(c, nodeId, sessionId, connectionId, "terminal-open", ",\"columns\":" + std::to_string(columns) + ",\"rows\":" + std::to_string(rows));
    }

    template <typename Context>
    ruvia::Task<std::string> terminalOperation(Context& c, std::string_view nodeId, std::string_view sessionId, std::string_view connectionId, std::string_view operation, std::string fields = {}) {
        const auto quote = service::utils::jsonQuoted;
        co_return co_await service::rpc::call(c, "edge-terminal", operation, "{\"nodeId\":" + quote(nodeId) + ",\"sessionId\":" + quote(sessionId) + ",\"connectionId\":" + quote(connectionId) + ",\"userId\":" + quote(c.userId) + fields + "}");
    }

    template <typename Context>
    ruvia::Task<void> writeTerminal(Context& c, std::string_view nodeId, std::string_view sessionId, std::string_view connectionId, unsigned protocolVersion, std::uint64_t& sequence, std::string_view encoded) {
        const auto bytes = service::utils::decodeBase64(encoded, 16384);
        if (!bytes) {
            service::common::fail(17018, "终端输入编码无效", 400);
        }
        const auto notification = service::live::bus().subscribe(c.worker(), service::edge::terminal_state::terminalAckTopic(nodeId, sessionId));
        for (std::size_t offset = 0; offset < bytes->size(); offset += 4096) {
            if (c.stopToken().stopRequested()) {
                service::common::fail(17018, "终端输入已取消", 409);
            }
            const auto part = std::string_view(*bytes).substr(offset, std::min<std::size_t>(4096, bytes->size() - offset));
            const auto next = protocolVersion >= 5 ? ++sequence : 0;
            if (next > 9007199254740991ULL) {
                service::common::fail(17018, "终端输入序号已耗尽", 409);
            }
            (void)co_await terminalOperation(c, nodeId, sessionId, connectionId, "terminal-write", ",\"content\":" + service::utils::jsonQuoted(service::utils::encodeBase64(part)) + ",\"sequence\":" + std::to_string(next));
            if (protocolVersion < 5) {
                continue;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            for (;;) {
                const auto reply = co_await terminalOperation(c, nodeId, sessionId, connectionId, "terminal-input-status", ",\"sequence\":" + std::to_string(next));
                const auto parsed = ruvia::JsonValue::parse(reply);
                const auto acknowledged = parsed ? parsed->template get<ruvia::Bool>("acknowledged") : std::nullopt;
                if (!acknowledged) {
                    service::common::fail(17018, "终端确认格式无效", 502);
                }
                if (acknowledged->value) {
                    break;
                }
                const auto remaining = deadline - std::chrono::steady_clock::now();
                if (remaining <= std::chrono::steady_clock::duration::zero()) {
                    service::common::fail(17018, "终端输入确认超时", 504);
                }
                const auto change = co_await notification->receiver.receiveFor(remaining, c.stopToken());
                if (c.stopToken().stopRequested()) {
                    service::common::fail(17018, "终端输入已取消", 409);
                }
                if (!change.hasValue()) {
                    service::common::fail(17018, "终端输入确认超时", 504);
                }
            }
        }
    }

    template <typename Context>
    ruvia::Task<LogsDto> logSnapshot(Context& c, std::string_view nodeId, const LogsQuery& query) {
        co_await requireNodeCapability(c, nodeId, "logs", "节点日志");
        LogsDto output(ruvia::ModelOptions{ .resource = c.arena() });
        ruvia::BoxedArray<LogLineDto> lines(ruvia::ModelOptions{ .resource = c.arena() });
        const auto payload = co_await c.redis().get("iot:edge:logs:snapshot:" + std::string(nodeId));
        if (payload) {
            module_pb::LogResult result;
            if (!result.ParseFromArray(payload->data(), static_cast<int>(payload->size()))) {
                service::common::fail(17020, "节点日志快照解析失败", 502);
            }
            const auto limit = std::clamp<std::int64_t>(query.get<"limit">().value_or(48), 1, 48);
            std::int64_t count = 0;
            for (const auto& line : result.lines()) {
                if (query.get<"level">() && query.get<"level">()->view() != line.level()) {
                    continue;
                }
                if (query.get<"source">() && query.get<"source">()->view() != line.source()) {
                    continue;
                }
                if (count++ >= limit) {
                    break;
                }
                lines.emplace(ruvia::ModelOptions{ .resource = c.arena() }).template set<"time">(service::common::utcTimestampFromMilliseconds(line.time_ms())).template set<"level">(line.level()).template set<"source">(line.source()).template set<"message">(line.message()).template set<"detail">(line.detail());
            }
        }
        output.template set<"lines">(std::move(lines));
        co_return output;
    }

    template <typename Context>
    ruvia::Task<LogsDto> logs(Context& c, std::string_view nodeId, const LogsQuery& query) {
        co_await requireNodeCapability(c, nodeId, "logs", "节点日志");
        const auto session = co_await c.redis().get(sessionKey(nodeId));
        if (!session) {
            service::common::fail(17019, "节点当前离线", 409);
        }

        const auto requestId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        module_pb::LogRequest request;
        std::uint8_t bytes[16]{};
        if (!module_wire::uuidBytes(requestId, bytes)) {
            service::common::fail(17020, "日志请求 ID 无效", 500);
        }
        request.set_request_id(module_wire::bytes(bytes, sizeof(bytes)));
        const auto limit = query.get<"limit">().value_or(48);
        if (limit < 1 || limit > 48) {
            service::common::fail(17020, "日志条数必须在 1 - 48 之间", 400);
        }
        request.set_limit(static_cast<std::uint32_t>(limit));
        if (query.get<"level">()) {
            const auto level = std::string(query.get<"level">()->view());
            if (level != "debug" && level != "info" && level != "warn" && level != "error") {
                service::common::fail(17020, "日志级别无效", 400);
            }
            request.set_level(level);
        }
        if (query.get<"source">()) {
            const auto sourceValue = std::string(query.get<"source">()->view());
            if (sourceValue.size() > 16) {
                service::common::fail(17020, "日志来源不能超过 16 个字符", 400);
            }
            request.set_source(sourceValue);
        }
        const auto key = logResultKey(requestId);
        const auto changed = service::live::bus().subscribe(c.worker(), key);
        co_await module_wire::queueControl(c, nodeId, "request-logs", request);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto payload = co_await c.redis().get(key)) {
                (void)co_await c.redis().del(key);
                module_pb::LogResult result;
                if (!result.ParseFromArray(payload->data(), static_cast<int>(payload->size()))) {
                    service::common::fail(17020, "节点日志回包解析失败", 502);
                }
                if (!result.success()) {
                    service::common::fail(17020, result.message(), 502);
                }
                LogsDto output(ruvia::ModelOptions{ .resource = c.arena() });
                ruvia::BoxedArray<LogLineDto> lines(
                    ruvia::ModelOptions{ .resource = c.arena() }
                );
                for (const auto& line : result.lines()) {
                    auto& item = lines.emplace(ruvia::ModelOptions{ .resource = c.arena() });
                    item.template set<"time">(service::common::utcTimestampFromMilliseconds(line.time_ms()))
                        .template set<"level">(line.level())
                        .template set<"source">(line.source())
                        .template set<"message">(line.message())
                        .template set<"detail">(line.detail());
                }
                output.template set<"lines">(std::move(lines));
                co_return output;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()
            );
            if (remaining.count() <= 0 || c.stopToken().stopRequested()) {
                break;
            }
            (void)co_await changed->receiver.receiveFor(remaining, c.stopToken());
        }
        service::common::fail(17020, "节点日志请求超时", 504);
    }

    template <typename Context>
    ruvia::Task<void> setLogLevel(Context& c, std::string_view nodeId, const LogLevelBody& body) {
        co_await requireNodeCapability(c, nodeId, "logs", "节点日志");
        const auto session = co_await c.redis().get(sessionKey(nodeId));
        if (!session) {
            service::common::fail(17019, "节点当前离线", 409);
        }

        const auto& levelValue = body.get<"level">();
        if (levelValue.view().empty()) {
            service::common::fail(17020, "日志级别不能为空", 400);
        }
        const auto level = std::string(levelValue.view());
        if (level != "silent" && level != "debug" && level != "info" && level != "warn" && level != "error") {
            service::common::fail(17020, "日志级别无效", 400);
        }
        const auto requestId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        module_pb::LogLevelRequest request;
        std::uint8_t bytes[16]{};
        if (!module_wire::uuidBytes(requestId, bytes)) {
            service::common::fail(17020, "日志请求 ID 无效", 500);
        }
        request.set_request_id(module_wire::bytes(bytes, sizeof(bytes)));
        request.set_level(level);
        const auto key = logLevelResultKey(requestId);
        const auto changed = service::live::bus().subscribe(c.worker(), key);
        co_await module_wire::queueControl(c, nodeId, "set-log-level", request);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto payload = co_await c.redis().get(key)) {
                (void)co_await c.redis().del(key);
                module_pb::LogLevelResult result;
                if (!result.ParseFromArray(payload->data(), static_cast<int>(payload->size()))) {
                    service::common::fail(17020, "节点日志等级回包解析失败", 502);
                }
                if (!result.success()) {
                    service::common::fail(17020, result.message(), 502);
                }
                const auto currentLevel = result.level().empty() ? level : result.level();
                ruvia::DbQuery update(c.pool());
                const auto logPath = update.array(
                    { update.cast(update.value("log"), ruvia::DbDataType::kText) }
                );
                const auto levelPath = update.array(
                    { update.cast(update.value("log"), ruvia::DbDataType::kText),
                      update.cast(update.value("level"), ruvia::DbDataType::kText) }
                );
                const auto withLog = update.call(
                    "jsonb_set",
                    { update.column("status"), logPath, update.coalesce({ update.binary(update.column("status"), ruvia::DbBinaryOperator::kJsonGet, update.cast(update.value("log"), ruvia::DbDataType::kText)), update.cast(update.value("{}"), ruvia::DbDataType::kJsonb) }), update.cast(update.value(true), ruvia::DbDataType::kBoolean) }
                );
                const auto withLevel = update.call(
                    "jsonb_set",
                    { withLog, levelPath, update.call("to_jsonb", { update.cast(update.value(currentLevel), ruvia::DbDataType::kText) }), update.cast(update.value(true), ruvia::DbDataType::kBoolean) }
                );
                update.update(EdgeNodeEntity::tableName())
                    .set("status", withLevel)
                    .set("updated_at", update.call("now"))
                    .where(update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual, update.cast(update.value(nodeId), ruvia::DbDataType::kUuid)));
                (void)co_await c.db().execute(update);
                co_return;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0 || c.stopToken().stopRequested()) {
                break;
            }
            (void)co_await changed->receiver.receiveFor(remaining, c.stopToken());
        }
        service::common::fail(17020, "节点日志等级请求超时", 504);
    }

  private:
    using Op = ruvia::DbBinaryOperator;
    using Unary = ruvia::DbUnaryOperator;
    using Type = ruvia::DbDataType;

    static ruvia::DbExpression textKey(ruvia::DbQuery& query, std::string_view key) {
        return query.cast(query.value(key), Type::kText);
    }

    static ruvia::DbExpression jsonValue(ruvia::DbQuery& query, ruvia::DbExpression object, std::string_view key) {
        return query.binary(object, Op::kJsonGet, textKey(query, key));
    }

    static ruvia::DbExpression jsonText(ruvia::DbQuery& query, ruvia::DbExpression object, std::string_view key) {
        return query.binary(object, Op::kJsonGetText, textKey(query, key));
    }

    static ruvia::DbExpression textDefault(ruvia::DbQuery& query, ruvia::DbExpression value, std::string_view fallback = {}) {
        return query.coalesce({ value, query.cast(query.value(fallback), Type::kText) });
    }

    static ruvia::DbExpression nullableText(ruvia::DbQuery& query, std::string_view value) {
        return query.nullIf(query.cast(query.value(value), Type::kText), query.cast(query.value(""), Type::kText));
    }

    static ruvia::DbExpression nullableUuid(ruvia::DbQuery& query, std::string_view value) {
        return query.cast(nullableText(query, value), Type::kUuid);
    }

    static ruvia::DbExpression booleanText(ruvia::DbQuery& query, ruvia::DbExpression value) {
        const auto normalized = query.call(
            "lower",
            { textDefault(query, value) }
        );
        const auto trueValue = query.cast(query.value(true), Type::kBoolean);
        const auto falseValue = query.cast(query.value(false), Type::kBoolean);
        return query.caseWhen(
            { { query.binary(normalized, Op::kEqual, query.cast(query.value("true"), Type::kText)), trueValue },
              { query.binary(normalized, Op::kEqual, query.cast(query.value("t"), Type::kText)), trueValue },
              { query.binary(normalized, Op::kEqual, query.cast(query.value("1"), Type::kText)), trueValue } },
            falseValue
        );
    }

    static ruvia::DbExpression safeBigInt(ruvia::DbQuery& query, ruvia::DbExpression value, std::int64_t fallback) {
        const auto text = textDefault(query, value);
        const auto valid = query.binary(
            text,
            Op::kRegex,
            query.cast(query.value("^-?[0-9]{1,18}$"), Type::kText)
        );
        return query.coalesce(
            { query.caseWhen({ { valid, query.cast(text, Type::kBigInt) } }),
              query.cast(query.value(fallback), Type::kBigInt) }
        );
    }

    static ruvia::DbQuery selectedGroupIds(const ruvia::DbQuery& source, std::string_view rootId) {
        ruvia::DbQuery roots(source.resource());
        roots.select(roots.column("id", "root"))
            .from(EdgeNodeGroupEntity::tableName(), "root")
            .where(roots.binary(roots.column("id", "root"), Op::kEqual, roots.cast(roots.value(rootId), Type::kUuid)))
            .andWhere(roots.unary(Unary::kIsNull, roots.column("deleted_at", "root")));

        ruvia::DbQuery descendants(source.resource());
        descendants.select(descendants.column("id", "child"))
            .from(EdgeNodeGroupEntity::tableName(), "child")
            .join(ruvia::DbJoinType::kInner, "selected_group", descendants.binary(descendants.column("parent_id", "child"), Op::kEqual, descendants.column("id", "parent")), "parent")
            .andWhere(descendants.unary(Unary::kIsNull, descendants.column("deleted_at", "child")));
        roots.combine(ruvia::DbSetOperation::kUnionAll, descendants);

        ruvia::DbQuery result(source.resource());
        result.with("selected_group", roots, { .recursive = true })
            .select(result.column("id"))
            .from("selected_group");
        return result;
    }

    static void applyNodeFilters(ruvia::DbQuery& query, const std::optional<std::string>& keyword, const std::optional<std::string>& status, const std::optional<std::string>& groupId) {
        std::vector<ruvia::DbExpression> predicates;
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            const auto nodeImei = query.binary(query.column("imei", "node"), Op::kILike, query.value(pattern));
            const auto nodeName = query.binary(
                textDefault(query, query.column("name", "node")),
                Op::kILike,
                query.value(pattern)
            );
            const auto groupName = query.binary(
                textDefault(query, query.column("name", "group_item")),
                Op::kILike,
                query.value(pattern)
            );
            const auto model = query.binary(query.column("model", "node"), Op::kILike, query.value(pattern));
            predicates.emplace_back(query.binary(
                query.binary(query.binary(nodeImei, Op::kOr, nodeName), Op::kOr, groupName),
                Op::kOr,
                model
            ));
        }
        if (status && !status->empty()) {
            predicates.emplace_back(query.binary(query.column("enrollment_status", "node"), Op::kEqual, query.value(*status)));
        }
        if (groupId && !groupId->empty()) {
            if (*groupId == "ungrouped") {
                predicates.emplace_back(
                    query.unary(Unary::kIsNull, query.column("group_id", "node"))
                );
            } else {
                const auto groups = selectedGroupIds(query, *groupId);
                predicates.emplace_back(query.binary(
                    query.column("group_id", "node"),
                    Op::kIn,
                    query.subquery(groups)
                ));
            }
        }
        if (predicates.empty()) {
            return;
        }
        auto combined = predicates.front();
        for (std::size_t index = 1; index < predicates.size(); ++index) {
            combined = query.binary(combined, Op::kAnd, predicates[index]);
        }
        query.where(combined);
    }

    template <typename Context>
    static ruvia::Task<void> validateGroupParent(Context& c, std::string_view parentId, std::string_view groupId) {
        if (parentId.empty()) {
            co_return;
        }
        if (parentId == groupId) {
            service::common::fail(17003, "上级分组不能是自身", 409);
        }
        ruvia::DbQuery parent(c.pool());
        parent.select(parent.cast(parent.value(1), Type::kInteger))
            .from(EdgeNodeGroupEntity::tableName())
            .where(parent.binary(parent.column("id"), Op::kEqual, parent.cast(parent.value(parentId), Type::kUuid)))
            .andWhere(parent.unary(Unary::kIsNull, parent.column("deleted_at")))
            .limit(1);
        if ((co_await c.db().query(parent)).empty()) {
            service::common::fail(17001, "上级分组不存在", 404);
        }
        if (!groupId.empty()) {
            ruvia::DbQuery roots(c.pool());
            roots.select(roots.column("id", "root"))
                .from(EdgeNodeGroupEntity::tableName(), "root")
                .where(roots.binary(roots.column("parent_id", "root"), Op::kEqual, roots.cast(roots.value(groupId), Type::kUuid)))
                .andWhere(roots.unary(Unary::kIsNull, roots.column("deleted_at", "root")));
            ruvia::DbQuery descendants(c.pool());
            descendants.select(descendants.column("id", "child"))
                .from(EdgeNodeGroupEntity::tableName(), "child")
                .join(ruvia::DbJoinType::kInner, "descendants", descendants.binary(descendants.column("parent_id", "child"), Op::kEqual, descendants.column("id", "parent")), "parent")
                .andWhere(descendants.unary(Unary::kIsNull, descendants.column("deleted_at", "child")));
            roots.combine(ruvia::DbSetOperation::kUnionAll, descendants);
            ruvia::DbQuery cycle(c.pool());
            cycle.with("descendants", roots, { .recursive = true })
                .select(cycle.cast(cycle.value(1), Type::kInteger))
                .from("descendants")
                .where(cycle.binary(cycle.column("id"), Op::kEqual, cycle.cast(cycle.value(parentId), Type::kUuid)))
                .limit(1);
            const auto cycleRows = co_await c.db().query(cycle);
            if (!cycleRows.empty()) {
                service::common::fail(17003, "不能把分组移动到自己的子分组", 409);
            }
        }
    }

    static ruvia::DbQuery nodeSelect(std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        const auto nodeId = query.column("id", "node");
        const auto status = query.column("status", "node");
        const auto capability = query.column("capability", "node");
        const auto mobile = query.column("mobile", "node");
        const auto config = jsonValue(query, status, "config");
        const auto outbox = jsonValue(query, status, "outbox");
        const auto signal = jsonValue(query, mobile, "signal");
        const auto vpnCapability = jsonValue(query, capability, "vpn");

        ruvia::DbQuery latest(resource);
        latest.select({ latest.alias(latest.column("status", "task"), "status"), latest.alias(jsonText(latest, latest.column("result", "task"), "progressPercent"), "progress_percent"), latest.alias(jsonText(latest, latest.column("result", "task"), "downloadedBytes"), "downloaded_bytes"), latest.alias(jsonText(latest, latest.column("result", "task"), "totalBytes"), "total_bytes"), latest.alias(jsonText(latest, latest.column("result", "task"), "message"), "message") })
            .from(EdgeTaskEntity::tableName(), "task")
            .where(latest.binary(latest.column("node_id", "task"), Op::kEqual, latest.importExpression(nodeId, "node", "node")))
            .andWhere(latest.binary(latest.column("task_type", "task"), Op::kEqual, latest.cast(latest.value("firmware"), Type::kText)))
            .orderBy(latest.column("created_at", "task"), ruvia::DbOrderDirection::kDesc)
            .limit(1);

        ruvia::DbQuery vpn(resource);
        const auto vpnNodeId = vpn.importExpression(nodeId, "node", "node");
        const std::array<ruvia::DbOrderTerm, 1> vpnOrder{ { { vpn.column(service::edge_node::entities::VpnRouteEntity::columnName<"virtual_cidr">(), "route"), ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kDefault } } };
        vpn.select(vpn.aggregate("string_agg", { vpn.column(service::edge_node::entities::VpnRouteEntity::columnName<"virtual_cidr">(), "route"), vpn.cast(vpn.value(","), Type::kText) }, false, vpnOrder))
            .from(service::edge_node::entities::VpnRouteEntity::tableName(), "route")
            .join(ruvia::DbJoinType::kInner, service::edge_node::entities::VpnPeerEntity::tableName(), vpn.binary(vpn.column(service::edge_node::entities::VpnPeerEntity::columnName<"id">(), "peer"), Op::kEqual, vpn.column(service::edge_node::entities::VpnRouteEntity::columnName<"edge_peer_id">(), "route")), "peer")
            .where(vpn.binary(vpn.column(service::edge_node::entities::VpnPeerEntity::columnName<"edge_node_id">(), "peer"), Op::kEqual, vpnNodeId))
            .andWhere(vpn.binary(vpn.column(service::edge_node::entities::VpnPeerEntity::columnName<"status">(), "peer"), Op::kEqual, vpn.value("active")))
            .andWhere(vpn.unary(Unary::kIsTrue, vpn.column(service::edge_node::entities::VpnRouteEntity::columnName<"enabled">(), "route")))
            .andWhere(vpn.binary(vpn.column(service::edge_node::entities::VpnRouteEntity::columnName<"status">(), "route"), Op::kEqual, vpn.value("active")));

        query.select({ query.cast(nodeId, Type::kText), query.column("imei", "node"), textDefault(query, query.column("name", "node")), query.column("model", "node"), query.column("software_version", "node"), query.column("hostname", "node"), query.column("architecture", "node"), query.column("openwrt_release", "node"), query.column("enrollment_status", "node"), query.cast(query.value(false), Type::kBoolean), textDefault(query, query.call("iot_utc_timestamp", { query.column("last_seen_at", "node") })), query.call("iot_utc_timestamp", { query.column("created_at", "node") }), safeBigInt(query, jsonText(query, config, "activeVersion"), 0), safeBigInt(query, jsonText(query, config, "desiredVersion"), 0), textDefault(query, jsonText(query, config, "state"), "idle"), textDefault(query, jsonText(query, config, "message")), safeBigInt(query, jsonText(query, outbox, "records"), 0), safeBigInt(query, jsonText(query, outbox, "bytes"), 0), textDefault(query, jsonText(query, jsonValue(query, status, "log"), "level"), "info"), booleanText(query, jsonText(query, capability, "networkConfig")), safeBigInt(query, jsonText(query, capability, "networkConfigVersion"), 0), booleanText(query, jsonText(query, capability, "firmwareUpdate")), booleanText(query, jsonText(query, capability, "deviceConfig")), booleanText(query, jsonText(query, capability, "modemControl")), booleanText(query, jsonText(query, capability, "terminal")), booleanText(query, jsonText(query, capability, "logs")), booleanText(query, jsonText(query, mobile, "available")), textDefault(query, jsonText(query, mobile, "simState"), "unknown"), textDefault(query, jsonText(query, mobile, "iccid")), safeBigInt(query, jsonText(query, signal, "csq"), 99), safeBigInt(query, jsonText(query, signal, "rssiDbm"), -1), safeBigInt(query, jsonText(query, signal, "percent"), 0), booleanText(query, jsonText(query, mobile, "registered")), safeBigInt(query, jsonText(query, mobile, "registrationStatus"), -1), textDefault(query, jsonText(query, mobile, "apn")), textDefault(query, jsonText(query, mobile, "operator")), booleanText(query, jsonText(query, mobile, "connected")), textDefault(query, jsonText(query, mobile, "ipv4")), textDefault(query, query.column("status", "firmware")), safeBigInt(query, query.column("progress_percent", "firmware"), 0), safeBigInt(query, query.column("downloaded_bytes", "firmware"), 0), safeBigInt(query, query.column("total_bytes", "firmware"), 0), textDefault(query, query.column("message", "firmware")), booleanText(query, jsonText(query, vpnCapability, "supportsVpn")), textDefault(query, jsonText(query, vpnCapability, "wireguardVersion")), textDefault(query, jsonText(query, vpnCapability, "agentVersion")), textDefault(query, jsonText(query, vpnCapability, "publicKey")), textDefault(query, query.cast(query.column("group_id", "node"), Type::kText)), textDefault(query, query.column("name", "group_item")), textDefault(query, query.subquery(vpn)), booleanText(query, jsonText(query, capability, "serialDebug")) })
            .from(EdgeNodeEntity::tableName(), "node")
            .join(ruvia::DbJoinType::kLeft, EdgeNodeGroupEntity::tableName(), query.binary(query.column("group_id", "node"), Op::kEqual, query.column("id", "group_item")), "group_item")
            .join(ruvia::DbJoinType::kLeft, latest, query.cast(query.value(true), Type::kBoolean), "firmware", { .lateral = true });
        return query;
    }

    static std::int64_t integer(std::string_view value) {
        std::int64_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }

    template <typename Context, typename Row>
    static ruvia::Task<void> fillNode(Context& c, EdgeNodeDto& node, const Row& row) {
        NodeStatusDto status(ruvia::ModelOptions{ .resource = c.arena() });
        ConfigStatusDto config(ruvia::ModelOptions{ .resource = c.arena() });
        config.template set<"activeVersion">(integer(row[12].value().value_or(std::string_view{})));
        config.template set<"desiredVersion">(integer(row[13].value().value_or(std::string_view{})));
        config.template set<"state">(row[14].value().value_or(std::string_view{}));
        config.template set<"message">(row[15].value().value_or(std::string_view{}));
        OutboxStatusDto outbox(ruvia::ModelOptions{ .resource = c.arena() });
        outbox.template set<"records">(integer(row[16].value().value_or(std::string_view{})));
        outbox.template set<"bytes">(integer(row[17].value().value_or(std::string_view{})));
        LogStatusDto log(ruvia::ModelOptions{ .resource = c.arena() });
        log.template set<"level">(row[18].value().value_or(std::string_view{}));
        const auto session = co_await c.redis().get(
            "iot:edge:session:" + std::string(row[0].value().value_or(std::string_view{}))
        );
        status.template set<"online">(row[8].value().value_or(std::string_view{}) == "approved" && session.has_value());
        status.template set<"lastSeenAt">(row[10].value().value_or(std::string_view{}));
        status.template set<"config">(std::move(config));
        status.template set<"outbox">(std::move(outbox));
        status.template set<"log">(std::move(log));

        CapabilityDto capability(ruvia::ModelOptions{ .resource = c.arena() });
        capability.template set<"networkConfig">(row[19].value().value_or(std::string_view{}) == "t");
        capability.template set<"networkConfigVersion">(
            integer(row[20].value().value_or(std::string_view{}))
        );
        capability.template set<"firmwareUpdate">(row[21].value().value_or(std::string_view{}) == "t");
        capability.template set<"deviceConfig">(row[22].value().value_or(std::string_view{}) == "t");
        capability.template set<"modemControl">(row[23].value().value_or(std::string_view{}) == "t");
        capability.template set<"terminal">(row[24].value().value_or(std::string_view{}) == "t");
        capability.template set<"logs">(row[25].value().value_or(std::string_view{}) == "t");
        capability.template set<"serialDebug">(row[50].value().value_or(std::string_view{}) == "t");
        VpnCapabilityDto vpn(ruvia::ModelOptions{ .resource = c.arena() });
        vpn.template set<"supportsVpn">(row[43].value().value_or(std::string_view{}) == "t");
        vpn.template set<"wireguardVersion">(row[44].value().value_or(std::string_view{}));
        vpn.template set<"agentVersion">(row[45].value().value_or(std::string_view{}));
        vpn.template set<"publicKey">(row[46].value().value_or(std::string_view{}));
        capability.template set<"vpn">(std::move(vpn));

        SignalDto signal(ruvia::ModelOptions{ .resource = c.arena() });
        signal.template set<"csq">(integer(row[29].value().value_or(std::string_view{})));
        signal.template set<"rssiDbm">(integer(row[30].value().value_or(std::string_view{})));
        signal.template set<"percent">(integer(row[31].value().value_or(std::string_view{})));
        MobileDto mobile(ruvia::ModelOptions{ .resource = c.arena() });
        mobile.template set<"available">(row[26].value().value_or(std::string_view{}) == "t");
        mobile.template set<"simState">(row[27].value().value_or(std::string_view{}));
        mobile.template set<"iccid">(row[28].value().value_or(std::string_view{}));
        mobile.template set<"signal">(std::move(signal));
        mobile.template set<"registered">(row[32].value().value_or(std::string_view{}) == "t");
        mobile.template set<"registrationStatus">(
            integer(row[33].value().value_or(std::string_view{}))
        );
        mobile.template set<"apn">(row[34].value().value_or(std::string_view{}));
        mobile.template set<"operatorName">(row[35].value().value_or(std::string_view{}));
        mobile.template set<"connected">(row[36].value().value_or(std::string_view{}) == "t");
        mobile.template set<"ipv4">(row[37].value().value_or(std::string_view{}));

        FirmwareStatusDto firmware(ruvia::ModelOptions{ .resource = c.arena() });
        firmware.template set<"state">(row[38].value().value_or(std::string_view{}));
        firmware.template set<"progressPercent">(
            integer(row[39].value().value_or(std::string_view{}))
        );
        firmware.template set<"downloadedBytes">(
            integer(row[40].value().value_or(std::string_view{}))
        );
        firmware.template set<"totalBytes">(integer(row[41].value().value_or(std::string_view{})));
        firmware.template set<"message">(row[42].value().value_or(std::string_view{}));

        node.template set<"id">(row[0].value().value_or(std::string_view{}));
        node.template set<"imei">(row[1].value().value_or(std::string_view{}));
        node.template set<"name">(row[2].value().value_or(std::string_view{}));
        node.template set<"groupId">(row[47].value().value_or(std::string_view{}));
        node.template set<"groupName">(row[48].value().value_or(std::string_view{}));
        node.template set<"model">(row[3].value().value_or(std::string_view{}));
        node.template set<"softwareVersion">(row[4].value().value_or(std::string_view{}));
        node.template set<"hostname">(row[5].value().value_or(std::string_view{}));
        node.template set<"architecture">(row[6].value().value_or(std::string_view{}));
        node.template set<"openwrtRelease">(row[7].value().value_or(std::string_view{}));
        node.template set<"enrollmentStatus">(row[8].value().value_or(std::string_view{}));
        node.template set<"status">(std::move(status));
        node.template set<"capability">(std::move(capability));
        node.template set<"mobile">(std::move(mobile));
        node.template set<"firmware">(std::move(firmware));
        ruvia::BoxedArray<ruvia::String> virtualCidrs(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& cidr : split(row[49].value().value_or(std::string_view{}))) {
            if (!cidr.empty()) {
                virtualCidrs.emplace(cidr, ruvia::ModelOptions{ .resource = c.arena() });
            }
        }
        node.template set<"vpnVirtualCidrs">(std::move(virtualCidrs));
        node.template set<"createdAt">(row[11].value().value_or(std::string_view{}));
    }

    static std::vector<std::string> split(std::string_view value) {
        std::vector<std::string> result;
        while (!value.empty()) {
            const auto comma = value.find(',');
            result.emplace_back(value.substr(0, comma));
            if (comma == std::string_view::npos) {
                break;
            }
            value.remove_prefix(comma + 1);
        }
        return result;
    }

    static ruvia::DbQuery taskSelect(std::pmr::memory_resource* resource, std::string_view nodeId) {
        ruvia::DbQuery query(resource);
        const auto result = query.column("result", "task");
        query.select({ query.cast(query.column("id", "task"), Type::kText), query.column("task_type", "task"), query.column("status", "task"), textDefault(query, jsonText(query, result, "message")), safeBigInt(query, jsonText(query, result, "progressPercent"), 0), safeBigInt(query, jsonText(query, result, "downloadedBytes"), 0), safeBigInt(query, jsonText(query, result, "totalBytes"), 0), query.call("iot_utc_timestamp", { query.column("created_at", "task") }), query.call("iot_utc_timestamp", { query.column("updated_at", "task") }) })
            .from(EdgeTaskEntity::tableName(), "task")
            .where(query.binary(query.column("node_id", "task"), Op::kEqual, query.cast(query.value(nodeId), Type::kUuid)))
            .orderBy(query.column("created_at", "task"), ruvia::DbOrderDirection::kDesc)
            .limit(50);
        return query;
    }

    template <typename Context>
    static ruvia::Task<ruvia::BoxedArray<InterfaceDto>> interfaces(Context& c, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        const auto name = query.column("name", "iface");
        const auto displayName = query.column("display_name", "iface");
        const auto mac = textDefault(query, query.column("mac", "iface"));
        const auto up = query.column("is_up", "iface");
        const auto bridge = query.column("is_bridge", "iface");
        const auto ipv4 = textDefault(query, query.column("ipv4", "iface"));
        const auto prefixLength = query.coalesce(
            { query.column("prefix_length", "iface"), query.cast(query.value(0), Type::kInteger) }
        );
        const auto gateway = textDefault(query, query.column("gateway", "iface"));
        const auto bridgePortsJson = query.coalesce(
            { query.column("bridge_ports", "iface"),
              query.cast(query.value("[]"), Type::kJsonb) }
        );
        const auto bridgeValue = query.column("value", "bridge_value");
        const std::array<ruvia::DbOrderTerm, 1> bridgeOrder{ { { bridgeValue, ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kDefault } } };
        const auto bridgePorts = query.coalesce(
            { query.aggregate("string_agg", { bridgeValue, query.cast(query.value(","), Type::kText) }, false, bridgeOrder),
              query.cast(query.value(""), Type::kText) }
        );
        query.select({ name, displayName, mac, up, bridge, ipv4, prefixLength, gateway, bridgePorts })
            .from(EdgeNodeInterfaceEntity::tableName(), "iface")
            .joinFunction(ruvia::DbJoinType::kLeft, query.call("jsonb_array_elements_text", { bridgePortsJson }), query.cast(query.value(true), Type::kBoolean), "bridge_value", { .lateral = true, .columns = { { .name = "value" } } })
            .where(query.binary(query.column("node_id", "iface"), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .groupBy({ name, displayName, mac, up, bridge, ipv4, prefixLength, gateway })
            .orderBy(name);
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<InterfaceDto> result(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            ruvia::BoxedArray<ruvia::String> ports(
                ruvia::ModelOptions{ .resource = c.arena() }
            );
            for (const auto& port : split(row[8].value().value_or(std::string_view{}))) {
                if (!port.empty()) {
                    ports.emplace(port, ruvia::ModelOptions{ .resource = c.arena() });
                }
            }
            item.template set<"name">(row[0].value().value_or(std::string_view{}))
                .template set<"displayName">(row[1].value().value_or(std::string_view{}))
                .template set<"mac">(row[2].value().value_or(std::string_view{}))
                .template set<"up">(row[3].value().value_or(std::string_view{}) == "t")
                .template set<"bridge">(row[4].value().value_or(std::string_view{}) == "t")
                .template set<"ipv4">(row[5].value().value_or(std::string_view{}))
                .template set<"prefixLength">(integer(row[6].value().value_or(std::string_view{})))
                .template set<"gateway">(row[7].value().value_or(std::string_view{}))
                .template set<"bridgePorts">(std::move(ports));
        }
        co_return result;
    }

    template <typename Context>
    static ruvia::Task<ruvia::BoxedArray<NetworkDto>> networks(Context& c, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        const auto name = query.column("name", "network");
        const auto mode = query.column("address_mode", "network");
        const auto device = query.column("device", "network");
        const auto up = query.column("is_up", "network");
        const auto bridge = query.column("is_bridge", "network");
        const auto ipv4 = textDefault(query, query.column("ipv4", "network"));
        const auto prefixLength = query.coalesce(
            { query.column("prefix_length", "network"), query.cast(query.value(0), Type::kInteger) }
        );
        const auto gateway = textDefault(query, query.column("gateway", "network"));
        const auto bridgePortsJson = query.coalesce(
            { query.column("bridge_ports", "network"),
              query.cast(query.value("[]"), Type::kJsonb) }
        );
        const auto bridgeValue = query.column("value", "bridge_value");
        const std::array<ruvia::DbOrderTerm, 1> bridgeOrder{ { { bridgeValue, ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kDefault } } };
        const auto bridgePorts = query.coalesce(
            { query.aggregate("string_agg", { bridgeValue, query.cast(query.value(","), Type::kText) }, false, bridgeOrder),
              query.cast(query.value(""), Type::kText) }
        );
        query.select({ name, mode, device, up, bridge, ipv4, prefixLength, gateway, bridgePorts })
            .from(EdgeNodeNetworkEntity::tableName(), "network")
            .joinFunction(ruvia::DbJoinType::kLeft, query.call("jsonb_array_elements_text", { bridgePortsJson }), query.cast(query.value(true), Type::kBoolean), "bridge_value", { .lateral = true, .columns = { { .name = "value" } } })
            .where(query.binary(query.column("node_id", "network"), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .groupBy({ name, mode, device, up, bridge, ipv4, prefixLength, gateway })
            .orderBy(name);
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<NetworkDto> result(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            ruvia::BoxedArray<ruvia::String> ports(
                ruvia::ModelOptions{ .resource = c.arena() }
            );
            for (const auto& port : split(row[8].value().value_or(std::string_view{}))) {
                if (!port.empty()) {
                    ports.emplace(port, ruvia::ModelOptions{ .resource = c.arena() });
                }
            }
            item.template set<"name">(row[0].value().value_or(std::string_view{}))
                .template set<"mode">(row[1].value().value_or(std::string_view{}))
                .template set<"device">(row[2].value().value_or(std::string_view{}))
                .template set<"up">(row[3].value().value_or(std::string_view{}) == "t")
                .template set<"bridge">(row[4].value().value_or(std::string_view{}) == "t")
                .template set<"ipv4">(row[5].value().value_or(std::string_view{}))
                .template set<"prefixLength">(integer(row[6].value().value_or(std::string_view{})))
                .template set<"gateway">(row[7].value().value_or(std::string_view{}))
                .template set<"bridgePorts">(std::move(ports));
        }
        co_return result;
    }

    template <typename Context>
    static ruvia::Task<ruvia::BoxedArray<SerialDto>> serialPorts(Context& c, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        query.select({ query.column("path"), query.column("display_name"), query.column("available"), query.column("rs485") })
            .from(EdgeNodeSerialEntity::tableName())
            .where(query.binary(query.column("node_id"), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .orderBy(query.column("path"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<SerialDto> result(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.template set<"path">(row[0].value().value_or(std::string_view{}))
                .template set<"displayName">(row[1].value().value_or(std::string_view{}))
                .template set<"available">(row[2].value().value_or(std::string_view{}) == "t")
                .template set<"rs485">(row[3].value().value_or(std::string_view{}) == "t");
        }
        co_return result;
    }

    template <typename Context>
    static ruvia::Task<ruvia::BoxedArray<TaskDto>> tasks(Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(taskSelect(c.pool(), id));
        ruvia::BoxedArray<TaskDto> result(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.template set<"id">(row[0].value().value_or(std::string_view{}))
                .template set<"taskType">(row[1].value().value_or(std::string_view{}))
                .template set<"status">(row[2].value().value_or(std::string_view{}))
                .template set<"message">(row[3].value().value_or(std::string_view{}))
                .template set<"progressPercent">(integer(row[4].value().value_or(std::string_view{})))
                .template set<"downloadedBytes">(integer(row[5].value().value_or(std::string_view{})))
                .template set<"totalBytes">(integer(row[6].value().value_or(std::string_view{})))
                .template set<"createdAt">(row[7].value().value_or(std::string_view{}))
                .template set<"updatedAt">(row[8].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    static std::string jsonEscape(std::string_view value) {
        std::string output;
        output.reserve(value.size());
        for (const char ch : value) {
            if (ch == '"' || ch == '\\') {
                output.push_back('\\');
            }
            if (static_cast<unsigned char>(ch) >= 0x20U) {
                output.push_back(ch);
            }
        }
        return output;
    }

    static bool ipv4(std::string_view input, std::uint32_t& output) {
        output = 0;
        for (int part = 0; part < 4; ++part) {
            const auto dot = input.find('.');
            const auto token = input.substr(0, dot);
            unsigned value{};
            const auto [end, error] =
                std::from_chars(token.data(), token.data() + token.size(), value);
            if (token.empty() || error != std::errc{} || end != token.data() + token.size() ||
                value > 255 || (token.size() > 1 && token.front() == '0')) {
                return false;
            }
            output = (output << 8U) | value;
            if (part == 3) {
                return dot == std::string_view::npos;
            }
            if (dot == std::string_view::npos) {
                return false;
            }
            input.remove_prefix(dot + 1);
        }
        return false;
    }

    static void validateStaticNetwork(std::string_view ipText, std::uint32_t prefix, std::string_view gatewayText) {
        std::uint32_t address{};
        if (!ipv4(ipText, address) || prefix == 0 || prefix > 30) {
            service::common::fail(17003, "静态 IPv4 地址或前缀长度无效", 400);
        }
        const auto mask = 0xffffffffU << (32U - prefix);
        const auto host = address & ~mask;
        if (host == 0 || host == ~mask) {
            service::common::fail(17003, "静态 IPv4 不能是网络地址或广播地址", 400);
        }
        if (!gatewayText.empty()) {
            std::uint32_t gateway{};
            if (!ipv4(gatewayText, gateway) || (gateway & mask) != (address & mask) ||
                gateway == address || (gateway & ~mask) == 0 || (gateway & ~mask) == ~mask) {
                service::common::fail(17003, "网关必须是同网段内不同的合法主机地址", 400);
            }
        }
    }

    template <typename Context>
    static ruvia::Task<std::unordered_set<std::string>>
    manageableInterfaces(Context& c, std::string_view nodeId) {
        ruvia::DbQuery query(c.pool());
        query.select(query.column("name"))
            .from(EdgeNodeInterfaceEntity::tableName())
            .where(query.binary(query.column("node_id"), Op::kEqual, query.cast(query.value(nodeId), Type::kUuid)))
            .andWhere(query.binary(query.column("name"), Op::kNotEqual, query.cast(query.value("lo"), Type::kText)))
            .andWhere(query.unary(Unary::kIsFalse, query.column("is_bridge")));
        const auto rows = co_await c.db().query(query);
        std::unordered_set<std::string> result;
        for (const auto& row : rows) {
            result.emplace(row[0].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    template <typename Ports>
    static void validateNetworkConfig(std::string_view name, std::string_view mode, std::string_view device, bool bridge, const Ports& ports, std::string_view ip, std::uint32_t prefix, std::string_view gateway, const std::unordered_set<std::string>& available, std::unordered_set<std::string>& selected) {
        if (name.size() > 15 || (bridge && name.size() > 12)) {
            service::common::fail(
                17003,
                bridge ? "网桥逻辑名称不能超过 12 个字符"
                       : "逻辑接口名称不能超过 15 个字符",
                400
            );
        }
        if (mode != "dhcp" && mode != "static") {
            service::common::fail(17003, "地址模式只支持 DHCP 或静态 IPv4", 400);
        }

        const auto useDevice = [&](std::string_view value) {
            const std::string text(value);
            if (!available.contains(text)) {
                service::common::fail(
                    17003,
                    "网卡 " + text + " 不可管理、未上报或属于受保护的 4G 上联",
                    400
                );
            }
            if (!selected.emplace(text).second) {
                service::common::fail(17003, "网卡 " + text + " 在同一请求中被重复占用", 400);
            }
        };

        if (bridge) {
            if (!device.empty()) {
                service::common::fail(17003, "网桥不能同时指定单一设备", 400);
            }
            if (!ports || ports->empty()) {
                service::common::fail(17003, "网桥至少需要一个成员网卡", 400);
            }
            for (const auto& port : *ports) {
                useDevice(port.view());
            }
        } else {
            if (device.empty()) {
                service::common::fail(17003, "非网桥接口必须选择一个网卡", 400);
            }
            if (ports && !ports->empty()) {
                service::common::fail(17003, "非网桥接口不能配置网桥成员", 400);
            }
            useDevice(device);
        }

        if (mode == "static") {
            validateStaticNetwork(ip, prefix, gateway);
        } else if (!ip.empty() || prefix != 0 || !gateway.empty()) {
            service::common::fail(17003, "DHCP 接口不能携带静态 IPv4 配置", 400);
        }
    }

    template <typename Context>
    static ruvia::Task<std::int64_t> requireNetworkManagement(
        Context& c,
        std::string_view nodeId
    ) {
        ruvia::DbQuery query(c.pool());
        const auto capability = query.column("capability");
        query.select({ query.column("enrollment_status"), booleanText(query, jsonText(query, capability, "networkConfig")), safeBigInt(query, jsonText(query, capability, "networkConfigVersion"), 0) })
            .from(EdgeNodeEntity::tableName())
            .where(query.binary(query.column("id"), Op::kEqual, query.cast(query.value(nodeId), Type::kUuid)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17001, "边缘节点不存在", 404);
        }
        if (rows.front()[0].value().value_or(std::string_view{}) != "approved") {
            service::common::fail(17002, "边缘节点尚未批准注册", 409);
        }
        if (rows.front()[1].value().value_or(std::string_view{}) != "t") {
            service::common::fail(17004, "网络配置不可用", 409);
        }
        const auto version = rows.front()[2].template as<std::int64_t>().value_or(0);
        if (version < 2) {
            service::common::fail(17004, "节点代理版本过旧，请先升级后再管理网络", 409);
        }
        co_return version;
    }

    static bool hex(std::string_view value, std::uint8_t* output, std::size_t size) {
        if (value.size() != size * 2) {
            return false;
        }
        for (std::size_t index = 0; index < size; ++index) {
            const int high = module_wire::hexDigit(value[index * 2]);
            const int low = module_wire::hexDigit(value[index * 2 + 1]);
            if (high < 0 || low < 0) {
                return false;
            }
            output[index] = static_cast<std::uint8_t>((high << 4U) | low);
        }
        return true;
    }

    static std::string randomToken() {
        std::array<unsigned char, 32> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            throw std::runtime_error("cannot generate firmware token");
        }
        constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(64);
        for (const auto byte : bytes) {
            output.push_back(digits[byte >> 4U]);
            output.push_back(digits[byte & 0x0fU]);
        }
        return output;
    }

    template <typename Context>
    static ruvia::Task<void> requireNodeCapability(Context& c, std::string_view nodeId, std::string_view key, std::string_view feature) {
        ruvia::DbQuery query(c.pool());
        query.select({ query.column("enrollment_status"), booleanText(query, jsonText(query, query.column("capability"), key)) })
            .from(EdgeNodeEntity::tableName())
            .where(query.binary(query.column("id"), Op::kEqual, query.cast(query.value(nodeId), Type::kUuid)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17001, "边缘节点不存在", 404);
        }
        if (rows.front()[0].value().value_or(std::string_view{}) != "approved") {
            service::common::fail(17002, "边缘节点尚未批准注册", 409);
        }
        if (!rows.front()[1].template as<bool>().value_or(false)) {
            service::common::fail(17004, std::string(feature) + "不可用", 409);
        }
    }

    template <typename Context>
    static ruvia::Task<void> insertTask(Context& c, std::string_view nodeId, std::string_view taskId, std::string_view type, std::string_view json, std::string_view userId) {
        ruvia::DbQuery query(c.pool());
        query.insertInto(EdgeTaskEntity::tableName(), { "id", "node_id", "task_type", "request", "created_by" })
            .values({ query.cast(query.value(taskId), Type::kUuid), query.cast(query.value(nodeId), Type::kUuid), query.value(type), query.cast(query.value(json), Type::kJsonb), query.cast(query.value(userId), Type::kUuid) });
        (void)co_await c.db().execute(query);
    }

    static std::string sessionKey(std::string_view nodeId) {
        return "iot:edge:session:" + std::string(nodeId);
    }

    static std::string logResultKey(std::string_view requestId) {
        return "iot:edge:logs:" + std::string(requestId);
    }

    static std::string logLevelResultKey(std::string_view requestId) {
        return "iot:edge:logs:level:" + std::string(requestId);
    }

    template <typename Context, typename Request>
    static ruvia::Task<void>
    createNetworkTaskAndQueue(Context& c, std::string_view nodeId, std::string_view taskId, std::size_t interfaceCount, const Request& request) {
        ruvia::DbQuery query(c.pool());
        const auto requestJson = query.call(
            "jsonb_build_object",
            { query.cast(query.value("interfaceCount"), Type::kText),
              query.cast(query.value(static_cast<std::int64_t>(interfaceCount)), Type::kBigInt) }
        );
        query.insertInto(EdgeTaskEntity::tableName(), { "id", "node_id", "task_type", "request", "created_by" })
            .values({ query.cast(query.value(taskId), Type::kUuid), query.cast(query.value(nodeId), Type::kUuid), query.value("network"), requestJson, query.cast(query.value(c.userId), Type::kUuid) });
        (void)co_await c.db().execute(query);
        co_await module_wire::queueControl(c, nodeId, "queue-network", request);
    }
};

inline EdgeService& edgeService() {
    thread_local EdgeService instance;
    return instance;
}

} // namespace service::edge
