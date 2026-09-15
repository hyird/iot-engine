#pragma once

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/middleware/rpc.h"
#include "service/modules/edge_node/edge_node.service.h"
#include "service/modules/link/link.entity.h"
#include "service/modules/protocol/protocol.entity.h"
#include "service/modules/protocol/protocol.schema.h"
#include "service/modules/system/outbox/outbox.service.h"
#include "service/modules/system/role/role.entity.h"
#include "service/modules/system/user/user.entity.h"
#include "service/utils/json.h"


namespace service::protocol {

class ProtocolService {
  public:
    static ProtocolService& instance() {
        static ProtocolService service;
        return service;
    }

    ruvia::Task<std::string> list(ruvia::Context& c, std::int64_t page, std::int64_t pageSize,
                                  std::optional<std::string> protocol) {
        co_await service::middleware::requirePermission(c, "iot:protocol:query");
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 1000);
        ruvia::DbFindOptions countOptions;
        countOptions.where = ProtocolConfigEntity::column<"deleted_at">().isNull();
        if (protocol && !protocol->empty())
            countOptions.where = std::move(countOptions.where) &&
                                 ProtocolConfigEntity::column<"protocol">() == *protocol;
        const auto total = co_await c.db().getRepository<ProtocolConfigEntity>().count(countOptions);

        auto query = protocolSelect(c.pool());
        if (protocol && !protocol->empty())
            query.andWhere(query.binary(query.column("protocol"),
                                        ruvia::DbBinaryOperator::kEqual, query.value(*protocol)));
        query.orderBy(query.column("id"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>((page - 1) * pageSize));
        const auto rows = co_await c.db().query(query);

        std::string result = "{\"list\":[";
        bool first = true;
        for (const auto& row : rows) {
            if (!first)
                result.push_back(',');
            first = false;
            result += itemJson(row);
        }
        result += "],\"total\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
                  ",\"pageSize\":" + std::to_string(pageSize) + ",\"totalPages\":" +
                  std::to_string(total == 0 ? 0 : (total + pageSize - 1) / pageSize) + "}";
        co_return result;
    }

    ruvia::Task<std::string> detail(ruvia::Context& c, std::string_view id) {
        co_await service::middleware::requirePermission(c, "iot:protocol:query");
        co_return co_await detailData(c, id);
    }

    ruvia::Task<std::string> options(ruvia::Context& c, const std::string& protocol,
                                     std::int64_t page, std::int64_t pageSize) {
        co_await service::middleware::requirePermission(c, "iot:protocol:query");
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 1000);
        ruvia::DbFindOptions options;
        options.where = ProtocolConfigEntity::column<"deleted_at">().isNull() &&
                        ProtocolConfigEntity::column<"protocol">() == protocol;
        const auto total = co_await c.db().getRepository<ProtocolConfigEntity>().count(options);
        options.where = std::move(options.where) && ProtocolConfigEntity::column<"enabled">() == true;
        options.order = {{"name"}};
        options.take = static_cast<std::uint64_t>(pageSize);
        options.skip = static_cast<std::uint64_t>((page - 1) * pageSize);
        const auto rows = co_await c.db().getRepository<ProtocolConfigEntity>().find(options);
        std::string result = "{\"list\":[";
        bool first = true;
        for (const auto& row : rows) {
            if (!first)
                result.push_back(',');
            first = false;
            result += "{\"id\":" + service::utils::jsonQuoted(row.get<"id">()) +
                      ",\"name\":" + service::utils::jsonQuoted(row.get<"name">()) + "}";
        }
        result += "],\"total\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
                  ",\"pageSize\":" + std::to_string(pageSize) + "}";
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const ruvia::JsonValue& payload) {
        co_await service::middleware::requirePermission(c, "iot:protocol:add");
        if (!payload.isObject())
            service::common::fail(16002, "请求体必须是对象", 400);
        const auto protocol = ProtocolPayloadValidator::requiredString(payload, "protocol", "协议不能为空");
        const auto name = ProtocolPayloadValidator::requiredString(payload, "name", "配置名称不能为空");
        ProtocolPayloadValidator::validateProtocol(protocol);
        ProtocolPayloadValidator::validateName(name);
        ProtocolPayloadValidator::validateOptionalNullableString(payload, "remark", "remark 必须是字符串或 null", 500);
        ProtocolPayloadValidator::validateConfig(payload, protocol, true);
        co_await ensureNameAvailable(c, name, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        const auto config = service::utils::jsonField(payload, "config");
        const auto remark = ProtocolPayloadValidator::nullableString(payload, "remark");
        const auto enabledValue = ProtocolPayloadValidator::optionalBool(payload, "enabled");
        const bool enabled = enabledValue.value_or(true);
        auto transaction = co_await c.db().beginTransaction();
        ProtocolConfigEntity configuration(c.pool());
        configuration.set<"id">(id);
        configuration.set<"protocol">(protocol);
        configuration.set<"name">(name);
        configuration.set<"enabled">(enabled);
        configuration.set<"config">(config->view());
        configuration.set<"created_by">(principal.userId);
        if (remark)
            configuration.set<"remark">(*remark);
        else
            configuration.setNull<"remark">();
        (void)co_await transaction.getRepository<ProtocolConfigEntity>().insert(configuration);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "protocol", "created", id);
        co_await transaction.commit();
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id,
                             const ruvia::JsonValue& payload) {
        co_await service::middleware::requirePermission(c, "iot:protocol:edit");
        if (!payload.isObject())
            service::common::fail(16002, "请求体必须是对象", 400);
        ruvia::DbFindOptions options;
        options.where = service::common::database::activeId<ProtocolConfigEntity>(id);
        const auto existing = co_await c.db().getRepository<ProtocolConfigEntity>().findOne(options);
        if (!existing)
            service::common::fail(16001, "协议配置不存在", 404);
        co_await requireOwner(c, existing->get<"created_by">());
        const std::string protocol(existing->get<"protocol">());
        if (const auto requested =
                ProtocolPayloadValidator::optionalString(payload, "protocol", "protocol 必须是字符串", 16)) {
            if (requested->empty())
                service::common::fail(16003, "protocol 不能为空", 400);
            if (*requested != protocol)
                service::common::fail(16006, "协议类型不可修改", 409);
        }
        if (const auto name = ProtocolPayloadValidator::optionalString(payload, "name", "name 必须是字符串", 64)) {
            ProtocolPayloadValidator::validateName(*name);
            co_await ensureNameAvailable(c, *name, std::string(id));
        }
        ProtocolPayloadValidator::validateOptionalNullableString(payload, "remark", "remark 必须是字符串或 null", 500);
        ProtocolPayloadValidator::validateConfig(payload, protocol, false);
        const auto name = ProtocolPayloadValidator::optionalString(payload, "name", "name 必须是字符串", 64);
        const auto remark = ProtocolPayloadValidator::nullableString(payload, "remark");
        const auto config = service::utils::jsonField(payload, "config");
        const auto enabled = ProtocolPayloadValidator::optionalBool(payload, "enabled");
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbExpressions expressions(c.pool());
        std::vector<ruvia::DbAssignment> changes{{"updated_at", expressions.call("now")}};
        if (name)
            changes.push_back({"name", expressions.value(*name)});
        if (enabled)
            changes.push_back({"enabled", expressions.value(*enabled)});
        if (config)
            changes.push_back({"config", expressions.binary(expressions.column("config"),
                ruvia::DbBinaryOperator::kJsonConcat,
                expressions.cast(expressions.value(config->view()), ruvia::DbDataType::kJsonb))});
        if (service::utils::jsonField(payload, "remark"))
            changes.push_back({"remark", remark ? expressions.value(*remark) : expressions.nullValue()});
        (void)co_await transaction.getRepository<ProtocolConfigEntity>().update(
            service::common::database::activeId<ProtocolConfigEntity>(id), changes);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "protocol", "updated", id);
        co_await transaction.commit();
        try {
            (void)co_await service::rpc::call(c, "telemetry", "project-protocol", std::string(id));
        } catch (...) {
            // Startup hydration repairs Redis read models if Redis is temporarily unavailable.
        }
        try {
            co_await syncEdgeNodes(c, id);
        } catch (const std::exception& error) {
            logPostUpdateFailure("edge-sync", id, error.what());
        } catch (...) {
            logPostUpdateFailure("edge-sync", id, "unknown exception");
        }
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id) {
        co_await service::middleware::requirePermission(c, "iot:protocol:delete");
        ruvia::DbFindOptions options;
        options.where = service::common::database::activeId<ProtocolConfigEntity>(id);
        const auto existing = co_await c.db().getRepository<ProtocolConfigEntity>().findOne(options);
        if (!existing)
            service::common::fail(16001, "协议配置不存在", 404);
        co_await requireOwner(c, existing->get<"created_by">());
        ruvia::DbFindOptions used;
        used.where = entities::DeviceEntity::column<"protocol_config_id">() == id &&
                     entities::DeviceEntity::column<"deleted_at">().isNull();
        if (co_await c.db().getRepository<entities::DeviceEntity>().exists(used))
            service::common::fail(16008, "协议配置已被设备使用，请先删除关联设备", 409);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbExpressions expressions(c.pool());
        (void)co_await transaction.getRepository<ProtocolConfigEntity>().update(
            service::common::database::activeId<ProtocolConfigEntity>(id),
            {{"deleted_at", expressions.call("now")}, {"updated_at", expressions.call("now")}});
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "protocol", "deleted", id);
        co_await transaction.commit();
    }

  private:
    static ruvia::Task<std::string> detailData(ruvia::Context& c, std::string_view id) {
        auto query = protocolSelect(c.pool());
        query.where(ProtocolConfigEntity::column<"deleted_at">().isNull().expression(query))
            .andWhere(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                   query.cast(query.value(id), ruvia::DbDataType::kUuid)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(16001, "协议配置不存在", 404);
        co_return itemJson(rows.front());
    }

    static ruvia::Task<void> syncEdgeNodes(ruvia::Context& c, std::string_view configId) {
        ruvia::DbQuery query(c.pool());
        const auto edgeNodeId =
            query.cast(query.column("edge_node_id", "l"), ruvia::DbDataType::kText);
        query.select(edgeNodeId)
            .from(service::protocol::entities::DeviceEntity::tableName(), "d")
            .join(ruvia::DbJoinType::kInner, service::link::LinkEntity::tableName(),
                  query.binary(query.column("id", "l"), ruvia::DbBinaryOperator::kEqual,
                               query.column(service::protocol::entities::DeviceEntity::columnName<"link_id">(), "d")),
                  "l")
            .where(query.binary(query.column(service::protocol::entities::DeviceEntity::columnName<"protocol_config_id">(), "d"),
                                ruvia::DbBinaryOperator::kEqual,
                                query.cast(query.value(configId), ruvia::DbDataType::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                                  query.column(service::protocol::entities::DeviceEntity::columnName<"deleted_at">(), "d")))
            .andWhere(query.binary(query.column("execution", "l"),
                                   ruvia::DbBinaryOperator::kEqual, query.value("edge")))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                                  query.column("deleted_at", "l")))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNotNull,
                                  query.column("edge_node_id", "l")))
            .distinct()
            .orderBy(edgeNodeId);
        const auto rows = co_await c.db().query(query);
        for (const auto& row : rows) {
            const auto nodeId = row[0].value().value_or(std::string_view{});
            if (nodeId.empty())
                continue;
            try {
                (void)co_await service::edge::EdgeService::queueSnapshot(c, nodeId);
            } catch (const std::exception& error) {
                std::cerr << "protocol edge config sync failed: node=" << nodeId
                          << " config=" << configId << " error=" << error.what() << '\n';
            } catch (...) {
                std::cerr << "protocol edge config sync failed: node=" << nodeId
                          << " config=" << configId << " error=unknown exception\n";
            }
        }
    }

    static void logPostUpdateFailure(std::string_view stage, std::string_view configId,
                                     std::string_view message) {
        std::cerr << "protocol post-update " << stage << " failed: config=" << configId
                  << " error=" << message << '\n';
    }

    static ruvia::DbQuery protocolSelect(std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        query.select({query.cast(query.column("id"), ruvia::DbDataType::kText),
                      query.column("protocol"), query.column("name"), query.column("enabled"),
                      query.column("config"), service::common::database::emptyText(query, "remark"),
                      query.cast(query.call("iot_utc_timestamp", {query.column("created_at")}),
                                 ruvia::DbDataType::kText),
                      query.cast(query.call("iot_utc_timestamp", {query.column("updated_at")}),
                                 ruvia::DbDataType::kText)})
            .from(ProtocolConfigEntity::tableName())
            .where(ProtocolConfigEntity::column<"deleted_at">().isNull().expression(query));
        return query;
    }

    static std::string itemJson(const ruvia::DbRow& row) {
        const auto enabled = row[3].as<bool>().value_or(false);
        std::string result = "{\"id\":" + service::utils::jsonQuoted(row[0].value().value_or("")) +
                             ",\"protocol\":" +
                             service::utils::jsonQuoted(row[1].value().value_or("")) +
                             ",\"name\":" + service::utils::jsonQuoted(row[2].value().value_or("")) +
                             ",\"enabled\":" + (enabled ? "true" : "false") +
                             ",\"config\":" + std::string(row[4].value().value_or("{}")) +
                             ",\"remark\":" + service::utils::jsonQuoted(row[5].value().value_or("")) +
                             ",\"created_at\":" +
                             service::utils::jsonQuoted(row[6].value().value_or("")) +
                             ",\"updated_at\":" +
                             service::utils::jsonQuoted(row[7].value().value_or("")) + "}";
        return result;
    }

    ruvia::Task<void> ensureNameAvailable(ruvia::Context& c, const std::string& name,
                                          std::optional<std::string> excludedId) {
        ruvia::DbFindOptions options;
        options.where = ProtocolConfigEntity::column<"deleted_at">().isNull() &&
                        ProtocolConfigEntity::column<"name">() == name;
        if (excludedId)
            options.where = std::move(options.where) && ProtocolConfigEntity::column<"id">() != *excludedId;
        if (co_await c.db().getRepository<ProtocolConfigEntity>().exists(options))
            service::common::fail(16005, "配置名称已存在", 409);
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
            .where(query.binary(query.column("user_id", "ur"), ruvia::DbBinaryOperator::kEqual,
                                query.value(principal.userId)))
            .andWhere(query.binary(query.column("code", "r"), ruvia::DbBinaryOperator::kEqual,
                                   query.value("superadmin")))
            .andWhere(query.binary(query.column("status", "r"), ruvia::DbBinaryOperator::kEqual,
                                   query.value("enabled")))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                                  query.column("deleted_at", "r")))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(16007, "只能修改或删除自己创建的协议配置", 403);
    }
};

inline ProtocolService& protocolService() { return ProtocolService::instance(); }

} // namespace service::protocol
