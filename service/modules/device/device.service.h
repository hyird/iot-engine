#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>
#include <ruvia/web/redis/Redis.h>

#include "service/modules/system/outbox/outbox.service.h"
#include "service/common/http.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/modules/edge_node/edge_node.service.h"
#include "service/modules/device/device.types.h"
#include "service/common/message.h"
#include "service/middleware/rpc.h"
#include "service/utils/number.h"

namespace service::device {

enum class DeviceAccessLevel : std::int64_t {
    none = 0,
    view = 1,
    operate = 2,
    owner = 4,
};

struct DeviceActor final {
    std::string userId;
    std::string departmentId;
    bool superadmin{};
    bool canEdit{};
    bool canDelete{};
    bool canShare{};
    bool canCommand{};
    bool canGroupShare{};
};

struct DeviceAccessDecision final {
    DeviceActor actor;
    DeviceAccessLevel level{DeviceAccessLevel::none};
};

struct DeviceCapabilities final {
    bool canEdit{};
    bool canDelete{};
    bool canShare{};
    bool canCommand{};
    std::string_view accessLevel{"none"};
};

inline ruvia::DbQuery::Expr andAll(ruvia::DbQuery& query, ruvia::DbQuery::Expr first,
                                   ruvia::DbQuery::Expr second) {
    return query.binary(first, ruvia::DbBinaryOperator::kAnd, second);
}

template <typename... Expressions>
inline ruvia::DbQuery::Expr andAll(ruvia::DbQuery& query, ruvia::DbQuery::Expr first,
                                   ruvia::DbQuery::Expr second, Expressions... rest) {
    return query.binary(first, ruvia::DbBinaryOperator::kAnd,
                        andAll(query, second, rest...));
}

class DeviceAccessService {
  public:
    static DeviceAccessService& instance() {
        static DeviceAccessService service;
        return service;
    }

    ruvia::Task<DeviceActor> actor(ruvia::Context& c) const {
        const auto principal = service::middleware::requireAuth(c);
        ruvia::DbQuery query(c.pool());
        const auto roleCode = query.column("code", "role");
        const auto isSuperadmin = query.binary(roleCode, ruvia::DbBinaryOperator::kEqual,
                                               query.value("superadmin"));
        const auto permissions = query.column("permissions", "role");
        const auto hasWildcard = query.binary(
            permissions, ruvia::DbBinaryOperator::kJsonHasKey, textKey(query, "*"));
        const auto permission = [&](std::string_view value) {
            return query.binary(permissions, ruvia::DbBinaryOperator::kJsonHasKey,
                                textKey(query, value));
        };
        const auto capability = [&](std::string_view value) {
            return query.binary(
                query.binary(isSuperadmin, ruvia::DbBinaryOperator::kOr, hasWildcard),
                ruvia::DbBinaryOperator::kOr, permission(value));
        };
        const auto boolOr = [&](ruvia::DbQuery::Expr expression) {
            return query.coalesce({query.aggregate("bool_or", {expression}),
                                   boolean(query, false)});
        };
        query.select({query.coalesce({text(query, query.column("id", "department")),
                                      query.value("")}),
                      boolOr(isSuperadmin), boolOr(capability("iot:device:edit")),
                      boolOr(capability("iot:device:delete")),
                      boolOr(capability("iot:device:share")),
                      boolOr(capability("iot:device:command")),
                      boolOr(capability("iot:device-group:share"))})
            .from("sys_user", "actor")
            .join(ruvia::DbJoinType::kLeft, "sys_department",
                  andAll(query,
                         query.binary(query.column("id", "department"),
                                     ruvia::DbBinaryOperator::kEqual,
                                     query.column("department_id", "actor")),
                         query.binary(query.column("status", "department"),
                                     ruvia::DbBinaryOperator::kEqual,
                                     query.value("enabled")),
                         query.unary(ruvia::DbUnaryOperator::kIsNull,
                                     query.column("deleted_at", "department"))),
                  "department")
            .join(ruvia::DbJoinType::kLeft, "sys_user_role",
                  query.binary(query.column("user_id", "user_role"),
                               ruvia::DbBinaryOperator::kEqual,
                               query.column("id", "actor")),
                  "user_role")
            .join(ruvia::DbJoinType::kLeft, "sys_role",
                  andAll(query,
                         query.binary(query.column("id", "role"),
                                     ruvia::DbBinaryOperator::kEqual,
                                     query.column("role_id", "user_role")),
                         query.binary(query.column("status", "role"),
                                     ruvia::DbBinaryOperator::kEqual,
                                     query.value("enabled")),
                         query.unary(ruvia::DbUnaryOperator::kIsNull,
                                     query.column("deleted_at", "role"))),
                  "role")
            .where(andAll(query,
                          query.binary(query.column("id", "actor"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       uuid(query, principal.userId)),
                          query.binary(query.column("status", "actor"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       query.value("enabled")),
                          query.unary(ruvia::DbUnaryOperator::kIsNull,
                                      query.column("deleted_at", "actor"))))
            .groupBy({query.column("id", "actor"), query.column("id", "department")});
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(service::common::kTokenInvalidErrorCode, "用户状态无效", 401);
        const auto& row = rows.front();
        DeviceActor result;
        result.userId = principal.userId;
        result.departmentId = std::string(row[0].value().value_or(std::string_view{}));
        result.superadmin = isTrue(row[1].value().value_or(std::string_view{}));
        result.canEdit = isTrue(row[2].value().value_or(std::string_view{}));
        result.canDelete = isTrue(row[3].value().value_or(std::string_view{}));
        result.canShare = isTrue(row[4].value().value_or(std::string_view{}));
        result.canCommand = isTrue(row[5].value().value_or(std::string_view{}));
        result.canGroupShare = isTrue(row[6].value().value_or(std::string_view{}));
        co_return result;
    }

    ruvia::Task<DeviceActor> requireGroupOwner(ruvia::Context& c,
                                                std::string_view groupId) const {
        auto currentActor = co_await actor(c);
        ruvia::DbQuery query(c.pool());
        query.select(text(query, query.column("created_by")))
            .from("device_group")
            .where(andAll(query,
                          query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                       uuid(query, groupId)),
                          query.unary(ruvia::DbUnaryOperator::kIsNull,
                                      query.column("deleted_at"))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(17001, "设备分组不存在", 404);
        if (!currentActor.superadmin && rows.front()[0].value().value_or(std::string_view{}) != currentActor.userId)
            service::common::fail(17005, "只能分享自己创建的设备分组", 403);
        co_return currentActor;
    }

    ruvia::Task<DeviceAccessDecision> require(ruvia::Context& c, std::string_view deviceId,
                                               DeviceAccessLevel minimum) const {
        auto currentActor = co_await actor(c);
        ruvia::DbQuery query(c.pool());
        addScopedDevicesCtes(query, currentActor);
        query.select(query.column("access_rank", "device"))
            .from("scoped_device", "device")
            .where(query.binary(query.column("id", "device"),
                                ruvia::DbBinaryOperator::kEqual, uuid(query, deviceId)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(18001, "设备不存在", 404);
        const auto level = rank(rows.front()[0].value().value_or(std::string_view{}));
        if (level == DeviceAccessLevel::none)
            service::common::fail(18001, "设备不存在", 404);
        if (level < minimum)
            service::common::fail(18005, "设备权限不足", 403);
        co_return DeviceAccessDecision{std::move(currentActor), level};
    }

    // Append the actor-scoped device relations to a query.  The CTEs are deliberately public
    // so callers can compose their own projection, ordering and pagination without embedding SQL.
    static void addScopedDevicesCtes(ruvia::DbQuery& query, const DeviceActor& actor) {
        ruvia::DbQuery shared(query.resource());
        ruvia::DbQuery sharedRecursive(query.resource());
        shared.select({shared.column("group_id", "access_grant"),
                       accessLevelRank(shared, shared.column("access_level", "access_grant"))})
            .from("device_group_access_grant", "access_grant")
            .join(ruvia::DbJoinType::kInner, "device_group",
                  andAll(shared,
                         shared.binary(shared.column("id", "granted_group"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       shared.column("group_id", "access_grant")),
                         shared.unary(ruvia::DbUnaryOperator::kIsNull,
                                      shared.column("deleted_at", "granted_group"))),
                  "granted_group")
            .where(andAll(shared,
                          shared.binary(
                              shared.binary(shared.column("user_id", "access_grant"),
                                            ruvia::DbBinaryOperator::kEqual,
                                            uuid(shared, actor.userId)),
                              ruvia::DbBinaryOperator::kOr,
                              shared.binary(shared.column("department_id", "access_grant"),
                                            ruvia::DbBinaryOperator::kEqual,
                                            nullableUuid(shared, actor.departmentId))),
                          shared.unary(ruvia::DbUnaryOperator::kNot,
                                       boolean(shared, actor.superadmin))));
        sharedRecursive.select({sharedRecursive.column("id", "child"),
                                sharedRecursive.column("access_rank", "shared")})
            .from("device_group", "child")
            .join(ruvia::DbJoinType::kInner, "shared_group_access",
                  sharedRecursive.binary(sharedRecursive.column("group_id", "shared"),
                                         ruvia::DbBinaryOperator::kEqual,
                                         sharedRecursive.column("parent_id", "child")),
                  "shared")
            .where(sharedRecursive.unary(ruvia::DbUnaryOperator::kIsNull,
                                          sharedRecursive.column("deleted_at", "child")));
        shared.combine(ruvia::DbSetOperation::kUnion, sharedRecursive);
        query.with("shared_group_access", shared,
                   {.recursive = true, .columns = {"group_id", "access_rank"}});

        ruvia::DbQuery groupAccess(query.resource());
        groupAccess.select(
                      {groupAccess.column("group_id"),
                       groupAccess.alias(
                           groupAccess.aggregate("max", {groupAccess.column("access_rank")}),
                           "access_rank")})
            .from("shared_group_access")
            .groupBy({groupAccess.column("group_id")});
        query.with("group_access", groupAccess);

        ruvia::DbQuery deviceAccess(query.resource());
        deviceAccess
            .select({deviceAccess.column("device_id"),
                     deviceAccess.alias(
                         deviceAccess.aggregate(
                             "max", {accessLevelRank(
                                         deviceAccess,
                                         deviceAccess.column("access_level", "access_grant"))}),
                         "access_rank")})
            .from("device_access_grant", "access_grant")
            .where(andAll(deviceAccess,
                          deviceAccess.binary(
                              deviceAccess.binary(deviceAccess.column("user_id", "access_grant"),
                                                  ruvia::DbBinaryOperator::kEqual,
                                                  uuid(deviceAccess, actor.userId)),
                              ruvia::DbBinaryOperator::kOr,
                              deviceAccess.binary(deviceAccess.column("department_id", "access_grant"),
                                                  ruvia::DbBinaryOperator::kEqual,
                                                  nullableUuid(deviceAccess, actor.departmentId))),
                          deviceAccess.unary(ruvia::DbUnaryOperator::kNot,
                                             boolean(deviceAccess, actor.superadmin))))
            .groupBy({deviceAccess.column("device_id")});
        query.with("device_access", deviceAccess);

        ruvia::DbQuery scoped(query.resource());
        const auto owned = scoped.binary(
            scoped.binary(boolean(scoped, actor.superadmin), ruvia::DbBinaryOperator::kOr,
                          scoped.binary(scoped.column("created_by", "source"),
                                        ruvia::DbBinaryOperator::kEqual,
                                        uuid(scoped, actor.userId))),
            ruvia::DbBinaryOperator::kEqual, boolean(scoped, true));
        const auto inherited = scoped.greatest(
            {scoped.coalesce({scoped.column("access_rank", "device_access"), integer(scoped, 0)}),
             scoped.coalesce({scoped.column("access_rank", "group_access"), integer(scoped, 0)})});
            scoped.select({scoped.star("source"),
                       scoped.caseWhen({{owned, integer(scoped, 4)}}, inherited)})
            .from("device", "source")
            .join(ruvia::DbJoinType::kLeft, "device_access",
                  scoped.binary(scoped.column("device_id", "device_access"),
                                ruvia::DbBinaryOperator::kEqual,
                                scoped.column("id", "source")),
                  "device_access")
            .join(ruvia::DbJoinType::kLeft, "group_access",
                  scoped.binary(scoped.column("group_id", "group_access"),
                                ruvia::DbBinaryOperator::kEqual,
                                scoped.column("group_id", "source")),
                  "group_access")
            .where(scoped.unary(ruvia::DbUnaryOperator::kIsNull,
                                scoped.column("deleted_at", "source")));
        query.with("scoped_device", scoped, {.columns = {"id", "name", "link_id",
                                                            "protocol_config_id", "group_id",
                                                                    "status", "protocol_params", "remark",
                                                                    "created_by", "created_at", "updated_at",
                                                            "deleted_at", "protocol_revision",
                                                            "protocol_address",
                                                            "access_rank"}});
    }

    static void addVisibleGroupsCtes(ruvia::DbQuery& query, const DeviceActor& actor) {
        addScopedDevicesCtes(query, actor);
        ruvia::DbQuery shared(query.resource());
        ruvia::DbQuery sharedRecursive(query.resource());
        shared.select(shared.column("group_id", "access_grant"))
            .from("device_group_access_grant", "access_grant")
            .where(shared.binary(
                shared.binary(shared.column("user_id", "access_grant"),
                              ruvia::DbBinaryOperator::kEqual, uuid(shared, actor.userId)),
                ruvia::DbBinaryOperator::kOr,
                shared.binary(shared.column("department_id", "access_grant"),
                              ruvia::DbBinaryOperator::kEqual,
                              nullableUuid(shared, actor.departmentId))));
        sharedRecursive.select(sharedRecursive.column("id", "child"))
            .from("device_group", "child")
            .join(ruvia::DbJoinType::kInner, "shared_group_tree",
                  sharedRecursive.binary(sharedRecursive.column("id", "parent"),
                                         ruvia::DbBinaryOperator::kEqual,
                                         sharedRecursive.column("parent_id", "child")),
                  "parent")
            .where(sharedRecursive.unary(ruvia::DbUnaryOperator::kIsNull,
                                          sharedRecursive.column("deleted_at", "child")));
        shared.combine(ruvia::DbSetOperation::kUnion, sharedRecursive);
        query.with("shared_group_tree", shared,
                   {.recursive = true, .columns = {"id"}});

        ruvia::DbQuery scopedGroups(query.resource());
        scopedGroups.select(scopedGroups.column("group_id", "scoped"))
            .from("scoped_device", "scoped")
            .where(andAll(scopedGroups,
                          scopedGroups.binary(scopedGroups.column("access_rank", "scoped"),
                                              ruvia::DbBinaryOperator::kGreater,
                                              integer(scopedGroups, 0)),
                          scopedGroups.unary(ruvia::DbUnaryOperator::kIsNotNull,
                                             scopedGroups.column("group_id", "scoped"))));
        ruvia::DbQuery ownedGroups(query.resource());
        ownedGroups.select(ownedGroups.column("id", "owned"))
            .from("device_group", "owned")
            .where(andAll(ownedGroups,
                          ownedGroups.unary(ruvia::DbUnaryOperator::kIsNull,
                                            ownedGroups.column("deleted_at", "owned")),
                          ownedGroups.binary(
                              ownedGroups.binary(boolean(ownedGroups, actor.superadmin),
                                                 ruvia::DbBinaryOperator::kOr,
                                                 ownedGroups.binary(
                                                     ownedGroups.column("created_by", "owned"),
                                                     ruvia::DbBinaryOperator::kEqual,
                                                     uuid(ownedGroups, actor.userId))),
                              ruvia::DbBinaryOperator::kEqual,
                              boolean(ownedGroups, true))));
        ruvia::DbQuery sharedGroups(query.resource());
        sharedGroups.select(sharedGroups.column("id", "shared"))
            .from("shared_group_tree", "shared");
        scopedGroups.combine(ruvia::DbSetOperation::kUnion, ownedGroups)
            .combine(ruvia::DbSetOperation::kUnion, sharedGroups);

        ruvia::DbQuery parents(query.resource());
        parents.select(parents.column("parent_id", "parent"))
            .from("device_group", "parent")
            .join(ruvia::DbJoinType::kInner, "visible_group",
                  parents.binary(parents.column("id", "visible"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 parents.column("id", "parent")),
                  "visible")
            .where(andAll(parents,
                          parents.unary(ruvia::DbUnaryOperator::kIsNotNull,
                                        parents.column("parent_id", "parent")),
                          parents.unary(ruvia::DbUnaryOperator::kIsNull,
                                        parents.column("deleted_at", "parent"))));
        scopedGroups.combine(ruvia::DbSetOperation::kUnion, parents);
        query.with("visible_group", scopedGroups,
                   {.recursive = true, .columns = {"id"}});
    }

    static ruvia::DbQuery::Expr text(ruvia::DbQuery& query, ruvia::DbQuery::Expr value) {
        return query.cast(value, ruvia::DbDataType::kText);
    }

    static ruvia::DbQuery::Expr textKey(ruvia::DbQuery& query, std::string_view value) {
        return text(query, query.value(value));
    }

    static ruvia::DbQuery::Expr text(ruvia::DbQuery& query, std::string_view value) {
        return text(query, query.value(value));
    }

    static ruvia::DbQuery::Expr boolean(ruvia::DbQuery& query, bool value) {
        return query.cast(query.value(value), ruvia::DbDataType::kBoolean);
    }

    static ruvia::DbQuery::Expr integer(ruvia::DbQuery& query, std::int64_t value) {
        return query.cast(query.value(value), ruvia::DbDataType::kInteger);
    }

    static ruvia::DbQuery::Expr uuid(ruvia::DbQuery& query, std::string_view value) {
        return query.cast(query.value(value), ruvia::DbDataType::kUuid);
    }

    static ruvia::DbQuery::Expr nullableUuid(ruvia::DbQuery& query, std::string_view value) {
        return query.cast(query.nullIf(text(query, query.value(value)), text(query, "")),
                          ruvia::DbDataType::kUuid);
    }

    static ruvia::DbQuery::Expr jsonValue(ruvia::DbQuery& query, ruvia::DbQuery::Expr object,
                                          std::string_view key) {
        return query.binary(object, ruvia::DbBinaryOperator::kJsonGet, textKey(query, key));
    }

    static ruvia::DbQuery::Expr jsonText(ruvia::DbQuery& query, ruvia::DbQuery::Expr object,
                                         std::string_view key) {
        return query.binary(object, ruvia::DbBinaryOperator::kJsonGetText, textKey(query, key));
    }

    static ruvia::DbQuery::Expr accessLevelRank(ruvia::DbQuery& query,
                                                ruvia::DbQuery::Expr level) {
        return query.caseWhen(
            {{query.binary(level, ruvia::DbBinaryOperator::kEqual, query.value("operate")),
              integer(query, 2)},
             {query.binary(level, ruvia::DbBinaryOperator::kEqual, query.value("view")),
              integer(query, 1)}},
            integer(query, 0));
    }

    static ruvia::DbQuery::Expr remoteControlEnabled(ruvia::DbQuery& query,
                                                     ruvia::DbQuery::Expr params) {
        const auto key = textKey(query, "remote_control");
        const auto hasKey = query.binary(params, ruvia::DbBinaryOperator::kJsonHasKey, key);
        const auto value = query.call(
            "lower", {query.coalesce({query.binary(params, ruvia::DbBinaryOperator::kJsonGetText,
                                                   key),
                                       query.value("")})});
        return query.caseWhen(
            {{hasKey,
              query.caseWhen(
                  {{query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("true")),
                    boolean(query, true)},
                   {query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("t")),
                    boolean(query, true)},
                   {query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("1")),
                    boolean(query, true)},
                   {query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("yes")),
                    boolean(query, true)},
                   {query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("y")),
                    boolean(query, true)},
                   {query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("on")),
                    boolean(query, true)}},
                  boolean(query, false))}},
            boolean(query, true));
    }

    static DeviceCapabilities capabilities(const DeviceActor& actor, DeviceAccessLevel level,
                                            bool remoteControl) {
        const auto rankValue = static_cast<std::int64_t>(level);
        DeviceCapabilities result;
        result.canEdit = actor.canEdit && level == DeviceAccessLevel::owner;
        result.canDelete = actor.canDelete && level == DeviceAccessLevel::owner;
        result.canShare = actor.canShare && level == DeviceAccessLevel::owner;
        result.canCommand = actor.canCommand && remoteControl &&
                            rankValue >= rankValueOf(DeviceAccessLevel::operate);
        result.accessLevel = name(level);
        return result;
    }

    static DeviceAccessLevel rank(std::string_view value) {
        const auto parsed =
            service::common::parseInt64(std::optional<std::string_view>{value}).value_or(0);
        if (parsed >= rankValueOf(DeviceAccessLevel::owner))
            return DeviceAccessLevel::owner;
        if (parsed == rankValueOf(DeviceAccessLevel::operate))
            return DeviceAccessLevel::operate;
        if (parsed == rankValueOf(DeviceAccessLevel::view))
            return DeviceAccessLevel::view;
        return DeviceAccessLevel::none;
    }

  private:
    static bool isTrue(std::string_view value) { return value == "t" || value == "true"; }

    static constexpr std::int64_t rankValueOf(DeviceAccessLevel level) {
        return static_cast<std::int64_t>(level);
    }

    static constexpr std::string_view name(DeviceAccessLevel level) {
        switch (level) {
        case DeviceAccessLevel::owner:
            return "owner";
        case DeviceAccessLevel::operate:
            return "operate";
        case DeviceAccessLevel::view:
            return "view";
        case DeviceAccessLevel::none:
            return "none";
        }
        return "none";
    }

};

inline DeviceAccessService& deviceAccessService() { return DeviceAccessService::instance(); }

class DeviceService {
  public:
    static DeviceService& instance() {
        static DeviceService service;
        return service;
    }

    ruvia::Task<DevicePageDataDto> list(ruvia::Context& c) {
        const auto actor = co_await deviceAccessService().actor(c);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addScopedDevicesCtes(query, actor);
        selectItemColumns(query);
        query.from("scoped_device", "d")
            .join(ruvia::DbJoinType::kLeft, "link",
                  query.binary(query.column("id", "l"), ruvia::DbBinaryOperator::kEqual,
                               query.column("link_id", "d")),
                  "l")
            .join(ruvia::DbJoinType::kLeft, "edge_node",
                  query.binary(query.column("id", "en"), ruvia::DbBinaryOperator::kEqual,
                               query.column("edge_node_id", "l")),
                  "en")
            .join(ruvia::DbJoinType::kInner, "device_model",
                  query.binary(query.column("device_id", "p"), ruvia::DbBinaryOperator::kEqual,
                               query.column("id", "d")),
                  "p")
            .where(query.binary(query.column("access_rank", "d"),
                                ruvia::DbBinaryOperator::kGreater,
                                DeviceAccessService::integer(query, 0)))
            .orderBy(query.column("group_id", "d"), ruvia::DbOrderDirection::kAsc,
                     ruvia::DbNullsOrder::kLast)
            .addOrderBy(query.column("created_at", "d"))
            .addOrderBy(query.column("id", "d"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceItemDto> items(ruvia::ModelOptions{.resource = c.arena()});
        std::map<std::string, DeviceItemDto*, std::less<>> itemsById;
        for (const auto& row : rows) {
            auto& item = items.emplace(ruvia::ModelOptions{.resource = c.arena()});
            fillItem(c, item, row, actor);
            itemsById.emplace(std::string(row[0].value().value_or(std::string_view{})), &item);
        }
        co_await fillLatest(c, itemsById);
        DevicePageDataDto result(ruvia::ModelOptions{.resource = c.arena()});
        result.set<"list">(std::move(items)).set<"total">(static_cast<std::int64_t>(rows.size()));
        co_return result;
    }

    ruvia::Task<DeviceRealtimePageDto> realtime(ruvia::Context& c) {
        const auto actor = co_await deviceAccessService().actor(c);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addScopedDevicesCtes(query, actor);
        const auto executionIsEdge = query.binary(query.column("execution", "l"),
                                                  ruvia::DbBinaryOperator::kEqual,
                                                  query.value("edge"));
        query.select({DeviceAccessService::text(query, query.column("id", "d")),
                      DeviceAccessService::jsonText(query, query.column("protocol_params", "d"),
                                                    "device_code"),
                      DeviceAccessService::remoteControlEnabled(
                          query, query.column("protocol_params", "d")),
                      query.column("access_rank", "d"),
                      query.caseWhen({{executionIsEdge,
                                      DeviceAccessService::text(
                                          query, query.column("edge_node_id", "l"))}}),
                      query.caseWhen({{executionIsEdge,
                                      DeviceAccessService::jsonText(
                                          query, query.column("endpoint", "l"), "transport")}})})
            .from("scoped_device", "d")
            .join(ruvia::DbJoinType::kLeft, "link",
                  query.binary(query.column("id", "l"), ruvia::DbBinaryOperator::kEqual,
                               query.column("link_id", "d")),
                  "l")
            .where(query.binary(query.column("access_rank", "d"),
                                ruvia::DbBinaryOperator::kGreater,
                                DeviceAccessService::integer(query, 0)))
            .orderBy(query.column("id", "d"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceRealtimeDto> items(
            ruvia::ModelOptions{.resource = c.arena()});
        std::map<std::string, DeviceRealtimeDto*, std::less<>> itemsById;
        for (const auto& row : rows) {
            const auto capabilities = DeviceAccessService::capabilities(
                actor, DeviceAccessService::rank(row[3].value().value_or(std::string_view{})), row[2].value().value_or(std::string_view{}) == "t");
            auto& item = items.emplace(ruvia::ModelOptions{.resource = c.arena()});
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"deviceCode">(row[1].value().value_or(std::string_view{}))
                .set<"connected">(false)
                .set<"connectionState">("disconnected")
                .set<"elements">(ruvia::BoxedArray<DeviceElementDto>(
                    ruvia::ModelOptions{.resource = c.arena()}))
                .set<"canEdit">(capabilities.canEdit)
                .set<"canDelete">(capabilities.canDelete)
                .set<"canShare">(capabilities.canShare)
                .set<"canCommand">(capabilities.canCommand)
                .set<"accessLevel">(capabilities.accessLevel);
            if (row[4].value().has_value())
                item.set<"edgeNodeId">(row[4].value().value_or(std::string_view{}));
            if (row[5].value().has_value())
                item.set<"edgeTransport">(row[5].value().value_or(std::string_view{}));
            itemsById.emplace(std::string(row[0].value().value_or(std::string_view{})), &item);
        }
        co_await fillLatest(c, itemsById);
        DeviceRealtimePageDto result(ruvia::ModelOptions{.resource = c.arena()});
        result.set<"list">(std::move(items)).set<"total">(static_cast<std::int64_t>(rows.size()));
        co_return result;
    }

    ruvia::Task<DeviceItemDto> detail(ruvia::Context& c, std::string_view id) {
        const auto actor = co_await deviceAccessService().actor(c);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addScopedDevicesCtes(query, actor);
        selectItemColumns(query);
        query.from("scoped_device", "d")
            .join(ruvia::DbJoinType::kLeft, "link",
                  query.binary(query.column("id", "l"), ruvia::DbBinaryOperator::kEqual,
                               query.column("link_id", "d")),
                  "l")
            .join(ruvia::DbJoinType::kLeft, "edge_node",
                  query.binary(query.column("id", "en"), ruvia::DbBinaryOperator::kEqual,
                               query.column("edge_node_id", "l")),
                  "en")
            .join(ruvia::DbJoinType::kInner, "device_model",
                  query.binary(query.column("device_id", "p"), ruvia::DbBinaryOperator::kEqual,
                               query.column("id", "d")),
                  "p")
            .where(andAll(query,
                          query.binary(query.column("id", "d"), ruvia::DbBinaryOperator::kEqual,
                                       DeviceAccessService::uuid(query, id)),
                          query.binary(query.column("access_rank", "d"),
                                       ruvia::DbBinaryOperator::kGreater,
                                       DeviceAccessService::integer(query, 0))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(18001, "设备不存在", 404);
        DeviceItemDto item(ruvia::ModelOptions{.resource = c.arena()});
        fillItem(c, item, rows.front(), actor);
        std::map<std::string, DeviceItemDto*, std::less<>> itemById{{std::string(id), &item}};
        co_await fillLatest(c, itemById);
        co_await fillCommandOperations(c, itemById, id);
        co_return item;
    }

    ruvia::Task<std::string> history(ruvia::Context& c, std::string_view id) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::view);

        const auto start = c.req().query("startTime").value_or("");
        const auto end = c.req().query("endTime").value_or("");
        if (start.empty() || end.empty())
            service::common::fail(18002, "startTime 和 endTime 不能为空", 400);

        const auto requestedPage = service::common::parseInt64(c.req().query("page")).value_or(1);
        const auto requestedPageSize =
            service::common::parseInt64(c.req().query("pageSize")).value_or(20);
        const auto page = requestedPage > 0 ? requestedPage : std::int64_t{1};
        const auto pageSize =
            requestedPageSize < 1 ? std::int64_t{20}
                                  : std::min<std::int64_t>(requestedPageSize, 100);
        const auto offset = (page - 1) * pageSize;

        try {
            const auto rows = co_await c.db().query(
                historyQuery(c.pool(), id, start, end, pageSize, offset, page));
            co_return rows.empty() ? std::string{"{\"list\":[],\"total\":0}"}
                                         : std::string{rows.front()[0].value().value_or(std::string_view{})};
        } catch (const std::exception&) {
            service::common::fail(18002, "时间范围格式错误", 400);
        }
    }

    ruvia::Task<ruvia::BoxedArray<DeviceOptionDto>> options(ruvia::Context& c) {
        const auto actor = co_await deviceAccessService().actor(c);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addScopedDevicesCtes(query, actor);
        query.select({DeviceAccessService::text(query, query.column("id")),
                      query.column("name"),
                      DeviceAccessService::jsonText(query, query.column("protocol_params"),
                                                    "device_code"),
                      DeviceAccessService::remoteControlEnabled(
                          query, query.column("protocol_params")),
                      query.column("access_rank")})
            .from("scoped_device")
            .where(andAll(query,
                          query.binary(query.column("access_rank"),
                                       ruvia::DbBinaryOperator::kGreater,
                                       DeviceAccessService::integer(query, 0)),
                          query.binary(query.column("status"), ruvia::DbBinaryOperator::kEqual,
                                       query.value("enabled"))))
            .orderBy(query.column("name"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceOptionDto> result(
            ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            const auto capabilities = DeviceAccessService::capabilities(
                actor, DeviceAccessService::rank(row[4].value().value_or(std::string_view{})), row[3].value().value_or(std::string_view{}) == "t");
            auto& item = result.emplace(ruvia::ModelOptions{.resource = c.arena()});
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"name">(row[1].value().value_or(std::string_view{}))
                .set<"deviceCode">(row[2].value().value_or(std::string_view{}))
                .set<"canEdit">(capabilities.canEdit)
                .set<"canDelete">(capabilities.canDelete)
                .set<"canShare">(capabilities.canShare)
                .set<"canCommand">(capabilities.canCommand)
                .set<"accessLevel">(capabilities.accessLevel);
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const SaveDeviceBody& body) {
        co_await validate(c, body, true);
        co_await ensureUnique(c, body, std::nullopt);
        co_await validateRuntimeIdentity(c, body, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        const std::string name(body.get<"name">()->view());
        const std::string deviceCode(body.get<"deviceCode">()->view());
        const std::string linkId = str(body.get<"linkId">());
        ruvia::DbQuery channelQuery(c.pool());
        channelQuery
            .select(channelQuery.coalesce({DeviceAccessService::text(
                                               channelQuery,
                                               channelQuery.column("edge_node_id")),
                                           channelQuery.value("")}))
            .from("link")
            .where(andAll(channelQuery,
                          channelQuery.binary(channelQuery.column("id"),
                                              ruvia::DbBinaryOperator::kEqual,
                                              DeviceAccessService::uuid(channelQuery, linkId)),
                          channelQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                             channelQuery.column("deleted_at"))));
        const auto channel = co_await c.db().query(channelQuery);
        if (channel.empty()) service::common::fail(18003, "通道不存在", 400);
        const std::string edgeNodeId(channel.front()[0].value().value_or(""));
        const std::string targetId = str(body.get<"targetId">());
        const std::string protocolConfigId(body.get<"protocolConfigId">()->view());
        const std::string groupId = str(body.get<"groupId">());
        const std::string status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        const std::int64_t onlineTimeout =
            body.get<"onlineTimeout">() ? static_cast<std::int64_t>(*body.get<"onlineTimeout">()) : 300;
        const std::string remoteControl =
            (!body.get<"remoteControl">() || *body.get<"remoteControl">()) ? "true" : "false";
        const std::string modbusMode = str(body.get<"modbusMode">());
        const std::string slaveId =
            body.get<"slaveId">() ? std::to_string(static_cast<std::int64_t>(*body.get<"slaveId">())) : "";
        const std::string timezone = (body.get<"timezone">() && !body.get<"timezone">()->view().empty())
                                         ? std::string(body.get<"timezone">()->view())
                                         : "+08:00";
        const std::string heartbeat = packetJson(body.get<"heartbeat">());
        const std::string registration =
            edgeNodeId.empty() ? packetJson(body.get<"registration">()) : R"({"mode":"OFF"})";
        const std::string remark = str(body.get<"remark">());
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery insert(c.pool());
        const auto jsonb = [&](std::string_view value) {
            return insert.cast(insert.value(value), ruvia::DbDataType::kJsonb);
        };
        const auto emptyPacket = jsonb(R"({"mode":"OFF"})");
        const auto protocolParams = insert.call(
            "jsonb_strip_nulls",
            {insert.call("jsonb_build_object",
                         {DeviceAccessService::textKey(insert, "device_code"),
                          DeviceAccessService::text(insert, insert.value(deviceCode)),
                          DeviceAccessService::textKey(insert, "target_id"),
                          insert.nullIf(DeviceAccessService::text(insert, insert.value(targetId)),
                                        insert.value("")),
                          DeviceAccessService::textKey(insert, "online_timeout"),
                          insert.cast(insert.value(onlineTimeout), ruvia::DbDataType::kInteger),
                          DeviceAccessService::textKey(insert, "remote_control"),
                          insert.cast(insert.value(remoteControl == "true"),
                                      ruvia::DbDataType::kBoolean),
                          DeviceAccessService::textKey(insert, "modbus_mode"),
                          insert.nullIf(DeviceAccessService::text(insert, insert.value(modbusMode)),
                                        insert.value("")),
                          DeviceAccessService::textKey(insert, "slave_id"),
                          insert.cast(insert.nullIf(DeviceAccessService::text(
                                                        insert, insert.value(slaveId)),
                                                    insert.value("")),
                                      ruvia::DbDataType::kInteger),
                          DeviceAccessService::textKey(insert, "timezone"),
                          DeviceAccessService::text(insert, insert.value(timezone)),
                          DeviceAccessService::textKey(insert, "heartbeat"),
                          insert.coalesce({insert.cast(insert.nullIf(
                                                     DeviceAccessService::text(insert,
                                                                               insert.value(heartbeat)),
                                                     insert.value("")),
                                                 ruvia::DbDataType::kJsonb),
                                           emptyPacket}),
                          DeviceAccessService::textKey(insert, "registration"),
                          insert.coalesce({insert.cast(insert.nullIf(
                                                     DeviceAccessService::text(insert,
                                                                               insert.value(registration)),
                                                     insert.value("")),
                                                 ruvia::DbDataType::kJsonb),
                                           emptyPacket})})});
        insert.insertInto("device", {"id", "name", "link_id", "protocol_config_id", "group_id",
                                      "status", "protocol_params", "remark", "created_by",
                                      "protocol_revision"})
            .values({DeviceAccessService::uuid(insert, id), insert.value(name),
                     DeviceAccessService::uuid(insert, linkId),
                     DeviceAccessService::uuid(insert, protocolConfigId),
                     DeviceAccessService::nullableUuid(insert, groupId), insert.value(status),
                     protocolParams,
                     insert.nullIf(DeviceAccessService::text(insert, insert.value(remark)),
                                   insert.value("")),
                     DeviceAccessService::uuid(insert, principal.userId),
                     insert.value(static_cast<std::int64_t>(*body.get<"protocolRevision">()))});
        (void)co_await transaction.execute(insert);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "device", "created", id);
        co_await transaction.commit();
        try {
            (void)co_await service::rpc::call(c, "telemetry", "initialize",
                                             id + "\n" + deviceCode);
        } catch (...) {
            // PostgreSQL is authoritative; startup hydration or the first report repairs Redis.
        }
        if (!edgeNodeId.empty())
            (void)co_await service::edge::EdgeService::queueSnapshot(c, edgeNodeId);
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id, const SaveDeviceBody& body) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::owner);
        const auto principal = service::middleware::requireAuth(c);
        ruvia::DbQuery currentQuery(c.pool());
        currentQuery
            .select({DeviceAccessService::text(currentQuery,
                                               currentQuery.column("link_id", "d")),
                     currentQuery.coalesce({DeviceAccessService::text(
                                                currentQuery,
                                                currentQuery.column("edge_node_id", "l")),
                                            currentQuery.value("")}),
                     DeviceAccessService::text(
                         currentQuery, currentQuery.column("protocol_config_id", "d")),
                     DeviceAccessService::jsonText(
                         currentQuery, currentQuery.column("protocol_params", "d"),
                         "device_code"),
                     currentQuery.column("execution", "l"),
                     DeviceAccessService::text(currentQuery,
                                               currentQuery.column("status", "d"))})
            .from("device", "d")
            .join(ruvia::DbJoinType::kInner, "link",
                  currentQuery.binary(currentQuery.column("id", "l"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      currentQuery.column("link_id", "d")),
                  "l")
            .where(andAll(currentQuery,
                          currentQuery.binary(currentQuery.column("id", "d"),
                                              ruvia::DbBinaryOperator::kEqual,
                                              DeviceAccessService::uuid(currentQuery, id)),
                          currentQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                             currentQuery.column("deleted_at", "d"))));
        const auto rows = co_await c.db().query(currentQuery);
        if (rows.empty())
            service::common::fail(18001, "设备不存在", 404);
        co_await validate(c, body, false);

        const auto& current = rows.front();
        const std::string currentLinkId(
            current[0].value().value_or(std::string_view{}));
        const std::string currentEdgeNodeId(
            current[1].value().value_or(std::string_view{}));
        const std::string currentProtocolConfigId(
            current[2].value().value_or(std::string_view{}));
        const std::string currentExecution(
            current[4].value().value_or(std::string_view{}));
        const std::string requestedLinkId = str(body.get<"linkId">());
        const std::string targetLinkId = requestedLinkId.empty() ? currentLinkId : requestedLinkId;
        const std::string targetProtocolConfigId = body.get<"protocolConfigId">() ? str(body.get<"protocolConfigId">()) : currentProtocolConfigId;
        ruvia::DbQuery targetQuery(c.pool());
        targetQuery
            .select(targetQuery.coalesce({DeviceAccessService::text(
                                              targetQuery,
                                              targetQuery.column("edge_node_id", "l")),
                                          targetQuery.value("")}))
            .from("link", "l")
            .join(ruvia::DbJoinType::kInner, "protocol_config",
                  targetQuery.binary(targetQuery.column("protocol", "p"),
                                     ruvia::DbBinaryOperator::kEqual,
                                     targetQuery.column("protocol", "l")),
                  "p")
            .where(andAll(targetQuery,
                          targetQuery.binary(targetQuery.column("id", "l"),
                                             ruvia::DbBinaryOperator::kEqual,
                                             DeviceAccessService::uuid(targetQuery, targetLinkId)),
                          targetQuery.binary(targetQuery.column("id", "p"),
                                             ruvia::DbBinaryOperator::kEqual,
                                             DeviceAccessService::uuid(targetQuery,
                                                                        targetProtocolConfigId)),
                          targetQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                            targetQuery.column("deleted_at", "l")),
                          targetQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                            targetQuery.column("deleted_at", "p"))));
        const auto target = co_await c.db().query(targetQuery);
        if (target.empty()) service::common::fail(18003, "通道或设备类型不存在，或协议不一致", 400);
        const std::string targetEdgeNodeId(target.front()[0].value().value_or(""));
        const bool targetEdge = !targetEdgeNodeId.empty();
        const bool connectionChanged = targetLinkId != currentLinkId || targetProtocolConfigId != currentProtocolConfigId;
        co_await ensureUnique(c, body, std::string(id));
        co_await validateRuntimeIdentity(c, body, std::string(id));

        ruvia::DbQuery update(c.pool());
        update.update("device");
        bool changed = false;
        const auto assign = [&](std::string_view column, ruvia::DbQuery::Expr value) {
            update.set(column, value);
            changed = true;
        };
        if (body.get<"name">())
            assign("name", update.value(body.get<"name">()->view()));
        if (targetLinkId != currentLinkId)
            assign("link_id", DeviceAccessService::uuid(update, targetLinkId));
        if (body.get<"protocolConfigId">())
            assign("protocol_config_id",
                   DeviceAccessService::uuid(update, targetProtocolConfigId));
        if (body.get<"groupId">())
            assign("group_id",
                   DeviceAccessService::nullableUuid(update, body.get<"groupId">()->view()));
        if (body.get<"protocolRevision">())
            assign("protocol_revision",
                   update.value(static_cast<std::int64_t>(*body.get<"protocolRevision">())));
        if (body.get<"status">())
            assign("status", update.value(body.get<"status">()->view()));
        auto protocolParams = update.column("protocol_params");
        const auto jsonPath = [&](std::string_view key) {
            return update.cast(update.array({DeviceAccessService::textKey(update, key)}),
                               ruvia::DbTypeDefinition{.dataType = ruvia::DbDataType::kText,
                                                       .array = true});
        };
        const auto jsonValue = [&](std::string_view key, ruvia::DbQuery::Expr value) {
            protocolParams = update.call(
                "jsonb_set",
                 {protocolParams, jsonPath(key),
                 update.call("to_jsonb", {value}), DeviceAccessService::boolean(update, true)});
            changed = true;
        };
        const auto jsonDocument = [&](std::string_view key, std::string_view value) {
            protocolParams = update.call(
                "jsonb_set",
                 {protocolParams, jsonPath(key),
                 update.cast(update.value(value), ruvia::DbDataType::kJsonb),
                 DeviceAccessService::boolean(update, true)});
            changed = true;
        };
        if (body.get<"deviceCode">())
            jsonValue("device_code", DeviceAccessService::text(
                                          update, update.value(body.get<"deviceCode">()->view())));
        if (body.get<"targetId">())
            jsonValue("target_id", DeviceAccessService::text(
                                        update, update.value(body.get<"targetId">()->view())));
        else if (connectionChanged)
            protocolParams = update.binary(protocolParams, ruvia::DbBinaryOperator::kJsonDelete,
                                           DeviceAccessService::textKey(update, "target_id")),
            changed = true;
        if (body.get<"onlineTimeout">())
            jsonValue("online_timeout",
                      update.cast(update.value(static_cast<std::int64_t>(
                                                   *body.get<"onlineTimeout">())),
                                               ruvia::DbDataType::kBigInt));
        if (body.get<"remoteControl">())
            jsonValue("remote_control",
                      update.cast(update.value(static_cast<bool>(*body.get<"remoteControl">())),
                                  ruvia::DbDataType::kBoolean));
        if (body.get<"modbusMode">())
            jsonValue("modbus_mode", DeviceAccessService::text(
                                         update, update.value(body.get<"modbusMode">()->view())));
        if (body.get<"slaveId">())
            jsonValue("slave_id",
                      update.cast(update.value(static_cast<std::int64_t>(*body.get<"slaveId">())),
                                  ruvia::DbDataType::kBigInt));
        if (body.get<"timezone">())
            jsonValue("timezone", DeviceAccessService::text(
                                      update, update.value(body.get<"timezone">()->view())));
        std::string heartbeat;
        if (body.get<"heartbeat">()) {
            heartbeat = packetJson(body.get<"heartbeat">());
            jsonDocument("heartbeat", heartbeat);
        }
        std::string registration;
        if (targetEdge) {
            registration = R"({"mode":"OFF"})";
            jsonDocument("registration", registration);
        } else if (body.get<"registration">()) {
            registration = packetJson(body.get<"registration">());
            jsonDocument("registration", registration);
        }
        if (changed)
            assign("protocol_params", protocolParams);
        if (body.get<"remark">())
            assign("remark", update.nullIf(DeviceAccessService::text(
                                                  update, update.value(body.get<"remark">()->view())),
                                              update.value("")));

        {
            auto transaction = co_await c.db().beginTransaction();
            if (changed)
                assign("updated_at", update.call("now"));
            if (changed)
                update.where(update.binary(update.column("id"),
                                           ruvia::DbBinaryOperator::kEqual,
                                           DeviceAccessService::uuid(update, id)));
            if (changed)
                (void)co_await transaction.execute(update);
            co_await service::system::OutboxService::enqueueConfigEvent(transaction, "device", "updated", id);
            co_await transaction.commit();
        }
        try {
            (void)co_await service::rpc::call(c, "telemetry", "project-device", std::string(id));
        } catch (...) {
            // PostgreSQL remains authoritative; startup hydration repairs Redis read models.
        }
        if (!currentEdgeNodeId.empty())
            (void)co_await service::edge::EdgeService::queueSnapshot(
                c, currentEdgeNodeId);
        if (!targetEdgeNodeId.empty() && targetEdgeNodeId != currentEdgeNodeId)
            (void)co_await service::edge::EdgeService::queueSnapshot(
                c, targetEdgeNodeId);
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::owner);
        ruvia::DbQuery currentQuery(c.pool());
        currentQuery
            .select({DeviceAccessService::jsonText(currentQuery,
                                                    currentQuery.column("protocol_params", "d"),
                                                    "device_code"),
                     currentQuery.coalesce({DeviceAccessService::text(
                                                currentQuery,
                                                currentQuery.column("edge_node_id", "l")),
                                            currentQuery.value("")}),
                     DeviceAccessService::text(currentQuery,
                                               currentQuery.column("link_id", "d")),
                     currentQuery.column("execution", "l")})
            .from("device", "d")
            .join(ruvia::DbJoinType::kInner, "link",
                  currentQuery.binary(currentQuery.column("id", "l"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      currentQuery.column("link_id", "d")),
                  "l")
            .where(andAll(currentQuery,
                          currentQuery.binary(currentQuery.column("id", "d"),
                                              ruvia::DbBinaryOperator::kEqual,
                                              DeviceAccessService::uuid(currentQuery, id)),
                          currentQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                             currentQuery.column("deleted_at", "d"))));
        const auto rows = co_await c.db().query(currentQuery);
        if (rows.empty())
            service::common::fail(18001, "设备不存在", 404);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery removeQuery(c.pool());
        removeQuery.update("device")
            .set("deleted_at", removeQuery.call("now"))
            .set("updated_at", removeQuery.call("now"))
            .where(removeQuery.binary(removeQuery.column("id"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      DeviceAccessService::uuid(removeQuery, id)));
        (void)co_await transaction.execute(removeQuery);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "device", "deleted", id);
        co_await transaction.commit();
        try {
            (void)co_await service::rpc::call(c, "telemetry", "erase-device", std::string(id));
        } catch (...) {
            // The next startup hydration removes stale Redis state for deleted devices.
        }
        if (!rows.front()[1].value().value_or(std::string_view{}).empty())
            (void)co_await service::edge::EdgeService::queueSnapshot(
                c, rows.front()[1].value().value_or(std::string_view{}));
    }

    // ===== 设备分组（合并入同一 DeviceService 类）=====

    ruvia::Task<ruvia::BoxedArray<DeviceGroupItemDto>> listGroups(ruvia::Context& c, bool withCount) {
        const auto actor = co_await deviceAccessService().actor(c);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addVisibleGroupsCtes(query, actor);
        ruvia::DbQuery visible(query.resource());
        visible.select(visible.column("id")).from("visible_group");
        ruvia::DbQuery count(query.resource());
        count.select(count.aggregate("count", {count.star()}))
            .from("scoped_device", "scoped")
            .where(andAll(count,
                          count.binary(count.column("group_id", "scoped"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       count.column("id", "g")),
                          count.binary(count.column("access_rank", "scoped"),
                                       ruvia::DbBinaryOperator::kGreater,
                                       DeviceAccessService::integer(count, 0))));
        query.select({DeviceAccessService::text(query, query.column("id", "g")),
                      query.column("name", "g"),
                      query.coalesce({DeviceAccessService::text(
                                          query, query.column("parent_id", "g")),
                                      query.value("")}),
                      query.column("status", "g"), query.column("sort_order", "g"),
                      query.coalesce({query.column("remark", "g"), query.value("")}),
                      withCount ? query.subquery(count) : DeviceAccessService::integer(query, 0),
                      query.call("iot_utc_timestamp", {query.column("created_at", "g")}),
                      query.call("iot_utc_timestamp", {query.column("updated_at", "g")}),
                      DeviceAccessService::text(query, query.column("created_by", "g"))})
            .from("device_group", "g")
            .where(andAll(query,
                          query.unary(ruvia::DbUnaryOperator::kIsNull,
                                      query.column("deleted_at", "g")),
                          query.binary(query.column("id", "g"), ruvia::DbBinaryOperator::kIn,
                                       query.subquery(visible))))
            .orderBy(query.column("sort_order", "g"))
            .addOrderBy(query.column("id", "g"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceGroupItemDto> result(
            ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows)
            fillGroup(result.emplace(ruvia::ModelOptions{.resource = c.arena()}), row, actor);
        co_return result;
    }

    ruvia::Task<DeviceGroupItemDto> groupDetail(ruvia::Context& c, std::string_view id) {
        const auto actor = co_await deviceAccessService().actor(c);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addVisibleGroupsCtes(query, actor);
        ruvia::DbQuery visible(query.resource());
        visible.select(visible.column("id")).from("visible_group");
        ruvia::DbQuery count(query.resource());
        count.select(count.aggregate("count", {count.star()}))
            .from("scoped_device", "scoped")
            .where(andAll(count,
                          count.binary(count.column("group_id", "scoped"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       count.column("id", "group_entry")),
                          count.binary(count.column("access_rank", "scoped"),
                                       ruvia::DbBinaryOperator::kGreater,
                                       DeviceAccessService::integer(count, 0))));
        query.select({DeviceAccessService::text(query, query.column("id", "group_entry")),
                      query.column("name", "group_entry"),
                      query.coalesce({DeviceAccessService::text(
                                          query, query.column("parent_id", "group_entry")),
                                      query.value("")}),
                      query.column("status", "group_entry"),
                      query.column("sort_order", "group_entry"),
                      query.coalesce({query.column("remark", "group_entry"), query.value("")}),
                      query.subquery(count),
                      query.call("iot_utc_timestamp", {query.column("created_at", "group_entry")}),
                      query.call("iot_utc_timestamp", {query.column("updated_at", "group_entry")}),
                      DeviceAccessService::text(query, query.column("created_by", "group_entry"))})
            .from("device_group", "group_entry")
            .where(andAll(query,
                          query.binary(query.column("id", "group_entry"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       DeviceAccessService::uuid(query, id)),
                          query.unary(ruvia::DbUnaryOperator::kIsNull,
                                      query.column("deleted_at", "group_entry")),
                          query.binary(query.column("id", "group_entry"),
                                       ruvia::DbBinaryOperator::kIn, query.subquery(visible))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(17001, "设备分组不存在", 404);
        DeviceGroupItemDto item(ruvia::ModelOptions{.resource = c.arena()});
        fillGroup(item, rows.front(), actor);
        co_return item;
    }

    ruvia::Task<void> createGroup(ruvia::Context& c, const SaveDeviceGroupBody& body) {
        co_await validateParent(c, body, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        const std::string name(body.get<"name">()->view());
        const std::string parentId = body.get<"parentId">() ? std::string(body.get<"parentId">()->view()) : "";
        const std::string status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        const std::int64_t sortOrder =
            body.get<"sortOrder">() ? static_cast<std::int64_t>(*body.get<"sortOrder">()) : 0;
        const std::string remark = body.get<"remark">() ? std::string(body.get<"remark">()->view()) : "";
        ruvia::DbQuery insert(c.pool());
        insert.insertInto("device_group",
                          {"id", "name", "parent_id", "status", "sort_order", "remark",
                           "created_by"})
            .values({DeviceAccessService::uuid(insert, id), insert.value(name),
                     DeviceAccessService::nullableUuid(insert, parentId), insert.value(status),
                     insert.value(sortOrder),
                     insert.nullIf(DeviceAccessService::text(insert, insert.value(remark)),
                                   insert.value("")),
                     DeviceAccessService::uuid(insert, principal.userId)});
        (void)co_await c.db().execute(insert);
    }

    ruvia::Task<void> updateGroup(ruvia::Context& c, std::string_view id,
                                  const SaveDeviceGroupBody& body) {
        ruvia::DbQuery currentQuery(c.pool());
        currentQuery.select(currentQuery.column("created_by"))
            .from("device_group")
            .where(andAll(currentQuery,
                          currentQuery.binary(currentQuery.column("id"),
                                              ruvia::DbBinaryOperator::kEqual,
                                              DeviceAccessService::uuid(currentQuery, id)),
                          currentQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                             currentQuery.column("deleted_at"))));
        const auto rows = co_await c.db().query(currentQuery);
        if (rows.empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await requireGroupOwner(c, rows.front()[0].value().value_or(std::string_view{}));
        co_await validateParent(c, body, std::string(id));

        ruvia::DbQuery update(c.pool());
        update.update("device_group");
        bool changed = false;
        const auto assign = [&](std::string_view column, ruvia::DbQuery::Expr value) {
            update.set(column, value);
            changed = true;
        };
        if (body.get<"name">())
            assign("name", update.value(body.get<"name">()->view()));
        if (body.get<"parentId">())
            assign("parent_id", DeviceAccessService::nullableUuid(
                                      update, body.get<"parentId">()->view()));
        if (body.get<"status">())
            assign("status", update.value(body.get<"status">()->view()));
        if (body.get<"sortOrder">())
            assign("sort_order",
                   update.value(static_cast<std::int64_t>(*body.get<"sortOrder">())));
        if (body.get<"remark">())
            assign("remark", update.nullIf(DeviceAccessService::text(
                                                   update,
                                                   update.value(body.get<"remark">()->view())),
                                               update.value("")));
        if (!changed)
            co_return;
        update.set("updated_at", update.call("now"))
            .where(update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual,
                                 DeviceAccessService::uuid(update, id)));
        (void)co_await c.db().execute(update);
    }

    ruvia::Task<void> removeGroup(ruvia::Context& c, std::string_view id) {
        ruvia::DbQuery currentQuery(c.pool());
        currentQuery.select(currentQuery.column("created_by"))
            .from("device_group")
            .where(andAll(currentQuery,
                          currentQuery.binary(currentQuery.column("id"),
                                              ruvia::DbBinaryOperator::kEqual,
                                              DeviceAccessService::uuid(currentQuery, id)),
                          currentQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                             currentQuery.column("deleted_at"))));
        const auto rows = co_await c.db().query(currentQuery);
        if (rows.empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await requireGroupOwner(c, rows.front()[0].value().value_or(std::string_view{}));
        ruvia::DbQuery childGroups(c.pool());
         childGroups.select(DeviceAccessService::integer(childGroups, 1))
            .from("device_group")
            .where(andAll(childGroups,
                          childGroups.binary(childGroups.column("parent_id"),
                                             ruvia::DbBinaryOperator::kEqual,
                                             DeviceAccessService::uuid(childGroups, id)),
                          childGroups.unary(ruvia::DbUnaryOperator::kIsNull,
                                            childGroups.column("deleted_at"))));
        ruvia::DbQuery childDevices(c.pool());
         childDevices.select(DeviceAccessService::integer(childDevices, 1))
            .from("device")
            .where(andAll(childDevices,
                          childDevices.binary(childDevices.column("group_id"),
                                              ruvia::DbBinaryOperator::kEqual,
                                              DeviceAccessService::uuid(childDevices, id)),
                          childDevices.unary(ruvia::DbUnaryOperator::kIsNull,
                                             childDevices.column("deleted_at"))));
        ruvia::DbQuery used(c.pool());
        used.select(used.binary(used.exists(childGroups), ruvia::DbBinaryOperator::kOr,
                                used.exists(childDevices)));
        const auto usedRows = co_await c.db().query(used);
        if (usedRows.front()[0].value().value_or(std::string_view{}) == "t")
            service::common::fail(17004, "请先移除子分组和设备", 409);
        ruvia::DbQuery removeQuery(c.pool());
        removeQuery.update("device_group")
            .set("deleted_at", removeQuery.call("now"))
            .set("updated_at", removeQuery.call("now"))
            .where(removeQuery.binary(removeQuery.column("id"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      DeviceAccessService::uuid(removeQuery, id)));
        (void)co_await c.db().execute(removeQuery);
    }

  private:
    static std::int64_t toInt(std::string_view value, std::int64_t fallback = 0) {
        return service::common::parseInt64(std::optional<std::string_view>{value})
            .value_or(fallback);
    }

    static std::optional<double> parseDouble(std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.remove_suffix(1);
        if (value.empty())
            return std::nullopt;
        return service::utils::decimal(value);
    }

    static double toDouble(std::string_view value, double fallback = 0) {
        return parseDouble(value).value_or(fallback);
    }

    static std::string edgeDeviceStatusKey(std::string_view nodeId, std::string_view deviceId) {
        return "iot:runtime:edge:" + std::string(nodeId) + ":device:" +
               std::string(deviceId);
    }

    static ruvia::DbQuery historyQuery(std::pmr::memory_resource* resource,
                                       std::string_view deviceId, std::string_view start,
                                       std::string_view end, std::int64_t pageSize,
                                       std::int64_t offset, std::int64_t page) {
        const auto timestamp = [](ruvia::DbQuery& query, std::string_view value) {
            return query.cast(query.value(value), ruvia::DbDataType::kTimestampTz);
        };
        ruvia::DbQuery counted(resource);
        const auto countedData = counted.column("data", "record");
        counted.select(counted.aggregate("count", {counted.star()}))
            .from("device_data", "record")
            .where(andAll(counted,
                          counted.binary(counted.column("device_id", "record"),
                                         ruvia::DbBinaryOperator::kEqual,
                                         DeviceAccessService::uuid(counted, deviceId)),
                          counted.binary(counted.column("report_time", "record"),
                                         ruvia::DbBinaryOperator::kGreaterEqual,
                                         timestamp(counted, start)),
                          counted.binary(counted.column("report_time", "record"),
                                         ruvia::DbBinaryOperator::kLessEqual,
                                         timestamp(counted, end)),
                          counted.binary(
                              counted.call("jsonb_typeof",
                                           {DeviceAccessService::jsonValue(
                                               counted, countedData, "values")}),
                              ruvia::DbBinaryOperator::kEqual, counted.value("object"))));

        ruvia::DbQuery filtered(resource);
        const auto filteredData = filtered.column("data", "record");
        filtered
            .select({filtered.column("id", "record"),
                     filtered.column("protocol", "record"),
                     filtered.column("report_time", "record"),
                     filtered.column("source", "record"), filteredData})
            .from("device_data", "record")
            .where(andAll(filtered,
                          filtered.binary(filtered.column("device_id", "record"),
                                          ruvia::DbBinaryOperator::kEqual,
                                          DeviceAccessService::uuid(filtered, deviceId)),
                          filtered.binary(filtered.column("report_time", "record"),
                                          ruvia::DbBinaryOperator::kGreaterEqual,
                                          timestamp(filtered, start)),
                          filtered.binary(filtered.column("report_time", "record"),
                                          ruvia::DbBinaryOperator::kLessEqual,
                                          timestamp(filtered, end)),
                          filtered.binary(
                              filtered.call("jsonb_typeof",
                                            {DeviceAccessService::jsonValue(
                                                filtered, filteredData, "values")}),
                              ruvia::DbBinaryOperator::kEqual, filtered.value("object"))))
            .orderBy(filtered.column("report_time", "record"), ruvia::DbOrderDirection::kDesc)
            .addOrderBy(filtered.column("id", "record"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>(offset));

        ruvia::DbQuery normalized(resource);
        ruvia::DbQuery normalizedValues(resource);
        const auto point = normalizedValues.column("value", "point");
        const auto pointValue = DeviceAccessService::jsonValue(
            normalizedValues, point, "value");
        const auto pointObject = andAll(
            normalizedValues,
            normalizedValues.binary(normalizedValues.call("jsonb_typeof", {point}),
                                    ruvia::DbBinaryOperator::kEqual,
                                    normalizedValues.value("object")),
            normalizedValues.binary(
                normalizedValues.call("jsonb_typeof", {pointValue}),
                ruvia::DbBinaryOperator::kEqual, normalizedValues.value("boolean")));
        const auto pointBoolean = normalizedValues.cast(
            DeviceAccessService::jsonText(normalizedValues, point, "value"),
            ruvia::DbDataType::kBoolean);
        const auto pointNumber = normalizedValues.caseWhen(
            {{pointBoolean, DeviceAccessService::integer(normalizedValues, 1)}},
            DeviceAccessService::integer(normalizedValues, 0));
        const auto pointPath = normalizedValues.cast(
            normalizedValues.array({DeviceAccessService::textKey(normalizedValues, "value")} ),
            ruvia::DbTypeDefinition{.dataType = ruvia::DbDataType::kText, .array = true});
        const auto normalizedPoint = normalizedValues.caseWhen(
            {{pointObject,
              normalizedValues.call("jsonb_set",
                                    {point, pointPath,
                                     normalizedValues.call("to_jsonb", {pointNumber}),
                                     DeviceAccessService::boolean(normalizedValues, false)})}},
            point);
        normalizedValues
            .select(normalizedValues.aggregate("jsonb_object_agg",
                                               {normalizedValues.column("key", "point"),
                                                normalizedPoint}))
            .fromFunction(
                normalizedValues.call(
                    "jsonb_each",
                    {normalizedValues.coalesce(
                        {DeviceAccessService::jsonValue(
                             normalizedValues,
                             normalizedValues.column("data", "filtered"), "values"),
                         normalizedValues.cast(normalizedValues.value("{}"),
                                               ruvia::DbDataType::kJsonb)})}),
                "point", {.lateral = true,
                           .columns = {{.name = "key"}, {.name = "value"}}});
        normalized
            .select({normalized.star("filtered"),
                     normalized.alias(
                         normalized.coalesce({normalized.subquery(normalizedValues),
                                              normalized.cast(normalized.value("{}"),
                                                              ruvia::DbDataType::kJsonb)}),
                         "normalized_values")})
            .from(filtered, "filtered");

        ruvia::DbQuery query(resource);
        query.with("counted", counted)
            .with("filtered", filtered)
            .with("normalized", normalized);
        const auto total = query.coalesce({query.subquery(counted),
                                           DeviceAccessService::integer(query, 0)});
        const auto item = query.call(
            "jsonb_build_object",
            {DeviceAccessService::textKey(query, "id"), query.column("id", "normalized"),
             DeviceAccessService::textKey(query, "protocol"),
             query.column("protocol", "normalized"),
             DeviceAccessService::textKey(query, "reportTime"),
             query.call("iot_utc_timestamp", {query.column("report_time", "normalized")}),
             DeviceAccessService::textKey(query, "source"),
             query.column("source", "normalized"),
             DeviceAccessService::textKey(query, "functionCode"),
             DeviceAccessService::jsonText(query, query.column("data", "normalized"),
                                           "function_code"),
             DeviceAccessService::textKey(query, "values"),
             query.column("normalized_values", "normalized")});
        const std::array<ruvia::DbOrderTerm, 2> historyOrder{{
            ruvia::DbOrderTerm{query.column("report_time", "normalized"),
                               ruvia::DbOrderDirection::kDesc, ruvia::DbNullsOrder::kDefault},
            ruvia::DbOrderTerm{query.column("id", "normalized"),
                               ruvia::DbOrderDirection::kDesc, ruvia::DbNullsOrder::kDefault}}};
        const auto list = query.coalesce(
            {query.aggregate("jsonb_agg", {item}, false, historyOrder),
             query.cast(query.value("[]"), ruvia::DbDataType::kJsonb)});
        const auto totalPages = query.cast(
            query.call("ceil",
                       {query.binary(query.cast(total, ruvia::DbDataType::kNumeric),
                                     ruvia::DbBinaryOperator::kDivide,
                                     query.cast(query.value(pageSize),
                                                ruvia::DbDataType::kNumeric))}),
            ruvia::DbDataType::kBigInt);
        query.select(query.cast(
            query.call("jsonb_build_object",
                       {DeviceAccessService::textKey(query, "list"), list,
                        DeviceAccessService::textKey(query, "total"), total,
                         DeviceAccessService::textKey(query, "page"),
                         query.cast(query.value(page), ruvia::DbDataType::kBigInt),
                         DeviceAccessService::textKey(query, "pageSize"),
                         query.cast(query.value(pageSize), ruvia::DbDataType::kBigInt),
                        DeviceAccessService::textKey(query, "totalPages"), totalPages}),
            ruvia::DbDataType::kText))
            .from("normalized");
        return query;
    }

    // 列顺序必须与 fillItem 的 row 下标严格对应。
    static void selectItemColumns(ruvia::DbQuery& query) {
        const auto protocolParams = query.column("protocol_params", "d");
        const auto endpoint = query.column("endpoint", "l");
        const auto protocolConfig = query.column("config", "p");
        const auto emptyJson = query.cast(query.value("{}"), ruvia::DbDataType::kJsonb);
        const auto emptyArray = query.cast(query.value("[]"), ruvia::DbDataType::kJsonb);
        const auto nullableJsonText = [&](std::string_view key) {
            return query.nullIf(DeviceAccessService::jsonText(query, protocolParams, key),
                                query.value(""));
        };
        const auto numericText = DeviceAccessService::jsonText(query, protocolParams,
                                                                "online_timeout");
        const auto numericOnlineTimeout = query.caseWhen(
            {{query.binary(query.coalesce({numericText, query.value("")}),
                           ruvia::DbBinaryOperator::kRegex,
                           query.value("^-?[0-9]{1,18}$")),
              query.cast(numericText, ruvia::DbDataType::kInteger)}});
        const auto onlineTimeout = query.coalesce({numericOnlineTimeout,
                                                    DeviceAccessService::integer(query, 300)});
        const auto timezone = query.coalesce(
            {query.nullIf(DeviceAccessService::jsonText(query, protocolParams, "timezone"),
                          query.value("")),
             query.value("+08:00")});
        const auto edgeExecution = query.binary(query.column("execution", "l"),
                                                ruvia::DbBinaryOperator::kEqual,
                                                query.value("edge"));
        const auto rs485 = query.call(
            "lower", {query.coalesce({DeviceAccessService::jsonText(query, endpoint, "rs485"),
                                       query.value("")})});
        const auto rs485Enabled = query.caseWhen(
            {{query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("true")),
              DeviceAccessService::boolean(query, true)},
             {query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("t")),
              DeviceAccessService::boolean(query, true)},
             {query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("1")),
              DeviceAccessService::boolean(query, true)},
             {query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("yes")),
              DeviceAccessService::boolean(query, true)},
             {query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("y")),
              DeviceAccessService::boolean(query, true)},
             {query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("on")),
              DeviceAccessService::boolean(query, true)}},
            DeviceAccessService::boolean(query, false));

        ruvia::DbQuery functionCount(query.resource());
        const auto functions = functionCount.coalesce(
            {DeviceAccessService::jsonValue(functionCount,
                                             functionCount.column("config", "p"), "funcs"),
             functionCount.cast(functionCount.value("[]"), ruvia::DbDataType::kJsonb)});
        const auto function = functionCount.column("value", "function");
        const auto elements = functionCount.coalesce(
            {DeviceAccessService::jsonValue(functionCount, function, "elements"),
             functionCount.cast(functionCount.value("[]"), ruvia::DbDataType::kJsonb)});
        const auto responseElements = functionCount.coalesce(
            {DeviceAccessService::jsonValue(functionCount, function, "responseElements"),
             functionCount.cast(functionCount.value("[]"), ruvia::DbDataType::kJsonb)});
        const auto elementCount = functionCount.call(
            "jsonb_array_length", {elements});
        const auto responseElementCount = functionCount.call(
            "jsonb_array_length", {responseElements});
        functionCount
            .select(functionCount.coalesce(
                {functionCount.aggregate(
                     "sum", {functionCount.binary(elementCount,
                                                    ruvia::DbBinaryOperator::kAdd,
                                                    responseElementCount)}),
                 DeviceAccessService::integer(functionCount, 0)}))
            .fromFunction(functionCount.call("jsonb_array_elements", {functions}), "function",
                          {.columns = {{.name = "value"}}});
        const auto protocolElementCount = query.caseWhen(
            {{query.binary(query.column("protocol", "p"), ruvia::DbBinaryOperator::kEqual,
                           query.value("Modbus")),
              query.call("jsonb_array_length",
                         {query.coalesce({DeviceAccessService::jsonValue(
                                             query, protocolConfig, "registers"),
                                         emptyArray})})},
             {query.binary(query.column("protocol", "p"), ruvia::DbBinaryOperator::kEqual,
                           query.value("S7")),
              query.call("jsonb_array_length",
                         {query.coalesce({DeviceAccessService::jsonValue(
                                             query, protocolConfig, "areas"),
                                         emptyArray})})}},
            query.coalesce({query.subquery(functionCount), DeviceAccessService::integer(query, 0)}));

        query.select({DeviceAccessService::text(query, query.column("id", "d")),
                      query.column("name", "d"),
                      DeviceAccessService::jsonText(query, protocolParams, "device_code"),
                      DeviceAccessService::text(query, query.column("link_id", "d")),
                      nullableJsonText("target_id"),
                      DeviceAccessService::text(query, query.column("protocol_config_id", "d")),
                      DeviceAccessService::text(query, query.column("group_id", "d")),
                      query.column("status", "d"), onlineTimeout,
                      DeviceAccessService::remoteControlEnabled(query, protocolParams),
                      nullableJsonText("modbus_mode"), nullableJsonText("slave_id"), timezone,
                      DeviceAccessService::jsonText(
                          query, DeviceAccessService::jsonValue(query, protocolParams, "heartbeat"),
                          "mode"),
                      DeviceAccessService::jsonText(
                          query, DeviceAccessService::jsonValue(query, protocolParams, "heartbeat"),
                          "content"),
                      DeviceAccessService::jsonText(
                          query, DeviceAccessService::jsonValue(query, protocolParams, "registration"),
                          "mode"),
                      DeviceAccessService::jsonText(
                          query, DeviceAccessService::jsonValue(query, protocolParams, "registration"),
                          "content"),
                      query.coalesce({query.column("remark", "d"), query.value("")}),
                      DeviceAccessService::text(query, query.column("created_by", "d")),
                      query.call("iot_utc_timestamp", {query.column("created_at", "d")}),
                      query.call("iot_utc_timestamp", {query.column("updated_at", "d")}),
                      query.coalesce({query.column("name", "l"), query.value("")}),
                      query.coalesce({DeviceAccessService::jsonText(query, endpoint, "mode"),
                                      query.value("")}),
                      query.coalesce({query.column("protocol", "l"), query.value("")}),
                      query.column("name", "p"), query.column("protocol", "p"),
                      query.nullIf(DeviceAccessService::jsonText(query, protocolConfig,
                                                                  "readInterval"),
                                   query.value("")),
                      query.nullIf(DeviceAccessService::jsonText(query, protocolConfig,
                                                                  "storagePolicy"),
                                   query.value("")),
                      protocolElementCount, query.column("access_rank", "d"),
                      query.caseWhen({{edgeExecution,
                                      DeviceAccessService::text(
                                          query, query.column("edge_node_id", "l"))}}),
                      query.coalesce({query.column("name", "en"), query.value("")}),
                      query.coalesce({query.column("imei", "en"), query.value("")}),
                      query.caseWhen({{edgeExecution,
                                      query.nullIf(DeviceAccessService::jsonText(
                                                       query, endpoint, "transport"),
                                                   query.value(""))}}),
                      query.caseWhen({{edgeExecution,
                                      query.nullIf(DeviceAccessService::jsonText(
                                                       query, endpoint, "interface"),
                                                   query.value(""))}}),
                      query.caseWhen({{edgeExecution,
                                      query.nullIf(DeviceAccessService::jsonText(
                                                       query, endpoint, "mode"),
                                                   query.value(""))}}),
                      query.caseWhen({{edgeExecution,
                                      query.nullIf(DeviceAccessService::jsonText(
                                                       query, endpoint, "ip"),
                                                   query.value(""))}}),
                      query.caseWhen({{edgeExecution,
                                      query.nullIf(DeviceAccessService::jsonText(
                                                       query, endpoint, "port"),
                                                   query.value(""))}}),
                      query.caseWhen({{edgeExecution,
                                      query.nullIf(DeviceAccessService::jsonText(
                                                       query, endpoint, "baud_rate"),
                                                   query.value(""))}}),
                      query.caseWhen({{edgeExecution,
                                      query.nullIf(DeviceAccessService::jsonText(
                                                       query, endpoint, "data_bits"),
                                                   query.value(""))}}),
                      query.caseWhen({{edgeExecution,
                                      query.nullIf(DeviceAccessService::jsonText(
                                                       query, endpoint, "stop_bits"),
                                                   query.value(""))}}),
                      query.caseWhen({{edgeExecution,
                                      query.nullIf(DeviceAccessService::jsonText(
                                                       query, endpoint, "parity"),
                                                   query.value(""))}}),
                      query.caseWhen({{edgeExecution, rs485Enabled}}),
                      query.column("protocol_revision", "d")});
    }

    template <typename Row>
    static void fillItem(ruvia::Context& c, DeviceItemDto& item, Row&& row,
                         const DeviceActor& actor) {
        item.set<"id">(row[0].value().value_or(std::string_view{}));
        item.set<"name">(row[1].value().value_or(std::string_view{}));
        item.set<"deviceCode">(row[2].value().value_or(std::string_view{}));
        if (row[3].value().has_value())
            item.set<"linkId">(row[3].value().value_or(std::string_view{}));
        if (row[4].value().has_value())
            item.set<"targetId">(row[4].value().value_or(std::string_view{}));
        item.set<"protocolConfigId">(row[5].value().value_or(std::string_view{}));
        item.set<"protocolRevision">(toInt(row[43].value().value_or(std::string_view{})));
        if (row[6].value().has_value())
            item.set<"groupId">(row[6].value().value_or(std::string_view{}));
        item.set<"status">(row[7].value().value_or(std::string_view{}));
        item.set<"onlineTimeout">(toInt(row[8].value().value_or(std::string_view{})));
        item.set<"remoteControl">(row[9].value().value_or(std::string_view{}) == "t");
        if (row[10].value().has_value())
            item.set<"modbusMode">(row[10].value().value_or(std::string_view{}));
        if (row[11].value().has_value())
            item.set<"slaveId">(toInt(row[11].value().value_or(std::string_view{})));
        item.set<"timezone">(row[12].value().value_or(std::string_view{}));
        {
            DevicePacketDto heartbeat(ruvia::ModelOptions{.resource = c.arena()});
            if (row[13].value().has_value())
                heartbeat.set<"mode">(row[13].value().value_or(std::string_view{}));
            if (row[14].value().has_value())
                heartbeat.set<"content">(row[14].value().value_or(std::string_view{}));
            item.set<"heartbeat">(std::move(heartbeat));
        }
        if (!row[30].value().has_value()) {
            DevicePacketDto registration(ruvia::ModelOptions{.resource = c.arena()});
            if (row[15].value().has_value())
                registration.set<"mode">(row[15].value().value_or(std::string_view{}));
            if (row[16].value().has_value())
                registration.set<"content">(row[16].value().value_or(std::string_view{}));
            item.set<"registration">(std::move(registration));
        }
        item.set<"remark">(row[17].value().value_or(std::string_view{}));
        item.set<"createdBy">(row[18].value().value_or(std::string_view{}));
        item.set<"createdAt">(row[19].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[20].value().value_or(std::string_view{}));
        item.set<"linkName">(row[21].value().value_or(std::string_view{}));
        item.set<"linkMode">(row[22].value().value_or(std::string_view{}));
        item.set<"linkProtocol">(row[23].value().value_or(std::string_view{}));
        item.set<"protocolName">(row[24].value().value_or(std::string_view{}));
        item.set<"protocolType">(row[25].value().value_or(std::string_view{}));
        if (row[26].value().has_value()) {
            if (const auto value = parseDouble(row[26].value().value_or(std::string_view{})))
                item.set<"readInterval">(*value);
        }
        if (row[27].value().has_value())
            item.set<"storagePolicy">(row[27].value().value_or(std::string_view{}));
        const auto capabilities = DeviceAccessService::capabilities(
            actor, DeviceAccessService::rank(row[29].value().value_or(std::string_view{})), row[9].value().value_or(std::string_view{}) == "t");
        item.set<"elementCount">(toInt(row[28].value().value_or(std::string_view{})));
        item.set<"connected">(false);
        item.set<"connectionState">("disconnected");
        item.set<"elements">(ruvia::BoxedArray<DeviceElementDto>(
            ruvia::ModelOptions{.resource = c.arena()}));
        item.set<"canEdit">(capabilities.canEdit);
        item.set<"canDelete">(capabilities.canDelete);
        item.set<"canShare">(capabilities.canShare);
        item.set<"canCommand">(capabilities.canCommand);
        item.set<"accessLevel">(capabilities.accessLevel);
        if (row[30].value().has_value()) {
            item.set<"edgeNodeId">(row[30].value().value_or(std::string_view{}));
            item.set<"edgeNodeName">(row[31].value().value_or(std::string_view{}));
            item.set<"edgeNodeImei">(row[32].value().value_or(std::string_view{}));
        }
        if (row[33].value().has_value())
            item.set<"edgeTransport">(row[33].value().value_or(std::string_view{}));
        if (row[34].value().has_value())
            item.set<"edgeInterface">(row[34].value().value_or(std::string_view{}));
        if (row[35].value().has_value())
            item.set<"edgeMode">(row[35].value().value_or(std::string_view{}));
        if (row[36].value().has_value())
            item.set<"edgeIp">(row[36].value().value_or(std::string_view{}));
        if (row[37].value().has_value())
            item.set<"edgePort">(toInt(row[37].value().value_or(std::string_view{})));
        if (row[38].value().has_value())
            item.set<"serialBaudRate">(toInt(row[38].value().value_or(std::string_view{})));
        if (row[39].value().has_value())
            item.set<"serialDataBits">(toInt(row[39].value().value_or(std::string_view{})));
        if (row[40].value().has_value())
            item.set<"serialStopBits">(toInt(row[40].value().value_or(std::string_view{})));
        if (row[41].value().has_value())
            item.set<"serialParity">(row[41].value().value_or(std::string_view{}));
        if (row[42].value().has_value())
            item.set<"serialRs485">(row[42].value().value_or(std::string_view{}) == "t");
    }

    template <typename Item>
    static ruvia::Task<void>
    fillLatest(ruvia::Context& c, const std::map<std::string, Item*, std::less<>>& items) {
        if (items.empty())
            co_return;
        auto pipeline = c.redis().pipeline();
        enum class ReplyKind { runtime, latest, edgeDevice };
        struct ReplyBinding {
            ReplyKind kind;
            Item* item;
        };
        std::vector<ReplyBinding> bindings;
        bindings.reserve(items.size() * 3);
        for (const auto& [id, item] : items) {
            (void)id;
            if (!item->template get<"deviceCode">())
                continue;
            // The runtime hash also contains worker/session bookkeeping. The list only needs
            // these two fields, so HMGET avoids transferring and parsing the rest of the hash.
            pipeline.command("HMGET", service::telemetry::latest::runtimeKey(
                                         id),
                             "connection_id", "last_report_at_ms");
            bindings.push_back({ReplyKind::runtime, item});
            pipeline.hgetAll(service::telemetry::latest::latestKey(
                id));
            bindings.push_back({ReplyKind::latest, item});
            if (item->template get<"edgeNodeId">() &&
                item->template get<"edgeTransport">() &&
                item->template get<"edgeTransport">()->view() == "tcp") {
                pipeline.command(
                    "HMGET",
                    edgeDeviceStatusKey(
                        item->template get<"edgeNodeId">()->view(),
                        item->template get<"id">()->view()),
                    "state", "reason", "client_count", "last_activity_at_ms");
                bindings.push_back({ReplyKind::edgeDevice, item});
            }
        }
        const auto replies = co_await std::move(pipeline).exec();
        for (std::size_t index = 0; index < bindings.size() && index < replies.size(); ++index) {
            const auto& binding = bindings[index];
            if (binding.kind == ReplyKind::runtime) {
                applyRuntime(*binding.item, replies[index]);
            } else if (binding.kind == ReplyKind::latest) {
                applyLatestElements(c, *binding.item, replies[index]);
            } else {
                applyEdgeRuntime(c, *binding.item, replies[index]);
            }
        }
    }

    static ruvia::Task<void>
    fillCommandOperations(ruvia::Context& c,
                          const std::map<std::string, DeviceItemDto*, std::less<>>& items,
                          std::optional<std::string_view> onlyDevice) {
        if (items.empty())
            co_return;
        const auto makeWritable = [](ruvia::DbQuery& query,
                                      ruvia::DbQuery::Expr value) {
            const auto normalized = query.call("lower", {query.coalesce({value, query.value("")})});
            return query.caseWhen(
                {{query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("true")),
                  DeviceAccessService::boolean(query, true)},
                 {query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("t")),
                  DeviceAccessService::boolean(query, true)},
                 {query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("1")),
                  DeviceAccessService::boolean(query, true)},
                 {query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("yes")),
                  DeviceAccessService::boolean(query, true)},
                 {query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("y")),
                  DeviceAccessService::boolean(query, true)},
                  {query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("on")),
                   DeviceAccessService::boolean(query, true)}},
                 DeviceAccessService::boolean(query, false));
        };
        const auto emptyJson = [](ruvia::DbQuery& query) {
            return query.cast(query.value("{}"), ruvia::DbDataType::kJsonb);
        };
        const auto emptyArray = [](ruvia::DbQuery& query) {
            return query.cast(query.value("[]"), ruvia::DbDataType::kJsonb);
        };
        const auto addDeviceFilters = [&](ruvia::DbQuery& query) {
            auto predicate = andAll(
                query,
                query.unary(ruvia::DbUnaryOperator::kIsNull,
                            query.column("deleted_at", "d")),
                query.unary(ruvia::DbUnaryOperator::kIsNull,
                            query.column("deleted_at", "p")));
            predicate = query.binary(
                predicate, ruvia::DbBinaryOperator::kAnd,
                 query.binary(query.column("enabled", "p"), ruvia::DbBinaryOperator::kEqual,
                             DeviceAccessService::boolean(query, true)));
            if (onlyDevice)
                predicate = query.binary(
                    predicate, ruvia::DbBinaryOperator::kAnd,
                    query.binary(query.column("id", "d"), ruvia::DbBinaryOperator::kEqual,
                                 DeviceAccessService::uuid(query, *onlyDevice)));
            query.where(predicate);
        };

        ruvia::DbQuery modbus(c.pool());
        const auto modbusElement = modbus.column("element", "elements");
        const auto modbusRegisterType = DeviceAccessService::jsonText(
            modbus, modbusElement, "registerType");
        ruvia::DbQuery modbusLabelOne(c.pool());
        const auto modbusLabelOneElement = modbusLabelOne.column("element", "elements");
        modbusLabelOne
            .select(DeviceAccessService::jsonText(
                modbusLabelOne, modbusLabelOne.column("value", "mapping"), "label"))
            .fromFunction(
                modbusLabelOne.call(
                    "jsonb_array_elements",
                    {modbusLabelOne.coalesce(
                        {DeviceAccessService::jsonValue(
                             modbusLabelOne,
                             DeviceAccessService::jsonValue(modbusLabelOne, modbusLabelOneElement,
                                                            "dictConfig"),
                             "items"),
                         emptyArray(modbusLabelOne)})}),
                "mapping", {.columns = {{.name = "value"}}})
            .where(modbusLabelOne.binary(
                DeviceAccessService::jsonText(modbusLabelOne,
                                               modbusLabelOne.column("value", "mapping"), "key"),
                ruvia::DbBinaryOperator::kEqual, modbusLabelOne.value("1")))
            .limit(1);
        ruvia::DbQuery modbusLabelZero(c.pool());
        const auto modbusLabelZeroElement = modbusLabelZero.column("element", "elements");
        modbusLabelZero
            .select(DeviceAccessService::jsonText(
                modbusLabelZero, modbusLabelZero.column("value", "mapping"), "label"))
            .fromFunction(
                modbusLabelZero.call(
                    "jsonb_array_elements",
                    {modbusLabelZero.coalesce(
                        {DeviceAccessService::jsonValue(
                             modbusLabelZero,
                             DeviceAccessService::jsonValue(modbusLabelZero, modbusLabelZeroElement,
                                                            "dictConfig"),
                             "items"),
                         emptyArray(modbusLabelZero)})}),
                "mapping", {.columns = {{.name = "value"}}})
            .where(modbusLabelZero.binary(
                DeviceAccessService::jsonText(modbusLabelZero,
                                               modbusLabelZero.column("value", "mapping"), "key"),
                ruvia::DbBinaryOperator::kEqual, modbusLabelZero.value("0")))
            .limit(1);
        const auto modbusPreset = modbus.caseWhen(
            {{modbus.binary(modbusRegisterType, ruvia::DbBinaryOperator::kEqual,
                            modbus.value("COIL")),
              modbus.call(
                  "jsonb_build_array",
                  {modbus.call("jsonb_build_object",
                               {DeviceAccessService::textKey(modbus, "label"),
                                 modbus.coalesce({modbus.subquery(modbusLabelOne),
                                                  DeviceAccessService::text(modbus, "1")}),
                                DeviceAccessService::textKey(modbus, "value"),
                                 DeviceAccessService::text(modbus, "1")}),
                   modbus.call("jsonb_build_object",
                               {DeviceAccessService::textKey(modbus, "label"),
                                 modbus.coalesce({modbus.subquery(modbusLabelZero),
                                                  DeviceAccessService::text(modbus, "0")}),
                                DeviceAccessService::textKey(modbus, "value"),
                                 DeviceAccessService::text(modbus, "0")})})}},
            emptyArray(modbus));
        modbus
             .select({modbus.column("id", "d"), DeviceAccessService::text(modbus, "MODBUS_WRITE"),
                      DeviceAccessService::text(modbus, "写寄存器"), modbusElement,
                      DeviceAccessService::integer(modbus, 1),
                     modbus.column("element_position", "elements"),
                     modbus.column("preset", "presets"),
                     modbus.column("preset_position", "presets")})
            .from("device", "d")
            .join(ruvia::DbJoinType::kInner, "device_model",
                  andAll(modbus,
                         modbus.binary(modbus.column("device_id", "p"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      modbus.column("id", "d")),
                         modbus.binary(modbus.column("protocol", "p"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      modbus.value("Modbus"))),
                  "p")
            .joinFunction(
                ruvia::DbJoinType::kCross,
                modbus.call("jsonb_array_elements",
                            {modbus.coalesce({DeviceAccessService::jsonValue(
                                                  modbus, modbus.column("config", "p"),
                                                  "registers"),
                                              emptyArray(modbus)})}),
                {},
                "elements", {.lateral = true, .withOrdinality = true,
                              .columns = {{.name = "element"}, {.name = "element_position"}}})
            .joinFunction(
                ruvia::DbJoinType::kLeft,
                 modbus.call("jsonb_array_elements", {modbusPreset}),
                 DeviceAccessService::boolean(modbus, true),
                "presets", {.lateral = true, .withOrdinality = true,
                             .columns = {{.name = "preset"}, {.name = "preset_position"}}});
        addDeviceFilters(modbus);
        modbus.andWhere(makeWritable(
            modbus, DeviceAccessService::jsonText(modbus, modbusElement, "writable")));

        ruvia::DbQuery s7(c.pool());
        const auto s7Element = s7.column("element", "elements");
        const auto s7Preset = s7.caseWhen(
            {{s7.binary(DeviceAccessService::jsonText(s7, s7Element, "dataType"),
                        ruvia::DbBinaryOperator::kEqual, s7.value("BOOL")),
              s7.cast(s7.value("[{\"label\":\"1\",\"value\":\"1\"},"
                                  "{\"label\":\"0\",\"value\":\"0\"}]"),
                      ruvia::DbDataType::kJsonb)}},
            emptyArray(s7));
         s7.select({s7.column("id", "d"), DeviceAccessService::text(s7, "S7_WRITE"),
                    DeviceAccessService::text(s7, "写寄存器"), s7Element,
                    DeviceAccessService::integer(s7, 2), s7.column("element_position", "elements"),
                   s7.column("preset", "presets"), s7.column("preset_position", "presets")})
            .from("device", "d")
            .join(ruvia::DbJoinType::kInner, "device_model",
                  andAll(s7,
                         s7.binary(s7.column("device_id", "p"),
                                  ruvia::DbBinaryOperator::kEqual, s7.column("id", "d")),
                         s7.binary(s7.column("protocol", "p"),
                                  ruvia::DbBinaryOperator::kEqual, s7.value("S7"))),
                  "p")
            .joinFunction(
                ruvia::DbJoinType::kCross,
                s7.call("jsonb_array_elements",
                        {s7.coalesce({DeviceAccessService::jsonValue(
                                         s7, s7.column("config", "p"), "areas"),
                                     emptyArray(s7)})}),
                {},
                "elements", {.lateral = true, .withOrdinality = true,
                              .columns = {{.name = "element"}, {.name = "element_position"}}})
             .joinFunction(ruvia::DbJoinType::kLeft,
                           s7.call("jsonb_array_elements", {s7Preset}),
                           DeviceAccessService::boolean(s7, true), "presets",
                          {.lateral = true, .withOrdinality = true,
                           .columns = {{.name = "preset"}, {.name = "preset_position"}}});
        addDeviceFilters(s7);
        s7.andWhere(makeWritable(s7, DeviceAccessService::jsonText(s7, s7Element, "writable")));

        ruvia::DbQuery sl651(c.pool());
        const auto function = sl651.column("function", "functions");
        const auto sl651Element = sl651.column("element", "elements");
        sl651.select({sl651.column("id", "d"),
                      DeviceAccessService::jsonText(sl651, function, "funcCode"),
                      sl651.coalesce({sl651.nullIf(DeviceAccessService::jsonText(
                                                          sl651, function, "name"),
                                                      sl651.value("")),
                                      DeviceAccessService::jsonText(sl651, function, "funcCode")}),
                      sl651Element,
                      sl651.binary(sl651.column("function_position", "functions"),
                                    ruvia::DbBinaryOperator::kAdd,
                                    DeviceAccessService::integer(sl651, 2)),
                      sl651.column("element_position", "elements"),
                      sl651.column("preset", "presets"),
                      sl651.column("preset_position", "presets")})
            .from("device", "d")
            .join(ruvia::DbJoinType::kInner, "device_model",
                  andAll(sl651,
                         sl651.binary(sl651.column("device_id", "p"),
                                     ruvia::DbBinaryOperator::kEqual, sl651.column("id", "d")),
                         sl651.binary(sl651.column("protocol", "p"),
                                     ruvia::DbBinaryOperator::kEqual, sl651.value("SL651"))),
                  "p")
            .joinFunction(
                ruvia::DbJoinType::kCross,
                sl651.call("jsonb_array_elements",
                           {sl651.coalesce({DeviceAccessService::jsonValue(
                                                sl651, sl651.column("config", "p"), "funcs"),
                                            emptyArray(sl651)})}),
                {},
                "functions", {.lateral = true, .withOrdinality = true,
                               .columns = {{.name = "function"}, {.name = "function_position"}}})
            .joinFunction(
                ruvia::DbJoinType::kCross,
                sl651.call("jsonb_array_elements",
                           {sl651.coalesce({DeviceAccessService::jsonValue(
                                                sl651, function, "elements"),
                                            emptyArray(sl651)})}),
                {},
                "elements", {.lateral = true, .withOrdinality = true,
                               .columns = {{.name = "element"}, {.name = "element_position"}}})
            .joinFunction(
                ruvia::DbJoinType::kLeft,
                sl651.call("jsonb_array_elements",
                           {sl651.coalesce({DeviceAccessService::jsonValue(
                                                sl651, sl651Element, "options"),
                                            emptyArray(sl651)})}),
                 DeviceAccessService::boolean(sl651, true), "presets",
                {.lateral = true, .withOrdinality = true,
                 .columns = {{.name = "preset"}, {.name = "preset_position"}}});
        addDeviceFilters(sl651);
        sl651.andWhere(sl651.binary(DeviceAccessService::jsonText(sl651, function, "dir"),
                                     ruvia::DbBinaryOperator::kEqual, sl651.value("DOWN")));
        sl651.andWhere(sl651.binary(
            sl651.coalesce({DeviceAccessService::jsonText(sl651, sl651Element, "encode"),
                            sl651.value("")}),
            ruvia::DbBinaryOperator::kNotEqual, sl651.value("JPEG")));

        modbus.combine(ruvia::DbSetOperation::kUnionAll, s7)
            .combine(ruvia::DbSetOperation::kUnionAll, sl651);
        ruvia::DbQuery query(c.pool());
        query.with("command_element", modbus,
                   {.columns = {"device_id", "operation_key", "operation_name", "element",
                                "operation_position", "element_position", "preset",
                                "preset_position"}});
        const auto element = query.column("element", "command_element");
        const auto preset = query.column("preset", "command_element");
        query.select({DeviceAccessService::text(query,
                                                query.column("device_id", "command_element")),
                      query.column("operation_key", "command_element"),
                      query.column("operation_name", "command_element"),
                      DeviceAccessService::jsonText(query, element, "id"),
                      DeviceAccessService::jsonText(query, element, "name"),
                      query.coalesce({DeviceAccessService::jsonText(query, element, "unit"),
                                      query.value("")}),
                      query.coalesce({DeviceAccessService::jsonText(query, element,
                                                                     "registerType"),
                                      query.value("")}),
                      query.coalesce({DeviceAccessService::jsonText(query, element, "dataType"),
                                      query.value("")}),
                      DeviceAccessService::jsonText(query, element, "size"),
                      query.coalesce({DeviceAccessService::jsonText(query, element, "encode"),
                                      query.value("")}),
                      DeviceAccessService::jsonText(query, element, "length"),
                      DeviceAccessService::jsonText(query, element, "digits"),
                      DeviceAccessService::jsonText(query, preset, "label"),
                      DeviceAccessService::jsonText(query, preset, "value"),
                      query.column("operation_position", "command_element"),
                      query.column("element_position", "command_element"),
                      query.column("preset_position", "command_element")})
            .from("command_element")
            .orderBy(query.column("device_id", "command_element"))
            .addOrderBy(query.column("operation_position", "command_element"))
            .addOrderBy(query.column("operation_key", "command_element"))
            .addOrderBy(query.column("element_position", "command_element"))
            .addOrderBy(query.column("preset_position", "command_element"),
                        ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kLast);
        const auto rows = co_await c.db().query(query);

        struct OptionData {
            std::string label;
            std::string value;
        };
        struct ElementData {
            std::string id;
            std::string name;
            std::string unit;
            std::string registerType;
            std::string dataType;
            std::optional<std::int64_t> size;
            std::string encode;
            std::optional<std::int64_t> length;
            std::optional<std::int64_t> digits;
            std::vector<OptionData> options;
        };
        struct OperationData {
            std::string key;
            std::string name;
            std::vector<ElementData> elements;
        };
        std::map<std::string, std::vector<OperationData>, std::less<>> configured;
        for (const auto& row : rows) {
            const auto deviceId = std::string(row[0].value().value_or(std::string_view{}));
            if (!items.contains(deviceId))
                continue;
            auto& operations = configured[deviceId];
            const auto operationKey = std::string(row[1].value().value_or(std::string_view{}));
            auto operation =
                std::find_if(operations.begin(), operations.end(),
                             [&](const auto& value) { return value.key == operationKey; });
            if (operation == operations.end()) {
                operations.push_back({operationKey, std::string(row[2].value().value_or(std::string_view{})), {}});
                operation = std::prev(operations.end());
            }
            const auto elementId = std::string(row[3].value().value_or(std::string_view{}));
            auto element = std::find_if(operation->elements.begin(), operation->elements.end(),
                                        [&](const auto& value) { return value.id == elementId; });
            if (element == operation->elements.end()) {
                ElementData data;
                data.id = elementId;
                data.name = std::string(row[4].value().value_or(std::string_view{}));
                data.unit = std::string(row[5].value().value_or(std::string_view{}));
                data.registerType = std::string(row[6].value().value_or(std::string_view{}));
                data.dataType = std::string(row[7].value().value_or(std::string_view{}));
                if (row[8].value().has_value())
                    data.size = toInt(row[8].value().value_or(std::string_view{}));
                data.encode = std::string(row[9].value().value_or(std::string_view{}));
                if (row[10].value().has_value())
                    data.length = toInt(row[10].value().value_or(std::string_view{}));
                if (row[11].value().has_value())
                    data.digits = toInt(row[11].value().value_or(std::string_view{}));
                operation->elements.push_back(std::move(data));
                element = std::prev(operation->elements.end());
            }
            if (row[12].value().has_value() && row[13].value().has_value())
                element->options.push_back(
                    {std::string(row[12].value().value_or(std::string_view{})), std::string(row[13].value().value_or(std::string_view{}))});
        }

        for (auto& [deviceId, operations] : configured) {
            const auto item = items.find(deviceId);
            if (item == items.end())
                continue;
            ruvia::BoxedArray<DeviceCommandOperationDto> operationDtos(
                ruvia::ModelOptions{.resource = c.arena()});
            for (const auto& operation : operations) {
                auto& operationDto = operationDtos.emplace(ruvia::ModelOptions{.resource = c.arena()});
                operationDto.set<"name">(operation.name);
                ruvia::BoxedArray<DeviceCommandOperationElementDto> elementDtos(
                    ruvia::ModelOptions{.resource = c.arena()});
                for (const auto& element : operation.elements) {
                    auto& elementDto = elementDtos.emplace(ruvia::ModelOptions{.resource = c.arena()});
                    elementDto.set<"elementId">(element.id).set<"name">(element.name).set<"value">("");
                    if (!element.unit.empty())
                        elementDto.set<"unit">(element.unit);
                    if (!element.registerType.empty())
                        elementDto.set<"registerType">(element.registerType);
                    if (!element.dataType.empty())
                        elementDto.set<"dataType">(element.dataType);
                    if (element.size)
                        elementDto.set<"size">(*element.size);
                    if (!element.encode.empty())
                        elementDto.set<"encode">(element.encode);
                    if (element.length)
                        elementDto.set<"length">(*element.length);
                    if (element.digits)
                        elementDto.set<"digits">(*element.digits);
                    if (!element.options.empty()) {
                        ruvia::BoxedArray<DeviceCommandOptionDto> optionDtos(
                            ruvia::ModelOptions{.resource = c.arena()});
                        for (const auto& option : element.options)
                            optionDtos.emplace(ruvia::ModelOptions{.resource = c.arena()}).set<"label">(option.label).set<"value">(option.value);
                        elementDto.set<"options">(std::move(optionDtos));
                    }
                }
                operationDto.set<"elements">(std::move(elementDtos));
            }
            item->second->set<"commandOperations">(std::move(operationDtos));
        }
    }

    static std::string redisHashField(const ruvia::RedisValue& value, std::string_view field) {
        if (value.kind() != ruvia::RedisValue::Kind::kArray)
            return {};
        const auto& entries = value.array();
        for (std::size_t index = 0; index + 1 < entries.size(); index += 2) {
            if (entries[index].kind() != ruvia::RedisValue::Kind::kString ||
                entries[index + 1].kind() != ruvia::RedisValue::Kind::kString)
                continue;
            if (entries[index].string() == field)
                return std::string(entries[index + 1].string());
        }
        return {};
    }

    static std::string redisArrayField(const ruvia::RedisValue& value, std::size_t index) {
        if (value.kind() != ruvia::RedisValue::Kind::kArray)
            return {};
        const auto entries = value.array();
        if (index >= entries.size() || entries[index].kind() != ruvia::RedisValue::Kind::kString)
            return {};
        return std::string(entries[index].string());
    }

    static std::optional<ruvia::JsonValue> jsonField(const ruvia::JsonValue& object,
                                                     std::string_view field) {
        if (!object.isObject())
            return std::nullopt;
        std::optional<ruvia::JsonValue> result;
        const auto valid = ruvia::detail::visitJsonObjectFields(
            ruvia::detail::ResolvedPmrResourceTag{}, object.view(),
            std::pmr::get_default_resource(),
            [&](std::string_view key, std::string_view value) {
                if (key == field)
                    result = ruvia::JsonValue::parse(value);
                return true;
            });
        return valid ? result : std::nullopt;
    }

    static std::optional<std::string> jsonString(const ruvia::JsonValue& object,
                                                 std::string_view field) {
        const auto value = object.get<ruvia::String>(field);
        if (!value)
            return std::nullopt;
        return std::string(value->view());
    }

    static std::int64_t jsonInt(const ruvia::JsonValue& object, std::string_view field,
                                std::int64_t fallback) {
        if (const auto value = object.get<ruvia::Int64>(field))
            return static_cast<std::int64_t>(*value);
        const auto raw = jsonField(object, field);
        if (!raw)
            return fallback;
        return service::common::parseInt64(std::optional<std::string_view>{raw->view()})
            .value_or(fallback);
    }

    static double jsonDouble(const ruvia::JsonValue& object, std::string_view field,
                             double fallback) {
        const auto raw = jsonField(object, field);
        if (!raw)
            return fallback;
        return toDouble(raw->view(), fallback);
    }

    template <typename Item>
    static void applyRuntime(Item& item, const ruvia::RedisValue& reply) {
        const auto reportTime = redisArrayField(reply, 1);
        if (!reportTime.empty()) {
            const auto milliseconds = toInt(reportTime);
            if (milliseconds > 0)
                item.template set<"reportTime">(
                    service::common::utcTimestampFromMilliseconds(milliseconds));
        }
        const bool connected = !redisArrayField(reply, 0).empty();
        item.template set<"connected">(connected);
        item.template set<"connectionState">(
            connected ? "connected" : "disconnected");
    }

    template <typename Item>
    static void applyEdgeRuntime(ruvia::Context& c, Item& item,
                                 const ruvia::RedisValue& reply) {
        const auto state = redisArrayField(reply, 0);
        if (state.empty())
            return;
        const bool connected = state == "connected" || state == "online";
        item.template set<"connected">(connected);
        item.template set<"connectionState">(
            connected ? "connected" : "disconnected");
        EdgeStatusDto status(ruvia::ModelOptions{.resource = c.arena()});
        status.set<"state">(state);
        const auto reason = redisArrayField(reply, 1);
        if (!reason.empty())
            status.set<"reason">(reason);
        const auto clientCount = redisArrayField(reply, 2);
        if (!clientCount.empty())
            status.set<"clientCount">(toInt(clientCount));
        const auto lastActivity = redisArrayField(reply, 3);
        if (!lastActivity.empty()) {
            const auto milliseconds = toInt(lastActivity);
            if (milliseconds > 0)
                status.set<"lastActivityAt">(
                    service::common::utcTimestampFromMilliseconds(milliseconds));
        }
        item.template set<"edgeStatus">(std::move(status));
    }

    struct LatestElement final {
        std::int64_t sort = 0;
        std::int64_t observedAt = 0;
        double scale = 1.0;
        std::int64_t decimals = -1;
        std::string id;
        std::string name;
        std::string value{"-"};
        std::string unit;
        std::string dataType;
        std::string group;
        std::string encode;
    };

    template <typename Item>
    static void applyLatestElements(ruvia::Context& c, Item& item,
                                    const ruvia::RedisValue& reply) {
        if (reply.kind() != ruvia::RedisValue::Kind::kArray)
            return;
        bool hasElementIds = false;
        std::set<std::string, std::less<>> elementIds;
        std::map<std::string, LatestElement, std::less<>> latest;

        const auto& entries = reply.array();
        for (std::size_t index = 0; index + 1 < entries.size(); index += 2) {
            if (entries[index].kind() != ruvia::RedisValue::Kind::kString ||
                entries[index + 1].kind() != ruvia::RedisValue::Kind::kString)
                continue;
            const auto field = entries[index].string();
            if (field == "_element_ids") {
                hasElementIds = true;
                const auto parsed = ruvia::JsonValue::parse(entries[index + 1].string());
                if (parsed && parsed->isObject()) {
                    (void)ruvia::detail::visitJsonObjectFields(
                        ruvia::detail::ResolvedPmrResourceTag{}, parsed->view(),
                        std::pmr::get_default_resource(),
                        [&](std::string_view key, std::string_view) {
                            if (!key.empty())
                                elementIds.emplace(key);
                            return true;
                        });
                }
                continue;
            }
            if (field.empty() || field.front() == '_')
                continue;
            const auto parsed = ruvia::JsonValue::parse(entries[index + 1].string());
            if (!parsed || !parsed->isObject())
                continue;
            LatestElement element;
            element.id = jsonString(*parsed, "id").value_or(std::string(field));
            element.name = jsonString(*parsed, "name").value_or(element.id);
            element.dataType = jsonString(*parsed, "dataType").value_or("");
            element.value = service::telemetry::latest::canonicalPointText(
                jsonString(*parsed, "value").value_or("-"), element.dataType);
            element.unit = jsonString(*parsed, "unit").value_or("");
            element.group = jsonString(*parsed, "group").value_or("");
            element.encode = jsonString(*parsed, "encode").value_or("");
            element.scale = jsonDouble(*parsed, "scale", 1.0);
            element.decimals = jsonInt(*parsed, "decimals", -1);
            element.sort = jsonInt(*parsed, "sort", 0);
            element.observedAt = jsonInt(*parsed, "observedAt", 0);
            const auto elementId = element.id;
            latest.insert_or_assign(elementId, std::move(element));
        }

        std::vector<LatestElement> elements;
        elements.reserve(latest.size());
        for (auto& [id, element] : latest) {
            if (hasElementIds && !elementIds.contains(id))
                continue;
            elements.push_back(std::move(element));
        }
        std::sort(elements.begin(), elements.end(), [](const auto& left, const auto& right) {
            if (left.sort != right.sort)
                return left.sort < right.sort;
            return left.id < right.id;
        });
        ruvia::BoxedArray<DeviceElementDto> dtos(
            ruvia::ModelOptions{.resource = c.arena()});
        std::int64_t reportTime = 0;
        for (const auto& element : elements) {
            auto& dto = dtos.emplace(ruvia::ModelOptions{.resource = c.arena()});
            dto.set<"id">(element.id)
                .set<"name">(element.name)
                .set<"value">(element.value)
                .set<"unit">(element.unit)
                .set<"scale">(element.scale)
                .set<"decimals">(element.decimals);
            if (!element.group.empty())
                dto.set<"group">(element.group);
            if (!element.encode.empty())
                dto.set<"encode">(element.encode);
            reportTime = std::max(reportTime, element.observedAt);
        }
        item.template set<"elements">(std::move(dtos));
        if (reportTime > 0 && !item.template get<"reportTime">())
            item.template set<"reportTime">(
                service::common::utcTimestampFromMilliseconds(reportTime));
    }

    static std::string str(const std::optional<ruvia::String>& value) {
        return value ? std::string(value->view()) : std::string{};
    }

    static void appendJsonString(std::string& out, std::string_view value) {
        out.push_back('"');
        for (const char ch : value) {
            switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned char>(ch));
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
            }
        }
        out.push_back('"');
    }

    // 未提供返回空串（SQL 里以 NULLIF 回退到默认或当前值）；提供则序列化为 {"mode":..,"content":..}
    static std::string packetJson(const std::optional<DevicePacketBody>& packet) {
        if (!packet)
            return "";
        std::string out = "{\"mode\":";
        appendJsonString(out, packet->get<"mode">() ? packet->get<"mode">()->view() : std::string_view("OFF"));
        if (packet->get<"content">()) {
            out += ",\"content\":";
            appendJsonString(out, packet->get<"content">()->view());
        }
        out.push_back('}');
        return out;
    }

    // 心跳/注册包内容校验（对应旧 SQL shape-check 的第 8、9 条，语义一致）
    static void validatePacket(const std::optional<DevicePacketBody>& packet) {
        if (!packet)
            return;
        const std::string_view mode =
            packet->get<"mode">() ? packet->get<"mode">()->view() : std::string_view("OFF");
        if (mode != "OFF" && mode != "HEX" && mode != "ASCII")
            service::common::fail(18002, "设备参数无效", 400);
        if (mode == "OFF")
            return;
        const std::string_view content =
            packet->get<"content">() ? packet->get<"content">()->view() : std::string_view{};
        if (content.empty())
            service::common::fail(18002, "设备参数无效", 400);
        if (mode == "ASCII" && content.size() > 256)
            service::common::fail(18002, "注册包或心跳包不能超过 256 字节", 400);
        if (mode == "HEX") {
            std::string stripped;
            for (const char ch : content)
                if (!std::isspace(static_cast<unsigned char>(ch)))
                    stripped.push_back(ch);
            if (stripped.empty() || stripped.size() % 2 != 0)
                service::common::fail(18002, "设备参数无效", 400);
            if (stripped.size() / 2 > 256)
                service::common::fail(18002, "注册包或心跳包不能超过 256 字节", 400);
            for (const char ch : stripped)
                if (!std::isxdigit(static_cast<unsigned char>(ch)))
                    service::common::fail(18002, "设备参数无效", 400);
        }
    }

    // 扁平字段（必填/长度/枚举/范围/UUID/timezone）由声明式校验器保证；
    // 此处只做跨字段、依赖 DB 与协议相关的校验（保留 18002/18003 域码）。
    ruvia::Task<void> validate(ruvia::Context& c, const SaveDeviceBody& body, bool required) {
        validatePacket(body.get<"heartbeat">());
        validatePacket(body.get<"registration">());
        const auto linkId = str(body.get<"linkId">());
        const auto configId = str(body.get<"protocolConfigId">());
        const auto& modelRevision = body.get<"protocolRevision">();
        if ((required || !configId.empty()) && !modelRevision)
            service::common::fail(18003, "请选择设备类型版本", 400);
        if (modelRevision && (configId.empty() || static_cast<std::int64_t>(*modelRevision) < 1))
            service::common::fail(18003, "设备类型及版本必须一起指定", 400);
        if (modelRevision) {
            ruvia::DbQuery revisionQuery(c.pool());
            revisionQuery
                .select(DeviceAccessService::integer(revisionQuery, 1))
                .from("protocol_revision")
                .where(andAll(revisionQuery,
                              revisionQuery.binary(revisionQuery.column("id"),
                                                   ruvia::DbBinaryOperator::kEqual,
                                                   DeviceAccessService::uuid(revisionQuery,
                                                                              configId)),
                              revisionQuery.binary(
                                  revisionQuery.column("revision"),
                                  ruvia::DbBinaryOperator::kEqual,
                                  revisionQuery.value(static_cast<std::int64_t>(*modelRevision)))));
            const auto revision = co_await c.db().query(revisionQuery);
            if (revision.empty())
                service::common::fail(18003, "设备类型版本不存在", 400);
        }
        if (required && linkId.empty()) service::common::fail(18003, "请选择通道", 400);
        if (required && configId.empty())
            service::common::fail(18003, "请选择设备类型", 400);

        const auto& code = body.get<"deviceCode">();
        if (code) {
            if (code->view().empty() || code->view().size() > 100)
                service::common::fail(18002, "设备编码长度必须在 1 - 100 之间", 400);
            for (const auto character : code->view())
                if (!std::isalnum(static_cast<unsigned char>(character)))
                    service::common::fail(18002, "设备编码只能包含字母和数字", 400);
        }
        if (body.get<"groupId">() && !body.get<"groupId">()->view().empty()) {
            ruvia::DbQuery groupQuery(c.pool());
            groupQuery
                .select(DeviceAccessService::integer(groupQuery, 1))
                .from("device_group")
                .where(andAll(groupQuery,
                              groupQuery.binary(groupQuery.column("id"),
                                               ruvia::DbBinaryOperator::kEqual,
                                               DeviceAccessService::uuid(
                                                   groupQuery, body.get<"groupId">()->view())),
                              groupQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                               groupQuery.column("deleted_at"))));
            const auto group = co_await c.db().query(groupQuery);
            if (group.empty())
                service::common::fail(18003, "设备分组不存在", 400);
        }
        if (configId.empty() || linkId.empty())
            co_return;

        ruvia::DbQuery relationQuery(c.pool());
        relationQuery
            .select(relationQuery.column("protocol", "l"))
            .from("link", "l")
            .join(ruvia::DbJoinType::kInner, "protocol_config",
                  relationQuery.binary(relationQuery.column("protocol", "p"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       relationQuery.column("protocol", "l")),
                  "p")
            .where(andAll(relationQuery,
                          relationQuery.binary(relationQuery.column("id", "l"),
                                               ruvia::DbBinaryOperator::kEqual,
                                               DeviceAccessService::uuid(relationQuery, linkId)),
                          relationQuery.binary(relationQuery.column("id", "p"),
                                               ruvia::DbBinaryOperator::kEqual,
                                               DeviceAccessService::uuid(relationQuery, configId)),
                          relationQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                              relationQuery.column("deleted_at", "l")),
                          relationQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                              relationQuery.column("deleted_at", "p"))));
        const auto relation = co_await c.db().query(relationQuery);
        if (relation.empty()) service::common::fail(18003, "通道或设备类型不存在，或协议不一致", 400);
        const std::string configProtocol(relation.front()[0].value().value_or(""));
        if (configProtocol == "SL651" &&
            (packetEnabled(body.get<"heartbeat">()) || packetEnabled(body.get<"registration">())))
            service::common::fail(18002, "SL651 设备不支持配置注册包或心跳包", 400);
        if (configProtocol == "SL651" && code) {
            if (code->view().size() != 10)
                service::common::fail(18002, "SL651 遥测站地址必须是 10 位数字，不足时左侧补零", 400);
            for (const auto character : code->view())
                if (!std::isdigit(static_cast<unsigned char>(character)))
                    service::common::fail(18002, "SL651 设备编码必须是数字遥测站地址", 400);
        }
    }

    static bool packetEnabled(const std::optional<DevicePacketBody>& packet) {
        return packet && packet->get<"mode">() && packet->get<"mode">()->view() != "OFF";
    }

    ruvia::Task<void> validateRuntimeIdentity(ruvia::Context& c, const SaveDeviceBody& body,
                                              std::optional<std::string> excludedId) {
        const std::string excluded = excludedId.value_or(std::string(kNilUuid));
        const std::string inLinkId = str(body.get<"linkId">());
        const std::string inTargetId = str(body.get<"targetId">());
        const std::string inConfigId = str(body.get<"protocolConfigId">());
        const std::string inSlaveId =
            body.get<"slaveId">() ? std::to_string(static_cast<std::int64_t>(*body.get<"slaveId">())) : "";
        const std::string inRegistration = packetJson(body.get<"registration">());
        const std::string inHeartbeat = packetJson(body.get<"heartbeat">());
        ruvia::DbQuery currentDevice(c.pool());
        currentDevice.select({currentDevice.column("link_id"),
                              currentDevice.column("protocol_config_id"),
                              currentDevice.column("protocol_params")})
            .from("device")
            .where(andAll(currentDevice,
                          currentDevice.binary(currentDevice.column("id"),
                                               ruvia::DbBinaryOperator::kEqual,
                                               DeviceAccessService::uuid(currentDevice, excluded)),
                          currentDevice.unary(ruvia::DbUnaryOperator::kIsNull,
                                              currentDevice.column("deleted_at"))));
        ruvia::DbQuery candidateQuery(c.pool());
        candidateQuery.with("current_device", currentDevice);
        const auto currentParams = candidateQuery.column("protocol_params", "current");
        const auto candidateLink = candidateQuery.coalesce(
            {DeviceAccessService::nullableUuid(candidateQuery, inLinkId),
             candidateQuery.column("link_id", "current")});
        const auto candidateTarget = candidateQuery.coalesce(
            {candidateQuery.nullIf(DeviceAccessService::text(candidateQuery,
                                                              candidateQuery.value(inTargetId)),
                                    candidateQuery.value("")),
             DeviceAccessService::jsonText(candidateQuery, currentParams, "target_id"),
             candidateQuery.value("")});
        const auto candidateConfig = candidateQuery.coalesce(
            {DeviceAccessService::nullableUuid(candidateQuery, inConfigId),
             candidateQuery.column("protocol_config_id", "current")});
        const auto inputSlave = candidateQuery.nullIf(
            DeviceAccessService::text(candidateQuery, candidateQuery.value(inSlaveId)),
            candidateQuery.value(""));
        const auto storedSlave = DeviceAccessService::jsonText(candidateQuery, currentParams,
                                                                "slave_id");
        const auto parseSlave = [&](ruvia::DbQuery::Expr value) {
            return candidateQuery.caseWhen(
                {{candidateQuery.binary(candidateQuery.coalesce({value, candidateQuery.value("")}),
                                        ruvia::DbBinaryOperator::kRegex,
                                        candidateQuery.value("^-?[0-9]{1,18}$")),
                  candidateQuery.cast(value, ruvia::DbDataType::kInteger)}});
        };
        const auto candidateSlave = candidateQuery.coalesce(
            {parseSlave(inputSlave), parseSlave(storedSlave),
             DeviceAccessService::integer(candidateQuery, 1)});
        const auto defaultPacket = candidateQuery.cast(
            candidateQuery.value(R"({"mode":"OFF"})"), ruvia::DbDataType::kJsonb);
        const auto candidateRegistration = candidateQuery.coalesce(
            {candidateQuery.cast(candidateQuery.nullIf(
                                   DeviceAccessService::text(candidateQuery,
                                                             candidateQuery.value(inRegistration)),
                                   candidateQuery.value("")),
                               ruvia::DbDataType::kJsonb),
             DeviceAccessService::jsonValue(candidateQuery, currentParams, "registration"),
             defaultPacket});
        const auto candidateHeartbeat = candidateQuery.coalesce(
            {candidateQuery.cast(candidateQuery.nullIf(
                                   DeviceAccessService::text(candidateQuery,
                                                             candidateQuery.value(inHeartbeat)),
                                   candidateQuery.value("")),
                               ruvia::DbDataType::kJsonb),
             DeviceAccessService::jsonValue(candidateQuery, currentParams, "heartbeat"),
             defaultPacket});
        const auto registrationModeExpr = candidateQuery.call(
            "upper", {candidateQuery.coalesce({DeviceAccessService::jsonText(
                                                   candidateQuery, candidateRegistration, "mode"),
                                               candidateQuery.value("OFF")})});
        const auto registrationContent = candidateQuery.coalesce(
            {DeviceAccessService::jsonText(candidateQuery, candidateRegistration, "content"),
             candidateQuery.value("")});
        const auto registrationKeyExpr = candidateQuery.caseWhen(
            {{candidateQuery.binary(registrationModeExpr, ruvia::DbBinaryOperator::kEqual,
                                    candidateQuery.value("OFF")),
              candidateQuery.value("OFF:")},
              {candidateQuery.binary(registrationModeExpr, ruvia::DbBinaryOperator::kEqual,
                                    candidateQuery.value("HEX")),
              candidateQuery.binary(
                  DeviceAccessService::text(candidateQuery, "HEX:"),
                  ruvia::DbBinaryOperator::kConcat,
                  candidateQuery.call(
                      "upper", {candidateQuery.call(
                                     "regexp_replace",
                                     {registrationContent, candidateQuery.value("\\s"),
                                      candidateQuery.value(""), candidateQuery.value("g")})}))}},
             candidateQuery.binary(DeviceAccessService::text(candidateQuery, "ASCII:"),
                                  ruvia::DbBinaryOperator::kConcat, registrationContent));
        const auto heartbeatModeExpr = candidateQuery.call(
            "upper", {candidateQuery.coalesce({DeviceAccessService::jsonText(
                                                   candidateQuery, candidateHeartbeat, "mode"),
                                               candidateQuery.value("OFF")})});
        candidateQuery
            .select({candidateLink,
                     DeviceAccessService::jsonText(candidateQuery,
                                                    candidateQuery.column("endpoint", "link"),
                                                    "mode"),
                     candidateQuery.column("protocol", "protocol"), candidateTarget,
                     candidateSlave, registrationModeExpr, registrationKeyExpr, heartbeatModeExpr})
            .fromFunction(candidateQuery.call("generate_series",
                                              {DeviceAccessService::integer(candidateQuery, 1),
                                               DeviceAccessService::integer(candidateQuery, 1)}),
                          "base")
            .join(ruvia::DbJoinType::kLeft, "current_device",
                  DeviceAccessService::boolean(candidateQuery, true),
                  "current")
            .join(ruvia::DbJoinType::kInner, "link",
                  andAll(candidateQuery,
                         candidateQuery.binary(candidateQuery.column("id", "link"),
                                              ruvia::DbBinaryOperator::kEqual, candidateLink),
                         candidateQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                              candidateQuery.column("deleted_at", "link"))),
                  "link")
            .join(ruvia::DbJoinType::kInner, "protocol_config",
                  andAll(candidateQuery,
                         candidateQuery.binary(candidateQuery.column("id", "protocol"),
                                              ruvia::DbBinaryOperator::kEqual, candidateConfig),
                         candidateQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                              candidateQuery.column("deleted_at", "protocol"))),
                  "protocol")
            .limit(1);
        const auto candidate = co_await c.db().query(candidateQuery);
        if (candidate.empty())
            co_return;

        const auto& current = candidate.front();
        const std::string linkId(current[0].value().value_or(std::string_view{}));
        const std::string linkMode(current[1].value().value_or(std::string_view{}));
        const std::string protocol(current[2].value().value_or(std::string_view{}));
        const std::string targetId(current[3].value().value_or(std::string_view{}));
        const auto slaveId = toInt(current[4].value().value_or(std::string_view{}));
        const std::string registrationMode(current[5].value().value_or(std::string_view{}));
        const std::string registrationKey(current[6].value().value_or(std::string_view{}));
        const std::string heartbeatMode(current[7].value().value_or(std::string_view{}));
        if (linkMode != "TCP Server" || protocol == "SL651") {
            if (registrationMode != "OFF" || heartbeatMode != "OFF")
                service::common::fail(18002,
                                      protocol == "SL651"
                                          ? "SL651 设备不支持配置注册包或心跳包"
                                          : "仅 TCP Server 设备支持配置注册包或心跳包",
                                      400);
        }
        if (protocol != "Modbus" && protocol != "S7")
            co_return;

        ruvia::DbQuery siblingsQuery(c.pool());
        const auto siblingParams = siblingsQuery.column("protocol_params", "device");
        const auto siblingSlaveText = DeviceAccessService::jsonText(
            siblingsQuery, siblingParams, "slave_id");
        const auto siblingSlave = siblingsQuery.coalesce(
            {siblingsQuery.caseWhen(
                 {{siblingsQuery.binary(siblingsQuery.coalesce({siblingSlaveText,
                                                                  siblingsQuery.value("")}),
                                         ruvia::DbBinaryOperator::kRegex,
                                         siblingsQuery.value("^-?[0-9]{1,18}$")),
                   siblingsQuery.cast(siblingSlaveText, ruvia::DbDataType::kInteger)}}),
              DeviceAccessService::integer(siblingsQuery, 1)});
        const auto siblingRegistration = DeviceAccessService::jsonValue(
            siblingsQuery, siblingParams, "registration");
        const auto siblingMode = siblingsQuery.call(
            "upper", {siblingsQuery.coalesce({DeviceAccessService::jsonText(
                                                   siblingsQuery, siblingRegistration, "mode"),
                                               siblingsQuery.value("OFF")})});
        const auto siblingContent = siblingsQuery.coalesce(
            {DeviceAccessService::jsonText(siblingsQuery, siblingRegistration, "content"),
             siblingsQuery.value("")});
        const auto siblingKey = siblingsQuery.caseWhen(
            {{siblingsQuery.binary(siblingMode, ruvia::DbBinaryOperator::kEqual,
                                   siblingsQuery.value("OFF")),
              siblingsQuery.value("OFF:")},
             {siblingsQuery.binary(siblingMode, ruvia::DbBinaryOperator::kEqual,
                                   siblingsQuery.value("HEX")),
              siblingsQuery.binary(
                  DeviceAccessService::text(siblingsQuery, "HEX:"),
                  ruvia::DbBinaryOperator::kConcat,
                  siblingsQuery.call(
                      "upper", {siblingsQuery.call(
                                     "regexp_replace",
                                     {siblingContent, siblingsQuery.value("\\s"),
                                      siblingsQuery.value(""), siblingsQuery.value("g")})}))}},
            siblingsQuery.binary(DeviceAccessService::text(siblingsQuery, "ASCII:"),
                                ruvia::DbBinaryOperator::kConcat, siblingContent));
        siblingsQuery
            .select({siblingsQuery.column("name", "device"), siblingSlave,
                     siblingsQuery.coalesce({DeviceAccessService::jsonText(
                                                 siblingsQuery, siblingParams, "target_id"),
                                             siblingsQuery.value("")}),
                     siblingMode, siblingKey})
            .from("device", "device")
            .join(ruvia::DbJoinType::kInner, "protocol_config",
                  andAll(siblingsQuery,
                         siblingsQuery.binary(siblingsQuery.column("id", "config"),
                                              ruvia::DbBinaryOperator::kEqual,
                                              siblingsQuery.column("protocol_config_id", "device")),
                         siblingsQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                             siblingsQuery.column("deleted_at", "config"))),
                  "config")
            .where(andAll(siblingsQuery,
                          siblingsQuery.binary(siblingsQuery.column("link_id", "device"),
                                               ruvia::DbBinaryOperator::kEqual,
                                               DeviceAccessService::uuid(siblingsQuery, linkId)),
                          siblingsQuery.binary(siblingsQuery.column("id", "device"),
                                               ruvia::DbBinaryOperator::kNotEqual,
                                               DeviceAccessService::uuid(siblingsQuery, excluded)),
                          siblingsQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                              siblingsQuery.column("deleted_at", "device")),
                          siblingsQuery.binary(siblingsQuery.column("protocol", "config"),
                                               ruvia::DbBinaryOperator::kEqual,
                                               siblingsQuery.value(protocol))))
            .orderBy(siblingsQuery.column("id", "device"));
        const auto siblings = co_await c.db().query(siblingsQuery);

        if (linkMode == "TCP Client") {
            if (targetId.empty())
                co_return;
            for (const auto& sibling : siblings) {
                if (sibling[2].value().value_or(std::string_view{}) != targetId)
                    continue;
                const std::string name(sibling[0].value().value_or(std::string_view{}));
                if (protocol == "S7")
                    service::common::fail(
                        18006, "S7 同一目标地址只能关联一个设备，冲突设备: " + name, 409);
                if (toInt(sibling[1].value().value_or(std::string_view{})) == slaveId)
                    service::common::fail(
                        18006, "Modbus 同一目标地址下 Slave ID 重复，冲突设备: " + name, 409);
            }
            co_return;
        }
        if (linkMode != "TCP Server")
            co_return;

        if (protocol == "Modbus" && !siblings.empty()) {
            if (registrationMode == "OFF")
                service::common::fail(18006, "Modbus TCP Server 链路存在多个设备时必须配置注册包",
                                      409);
            for (const auto& sibling : siblings) {
                const std::string name(sibling[0].value().value_or(std::string_view{}));
                if (sibling[3].value().value_or(std::string_view{}) == "OFF")
                    service::common::fail(
                        18006, "Modbus TCP Server 链路存在未配置注册包的设备: " + name, 409);
                if (sibling[4].value().value_or(std::string_view{}) == registrationKey && toInt(sibling[1].value().value_or(std::string_view{})) == slaveId)
                    service::common::fail(
                        18006, "Modbus 同一链路和注册码下 Slave ID 重复，冲突设备: " + name, 409);
            }
            co_return;
        }

        if (protocol == "S7") {
            for (const auto& sibling : siblings)
                if (sibling[4].value().value_or(std::string_view{}) == registrationKey)
                    service::common::fail(18006,
                                          "S7 TCP Server 同一链路下注册码重复，冲突设备: " +
                                              std::string(sibling[0].value().value_or(std::string_view{})),
                                          409);
        }
    }

    ruvia::Task<void> ensureUnique(ruvia::Context& c, const SaveDeviceBody& body,
                                   std::optional<std::string> excludedId) {
        const auto& name = body.get<"name">();
        const auto& code = body.get<"deviceCode">();
        if (!name && !code)
            co_return;
        const std::string nameValue = str(name);
        const std::string codeValue = str(code);
        const std::string linkValue = str(body.get<"linkId">());
        const std::string excluded = excludedId.value_or(std::string(kNilUuid));
        ruvia::DbQuery query(c.pool());
        ruvia::DbQuery currentLink(c.pool());
        currentLink.select(currentLink.column("link_id"))
            .from("device")
            .where(currentLink.binary(currentLink.column("id"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      DeviceAccessService::uuid(currentLink, excluded)))
            .limit(1);
        const auto nameMatch = andAll(
            query,
            query.binary(DeviceAccessService::text(query, nameValue),
                         ruvia::DbBinaryOperator::kNotEqual,
                         DeviceAccessService::text(query, "")),
            query.binary(query.column("name"), ruvia::DbBinaryOperator::kEqual,
                         query.value(nameValue)));
        const auto codeMatch = andAll(
            query,
            query.binary(DeviceAccessService::text(query, codeValue),
                         ruvia::DbBinaryOperator::kNotEqual,
                         DeviceAccessService::text(query, "")),
            query.binary(
                DeviceAccessService::jsonText(query, query.column("protocol_params"),
                                               "device_code"),
                ruvia::DbBinaryOperator::kEqual, query.value(codeValue)),
            query.binary(query.column("link_id"), ruvia::DbBinaryOperator::kEqual,
                         query.coalesce({DeviceAccessService::nullableUuid(query, linkValue),
                                         query.subquery(currentLink)})));
        query.select(DeviceAccessService::integer(query, 1))
            .from("device")
            .where(andAll(query,
                          query.unary(ruvia::DbUnaryOperator::kIsNull,
                                      query.column("deleted_at")),
                          query.binary(query.column("id"), ruvia::DbBinaryOperator::kNotEqual,
                                       DeviceAccessService::uuid(query, excluded)),
                          query.binary(nameMatch, ruvia::DbBinaryOperator::kOr, codeMatch)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (!rows.empty())
            service::common::fail(18004, "设备名称已存在或同一链路的设备编码重复", 409);
    }

    // ----- 设备分组私有工具 -----

    template <typename Row>
    static void fillGroup(DeviceGroupItemDto& item, const Row& row, const DeviceActor& actor) {
        item.set<"id">(row[0].value().value_or(std::string_view{}));
        item.set<"name">(row[1].value().value_or(std::string_view{}));
        item.set<"parentId">(row[2].value().value_or(std::string_view{}));
        item.set<"status">(row[3].value().value_or(std::string_view{}));
        item.set<"sortOrder">(toInt(row[4].value().value_or(std::string_view{})));
        item.set<"remark">(row[5].value().value_or(std::string_view{}));
        item.set<"deviceCount">(toInt(row[6].value().value_or(std::string_view{})));
        item.set<"createdAt">(row[7].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[8].value().value_or(std::string_view{}));
        item.set<"canShare">(
            actor.canGroupShare &&
            (actor.superadmin ||
             row[9].value().value_or(std::string_view{}) == actor.userId));
    }

    ruvia::Task<void> validateParent(ruvia::Context& c, const SaveDeviceGroupBody& body,
                                     std::optional<std::string> currentId) {
        const auto& parent = body.get<"parentId">();
        if (!parent || parent->view().empty())
            co_return;
        if (!service::common::isUuid(parent->view()))
            service::common::fail(17002, "上级分组必须是 UUID", 400);
        if (currentId && parent->view() == *currentId)
            service::common::fail(17003, "上级分组不能是自身", 409);
        ruvia::DbQuery query(c.pool());
        query.select(DeviceAccessService::integer(query, 1))
            .from("device_group")
            .where(andAll(query,
                          query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                       DeviceAccessService::uuid(query, parent->view())),
                          query.unary(ruvia::DbUnaryOperator::kIsNull,
                                      query.column("deleted_at"))))
            .limit(1);
        const auto exists = co_await c.db().query(query);
        if (exists.empty())
            service::common::fail(17003, "上级分组不存在", 400);
    }

    ruvia::Task<void> requireGroupOwner(ruvia::Context& c, std::string_view ownerId) {
        const auto principal = service::middleware::requireAuth(c);
        if (principal.userId == ownerId)
            co_return;
        ruvia::DbQuery roles(c.pool());
        roles.select(DeviceAccessService::integer(roles, 1))
            .from("sys_user_role", "ur")
            .join(ruvia::DbJoinType::kInner, "sys_role",
                  andAll(roles,
                         roles.binary(roles.column("id", "r"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      roles.column("role_id", "ur")),
                         roles.binary(roles.column("code", "r"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      roles.value("superadmin")),
                         roles.binary(roles.column("status", "r"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      roles.value("enabled")),
                         roles.unary(ruvia::DbUnaryOperator::kIsNull,
                                     roles.column("deleted_at", "r"))),
                  "r")
            .where(roles.binary(roles.column("user_id", "ur"),
                                ruvia::DbBinaryOperator::kEqual,
                                DeviceAccessService::uuid(roles, principal.userId)))
            .limit(1);
        ruvia::DbQuery superadmin(c.pool());
        superadmin.select(superadmin.exists(roles));
        const auto rows = co_await c.db().query(superadmin);
        if (rows.front()[0].value().value_or(std::string_view{}) != "t")
            service::common::fail(17005, "只能管理自己创建的设备分组", 403);
    }

    static constexpr std::string_view kNilUuid = "00000000-0000-0000-0000-000000000000";
};

class DeviceShareService {
  public:
    static DeviceShareService& instance() {
        static DeviceShareService service;
        return service;
    }

    ruvia::Task<ruvia::BoxedArray<DeviceShareItemDto>> list(ruvia::Context& c,
                                                       std::string_view deviceId) {
        (void)co_await deviceAccessService().require(c, deviceId, DeviceAccessLevel::owner);
        ruvia::DbQuery currentDevice(c.pool());
        currentDevice.select(currentDevice.column("group_id"))
            .from("device")
            .where(currentDevice.binary(currentDevice.column("id"),
                                        ruvia::DbBinaryOperator::kEqual,
                                        DeviceAccessService::uuid(currentDevice, deviceId)));
        ruvia::DbQuery ancestor(c.pool());
        ruvia::DbQuery ancestorRecursive(c.pool());
        ancestor.select({ancestor.column("id", "device_group"),
                         ancestor.column("parent_id", "device_group"),
                         ancestor.column("name", "device_group")})
            .from("device_group", "device_group")
            .join(ruvia::DbJoinType::kInner, "current_device",
                  ancestor.binary(ancestor.column("group_id", "current_device"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 ancestor.column("id", "device_group")),
                  "current_device")
            .where(ancestor.unary(ruvia::DbUnaryOperator::kIsNull,
                                  ancestor.column("deleted_at", "device_group")));
        ancestorRecursive
            .select({ancestorRecursive.column("id", "parent"),
                     ancestorRecursive.column("parent_id", "parent"),
                     ancestorRecursive.column("name", "parent")})
            .from("device_group", "parent")
            .join(ruvia::DbJoinType::kInner, "ancestor_group",
                  ancestorRecursive.binary(ancestorRecursive.column("parent_id", "child"),
                                            ruvia::DbBinaryOperator::kEqual,
                                            ancestorRecursive.column("id", "parent")),
                  "child")
            .where(ancestorRecursive.unary(ruvia::DbUnaryOperator::kIsNull,
                                           ancestorRecursive.column("deleted_at", "parent")));
        ancestor.combine(ruvia::DbSetOperation::kUnionAll, ancestorRecursive);

        ruvia::DbQuery direct(c.pool());
        const auto directUser = direct.unary(ruvia::DbUnaryOperator::kIsNotNull,
                                             direct.column("user_id", "access_grant"));
        const auto directSubjectType = direct.caseWhen(
            {{directUser, DeviceAccessService::text(direct, direct.value("user"))}},
            DeviceAccessService::text(direct, direct.value("department")));
        const auto directName = direct.caseWhen(
            {{directUser,
              direct.coalesce({direct.nullIf(direct.column("nickname", "target_user"),
                                             direct.value("")),
                               direct.column("username", "target_user"),
                               direct.value("已删除用户")})}},
            direct.coalesce({direct.column("name", "target_department"),
                             direct.value("已删除部门")}));
        direct
            .select({DeviceAccessService::text(direct,
                                               direct.column("id", "access_grant")),
                     directSubjectType,
                     direct.coalesce({DeviceAccessService::text(
                                          direct, direct.column("user_id", "access_grant")),
                                      DeviceAccessService::text(
                                          direct, direct.column("department_id", "access_grant"))}),
                     directName, direct.column("access_level", "access_grant"),
                     DeviceAccessService::text(direct, direct.value("device")),
                     DeviceAccessService::text(direct, direct.value("")),
                      DeviceAccessService::text(direct, direct.value("")),
                      DeviceAccessService::boolean(direct, false),
                     direct.call("iot_utc_timestamp", {direct.column("created_at", "access_grant")}),
                     direct.call("iot_utc_timestamp", {direct.column("updated_at", "access_grant")})})
            .from("device_access_grant", "access_grant")
            .join(ruvia::DbJoinType::kLeft, "sys_user",
                  direct.binary(direct.column("id", "target_user"),
                                ruvia::DbBinaryOperator::kEqual,
                                direct.column("user_id", "access_grant")),
                  "target_user")
            .join(ruvia::DbJoinType::kLeft, "sys_department",
                  direct.binary(direct.column("id", "target_department"),
                                ruvia::DbBinaryOperator::kEqual,
                                direct.column("department_id", "access_grant")),
                  "target_department")
            .where(direct.binary(direct.column("device_id", "access_grant"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 DeviceAccessService::uuid(direct, deviceId)));

        ruvia::DbQuery inherited(c.pool());
        const auto inheritedUser = inherited.unary(
            ruvia::DbUnaryOperator::kIsNotNull, inherited.column("user_id", "group_access"));
        const auto inheritedSubjectType = inherited.caseWhen(
            {{inheritedUser, DeviceAccessService::text(inherited, inherited.value("user"))}},
            DeviceAccessService::text(inherited, inherited.value("department")));
        const auto inheritedName = inherited.caseWhen(
            {{inheritedUser,
              inherited.coalesce({inherited.nullIf(
                                     inherited.column("nickname", "inherited_user"),
                                     inherited.value("")),
                                 inherited.column("username", "inherited_user"),
                                 inherited.value("已删除用户")})}},
            inherited.coalesce({inherited.column("name", "inherited_department"),
                                inherited.value("已删除部门")}));
        inherited
            .select({DeviceAccessService::text(inherited,
                                               inherited.column("id", "group_access")),
                     inheritedSubjectType,
                     inherited.coalesce({DeviceAccessService::text(
                                              inherited,
                                              inherited.column("user_id", "group_access")),
                                          DeviceAccessService::text(
                                              inherited,
                                              inherited.column("department_id", "group_access"))}),
                     inheritedName, inherited.column("access_level", "group_access"),
                     DeviceAccessService::text(inherited, inherited.value("group")),
                     DeviceAccessService::text(inherited,
                                               inherited.column("id", "ancestor")),
                      inherited.column("name", "ancestor"),
                      DeviceAccessService::boolean(inherited, true),
                     inherited.call("iot_utc_timestamp",
                                    {inherited.column("created_at", "group_access")}),
                     inherited.call("iot_utc_timestamp",
                                    {inherited.column("updated_at", "group_access")})})
            .from("device_group_access_grant", "group_access")
            .join(ruvia::DbJoinType::kInner, "ancestor_group",
                  inherited.binary(inherited.column("id", "ancestor"),
                                   ruvia::DbBinaryOperator::kEqual,
                                   inherited.column("group_id", "group_access")),
                  "ancestor")
            .join(ruvia::DbJoinType::kLeft, "sys_user",
                  inherited.binary(inherited.column("id", "inherited_user"),
                                   ruvia::DbBinaryOperator::kEqual,
                                   inherited.column("user_id", "group_access")),
                  "inherited_user")
            .join(ruvia::DbJoinType::kLeft, "sys_department",
                  inherited.binary(inherited.column("id", "inherited_department"),
                                   ruvia::DbBinaryOperator::kEqual,
                                   inherited.column("department_id", "group_access")),
                  "inherited_department");
        direct.combine(ruvia::DbSetOperation::kUnionAll, inherited);
        ruvia::DbQuery query(c.pool());
        query.with("current_device", currentDevice)
            .with("ancestor_group", ancestor,
                  {.recursive = true, .columns = {"id", "parent_id", "name"}})
            .with("device_share", direct,
                  {.columns = {"id", "subject_type", "subject_id", "subject_name",
                               "access_level", "source_type", "source_group_id",
                               "source_group_name", "inherited", "created_at", "updated_at"}});
        query.select(query.star("device_share"))
            .from("device_share", "device_share")
            .orderBy(query.column("subject_type", "device_share"))
            .addOrderBy(query.column("subject_name", "device_share"))
            .addOrderBy(query.column("inherited", "device_share"))
            .addOrderBy(query.column("id", "device_share"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceShareItemDto> result(
            ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{.resource = c.arena()});
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"subjectType">(row[1].value().value_or(std::string_view{}))
                .set<"subjectId">(row[2].value().value_or(std::string_view{}))
                .set<"subjectName">(row[3].value().value_or(std::string_view{}))
                .set<"accessLevel">(row[4].value().value_or(std::string_view{}))
                .set<"sourceType">(row[5].value().value_or(std::string_view{}))
                .set<"sourceGroupId">(row[6].value().value_or(std::string_view{}))
                .set<"sourceGroupName">(row[7].value().value_or(std::string_view{}))
                .set<"inherited">(row[8].value().value_or(std::string_view{}) == "t")
                .set<"createdAt">(row[9].value().value_or(std::string_view{}))
                .set<"updatedAt">(row[10].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<ruvia::BoxedArray<DeviceShareTargetDto>> targets(ruvia::Context& c,
                                                            std::string_view deviceId) {
        (void)co_await deviceAccessService().require(c, deviceId, DeviceAccessLevel::owner);
        ruvia::DbQuery owner(c.pool());
        owner.select(owner.column("created_by"))
            .from("device")
            .where(owner.binary(owner.column("id"), ruvia::DbBinaryOperator::kEqual,
                                DeviceAccessService::uuid(owner, deviceId)))
            .limit(1);
        ruvia::DbQuery users(c.pool());
        users.select({DeviceAccessService::text(users, users.value("user")),
                      DeviceAccessService::text(users, users.column("id", "target")),
                      users.coalesce({users.nullIf(users.column("nickname", "target"),
                                                   users.value("")),
                                      users.column("username", "target")})})
            .from("sys_user", "target")
            .where(andAll(users,
                          users.binary(users.column("status", "target"),
                                       ruvia::DbBinaryOperator::kEqual, users.value("enabled")),
                          users.unary(ruvia::DbUnaryOperator::kIsNull,
                                     users.column("deleted_at", "target")),
                          users.binary(users.column("id", "target"),
                                       ruvia::DbBinaryOperator::kNotEqual,
                                       users.subquery(owner))));
        ruvia::DbQuery departments(c.pool());
        departments.select({DeviceAccessService::text(departments,
                                                       departments.value("department")),
                            DeviceAccessService::text(departments,
                                                      departments.column("id", "department")),
                            departments.column("name", "department")})
            .from("sys_department", "department")
            .where(andAll(departments,
                          departments.binary(departments.column("status", "department"),
                                              ruvia::DbBinaryOperator::kEqual,
                                              departments.value("enabled")),
                          departments.unary(ruvia::DbUnaryOperator::kIsNull,
                                            departments.column("deleted_at", "department"))));
        users.combine(ruvia::DbSetOperation::kUnionAll, departments);
        ruvia::DbQuery query(c.pool());
        query.with("share_target", users,
                   {.columns = {"subject_type", "subject_id", "subject_name"}})
            .select(query.star("share_target"))
            .from("share_target", "share_target")
            .orderBy(query.column("subject_type", "share_target"))
            .addOrderBy(query.column("subject_name", "share_target"))
            .addOrderBy(query.column("subject_id", "share_target"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceShareTargetDto> result(
            ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{.resource = c.arena()});
            item.set<"subjectType">(row[0].value().value_or(std::string_view{})).set<"subjectId">(row[1].value().value_or(std::string_view{})).set<"subjectName">(row[2].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<void> replace(ruvia::Context& c, std::string_view deviceId,
                              const ReplaceDeviceSharesBody& body) {
        auto decision =
            co_await deviceAccessService().require(c, deviceId, DeviceAccessLevel::owner);
        auto shares = normalize(body);

        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery deviceQuery(c.pool());
        deviceQuery.select(DeviceAccessService::text(
                               deviceQuery, deviceQuery.column("created_by")))
            .from("device")
            .where(andAll(deviceQuery,
                          deviceQuery.binary(deviceQuery.column("id"),
                                             ruvia::DbBinaryOperator::kEqual,
                                             DeviceAccessService::uuid(deviceQuery, deviceId)),
                          deviceQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                            deviceQuery.column("deleted_at"))))
            .lock({.mode = ruvia::DbRowLock::kUpdate});
        const auto deviceRows = co_await transaction.query(deviceQuery);
        if (deviceRows.empty())
            service::common::fail(18001, "设备不存在", 404);
        const std::string ownerId(deviceRows.front()[0].value().value_or(std::string_view{}));

        co_await validateTargets(transaction, shares, ownerId);

        ruvia::DbQuery remove(c.pool());
        remove.deleteFrom("device_access_grant")
            .where(remove.binary(remove.column("device_id"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 DeviceAccessService::uuid(remove, deviceId)));
        (void)co_await transaction.execute(remove);
        for (const auto& share : shares) {
            const std::string userId = share.subjectType == "user" ? share.subjectId : "";
            const std::string departmentId =
                share.subjectType == "department" ? share.subjectId : "";
            const auto grantId = service::common::nextUuidV7();
            ruvia::DbQuery insert(c.pool());
            insert.insertInto("device_access_grant",
                              {"id", "device_id", "user_id", "department_id", "access_level",
                               "granted_by"})
                .values({DeviceAccessService::uuid(insert, grantId),
                         DeviceAccessService::uuid(insert, deviceId),
                         DeviceAccessService::nullableUuid(insert, userId),
                         DeviceAccessService::nullableUuid(insert, departmentId),
                         insert.value(share.accessLevel),
                         DeviceAccessService::uuid(insert, decision.actor.userId)});
            (void)co_await transaction.execute(insert);
        }
        const auto auditId = service::common::nextUuidV7();
        const auto shareCount = static_cast<std::int64_t>(shares.size());
        ruvia::DbQuery audit(c.pool());
        audit.insertInto("security_audit_log",
                         {"id", "actor_user_id", "action", "resource_type", "resource_id",
                          "outcome", "details"})
            .values({DeviceAccessService::uuid(audit, auditId),
                     DeviceAccessService::uuid(audit, decision.actor.userId),
                     audit.value("device.share.replace"), audit.value("device"),
                     DeviceAccessService::uuid(audit, deviceId), audit.value("success"),
                     audit.call("jsonb_build_object",
                                {DeviceAccessService::textKey(audit, "share_count"),
                                 audit.cast(audit.value(shareCount),
                                            ruvia::DbDataType::kInteger)})});
        (void)co_await transaction.execute(audit);
        co_await transaction.commit();
    }

    ruvia::Task<ruvia::BoxedArray<DeviceShareItemDto>> listGroup(ruvia::Context& c,
                                                             std::string_view groupId) {
        (void)co_await deviceAccessService().requireGroupOwner(c, groupId);
        ruvia::DbQuery query(c.pool());
        const auto isUser = query.unary(ruvia::DbUnaryOperator::kIsNotNull,
                                        query.column("user_id", "access_grant"));
        const auto subjectType = query.caseWhen(
            {{isUser, DeviceAccessService::text(query, query.value("user"))}},
            DeviceAccessService::text(query, query.value("department")));
        const auto subjectName = query.caseWhen(
            {{isUser,
              query.coalesce({query.nullIf(query.column("nickname", "target_user"),
                                            query.value("")),
                              query.column("username", "target_user"),
                              query.value("已删除用户")})}},
            query.coalesce({query.column("name", "target_department"),
                            query.value("已删除部门")}));
        query.select({DeviceAccessService::text(query,
                                                query.column("id", "access_grant")),
                      subjectType,
                      query.coalesce({DeviceAccessService::text(
                                          query, query.column("user_id", "access_grant")),
                                      DeviceAccessService::text(
                                          query, query.column("department_id", "access_grant"))}),
                      subjectName, query.column("access_level", "access_grant"),
                      DeviceAccessService::text(query, query.value("group")),
                      DeviceAccessService::text(query, query.column("id", "target_group")),
                      query.column("name", "target_group"),
                      DeviceAccessService::boolean(query, false),
                      query.call("iot_utc_timestamp", {query.column("created_at", "access_grant")}),
                      query.call("iot_utc_timestamp", {query.column("updated_at", "access_grant")})})
            .from("device_group_access_grant", "access_grant")
            .join(ruvia::DbJoinType::kInner, "device_group",
                  query.binary(query.column("id", "target_group"),
                               ruvia::DbBinaryOperator::kEqual,
                               query.column("group_id", "access_grant")),
                  "target_group")
            .join(ruvia::DbJoinType::kLeft, "sys_user",
                  query.binary(query.column("id", "target_user"),
                               ruvia::DbBinaryOperator::kEqual,
                               query.column("user_id", "access_grant")),
                  "target_user")
            .join(ruvia::DbJoinType::kLeft, "sys_department",
                  query.binary(query.column("id", "target_department"),
                               ruvia::DbBinaryOperator::kEqual,
                               query.column("department_id", "access_grant")),
                  "target_department")
            .where(query.binary(query.column("group_id", "access_grant"),
                                ruvia::DbBinaryOperator::kEqual,
                                DeviceAccessService::uuid(query, groupId)))
            .orderBy(subjectType)
            .addOrderBy(subjectName)
            .addOrderBy(query.column("id", "access_grant"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceShareItemDto> result(
            ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{.resource = c.arena()});
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"subjectType">(row[1].value().value_or(std::string_view{}))
                .set<"subjectId">(row[2].value().value_or(std::string_view{}))
                .set<"subjectName">(row[3].value().value_or(std::string_view{}))
                .set<"accessLevel">(row[4].value().value_or(std::string_view{}))
                .set<"sourceType">(row[5].value().value_or(std::string_view{}))
                .set<"sourceGroupId">(row[6].value().value_or(std::string_view{}))
                .set<"sourceGroupName">(row[7].value().value_or(std::string_view{}))
                .set<"inherited">(false)
                .set<"createdAt">(row[9].value().value_or(std::string_view{}))
                .set<"updatedAt">(row[10].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<ruvia::BoxedArray<DeviceShareTargetDto>> groupTargets(ruvia::Context& c,
                                                                 std::string_view groupId) {
        (void)co_await deviceAccessService().requireGroupOwner(c, groupId);
        ruvia::DbQuery owner(c.pool());
        owner.select(owner.column("created_by"))
            .from("device_group")
            .where(owner.binary(owner.column("id"), ruvia::DbBinaryOperator::kEqual,
                                DeviceAccessService::uuid(owner, groupId)))
            .limit(1);
        ruvia::DbQuery users(c.pool());
        users.select({DeviceAccessService::text(users, users.value("user")),
                      DeviceAccessService::text(users, users.column("id", "target")),
                      users.coalesce({users.nullIf(users.column("nickname", "target"),
                                                   users.value("")),
                                      users.column("username", "target")})})
            .from("sys_user", "target")
            .where(andAll(users,
                          users.binary(users.column("status", "target"),
                                       ruvia::DbBinaryOperator::kEqual, users.value("enabled")),
                          users.unary(ruvia::DbUnaryOperator::kIsNull,
                                     users.column("deleted_at", "target")),
                          users.binary(users.column("id", "target"),
                                       ruvia::DbBinaryOperator::kNotEqual,
                                       users.subquery(owner))));
        ruvia::DbQuery departments(c.pool());
        departments.select({DeviceAccessService::text(departments,
                                                       departments.value("department")),
                            DeviceAccessService::text(departments,
                                                      departments.column("id", "department")),
                            departments.column("name", "department")})
            .from("sys_department", "department")
            .where(andAll(departments,
                          departments.binary(departments.column("status", "department"),
                                              ruvia::DbBinaryOperator::kEqual,
                                              departments.value("enabled")),
                          departments.unary(ruvia::DbUnaryOperator::kIsNull,
                                            departments.column("deleted_at", "department"))));
        users.combine(ruvia::DbSetOperation::kUnionAll, departments);
        ruvia::DbQuery query(c.pool());
        query.with("share_target", users,
                   {.columns = {"subject_type", "subject_id", "subject_name"}})
            .select(query.star("share_target"))
            .from("share_target", "share_target")
            .orderBy(query.column("subject_type", "share_target"))
            .addOrderBy(query.column("subject_name", "share_target"))
            .addOrderBy(query.column("subject_id", "share_target"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceShareTargetDto> result(
            ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{.resource = c.arena()});
            item.set<"subjectType">(row[0].value().value_or(std::string_view{})).set<"subjectId">(row[1].value().value_or(std::string_view{})).set<"subjectName">(row[2].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<void> replaceGroup(ruvia::Context& c, std::string_view groupId,
                                   const ReplaceDeviceSharesBody& body) {
        auto actor = co_await deviceAccessService().requireGroupOwner(c, groupId);
        auto shares = normalize(body);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery groupQuery(c.pool());
        groupQuery.select(DeviceAccessService::text(groupQuery,
                                                     groupQuery.column("created_by")))
            .from("device_group")
            .where(andAll(groupQuery,
                          groupQuery.binary(groupQuery.column("id"),
                                            ruvia::DbBinaryOperator::kEqual,
                                            DeviceAccessService::uuid(groupQuery, groupId)),
                          groupQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                           groupQuery.column("deleted_at"))))
            .lock({.mode = ruvia::DbRowLock::kUpdate});
        const auto groupRows = co_await transaction.query(groupQuery);
        if (groupRows.empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await validateTargets(transaction, shares, groupRows.front()[0].value().value_or(std::string_view{}));

        ruvia::DbQuery remove(c.pool());
        remove.deleteFrom("device_group_access_grant")
            .where(remove.binary(remove.column("group_id"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 DeviceAccessService::uuid(remove, groupId)));
        (void)co_await transaction.execute(remove);
        for (const auto& share : shares) {
            const std::string userId = share.subjectType == "user" ? share.subjectId : "";
            const std::string departmentId =
                share.subjectType == "department" ? share.subjectId : "";
            const auto grantId = service::common::nextUuidV7();
            ruvia::DbQuery insert(c.pool());
            insert.insertInto("device_group_access_grant",
                              {"id", "group_id", "user_id", "department_id", "access_level",
                               "granted_by"})
                .values({DeviceAccessService::uuid(insert, grantId),
                         DeviceAccessService::uuid(insert, groupId),
                         DeviceAccessService::nullableUuid(insert, userId),
                         DeviceAccessService::nullableUuid(insert, departmentId),
                         insert.value(share.accessLevel),
                         DeviceAccessService::uuid(insert, actor.userId)});
            (void)co_await transaction.execute(insert);
        }
        const auto auditId = service::common::nextUuidV7();
        const auto shareCount = static_cast<std::int64_t>(shares.size());
        ruvia::DbQuery audit(c.pool());
        audit.insertInto("security_audit_log",
                         {"id", "actor_user_id", "action", "resource_type", "resource_id",
                          "outcome", "details"})
            .values({DeviceAccessService::uuid(audit, auditId),
                     DeviceAccessService::uuid(audit, actor.userId),
                     audit.value("device_group.share.replace"), audit.value("device_group"),
                     DeviceAccessService::uuid(audit, groupId), audit.value("success"),
                     audit.call("jsonb_build_object",
                                {DeviceAccessService::textKey(audit, "share_count"),
                                 audit.cast(audit.value(shareCount),
                                            ruvia::DbDataType::kInteger)})});
        (void)co_await transaction.execute(audit);
        co_await transaction.commit();
    }

  private:
    struct NormalizedShare final {
        std::string subjectType;
        std::string subjectId;
        std::string accessLevel;
    };

    static std::vector<NormalizedShare> normalize(const ReplaceDeviceSharesBody& body) {
        if (!body.get<"shares">())
            service::common::fail(18010, "分享列表不能为空", 400);
        std::vector<NormalizedShare> shares;
        shares.reserve(body.get<"shares">()->size());
        std::set<std::string, std::less<>> uniqueSubjects;
        for (const auto& item : *body.get<"shares">()) {
            if (!item.get<"subjectType">() || !item.get<"subjectId">() || !item.get<"accessLevel">())
                service::common::fail(18010, "分享对象参数不完整", 400);
            NormalizedShare share{std::string(item.get<"subjectType">()->view()),
                                  std::string(item.get<"subjectId">()->view()),
                                  std::string(item.get<"accessLevel">()->view())};
            if (share.subjectType != "user" && share.subjectType != "department")
                service::common::fail(18010, "分享对象类型无效", 400);
            if (!service::common::isUuid(share.subjectId))
                service::common::fail(18010, "分享对象 ID 必须是 UUID", 400);
            if (share.accessLevel != "view" && share.accessLevel != "operate")
                service::common::fail(18010, "设备访问级别无效", 400);
            if (!uniqueSubjects.emplace(share.subjectType + ":" + share.subjectId).second)
                service::common::fail(18010, "分享对象不能重复", 400);
            shares.emplace_back(std::move(share));
        }
        return shares;
    }

    static ruvia::Task<void> validateTargets(ruvia::DbTransaction& transaction,
                                              const std::vector<NormalizedShare>& shares,
                                              std::string_view ownerId) {
        for (const auto& share : shares) {
            if (share.subjectType == "user") {
                if (share.subjectId == ownerId)
                    service::common::fail(18010, "不能向资源所有者重复授权", 400);
                ruvia::DbQuery targetQuery;
                targetQuery.select(DeviceAccessService::integer(targetQuery, 1))
                    .from("sys_user")
                    .where(andAll(targetQuery,
                                  targetQuery.binary(targetQuery.column("id"),
                                                     ruvia::DbBinaryOperator::kEqual,
                                                     DeviceAccessService::uuid(
                                                         targetQuery, share.subjectId)),
                                  targetQuery.binary(targetQuery.column("status"),
                                                     ruvia::DbBinaryOperator::kEqual,
                                                     targetQuery.value("enabled")),
                                  targetQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                                    targetQuery.column("deleted_at"))))
                    .limit(1);
                const auto target = co_await transaction.query(targetQuery);
                if (target.empty())
                    service::common::fail(18010, "包含不存在或已禁用的用户", 400);
            } else {
                ruvia::DbQuery targetQuery;
                targetQuery.select(DeviceAccessService::integer(targetQuery, 1))
                    .from("sys_department")
                    .where(andAll(targetQuery,
                                  targetQuery.binary(targetQuery.column("id"),
                                                     ruvia::DbBinaryOperator::kEqual,
                                                     DeviceAccessService::uuid(
                                                         targetQuery, share.subjectId)),
                                  targetQuery.binary(targetQuery.column("status"),
                                                     ruvia::DbBinaryOperator::kEqual,
                                                     targetQuery.value("enabled")),
                                  targetQuery.unary(ruvia::DbUnaryOperator::kIsNull,
                                                    targetQuery.column("deleted_at"))))
                    .limit(1);
                const auto target = co_await transaction.query(targetQuery);
                if (target.empty())
                    service::common::fail(18010, "包含不存在或已禁用的部门", 400);
            }
        }
    }
};

inline DeviceShareService& deviceShareService() { return DeviceShareService::instance(); }

inline DeviceService& deviceService() { return DeviceService::instance(); }

} // namespace service::device
