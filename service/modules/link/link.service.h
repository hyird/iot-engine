#pragma once
#include "service/utils/redis.h"

#include "service/utils/number.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/HttpClientHandle.h>
#include <ruvia/web/HttpClientResponse.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/modules/system/outbox/outbox.service.h"
#include "service/modules/edge_node/edge_node.service.h"
#include <ruvia/web/ModelJson.h>
#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/modules/link/link.types.h"
#include "service/modules/link/link.schema.h"
#include "service/modules/link/link.entity.h"
#include "service/modules/system/role/role.entity.h"
#include "service/modules/system/user/user.entity.h"

namespace service::link {

class LinkService {
  public:
    static LinkService& instance() {
        static thread_local LinkService service;
        return service;
    }

    ruvia::Task<LinkPageDataDto> list(ruvia::Context& c, std::int64_t page, std::int64_t pageSize,
                                      std::optional<std::string> keyword,
                                      std::optional<std::string> mode,
                                      std::optional<std::string> protocol,
                                      std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        ruvia::DbQuery count(c.pool());
        count.select(count.aggregate("count", {count.star()})).from(LinkEntity::tableName());
        applyFilters(count, keyword, mode, protocol, status);
        const auto countRows = co_await c.db().query(count);
        const ruvia::Int64 total = countRows.empty()
                                       ? ruvia::Int64{0}
                                       : toInt(countRows.front()[0].value().value_or(std::string_view{}));

        auto query = linkSelect(c.pool());
        applyFilters(query, keyword, mode, protocol, status);
        query.orderBy(query.column("id"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>((page - 1) * pageSize));
        const auto rows = co_await c.db().query(query);

        ruvia::BoxedArray<LinkItemDto> links(ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            auto& item = links.emplace(ruvia::ModelOptions{.resource = c.arena()});
            co_await fill(c, item, row);
        }
        LinkPageDataDto result(ruvia::ModelOptions{.resource = c.arena()});
        result.set<"list">(std::move(links))
            .set<"total">(total)
            .set<"page">(page)
            .set<"pageSize">(pageSize)
            .set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<LinkItemDto> detail(ruvia::Context& c, std::string_view id) {
        auto query = linkSelect(c.pool());
        query.where((LinkEntity::column<"id">() == id &&
                     LinkEntity::column<"deleted_at">().isNull())
                        .expression(query))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(15001, "链路不存在", 404);
        LinkItemDto item(ruvia::ModelOptions{.resource = c.arena()});
        co_await fill(c, item, rows.front());
        co_return item;
    }

    ruvia::Task<ruvia::BoxedArray<LinkOptionDto>> options(ruvia::Context& c) {
        auto query = linkOptionSelect(c.pool());
        query.where((LinkEntity::column<"deleted_at">().isNull() &&
                     LinkEntity::column<"status">() == "enabled")
                        .expression(query))
            .orderBy(query.column("name"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<LinkOptionDto> result(
            ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{.resource = c.arena()});
            LinkEndpointDto endpoint(ruvia::ModelOptions{.resource = c.arena()});
            endpoint.set<"mode">(row[3].value().value_or(std::string_view{}))
                .set<"ip">(row[4].value().value_or(std::string_view{}))
                .set<"port">(toInt(row[5].value().value_or(std::string_view{})))
                .set<"targets">(co_await loadTargets(c, row[0].value().value_or(std::string_view{}), RuntimeStatus{}));
            item.set<"execution">(row[6].value().value_or("collector"));
            item.set<"edgeNodeId">(row[7].value().value_or(""));
            co_await fillEndpoint(c, endpoint, row[0].value().value_or(""));
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"name">(row[1].value().value_or(std::string_view{}))
                .set<"protocol">(row[2].value().value_or(std::string_view{}))
                .set<"endpoint">(std::move(endpoint));
        }
        co_return result;
    }

    LinkEnumsDto enums(ruvia::Context& c) {
        LinkEnumsDto result(ruvia::ModelOptions{.resource = c.arena()});
        const auto modelOptions = ruvia::ModelOptions{.resource = c.arena()};
        ruvia::BoxedArray<ruvia::String> modes(modelOptions);
        modes.emplace("TCP Server", modelOptions);
        modes.emplace("TCP Client", modelOptions);
        ruvia::BoxedArray<ruvia::String> protocols(modelOptions);
        protocols.emplace("SL651", modelOptions);
        protocols.emplace("Modbus", modelOptions);
        protocols.emplace("S7", modelOptions);
        ruvia::BoxedArray<ruvia::String> statuses(modelOptions);
        statuses.emplace("enabled", modelOptions);
        statuses.emplace("disabled", modelOptions);
        result.set<"modes">(std::move(modes))
            .set<"protocols">(std::move(protocols))
            .set<"statuses">(std::move(statuses));
        return result;
    }

    // 客户端、请求和缓存均由接入请求的 Service Worker 持有。
    ruvia::Task<std::string> publicIp(ruvia::Context& c) {
        const auto now = std::chrono::steady_clock::now();
        if (!cachedPublicIp_.empty() && now - publicIpCachedAt_ < std::chrono::minutes(5))
            co_return cachedPublicIp_;
        try {
            const std::array<ruvia::HttpHeaderView, 1> headers{{{"Accept", "text/plain"}}};
            auto response = co_await c.httpClient("link-public-ip").send({.headers = headers});
            if (response.status().value() == 200) {
                const auto body = co_await response.body().readAll(64U * 1024U);
                const auto resolved = parsePublicIp(body);
                if (!resolved.empty()) {
                    cachedPublicIp_ = resolved;
                    publicIpCachedAt_ = std::chrono::steady_clock::now();
                }
            }
        } catch (const ruvia::HttpClientError&) {
            // 查询失败保留旧值；首次查询失败仍返回空字符串。
        }
        co_return cachedPublicIp_;
    }

    ruvia::Task<void> create(ruvia::Context& c, const SaveLinkBody& body) {
        if (body.get<"execution">() && body.get<"execution">()->view() == "edge") {
            co_await saveEdgeChannel(c, {}, body); co_return;
        }
        const auto principal = service::middleware::requireAuth(c);
        const auto name = LinkPayloadValidator::required(body.get<"name">(), "链路名称不能为空");
        const auto protocol = LinkPayloadValidator::required(body.get<"protocol">(), "协议不能为空");
        const auto& endpoint = LinkPayloadValidator::requiredEndpoint(body);
        const auto mode = LinkPayloadValidator::required(endpoint.get<"mode">(), "链路模式不能为空");
        const auto ip = endpoint.get<"ip">() ? std::string(endpoint.get<"ip">()->view()) : "";
        const auto port = endpoint.get<"port">() ? static_cast<std::int64_t>(*endpoint.get<"port">()) : 0;
        const auto status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        LinkPayloadValidator::validateStatus(status);
        const auto& targets = LinkPayloadValidator::requiredTargets(endpoint);
        LinkPayloadValidator::validateConfiguration(mode, protocol, ip, port, targets);
        co_await ensureAvailable(c, name, mode, ip, port, std::nullopt);
        const auto endpointJson = serializeEndpoint(mode, ip, port, targets);
        const auto id = service::common::nextUuidV7();
        auto transaction = co_await c.db().beginTransaction();
        LinkEntity link(c.pool());
        link.set<"id">(id);
        link.set<"name">(name);
        link.set<"protocol">(protocol);
        link.set<"endpoint">(endpointJson);
        link.set<"status">(status);
        link.set<"created_by">(principal.userId);
        link.set<"execution">("collector");
        (void)co_await transaction.getRepository<LinkEntity>().insert(link);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "created", id);
        co_await transaction.commit();
    }

    ruvia::Task<ruvia::BoxedArray<LinkDebugPacketDto>> debugPackets(ruvia::Context& c, std::string_view id) {
        (void)co_await detail(c, id);
        const auto key = service::link::LinkDebugStream::key(id);
        const auto reply = co_await service::message::redis::command(c.redis(), {"XREVRANGE", key, "+", "-", "COUNT", "500"});
        if (reply.kind() != ruvia::RedisValue::Kind::kArray)
            service::message::redis::throwValue("read debug packets", reply);
        ruvia::BoxedArray<LinkDebugPacketDto> result(ruvia::ModelOptions{.resource=c.arena()});
        for (const auto& row : reply.array()) {
            if (row.kind() != ruvia::RedisValue::Kind::kArray || row.array().size() != 2) continue;
            auto& packet = result.emplace(ruvia::ModelOptions{.resource=c.arena()});
            packet.set<"id">(row.array()[0].string());
            const auto fields = row.array()[1].array();
            for (std::size_t index = 0; index + 1 < fields.size(); index += 2) {
                const auto name = fields[index].string();
                const auto value = fields[index + 1].string();
                if (name == "device_id") packet.set<"deviceId">(value);
                else if (name == "direction") packet.set<"direction">(value);
                else if (name == "source") packet.set<"source">(value);
                else if (name == "address") packet.set<"address">(value);
                else if (name == "payload_hex") packet.set<"payloadHex">(value);
                else if (name == "time_ms") packet.set<"timeMs">(value);
            }
        }
        co_return result;
    }

    ruvia::Task<void> setDebug(ruvia::Context& c, std::string_view id, bool enabled) {
        (void)co_await detail(c, id);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery query(c.pool());
        query.update(LinkEntity::tableName())
            .set(LinkEntity::columnName<"debug_enabled">(), query.value(enabled))
            .set("updated_at", query.call("now"))
            .where((LinkEntity::column<"id">() == id && LinkEntity::column<"deleted_at">().isNull()).expression(query));
        (void)co_await transaction.execute(query);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "updated", id);
        co_await transaction.commit();
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id, const SaveLinkBody& body) {
        if (body.get<"execution">() && body.get<"execution">()->view() == "edge") {
            co_await saveEdgeChannel(c, id, body); co_return;
        }
        ruvia::DbQuery lookup(c.pool());
        lookup
            .select({jsonText(lookup, "endpoint", "mode"), lookup.column("protocol"),
                     lookup.column("created_by")})
            .from(LinkEntity::tableName())
            .where((LinkEntity::column<"id">() == id &&
                    LinkEntity::column<"deleted_at">().isNull() &&
                    LinkEntity::column<"execution">() == "collector")
                       .expression(lookup))
            .limit(1);
        const auto rows = co_await c.db().query(lookup);
        if (rows.empty())
            service::common::fail(15001, "链路不存在", 404);
        co_await requireOwner(c, rows.front()[2].value().value_or(std::string_view{}));

        const auto name = LinkPayloadValidator::required(body.get<"name">(), "链路名称不能为空");
        const auto protocol = LinkPayloadValidator::required(body.get<"protocol">(), "协议不能为空");
        const auto& endpoint = LinkPayloadValidator::requiredEndpoint(body);
        const auto mode = LinkPayloadValidator::required(endpoint.get<"mode">(), "链路模式不能为空");
        if (mode != rows.front()[0].value().value_or(std::string_view{}) || protocol != rows.front()[1].value().value_or(std::string_view{}))
            service::common::fail(15006, "链路模式和协议创建后不能修改", 400);
        const auto ip = endpoint.get<"ip">() ? std::string(endpoint.get<"ip">()->view()) : "";
        const auto port = endpoint.get<"port">() ? static_cast<std::int64_t>(*endpoint.get<"port">()) : 0;
        const auto status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        LinkPayloadValidator::validateStatus(status);
        const auto& targets = LinkPayloadValidator::requiredTargets(endpoint);
        LinkPayloadValidator::validateConfiguration(mode, protocol, ip, port, targets);
        co_await ensureAvailable(c, name, mode, ip, port, std::string(id));
        const auto endpointJson = serializeEndpoint(mode, ip, port, targets);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery query(c.pool());
        query.update(LinkEntity::tableName())
            .set("name", query.value(name))
            .set("endpoint", query.cast(query.value(endpointJson), ruvia::DbDataType::kJsonb))
            .set("status", query.value(status))
            .set("updated_at", query.call("now"))
            .where((LinkEntity::column<"id">() == id &&
                    LinkEntity::column<"deleted_at">().isNull() &&
                    LinkEntity::column<"execution">() == "collector")
                       .expression(query));
        const auto nameChanged = query.binary(query.column("name"),
                                               ruvia::DbBinaryOperator::kIsDistinctFrom,
                                               query.value(name));
        const auto endpointChanged = query.binary(
            query.column("endpoint"), ruvia::DbBinaryOperator::kIsDistinctFrom,
            query.cast(query.value(endpointJson), ruvia::DbDataType::kJsonb));
        const auto statusChanged = query.binary(query.column("status"),
                                                ruvia::DbBinaryOperator::kIsDistinctFrom,
                                                query.value(status));
        query.andWhere(query.binary(query.binary(nameChanged, ruvia::DbBinaryOperator::kOr,
                                                  endpointChanged),
                                    ruvia::DbBinaryOperator::kOr, statusChanged));
        const auto updated = co_await transaction.execute(query);
        if (updated.affectedRows() != 0)
            co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "updated", id);
        co_await transaction.commit();
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id) {
        ruvia::DbFindOptions options;
        options.where = LinkEntity::column<"id">() == id && LinkEntity::column<"deleted_at">().isNull();
        const auto link = co_await c.db().getRepository<LinkEntity>().findOne(options);
        if (!link)
            service::common::fail(15001, "链路不存在", 404);
        co_await requireOwner(c, link->get<"created_by">());
        ruvia::DbFindOptions used;
        used.where = entities::DeviceEntity::column<"link_id">() == id &&
                     entities::DeviceEntity::column<"deleted_at">().isNull();
        if (co_await c.db().getRepository<entities::DeviceEntity>().exists(used))
            service::common::fail(15008, "链路已被设备使用，请先删除关联设备", 409);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbExpressions expressions(c.pool());
        (void)co_await transaction.getRepository<LinkEntity>().update(
            LinkEntity::column<"id">() == id && LinkEntity::column<"deleted_at">().isNull(),
            {{"deleted_at", expressions.call("now")}, {"updated_at", expressions.call("now")}});
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "deleted", id);
        co_await transaction.commit();
    }

  private:
    static ruvia::DbExpression jsonText(ruvia::DbQuery& query, std::string_view column,
                                        std::string_view key, std::string_view table = {}) {
        return query.binary(query.column(column, table), ruvia::DbBinaryOperator::kJsonGetText,
                            query.cast(query.value(key), ruvia::DbDataType::kText));
    }

    static ruvia::DbExpression jsonValue(ruvia::DbQuery& query, std::string_view column,
                                         std::string_view key, std::string_view table = {}) {
        return query.binary(query.column(column, table), ruvia::DbBinaryOperator::kJsonGet,
                            query.cast(query.value(key), ruvia::DbDataType::kText));
    }

    static ruvia::DbQuery linkSelect(std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        const auto endpointPort = query.coalesce(
            {query.nullIf(jsonText(query, "endpoint", "port"), query.value("")),
             query.value("0")});
        query.select({query.cast(query.column("id"), ruvia::DbDataType::kText),
                      query.column("name"), query.column("protocol"),
                      jsonText(query, "endpoint", "mode"),
                      query.coalesce({jsonText(query, "endpoint", "ip"), query.value("")}),
                      endpointPort, query.column("status"),
                      query.cast(query.column("created_by"), ruvia::DbDataType::kText),
                      query.call("iot_utc_timestamp", {query.column("created_at")}),
                      query.call("iot_utc_timestamp", {query.column("updated_at")}),
                      query.column("execution"),
                      query.coalesce({query.cast(query.column("edge_node_id"),
                                                 ruvia::DbDataType::kText),
                                      query.value("")}), query.column(LinkEntity::columnName<"debug_enabled">())})
            .from(LinkEntity::tableName());
        return query;
    }

    static ruvia::DbQuery linkOptionSelect(std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        const auto endpointPort = query.coalesce(
            {query.nullIf(jsonText(query, "endpoint", "port"), query.value("")),
             query.value("0")});
        query.select({query.cast(query.column("id"), ruvia::DbDataType::kText),
                      query.column("name"), query.column("protocol"),
                      jsonText(query, "endpoint", "mode"),
                      query.coalesce({jsonText(query, "endpoint", "ip"), query.value("")}),
                      endpointPort, query.column("execution"),
                      query.coalesce({query.cast(query.column("edge_node_id"),
                                                 ruvia::DbDataType::kText),
                                      query.value("")})})
            .from(LinkEntity::tableName());
        return query;
    }

    static void applyFilters(ruvia::DbQuery& query, const std::optional<std::string>& keyword,
                             const std::optional<std::string>& mode,
                             const std::optional<std::string>& protocol,
                             const std::optional<std::string>& status) {
        query.where(LinkEntity::column<"deleted_at">().isNull().expression(query));
        if (keyword && !keyword->empty())
            query.andWhere(LinkEntity::column<"name">().ilike("%" + *keyword + "%")
                               .expression(query));
        if (mode && !mode->empty())
            query.andWhere(query.binary(jsonText(query, "endpoint", "mode"),
                                        ruvia::DbBinaryOperator::kEqual, query.value(*mode)));
        if (protocol && !protocol->empty())
            query.andWhere((LinkEntity::column<"protocol">() == *protocol).expression(query));
        if (status && !status->empty())
            query.andWhere((LinkEntity::column<"status">() == *status).expression(query));
    }

    static ruvia::Task<void> fillEndpoint(ruvia::Context& c, LinkEndpointDto& endpoint, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        query.select({jsonText(query, "endpoint", "transport"),
                      jsonText(query, "endpoint", "interface"),
                      jsonText(query, "endpoint", "baud_rate"),
                      jsonText(query, "endpoint", "data_bits"),
                      jsonText(query, "endpoint", "stop_bits"),
                      jsonText(query, "endpoint", "parity"),
                      jsonText(query, "endpoint", "rs485")})
            .from(LinkEntity::tableName())
            .where((LinkEntity::column<"id">() == id &&
                    LinkEntity::column<"execution">() == "edge")
                       .expression(query))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) co_return;
        const auto& row = rows.front();
        endpoint.set<"transport">(row[0].value().value_or(""));
        endpoint.set<"interfaceName">(row[1].value().value_or(""));
        endpoint.set<"baudRate">(toInt(row[2].value().value_or("9600")));
        endpoint.set<"dataBits">(toInt(row[3].value().value_or("8")));
        endpoint.set<"stopBits">(toInt(row[4].value().value_or("1")));
        endpoint.set<"parity">(row[5].value().value_or("none"));
        endpoint.set<"rs485">(row[6].value().value_or("false") == "true");
    }

    ruvia::Task<void> saveEdgeChannel(ruvia::Context& c, std::string_view existingId, const SaveLinkBody& body) {
        const auto principal = service::middleware::requireAuth(c);
        const auto name = LinkPayloadValidator::required(body.get<"name">(), "通道名称不能为空");
        const auto protocol = LinkPayloadValidator::required(body.get<"protocol">(), "协议不能为空");
        const auto nodeId = LinkPayloadValidator::required(body.get<"edgeNodeId">(), "请选择边缘节点");
        LinkPayloadValidator::validateNodeId(nodeId);
        const auto& endpoint = LinkPayloadValidator::requiredEndpoint(body);
        const auto transport = LinkPayloadValidator::required(endpoint.get<"transport">(), "请选择传输类型");
        const auto interfaceName = LinkPayloadValidator::required(endpoint.get<"interfaceName">(), "请选择接口");
        LinkPayloadValidator::validateEdgeEndpoint(endpoint, protocol, transport, interfaceName);
        ruvia::DbQuery nodeQuery(c.pool());
        nodeQuery.select(nodeQuery.cast(nodeQuery.value(1), ruvia::DbDataType::kInteger))
            .from(service::link::entities::EdgeNodeEntity::tableName())
            .where(nodeQuery.binary(nodeQuery.column(service::link::entities::EdgeNodeEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual,
                                   nodeQuery.cast(nodeQuery.value(nodeId),
                                                 ruvia::DbDataType::kUuid)))
            .andWhere(nodeQuery.binary(nodeQuery.column(service::link::entities::EdgeNodeEntity::columnName<"enrollment_status">()),
                                      ruvia::DbBinaryOperator::kEqual,
                                      nodeQuery.value("approved")))
            .andWhere(nodeQuery.binary(jsonText(nodeQuery, "capability", "deviceConfig"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       nodeQuery.value("true")));
        const auto node = co_await c.db().query(nodeQuery);
        if (node.empty()) service::common::fail(15002, "节点未批准或不支持采集配置", 400);
        if (transport == "serial") {
            ruvia::DbQuery serialQuery(c.pool());
            serialQuery.select(serialQuery.cast(serialQuery.value(1), ruvia::DbDataType::kInteger))
                .from(service::link::entities::EdgeNodeSerialEntity::tableName())
                .where(serialQuery.binary(serialQuery.column(service::link::entities::EdgeNodeSerialEntity::columnName<"node_id">()),
                                         ruvia::DbBinaryOperator::kEqual,
                                         serialQuery.cast(serialQuery.value(nodeId),
                                                         ruvia::DbDataType::kUuid)))
                .andWhere(serialQuery.binary(serialQuery.column(service::link::entities::EdgeNodeSerialEntity::columnName<"path">()),
                                             ruvia::DbBinaryOperator::kEqual,
                                             serialQuery.value(interfaceName)))
                .andWhere(serialQuery.unary(ruvia::DbUnaryOperator::kIsTrue,
                                             serialQuery.column(service::link::entities::EdgeNodeSerialEntity::columnName<"available">())));
            const auto serial = co_await c.db().query(serialQuery);
            if (serial.empty()) service::common::fail(15002, "所选串口不存在或当前不可用", 409);
        } else {
            ruvia::DbQuery networkQuery(c.pool());
            networkQuery.select(networkQuery.column(service::link::entities::EdgeNodeInterfaceEntity::columnName<"ipv4">()))
                .from(service::link::entities::EdgeNodeInterfaceEntity::tableName())
                .where(networkQuery.binary(networkQuery.column(service::link::entities::EdgeNodeInterfaceEntity::columnName<"node_id">()),
                                           ruvia::DbBinaryOperator::kEqual,
                                           networkQuery.cast(networkQuery.value(nodeId),
                                                             ruvia::DbDataType::kUuid)))
                .andWhere(networkQuery.binary(networkQuery.column(service::link::entities::EdgeNodeInterfaceEntity::columnName<"name">()),
                                              ruvia::DbBinaryOperator::kEqual,
                                              networkQuery.value(interfaceName)))
                .andWhere(networkQuery.binary(
                    networkQuery.coalesce({networkQuery.column(service::link::entities::EdgeNodeInterfaceEntity::columnName<"ipv4">()), networkQuery.value("")}),
                    ruvia::DbBinaryOperator::kNotEqual, networkQuery.value("")))
                .limit(1);
            const auto network = co_await c.db().query(networkQuery);
            if (network.empty()) service::common::fail(15002, "所选网口不存在或未上报 IPv4", 409);
            const auto mode = endpoint.get<"mode">()->view();
            const auto ip = endpoint.get<"ip">()->view();
            if ((protocol == "S7" && mode != "TCP Client") || (protocol == "SL651" && mode != "TCP Server"))
                service::common::fail(15002, "协议不支持所选 TCP 模式", 400);
            if (mode == "TCP Server" && ip != "0.0.0.0" && ip != network.front()[0].value().value_or(""))
                service::common::fail(15002, "监听地址必须是所选网口地址", 400);
        }
        std::string priorNode;
        if (!existingId.empty()) {
            ruvia::DbQuery currentQuery(c.pool());
            currentQuery.select({currentQuery.column("created_by"),
                                 currentQuery.cast(currentQuery.column("edge_node_id"),
                                                   ruvia::DbDataType::kText)})
                .from(LinkEntity::tableName())
                .where((LinkEntity::column<"id">() == existingId &&
                        LinkEntity::column<"execution">() == "edge" &&
                        LinkEntity::column<"deleted_at">().isNull())
                           .expression(currentQuery))
                .limit(1);
            const auto current = co_await c.db().query(currentQuery);
            if (current.empty()) service::common::fail(15001, "通道不存在", 404);
            co_await requireOwner(c, current.front()[0].value().value_or(""));
            priorNode = current.front()[1].value().value_or("");
        }
        const auto id = existingId.empty() ? service::common::nextUuidV7() : std::string(existingId);
        std::string endpointJson = "{\"transport\":";
        appendJsonString(endpointJson, transport);
        endpointJson += ",\"interface\":";
        appendJsonString(endpointJson, interfaceName);
        if (transport == "serial") {
            endpointJson += ",\"baud_rate\":" + std::to_string(endpoint.get<"baudRate">().value_or(9600));
            endpointJson += ",\"data_bits\":" + std::to_string(endpoint.get<"dataBits">().value_or(8));
            endpointJson += ",\"stop_bits\":" + std::to_string(endpoint.get<"stopBits">().value_or(1));
            endpointJson += ",\"parity\":";
            appendJsonString(endpointJson, endpoint.get<"parity">() ? endpoint.get<"parity">()->view() : std::string_view("none"));
            endpointJson += endpoint.get<"rs485">().value_or(false) ? ",\"rs485\":true" : ",\"rs485\":false";
        } else {
            endpointJson += ",\"mode\":"; appendJsonString(endpointJson, endpoint.get<"mode">()->view());
            endpointJson += ",\"ip\":"; appendJsonString(endpointJson, endpoint.get<"ip">()->view());
            endpointJson += ",\"port\":" + std::to_string(*endpoint.get<"port">());
        }
        endpointJson += '}';
        const auto status = body.get<"status">() ? body.get<"status">()->view() : std::string_view("enabled");
        auto tx = co_await c.db().beginTransaction();
        if (existingId.empty()) {
            ruvia::DbQuery query(c.pool());
            query.insertInto(LinkEntity::tableName(),
                             {"id", "name", "protocol", "endpoint", "status", "created_by",
                              "execution", "edge_node_id"})
                .values({query.cast(query.value(id), ruvia::DbDataType::kUuid),
                         query.value(name), query.value(protocol),
                         query.cast(query.value(endpointJson), ruvia::DbDataType::kJsonb),
                         query.value(status),
                         query.cast(query.value(principal.userId), ruvia::DbDataType::kUuid),
                         query.value("edge"),
                         query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)});
            (void)co_await tx.execute(query);
        } else {
            ruvia::DbQuery query(c.pool());
            query.update(LinkEntity::tableName())
                .set("name", query.value(name))
                .set("protocol", query.value(protocol))
                .set("endpoint", query.cast(query.value(endpointJson), ruvia::DbDataType::kJsonb))
                .set("status", query.value(status))
                .set("edge_node_id", query.cast(query.value(nodeId), ruvia::DbDataType::kUuid))
                .set("updated_at", query.call("now"))
                .where((LinkEntity::column<"id">() == id &&
                        LinkEntity::column<"execution">() == "edge" &&
                        LinkEntity::column<"deleted_at">().isNull())
                           .expression(query));
            (void)co_await tx.execute(query);
        }
        co_await service::system::OutboxService::enqueueConfigEvent(tx,"link",existingId.empty()?"created":"updated",id);
        co_await tx.commit();
        (void)co_await service::edge::EdgeService::queueSnapshot(c,nodeId);
        if (!priorNode.empty() && priorNode != nodeId)
            (void)co_await service::edge::EdgeService::queueSnapshot(c,priorNode);
    }

    struct RuntimeStatus {
        std::map<std::string, std::string> fields;

        [[nodiscard]] std::string text(std::string_view name,
                                       std::string_view fallback = {}) const {
            const auto current = fields.find(std::string(name));
            return current == fields.end() ? std::string(fallback) : current->second;
        }

        [[nodiscard]] std::int64_t integer(std::string_view name) const {
            const auto value = text(name);
            return service::utils::parseInt64(std::optional<std::string_view>{value})
                .value_or(0);
        }
    };

    static ruvia::Int64 toInt(std::string_view value) {
        return static_cast<ruvia::Int64>(
            service::utils::parseInt64(std::optional<std::string_view>{value}).value_or(0));
    }

    static std::string parsePublicIp(std::string_view body) {
        std::string value(body);
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

    template <typename Row>
    ruvia::Task<void> fill(ruvia::Context& c, LinkItemDto& item, const Row& row) {
        const auto id = std::string(row[0].value().value_or(std::string_view{}));
        const auto runtime = co_await loadRuntimeStatus(c, id);
        ruvia::BoxedArray<ruvia::String> clients(
            ruvia::ModelOptions{.resource = c.arena()});
        const auto clientText = runtime.text("clients");
        std::size_t start = 0;
        while (start < clientText.size()) {
            const auto end = clientText.find('\n', start);
            clients.emplace(clientText.substr(start, end == std::string::npos ? std::string::npos
                                                                              : end - start),
                            ruvia::ModelOptions{.resource = c.arena()});
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        RuntimeDto runtimeDto(ruvia::ModelOptions{.resource = c.arena()});
        runtimeDto.set<"state">(runtime.text("state", "stopped"));
        runtimeDto.set<"reason">(runtime.text("state_reason"));
        runtimeDto.set<"error">(runtime.text("error"));
        runtimeDto.set<"clientCount">(runtime.integer("connection_count"));
        runtimeDto.set<"clients">(std::move(clients));
        const auto lastActivityAt = runtime.integer("last_activity_at_ms");
        if (lastActivityAt > 0)
            runtimeDto.set<"lastActivityAt">(
                service::common::utcTimestampFromMilliseconds(lastActivityAt));
        item.set<"id">(id);
        item.set<"name">(row[1].value().value_or(std::string_view{}));
        item.set<"protocol">(row[2].value().value_or(std::string_view{}));
        item.set<"status">(row[6].value().value_or(std::string_view{}));
        item.set<"runtime">(std::move(runtimeDto));
        item.set<"createdBy">(row[7].value().value_or(std::string_view{}));
        item.set<"createdAt">(row[8].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[9].value().value_or(std::string_view{}));
        item.set<"debugEnabled">(row[12].value().value_or("") == "t");
        item.set<"execution">(row[10].value().value_or("collector"));
        item.set<"edgeNodeId">(row[11].value().value_or(""));
        LinkEndpointDto endpoint(ruvia::ModelOptions{.resource = c.arena()});
        endpoint.set<"mode">(row[3].value().value_or(std::string_view{}));
        endpoint.set<"ip">(row[4].value().value_or(std::string_view{}));
        endpoint.set<"port">(toInt(row[5].value().value_or(std::string_view{})));
        endpoint.set<"targets">(co_await loadTargets(c, id, runtime));
        co_await fillEndpoint(c, endpoint, id);
        item.set<"endpoint">(std::move(endpoint));
    }

    ruvia::Task<ruvia::BoxedArray<LinkTargetDto>> loadTargets(ruvia::Context& c, std::string_view id,
                                                         const RuntimeStatus& runtime) {
        ruvia::DbQuery query(c.pool());
        const auto targets = query.coalesce(
            {jsonValue(query, "endpoint", "targets"),
             query.cast(query.value("[]"), ruvia::DbDataType::kJsonb)});
        query.select({jsonText(query, "target", "id", "value"),
                      jsonText(query, "target", "name", "value"),
                      jsonText(query, "target", "ip", "value"),
                      jsonText(query, "target", "port", "value"),
                      jsonText(query, "target", "status", "value")})
            .from(LinkEntity::tableName(), "link")
            .joinFunction(ruvia::DbJoinType::kCross,
                          query.call("jsonb_array_elements", {targets}), {}, "value",
                          {.lateral = true,
                           .withOrdinality = true,
                           .columns = {{.name = "target"}, {.name = "position"}}})
            .where(query.binary(query.column("id", "link"), ruvia::DbBinaryOperator::kEqual,
                                query.value(id)))
            .orderBy(query.column("position", "value"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<LinkTargetDto> result(
            ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            auto& target = result.emplace(ruvia::ModelOptions{.resource = c.arena()});
            const auto targetId = std::string(row[0].value().value_or(std::string_view{}));
            const auto prefix = "target:" + targetId + ':';
            RuntimeDto targetRuntime(ruvia::ModelOptions{.resource = c.arena()});
            targetRuntime.set<"state">(runtime.text(prefix + "state", "stopped"))
                .set<"reason">(runtime.text(prefix + "reason"))
                .set<"error">(runtime.text(prefix + "error"));
            const auto lastActivityAt = runtime.integer(prefix + "last_activity_at_ms");
            if (lastActivityAt > 0)
                targetRuntime.set<"lastActivityAt">(
                    service::common::utcTimestampFromMilliseconds(lastActivityAt));
            target.set<"id">(targetId)
                .set<"name">(row[1].value().value_or(std::string_view{}))
                .set<"ip">(row[2].value().value_or(std::string_view{}))
                .set<"port">(toInt(row[3].value().value_or(std::string_view{})))
                .set<"status">(row[4].value().value_or(std::string_view{}))
                .set<"runtime">(std::move(targetRuntime));
        }
        co_return result;
    }

    static ruvia::Task<RuntimeStatus> loadRuntimeStatus(ruvia::Context& c, std::string_view id) {
        RuntimeStatus status;
        try {
            const auto owner = co_await c.redis().get("iot:v2:owner:link:" + std::string(id));
            if (!owner) co_return status;
            const auto pattern = "iot:runtime:link:" + std::string(id) + ":worker:" +
                                 std::string(owner->data(),owner->size()) + ":*";
            std::string cursor = "0";
            std::vector<std::string> keys;
            do {
                const std::array<std::string_view, 6> scanArgs{"SCAN", cursor, "MATCH", pattern,
                                                               "COUNT", "100"};
                const auto page = co_await c.redis().command(scanArgs);
                if (page.kind() != ruvia::RedisValue::Kind::kArray || page.array().size() != 2 ||
                    page.array()[0].kind() != ruvia::RedisValue::Kind::kString ||
                    page.array()[1].kind() != ruvia::RedisValue::Kind::kArray)
                    break;
                cursor.assign(page.array()[0].string());
                for (const auto& value : page.array()[1].array())
                    if (value.kind() == ruvia::RedisValue::Kind::kString)
                        keys.emplace_back(value.string());
            } while (cursor != "0");

            std::int64_t connectionCount = 0;
            std::int64_t lastActivityAt = 0;
            std::string clients;
            std::string aggregateState = "stopped";
            const auto stateRank = [](std::string_view state) {
                if (state == "connected")
                    return 6;
                if (state == "listening")
                    return 5;
                if (state == "reconnecting")
                    return 4;
                if (state == "connecting")
                    return 3;
                if (state == "error")
                    return 2;
                if (state == "idle")
                    return 1;
                return 0;
            };
            for (const auto& key : keys) {
                const std::array<std::string_view, 2> hashArgs{"HGETALL", key};
                const auto reply = co_await c.redis().command(hashArgs);
                if (reply.kind() != ruvia::RedisValue::Kind::kArray)
                    continue;
                RuntimeStatus worker;
                const auto values = reply.array();
                for (std::size_t index = 0; index + 1 < values.size(); index += 2) {
                    if (values[index].kind() != ruvia::RedisValue::Kind::kString ||
                        values[index + 1].kind() != ruvia::RedisValue::Kind::kString)
                        continue;
                    worker.fields.insert_or_assign(std::string(values[index].string()),
                                                   std::string(values[index + 1].string()));
                }
                connectionCount += worker.integer("connection_count");
                lastActivityAt = std::max(lastActivityAt, worker.integer("last_activity_at_ms"));
                const auto endpoints = worker.text("remote_endpoints");
                std::size_t start = 0;
                while (start < endpoints.size()) {
                    const auto end = endpoints.find(',', start);
                    if (!clients.empty())
                        clients.push_back('\n');
                    clients.append(endpoints, start,
                                   end == std::string::npos ? std::string::npos : end - start);
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
                const auto workerState = worker.text("state", "stopped");
                if (stateRank(workerState) > stateRank(aggregateState))
                    aggregateState = workerState;
                if (status.text("state_reason").empty() && !worker.text("state_reason").empty())
                    status.fields["state_reason"] = worker.text("state_reason");
                if (status.text("error").empty() && !worker.text("error").empty())
                    status.fields["error"] = worker.text("error");
                for (const auto& [name, value] : worker.fields) {
                    if (!name.starts_with("target:"))
                        continue;
                    if (!value.empty() || !status.fields.contains(name))
                        status.fields[name] = value;
                }
            }
            status.fields["state"] = aggregateState;
            status.fields["connection_count"] = std::to_string(connectionCount);
            status.fields["clients"] = std::move(clients);
            status.fields["last_activity_at_ms"] = std::to_string(lastActivityAt);
        } catch (const std::exception&) {
        }
        co_return status;
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
            appendJsonString(result, target.template get<"id">()->view());
            result += ",\"name\":";
            appendJsonString(result, target.template get<"name">()->view());
            result += ",\"ip\":";
            appendJsonString(result, target.template get<"ip">()->view());
            result += ",\"port\":" +
                      std::to_string(static_cast<std::int64_t>(*target.template get<"port">()));
            result += ",\"status\":";
            const auto& status = target.template get<"status">();
            appendJsonString(result, status ? status->view() : "enabled");
            result.push_back('}');
        }
        result.push_back(']');
        return result;
    }

    template <typename Targets>
    static std::string serializeEndpoint(std::string_view mode, std::string_view ip,
                                         std::int64_t port, const Targets& targets) {
        std::string result = "{\"transport\":\"tcp\",\"mode\":";
        appendJsonString(result, mode);
        result += ",\"ip\":";
        appendJsonString(result, mode == "TCP Server" ? ip : "");
        result += ",\"port\":" + std::to_string(mode == "TCP Server" ? port : 0);
        result += ",\"targets\":";
        result += mode == "TCP Client" ? serializeTargets(targets) : "[]";
        result.push_back('}');
        return result;
    }

    ruvia::Task<void> ensureAvailable(ruvia::Context& c, const std::string& name,
                                      const std::string& mode, const std::string& ip,
                                      std::int64_t port, std::optional<std::string> excludedId) {
        ruvia::DbQuery query(c.pool());
        const auto endpointPort = jsonText(query, "endpoint", "port");
        const auto endpointPortText = query.coalesce({endpointPort, query.value("")});
        const auto endpointPortIsInteger = query.binary(
            endpointPortText, ruvia::DbBinaryOperator::kRegex,
            query.value("^[0-9]{1,5}$"));
        const auto safeEndpointPort = query.caseWhen(
            {{endpointPortIsInteger, query.cast(endpointPort, ruvia::DbDataType::kInteger)}},
            query.cast(query.value(std::int64_t{0}), ruvia::DbDataType::kInteger));
        const auto nameTaken = query.binary(query.column("name"), ruvia::DbBinaryOperator::kEqual,
                                            query.value(name));
        query.select(query.cast(query.value(1), ruvia::DbDataType::kInteger))
            .from(LinkEntity::tableName())
            .where(LinkEntity::column<"deleted_at">().isNull().expression(query));
        if (mode == "TCP Server") {
            const auto serverAddress = query.binary(
                query.binary(query.column("execution"), ruvia::DbBinaryOperator::kEqual,
                             query.value("collector")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(
                    query.binary(jsonText(query, "endpoint", "mode"),
                                 ruvia::DbBinaryOperator::kEqual, query.value(mode)),
                    ruvia::DbBinaryOperator::kAnd,
                    query.binary(
                        query.binary(jsonText(query, "endpoint", "ip"),
                                     ruvia::DbBinaryOperator::kEqual, query.value(ip)),
                        ruvia::DbBinaryOperator::kAnd,
                        query.binary(safeEndpointPort, ruvia::DbBinaryOperator::kEqual,
                                     query.value(port)))));
            query.andWhere(query.binary(nameTaken, ruvia::DbBinaryOperator::kOr, serverAddress));
        } else {
            query.andWhere(nameTaken);
        }
        if (excludedId)
            query.andWhere(query.binary(query.column("id"), ruvia::DbBinaryOperator::kNotEqual,
                                        query.cast(query.value(*excludedId),
                                                   ruvia::DbDataType::kUuid)));
        query.limit(1);
        const auto rows = co_await c.db().query(query);
        if (!rows.empty())
            service::common::fail(15005, "链路名称或监听地址已存在", 409);
    }

    ruvia::Task<void> requireOwner(ruvia::Context& c, std::string_view ownerId) {
        const auto principal = service::middleware::requireAuth(c);
        if (principal.userId == ownerId)
            co_return;
        ruvia::DbQuery query(c.pool());
        query.select(query.cast(query.value(1), ruvia::DbDataType::kInteger))
            .from(service::user::UserRoleEntity::tableName(), "ur")
            .join(ruvia::DbJoinType::kInner, service::role::RoleEntity::tableName(),
                  query.binary(query.column("role_id", "ur"), ruvia::DbBinaryOperator::kEqual,
                               query.column("id", "r")),
                  "r")
            .where(query.binary(query.column("user_id", "ur"),
                                ruvia::DbBinaryOperator::kEqual,
                                query.value(principal.userId)))
            .andWhere(query.binary(query.column("code", "r"),
                                   ruvia::DbBinaryOperator::kEqual,
                                   query.value("superadmin")))
            .andWhere(query.binary(query.column("status", "r"),
                                   ruvia::DbBinaryOperator::kEqual,
                                   query.value("enabled")))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                                  query.column("deleted_at", "r")))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(15007, "只能修改或删除自己创建的链路", 403);
    }

    std::string cachedPublicIp_;
    std::chrono::steady_clock::time_point publicIpCachedAt_{};
};

inline LinkService& linkService() { return LinkService::instance(); }

} // namespace service::link
