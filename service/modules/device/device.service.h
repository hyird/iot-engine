#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
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

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/middleware/rpc.h"
#include "service/modules/device/device.entity.h"
#include "service/modules/device/device.types.h"
#include "service/modules/edge_node/edge_node.service.h"
#include "service/modules/system/auth/auth.service.h"
#include "service/modules/system/outbox/outbox.service.h"
#include "service/utils/number.h"
#include "service/utils/redis.h"

namespace service::device {

inline ruvia::DbQuery::Expr andAll(ruvia::DbQuery& query, ruvia::DbQuery::Expr first, ruvia::DbQuery::Expr second) {
    return query.binary(first, ruvia::DbBinaryOperator::kAnd, second);
}

template <typename... Expressions>
inline ruvia::DbQuery::Expr andAll(ruvia::DbQuery& query, ruvia::DbQuery::Expr first, ruvia::DbQuery::Expr second, Expressions... rest) {
    return query.binary(first, ruvia::DbBinaryOperator::kAnd, andAll(query, second, rest...));
}

class DeviceAccessService {
  public:
    static DeviceAccessService& instance() {
        static thread_local DeviceAccessService service;
        return service;
    }

    template <typename Context>
    ruvia::Task<DeviceActor> actor(Context& c, std::string_view userId) const {
        ruvia::DbQuery query(c.pool());
        const auto roleCode = query.column(service::device::entities::SysRoleEntity::columnName<"code">(), "role");
        const auto isSuperadmin = query.binary(roleCode, ruvia::DbBinaryOperator::kEqual, query.value("superadmin"));
        const auto permissions = query.column(service::device::entities::SysRoleEntity::columnName<"permissions">(), "role");
        const auto hasWildcard = query.binary(
            permissions,
            ruvia::DbBinaryOperator::kJsonHasKey,
            textKey(query, "*")
        );
        const auto permission = [&](std::string_view value) {
            return query.binary(permissions, ruvia::DbBinaryOperator::kJsonHasKey, textKey(query, value));
        };
        const auto capability = [&](std::string_view value) {
            return query.binary(
                query.binary(isSuperadmin, ruvia::DbBinaryOperator::kOr, hasWildcard),
                ruvia::DbBinaryOperator::kOr,
                permission(value)
            );
        };
        const auto boolOr = [&](ruvia::DbQuery::Expr expression) {
            return query.coalesce({ query.aggregate("bool_or", { expression }), boolean(query, false) });
        };
        query.select({ query.coalesce({ text(query, query.column(service::device::entities::SysDepartmentEntity::columnName<"id">(), "department")), query.value("") }), boolOr(isSuperadmin), boolOr(capability("iot:device:edit")), boolOr(capability("iot:device:delete")), boolOr(capability("iot:device:share")), boolOr(capability("iot:device:command")), boolOr(capability("iot:device-group:share")) })
            .from(service::device::entities::SysUserEntity::tableName(), "actor")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::SysDepartmentEntity::tableName(), andAll(query, query.binary(query.column(service::device::entities::SysDepartmentEntity::columnName<"id">(), "department"), ruvia::DbBinaryOperator::kEqual, query.column(service::device::entities::SysUserEntity::columnName<"department_id">(), "actor")), query.binary(query.column(service::device::entities::SysDepartmentEntity::columnName<"status">(), "department"), ruvia::DbBinaryOperator::kEqual, query.value("enabled")), query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::device::entities::SysDepartmentEntity::columnName<"deleted_at">(), "department"))), "department")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::SysUserRoleEntity::tableName(), query.binary(query.column(service::device::entities::SysUserRoleEntity::columnName<"user_id">(), "user_role"), ruvia::DbBinaryOperator::kEqual, query.column(service::device::entities::SysUserEntity::columnName<"id">(), "actor")), "user_role")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::SysRoleEntity::tableName(), andAll(query, query.binary(query.column(service::device::entities::SysRoleEntity::columnName<"id">(), "role"), ruvia::DbBinaryOperator::kEqual, query.column(service::device::entities::SysUserRoleEntity::columnName<"role_id">(), "user_role")), query.binary(query.column(service::device::entities::SysRoleEntity::columnName<"status">(), "role"), ruvia::DbBinaryOperator::kEqual, query.value("enabled")), query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::device::entities::SysRoleEntity::columnName<"deleted_at">(), "role"))), "role")
            .where(andAll(query, query.binary(query.column(service::device::entities::SysUserEntity::columnName<"id">(), "actor"), ruvia::DbBinaryOperator::kEqual, uuid(query, userId)), query.binary(query.column(service::device::entities::SysUserEntity::columnName<"status">(), "actor"), ruvia::DbBinaryOperator::kEqual, query.value("enabled")), query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::device::entities::SysUserEntity::columnName<"deleted_at">(), "actor"))))
            .groupBy({ query.column(service::device::entities::SysUserEntity::columnName<"id">(), "actor"), query.column(service::device::entities::SysDepartmentEntity::columnName<"id">(), "department") });
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(service::common::kTokenInvalidErrorCode, "用户状态无效", 401);
        }
        const auto& row = rows.front();
        DeviceActor result;
        result.userId = userId;
        result.departmentId = std::string(row[0].value().value_or(std::string_view{}));
        result.superadmin = isTrue(row[1].value().value_or(std::string_view{}));
        result.canEdit = isTrue(row[2].value().value_or(std::string_view{}));
        result.canDelete = isTrue(row[3].value().value_or(std::string_view{}));
        result.canShare = isTrue(row[4].value().value_or(std::string_view{}));
        result.canCommand = isTrue(row[5].value().value_or(std::string_view{}));
        result.canGroupShare = isTrue(row[6].value().value_or(std::string_view{}));
        co_return result;
    }

    template <typename Context>
    ruvia::Task<DeviceActor> requireGroupOwner(Context& c, std::string_view groupId, std::string_view userId) const {
        auto currentActor = co_await actor(c, userId);
        ruvia::DbQuery query(c.pool());
        query.select(text(query, query.column(service::device::entities::DeviceGroupEntity::columnName<"created_by">())))
            .from(service::device::entities::DeviceGroupEntity::tableName())
            .where(andAll(query, query.binary(query.column(service::device::entities::DeviceGroupEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, uuid(query, groupId)), query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">()))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17001, "设备分组不存在", 404);
        }
        if (!currentActor.superadmin && rows.front()[0].value().value_or(std::string_view{}) != currentActor.userId) {
            service::common::fail(17005, "只能分享自己创建的设备分组", 403);
        }
        co_return currentActor;
    }

    template <typename Context>
    ruvia::Task<DeviceAccessDecision> require(Context& c, std::string_view deviceId, DeviceAccessLevel minimum, std::string_view userId) const {
        auto currentActor = co_await actor(c, userId);
        ruvia::DbQuery query(c.pool());
        addScopedDevicesCtes(query, currentActor);
        query.select(query.column("access_rank", "device"))
            .from("scoped_device", "device")
            .where(query.binary(query.column("id", "device"), ruvia::DbBinaryOperator::kEqual, uuid(query, deviceId)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(18001, "设备不存在", 404);
        }
        const auto level = rank(rows.front()[0].value().value_or(std::string_view{}));
        if (level == DeviceAccessLevel::none) {
            service::common::fail(18001, "设备不存在", 404);
        }
        if (level < minimum) {
            service::common::fail(18005, "设备权限不足", 403);
        }
        co_return DeviceAccessDecision{ std::move(currentActor), level };
    }

    // Append the actor-scoped device relations to a query.  The CTEs are deliberately public
    // so callers can compose their own projection, ordering and pagination without embedding SQL.
    static void addScopedDevicesCtes(ruvia::DbQuery& query, const DeviceActor& actor) {
        ruvia::DbQuery shared(query.resource());
        ruvia::DbQuery sharedRecursive(query.resource());
        shared.select({ shared.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"group_id">(), "access_grant"), accessLevelRank(shared, shared.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"access_level">(), "access_grant")) })
            .from(service::device::entities::DeviceGroupAccessGrantEntity::tableName(), "access_grant")
            .join(ruvia::DbJoinType::kInner, service::device::entities::DeviceGroupEntity::tableName(), andAll(shared, shared.binary(shared.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "granted_group"), ruvia::DbBinaryOperator::kEqual, shared.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"group_id">(), "access_grant")), shared.unary(ruvia::DbUnaryOperator::kIsNull, shared.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">(), "granted_group"))), "granted_group")
            .where(andAll(shared, shared.binary(shared.binary(shared.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"user_id">(), "access_grant"), ruvia::DbBinaryOperator::kEqual, uuid(shared, actor.userId)), ruvia::DbBinaryOperator::kOr, shared.binary(shared.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"department_id">(), "access_grant"), ruvia::DbBinaryOperator::kEqual, nullableUuid(shared, actor.departmentId))), shared.unary(ruvia::DbUnaryOperator::kNot, boolean(shared, actor.superadmin))));
        sharedRecursive.select({ sharedRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "child"), sharedRecursive.column("access_rank", "shared") })
            .from(service::device::entities::DeviceGroupEntity::tableName(), "child")
            .join(ruvia::DbJoinType::kInner, "shared_group_access", sharedRecursive.binary(sharedRecursive.column("group_id", "shared"), ruvia::DbBinaryOperator::kEqual, sharedRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"parent_id">(), "child")), "shared")
            .where(sharedRecursive.unary(ruvia::DbUnaryOperator::kIsNull, sharedRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">(), "child")));
        shared.combine(ruvia::DbSetOperation::kUnion, sharedRecursive);
        query.with("shared_group_access", shared, { .recursive = true, .columns = { "group_id", "access_rank" } });

        ruvia::DbQuery groupAccess(query.resource());
        groupAccess.select(
                       { groupAccess.column("group_id"),
                         groupAccess.alias(
                             groupAccess.aggregate("max", { groupAccess.column("access_rank") }),
                             "access_rank"
                         ) }
        )
            .from("shared_group_access")
            .groupBy({ groupAccess.column("group_id") });
        query.with("group_access", groupAccess);

        ruvia::DbQuery deviceAccess(query.resource());
        deviceAccess
            .select({ deviceAccess.column(service::device::entities::DeviceAccessGrantEntity::columnName<"device_id">()), deviceAccess.alias(deviceAccess.aggregate("max", { accessLevelRank(deviceAccess, deviceAccess.column(service::device::entities::DeviceAccessGrantEntity::columnName<"access_level">(), "access_grant")) }), "access_rank") })
            .from(service::device::entities::DeviceAccessGrantEntity::tableName(), "access_grant")
            .where(andAll(deviceAccess, deviceAccess.binary(deviceAccess.binary(deviceAccess.column(service::device::entities::DeviceAccessGrantEntity::columnName<"user_id">(), "access_grant"), ruvia::DbBinaryOperator::kEqual, uuid(deviceAccess, actor.userId)), ruvia::DbBinaryOperator::kOr, deviceAccess.binary(deviceAccess.column(service::device::entities::DeviceAccessGrantEntity::columnName<"department_id">(), "access_grant"), ruvia::DbBinaryOperator::kEqual, nullableUuid(deviceAccess, actor.departmentId))), deviceAccess.unary(ruvia::DbUnaryOperator::kNot, boolean(deviceAccess, actor.superadmin))))
            .groupBy({ deviceAccess.column(service::device::entities::DeviceAccessGrantEntity::columnName<"device_id">()) });
        query.with("device_access", deviceAccess);

        ruvia::DbQuery scoped(query.resource());
        const auto owned = scoped.binary(
            scoped.binary(boolean(scoped, actor.superadmin), ruvia::DbBinaryOperator::kOr, scoped.binary(scoped.column(service::device::entities::DeviceEntity::columnName<"created_by">(), "source"), ruvia::DbBinaryOperator::kEqual, uuid(scoped, actor.userId))),
            ruvia::DbBinaryOperator::kEqual,
            boolean(scoped, true)
        );
        const auto inherited = scoped.greatest(
            { scoped.coalesce({ scoped.column("access_rank", "device_access"), integer(scoped, 0) }),
              scoped.coalesce({ scoped.column("access_rank", "group_access"), integer(scoped, 0) }) }
        );
        scoped.select({ scoped.column("id", "source"), scoped.column("name", "source"), scoped.column("link_id", "source"), scoped.column("protocol_config_id", "source"), scoped.column("group_id", "source"), scoped.column("status", "source"), scoped.column("protocol_params", "source"), scoped.column("remark", "source"), scoped.column("created_by", "source"), scoped.column("created_at", "source"), scoped.column("updated_at", "source"), scoped.column("deleted_at", "source"), scoped.column("protocol_address", "source"), scoped.column("debug_enabled", "source"), scoped.caseWhen({ { owned, integer(scoped, 4) } }, inherited) })
            .from(service::device::entities::DeviceEntity::tableName(), "source")
            .join(ruvia::DbJoinType::kLeft, "device_access", scoped.binary(scoped.column("device_id", "device_access"), ruvia::DbBinaryOperator::kEqual, scoped.column(service::device::entities::DeviceEntity::columnName<"id">(), "source")), "device_access")
            .join(ruvia::DbJoinType::kLeft, "group_access", scoped.binary(scoped.column("group_id", "group_access"), ruvia::DbBinaryOperator::kEqual, scoped.column(service::device::entities::DeviceEntity::columnName<"group_id">(), "source")), "group_access")
            .where(scoped.unary(ruvia::DbUnaryOperator::kIsNull, scoped.column(service::device::entities::DeviceEntity::columnName<"deleted_at">(), "source")));
        query.with("scoped_device", scoped, { .columns = { "id", "name", "link_id", "protocol_config_id", "group_id", "status", "protocol_params", "remark", "created_by", "created_at", "updated_at", "deleted_at", "protocol_address", "debug_enabled", "access_rank" } });
    }

    static void addVisibleGroupsCtes(ruvia::DbQuery& query, const DeviceActor& actor) {
        addScopedDevicesCtes(query, actor);
        ruvia::DbQuery shared(query.resource());
        ruvia::DbQuery sharedRecursive(query.resource());
        shared.select(shared.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"group_id">(), "access_grant"))
            .from(service::device::entities::DeviceGroupAccessGrantEntity::tableName(), "access_grant")
            .where(shared.binary(
                shared.binary(shared.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"user_id">(), "access_grant"), ruvia::DbBinaryOperator::kEqual, uuid(shared, actor.userId)),
                ruvia::DbBinaryOperator::kOr,
                shared.binary(shared.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"department_id">(), "access_grant"), ruvia::DbBinaryOperator::kEqual, nullableUuid(shared, actor.departmentId))
            ));
        sharedRecursive.select(sharedRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "child"))
            .from(service::device::entities::DeviceGroupEntity::tableName(), "child")
            .join(ruvia::DbJoinType::kInner, "shared_group_tree", sharedRecursive.binary(sharedRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "parent"), ruvia::DbBinaryOperator::kEqual, sharedRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"parent_id">(), "child")), "parent")
            .where(sharedRecursive.unary(ruvia::DbUnaryOperator::kIsNull, sharedRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">(), "child")));
        shared.combine(ruvia::DbSetOperation::kUnion, sharedRecursive);
        query.with("shared_group_tree", shared, { .recursive = true, .columns = { "id" } });

        ruvia::DbQuery scopedGroups(query.resource());
        scopedGroups.select(scopedGroups.column("group_id", "scoped"))
            .from("scoped_device", "scoped")
            .where(andAll(scopedGroups, scopedGroups.binary(scopedGroups.column("access_rank", "scoped"), ruvia::DbBinaryOperator::kGreater, integer(scopedGroups, 0)), scopedGroups.unary(ruvia::DbUnaryOperator::kIsNotNull, scopedGroups.column("group_id", "scoped"))));
        ruvia::DbQuery ownedGroups(query.resource());
        ownedGroups.select(ownedGroups.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "owned"))
            .from(service::device::entities::DeviceGroupEntity::tableName(), "owned")
            .where(andAll(ownedGroups, ownedGroups.unary(ruvia::DbUnaryOperator::kIsNull, ownedGroups.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">(), "owned")), ownedGroups.binary(ownedGroups.binary(boolean(ownedGroups, actor.superadmin), ruvia::DbBinaryOperator::kOr, ownedGroups.binary(ownedGroups.column(service::device::entities::DeviceGroupEntity::columnName<"created_by">(), "owned"), ruvia::DbBinaryOperator::kEqual, uuid(ownedGroups, actor.userId))), ruvia::DbBinaryOperator::kEqual, boolean(ownedGroups, true))));
        ruvia::DbQuery sharedGroups(query.resource());
        sharedGroups.select(sharedGroups.column("id", "shared"))
            .from("shared_group_tree", "shared");
        scopedGroups.combine(ruvia::DbSetOperation::kUnion, ownedGroups)
            .combine(ruvia::DbSetOperation::kUnion, sharedGroups);

        ruvia::DbQuery parents(query.resource());
        parents.select(parents.column(service::device::entities::DeviceGroupEntity::columnName<"parent_id">(), "parent"))
            .from(service::device::entities::DeviceGroupEntity::tableName(), "parent")
            .join(ruvia::DbJoinType::kInner, "visible_group", parents.binary(parents.column("id", "visible"), ruvia::DbBinaryOperator::kEqual, parents.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "parent")), "visible")
            .where(andAll(parents, parents.unary(ruvia::DbUnaryOperator::kIsNotNull, parents.column(service::device::entities::DeviceGroupEntity::columnName<"parent_id">(), "parent")), parents.unary(ruvia::DbUnaryOperator::kIsNull, parents.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">(), "parent"))));
        scopedGroups.combine(ruvia::DbSetOperation::kUnion, parents);
        query.with("visible_group", scopedGroups, { .recursive = true, .columns = { "id" } });
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
        return query.cast(query.nullIf(text(query, query.value(value)), text(query, "")), ruvia::DbDataType::kUuid);
    }

    static ruvia::DbQuery::Expr jsonValue(ruvia::DbQuery& query, ruvia::DbQuery::Expr object, std::string_view key) {
        return query.binary(object, ruvia::DbBinaryOperator::kJsonGet, textKey(query, key));
    }

    static ruvia::DbQuery::Expr jsonText(ruvia::DbQuery& query, ruvia::DbQuery::Expr object, std::string_view key) {
        return query.binary(object, ruvia::DbBinaryOperator::kJsonGetText, textKey(query, key));
    }

    static ruvia::DbQuery::Expr accessLevelRank(ruvia::DbQuery& query, ruvia::DbQuery::Expr level) {
        return query.caseWhen(
            { { query.binary(level, ruvia::DbBinaryOperator::kEqual, query.value("operate")),
                integer(query, 2) },
              { query.binary(level, ruvia::DbBinaryOperator::kEqual, query.value("view")),
                integer(query, 1) } },
            integer(query, 0)
        );
    }

    static ruvia::DbQuery::Expr remoteControlEnabled(ruvia::DbQuery& query, ruvia::DbQuery::Expr params) {
        const auto key = textKey(query, "remote_control");
        const auto hasKey = query.binary(params, ruvia::DbBinaryOperator::kJsonHasKey, key);
        const auto value = query.call(
            "lower",
            { query.coalesce({ query.binary(params, ruvia::DbBinaryOperator::kJsonGetText, key), query.value("") }) }
        );
        return query.caseWhen(
            { { hasKey,
                query.caseWhen(
                    { { query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("true")),
                        boolean(query, true) },
                      { query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("t")),
                        boolean(query, true) },
                      { query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("1")),
                        boolean(query, true) },
                      { query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("yes")),
                        boolean(query, true) },
                      { query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("y")),
                        boolean(query, true) },
                      { query.binary(value, ruvia::DbBinaryOperator::kEqual, query.value("on")),
                        boolean(query, true) } },
                    boolean(query, false)
                ) } },
            boolean(query, true)
        );
    }

    static DeviceCapabilities capabilities(const DeviceActor& actor, DeviceAccessLevel level, bool remoteControl) {
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
            service::utils::parseInt64(std::optional<std::string_view>{ value }).value_or(0);
        if (parsed >= rankValueOf(DeviceAccessLevel::owner)) {
            return DeviceAccessLevel::owner;
        }
        if (parsed == rankValueOf(DeviceAccessLevel::operate)) {
            return DeviceAccessLevel::operate;
        }
        if (parsed == rankValueOf(DeviceAccessLevel::view)) {
            return DeviceAccessLevel::view;
        }
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

inline DeviceAccessService& deviceAccessService() {
    return DeviceAccessService::instance();
}

class DeviceService {
  public:
    static DeviceService& instance() {
        static thread_local DeviceService service;
        return service;
    }

    template <typename Context>
    ruvia::Task<DeviceCommandStatusDto> commandStatus(Context& context, std::string_view commandId) {
        co_await service::auth::AuthService::requirePermission(context, context.userId, "iot:device:command");
        co_return co_await readCommandStatus(context, commandId);
    }

    template <typename Context>
    ruvia::Task<service::device::DeviceCommandStatusesDto>
    commandStatuses(Context& context, std::string_view ids) {
        co_await service::auth::AuthService::requirePermission(context, context.userId, "iot:device:command");
        ruvia::BoxedArray<service::device::DeviceCommandStatusDto> statuses(
            ruvia::ModelOptions{ .resource = context.arena() }
        );
        bool complete = true;
        std::size_t offset = 0, count = 0;
        for (;;) {
            const auto end = ids.find(',', offset);
            const auto id = ids.substr(offset, end == std::string_view::npos ? end : end - offset);
            if (!service::common::isUuid(id) || ++count > 256) {
                service::common::fail(18012, "Invalid command ID list", 400);
            }
            auto result = co_await readCommandStatus(context, id);
            complete = complete && commandTerminalState(result.template get<"status">()->view());
            statuses.emplace(std::move(result));
            if (end == std::string_view::npos) {
                break;
            }
            offset = end + 1;
        }
        service::device::DeviceCommandStatusesDto result(ruvia::ModelOptions{ .resource = context.arena() });
        result.template set<"complete">(complete).template set<"statuses">(std::move(statuses));
        co_return result;
    }

    template <typename Context>
    ruvia::Task<DevicePageDataDto> list(Context& c) {
        const auto actor = co_await deviceAccessService().actor(c, c.userId);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addScopedDevicesCtes(query, actor);
        selectItemColumns(query);
        query.from("scoped_device", "d")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::LinkEntity::tableName(), query.binary(query.column(service::device::entities::LinkEntity::columnName<"id">(), "l"), ruvia::DbBinaryOperator::kEqual, query.column("link_id", "d")), "l")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::EdgeNodeEntity::tableName(), query.binary(query.column(service::device::entities::EdgeNodeEntity::columnName<"id">(), "en"), ruvia::DbBinaryOperator::kEqual, query.column(service::device::entities::LinkEntity::columnName<"edge_node_id">(), "l")), "en")
            .join(ruvia::DbJoinType::kInner, service::device::entities::DeviceModelEntity::tableName(), query.binary(query.column(service::device::entities::DeviceModelEntity::columnName<"device_id">(), "p"), ruvia::DbBinaryOperator::kEqual, query.column("id", "d")), "p")
            .where(query.binary(query.column("access_rank", "d"), ruvia::DbBinaryOperator::kGreater, DeviceAccessService::integer(query, 0)))
            .orderBy(query.column("group_id", "d"), ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kLast)
            .addOrderBy(query.column("created_at", "d"))
            .addOrderBy(query.column("id", "d"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceItemDto> items(ruvia::ModelOptions{ .resource = c.arena() });
        std::map<std::string, DeviceItemDto*, std::less<>> itemsById;
        for (const auto& row : rows) {
            auto& item = items.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            fillItem(c, item, row, actor);
            itemsById.emplace(std::string(row[0].value().value_or(std::string_view{})), &item);
        }
        co_await fillLatest(c, itemsById);
        DevicePageDataDto result(ruvia::ModelOptions{ .resource = c.arena() });
        result.template set<"list">(std::move(items)).template set<"total">(static_cast<std::int64_t>(rows.size()));
        co_return result;
    }

    template <typename Context>
    ruvia::Task<DeviceRealtimePageDto> realtime(Context& c) {
        const auto actor = co_await deviceAccessService().actor(c, c.userId);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addScopedDevicesCtes(query, actor);
        const auto executionIsEdge = query.binary(query.column(service::device::entities::LinkEntity::columnName<"execution">(), "l"), ruvia::DbBinaryOperator::kEqual, query.value("edge"));
        query.select({ DeviceAccessService::text(query, query.column("id", "d")), DeviceAccessService::jsonText(query, query.column("protocol_params", "d"), "device_code"), DeviceAccessService::remoteControlEnabled(query, query.column("protocol_params", "d")), query.column("access_rank", "d"), query.caseWhen({ { executionIsEdge, DeviceAccessService::text(query, query.column(service::device::entities::LinkEntity::columnName<"edge_node_id">(), "l")) } }), query.caseWhen({ { executionIsEdge, DeviceAccessService::jsonText(query, query.column(service::device::entities::LinkEntity::columnName<"endpoint">(), "l"), "transport") } }) })
            .from("scoped_device", "d")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::LinkEntity::tableName(), query.binary(query.column(service::device::entities::LinkEntity::columnName<"id">(), "l"), ruvia::DbBinaryOperator::kEqual, query.column("link_id", "d")), "l")
            .where(query.binary(query.column("access_rank", "d"), ruvia::DbBinaryOperator::kGreater, DeviceAccessService::integer(query, 0)))
            .orderBy(query.column("id", "d"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceRealtimeDto> items(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        std::map<std::string, DeviceRealtimeDto*, std::less<>> itemsById;
        for (const auto& row : rows) {
            const auto capabilities = DeviceAccessService::capabilities(
                actor,
                DeviceAccessService::rank(row[3].value().value_or(std::string_view{})),
                row[2].value().value_or(std::string_view{}) == "t"
            );
            auto& item = items.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.template set<"id">(row[0].value().value_or(std::string_view{}))
                .template set<"deviceCode">(row[1].value().value_or(std::string_view{}))
                .template set<"connected">(false)
                .template set<"connectionState">("disconnected")
                .template set<"elements">(ruvia::BoxedArray<DeviceElementDto>(
                    ruvia::ModelOptions{ .resource = c.arena() }
                ))
                .template set<"canEdit">(capabilities.canEdit)
                .template set<"canDelete">(capabilities.canDelete)
                .template set<"canShare">(capabilities.canShare)
                .template set<"canCommand">(capabilities.canCommand)
                .template set<"accessLevel">(capabilities.accessLevel);
            if (row[4].value().has_value()) {
                item.template set<"edgeNodeId">(row[4].value().value_or(std::string_view{}));
            }
            if (row[5].value().has_value()) {
                item.template set<"edgeTransport">(row[5].value().value_or(std::string_view{}));
            }
            itemsById.emplace(std::string(row[0].value().value_or(std::string_view{})), &item);
        }
        co_await fillLatest(c, itemsById);
        DeviceRealtimePageDto result(ruvia::ModelOptions{ .resource = c.arena() });
        result.template set<"list">(std::move(items)).template set<"total">(static_cast<std::int64_t>(rows.size()));
        co_return result;
    }

    template <typename Context>
    ruvia::Task<DeviceItemDto> detail(Context& c, std::string_view id) {
        const auto actor = co_await deviceAccessService().actor(c, c.userId);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addScopedDevicesCtes(query, actor);
        selectItemColumns(query);
        query.from("scoped_device", "d")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::LinkEntity::tableName(), query.binary(query.column(service::device::entities::LinkEntity::columnName<"id">(), "l"), ruvia::DbBinaryOperator::kEqual, query.column("link_id", "d")), "l")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::EdgeNodeEntity::tableName(), query.binary(query.column(service::device::entities::EdgeNodeEntity::columnName<"id">(), "en"), ruvia::DbBinaryOperator::kEqual, query.column(service::device::entities::LinkEntity::columnName<"edge_node_id">(), "l")), "en")
            .join(ruvia::DbJoinType::kInner, service::device::entities::DeviceModelEntity::tableName(), query.binary(query.column(service::device::entities::DeviceModelEntity::columnName<"device_id">(), "p"), ruvia::DbBinaryOperator::kEqual, query.column("id", "d")), "p")
            .where(andAll(query, query.binary(query.column("id", "d"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(query, id)), query.binary(query.column("access_rank", "d"), ruvia::DbBinaryOperator::kGreater, DeviceAccessService::integer(query, 0))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(18001, "设备不存在", 404);
        }
        DeviceItemDto item(ruvia::ModelOptions{ .resource = c.arena() });
        fillItem(c, item, rows.front(), actor);
        std::map<std::string, DeviceItemDto*, std::less<>> itemById{ { std::string(id), &item } };
        co_await fillLatest(c, itemById);
        co_await fillCommandOperations(c, itemById, id);
        co_return item;
    }

    template <typename Context>
    ruvia::Task<std::string> history(Context& c, std::string_view id, const DeviceHistoryQuery& body) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::view, c.userId);
        const auto start = body.template get<"startTime">().view();
        const auto end = body.template get<"endTime">().view();
        const auto requestedPage = static_cast<std::int64_t>(*body.template get<"page">());
        const auto requestedPageSize = static_cast<std::int64_t>(*body.template get<"pageSize">());
        const auto page = requestedPage > 0 ? requestedPage : std::int64_t{ 1 };
        const auto pageSize =
            requestedPageSize < 1 ? std::int64_t{ 20 }
                                  : std::min<std::int64_t>(requestedPageSize, 100);
        const auto offset = (page - 1) * pageSize;

        try {
            const auto rows = co_await c.db().query(
                historyQuery(c.pool(), id, start, end, pageSize, offset, page)
            );
            co_return rows.empty() ? std::string{ "{\"list\":[],\"total\":0}" }
                                   : std::string{ rows.front()[0].value().value_or(std::string_view{}) };
        } catch (const std::exception&) {
            service::common::fail(18002, "时间范围格式错误", 400);
        }
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<DeviceOptionDto>> options(Context& c) {
        const auto actor = co_await deviceAccessService().actor(c, c.userId);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addScopedDevicesCtes(query, actor);
        query.select({ DeviceAccessService::text(query, query.column("id")), query.column("name"), DeviceAccessService::jsonText(query, query.column("protocol_params"), "device_code"), DeviceAccessService::remoteControlEnabled(query, query.column("protocol_params")), query.column("access_rank") })
            .from("scoped_device")
            .where(andAll(query, query.binary(query.column("access_rank"), ruvia::DbBinaryOperator::kGreater, DeviceAccessService::integer(query, 0)), query.binary(query.column("status"), ruvia::DbBinaryOperator::kEqual, query.value("enabled"))))
            .orderBy(query.column("name"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceOptionDto> result(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            const auto capabilities = DeviceAccessService::capabilities(
                actor,
                DeviceAccessService::rank(row[4].value().value_or(std::string_view{})),
                row[3].value().value_or(std::string_view{}) == "t"
            );
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.template set<"id">(row[0].value().value_or(std::string_view{}))
                .template set<"name">(row[1].value().value_or(std::string_view{}))
                .template set<"deviceCode">(row[2].value().value_or(std::string_view{}))
                .template set<"canEdit">(capabilities.canEdit)
                .template set<"canDelete">(capabilities.canDelete)
                .template set<"canShare">(capabilities.canShare)
                .template set<"canCommand">(capabilities.canCommand)
                .template set<"accessLevel">(capabilities.accessLevel);
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<void> create(Context& c, const SaveDeviceBody& body) {
        co_await validate(c, body, true);
        co_await ensureUnique(c, body, std::nullopt);
        co_await validateRuntimeIdentity(c, body, std::nullopt);
        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        const std::string name(body.template get<"name">()->view());
        const std::string deviceCode(body.template get<"deviceCode">()->view());
        const std::string linkId = str(body.template get<"linkId">());
        ruvia::DbQuery channelQuery(c.pool());
        channelQuery
            .select(channelQuery.coalesce({ DeviceAccessService::text(channelQuery, channelQuery.column(service::device::entities::LinkEntity::columnName<"edge_node_id">())), channelQuery.value("") }))
            .from(service::device::entities::LinkEntity::tableName())
            .where(andAll(channelQuery, channelQuery.binary(channelQuery.column(service::device::entities::LinkEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(channelQuery, linkId)), channelQuery.unary(ruvia::DbUnaryOperator::kIsNull, channelQuery.column(service::device::entities::LinkEntity::columnName<"deleted_at">()))));
        const auto channel = co_await c.db().query(channelQuery);
        if (channel.empty()) {
            service::common::fail(18003, "通道不存在", 400);
        }
        const std::string edgeNodeId(channel.front()[0].value().value_or(""));
        const std::string targetId = str(body.template get<"targetId">());
        const std::string protocolConfigId(body.template get<"protocolConfigId">()->view());
        const std::string groupId = str(body.template get<"groupId">());
        const std::string status = body.template get<"status">() ? std::string(body.template get<"status">()->view()) : "enabled";
        const std::int64_t onlineTimeout =
            body.template get<"onlineTimeout">() ? static_cast<std::int64_t>(*body.template get<"onlineTimeout">()) : 300;
        const std::string remoteControl =
            (!body.template get<"remoteControl">() || *body.template get<"remoteControl">()) ? "true" : "false";
        const std::string modbusMode = str(body.template get<"modbusMode">());
        const std::string slaveId =
            body.template get<"slaveId">() ? std::to_string(static_cast<std::int64_t>(*body.template get<"slaveId">())) : "";
        const std::string timezone = (body.template get<"timezone">() && !body.template get<"timezone">()->view().empty())
            ? std::string(body.template get<"timezone">()->view())
            : "+08:00";
        const std::string heartbeat = packetJson(body.template get<"heartbeat">());
        const std::string registration =
            edgeNodeId.empty() ? packetJson(body.template get<"registration">()) : R"({"mode":"OFF"})";
        const std::string remark = str(body.template get<"remark">());
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery insert(c.pool());
        const auto jsonb = [&](std::string_view value) {
            return insert.cast(insert.value(value), ruvia::DbDataType::kJsonb);
        };
        const auto emptyPacket = jsonb(R"({"mode":"OFF"})");
        const auto protocolParams = insert.call(
            "jsonb_strip_nulls",
            { insert.call("jsonb_build_object", { DeviceAccessService::textKey(insert, "device_code"), DeviceAccessService::text(insert, insert.value(deviceCode)), DeviceAccessService::textKey(insert, "target_id"), insert.nullIf(DeviceAccessService::text(insert, insert.value(targetId)), insert.value("")), DeviceAccessService::textKey(insert, "online_timeout"), insert.cast(insert.value(onlineTimeout), ruvia::DbDataType::kInteger), DeviceAccessService::textKey(insert, "remote_control"), insert.cast(insert.value(remoteControl == "true"), ruvia::DbDataType::kBoolean), DeviceAccessService::textKey(insert, "modbus_mode"), insert.nullIf(DeviceAccessService::text(insert, insert.value(modbusMode)), insert.value("")), DeviceAccessService::textKey(insert, "slave_id"), insert.cast(insert.nullIf(DeviceAccessService::text(insert, insert.value(slaveId)), insert.value("")), ruvia::DbDataType::kInteger), DeviceAccessService::textKey(insert, "timezone"), DeviceAccessService::text(insert, insert.value(timezone)), DeviceAccessService::textKey(insert, "heartbeat"), insert.coalesce({ insert.cast(insert.nullIf(DeviceAccessService::text(insert, insert.value(heartbeat)), insert.value("")), ruvia::DbDataType::kJsonb), emptyPacket }), DeviceAccessService::textKey(insert, "registration"), insert.coalesce({ insert.cast(insert.nullIf(DeviceAccessService::text(insert, insert.value(registration)), insert.value("")), ruvia::DbDataType::kJsonb), emptyPacket }) }) }
        );
        insert.insertInto(service::device::entities::DeviceEntity::tableName(), { "id", "name", "link_id", "protocol_config_id", "group_id", "status", "protocol_params", "remark", "created_by" })
            .values({ DeviceAccessService::uuid(insert, id), insert.value(name), DeviceAccessService::uuid(insert, linkId), DeviceAccessService::uuid(insert, protocolConfigId), DeviceAccessService::nullableUuid(insert, groupId), insert.value(status), protocolParams, insert.nullIf(DeviceAccessService::text(insert, insert.value(remark)), insert.value("")), DeviceAccessService::uuid(insert, c.userId) });
        (void)co_await transaction.execute(insert);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "device", "created", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
        try {
            (void)co_await service::rpc::call(c, "telemetry", "initialize", id + "\n" + deviceCode);
        } catch (...) {
            // PostgreSQL is authoritative; startup hydration or the first report repairs Redis.
        }
        if (!edgeNodeId.empty()) {
            (void)co_await service::edge::EdgeService::queueSnapshot(c, edgeNodeId, c.userId);
        }
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<DeviceDebugAcquisitionDto>> debugPackets(Context& c, std::string_view id) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::owner, c.userId);
        const auto key = service::device::entities::DeviceDebugIndex::key(id);
        static constexpr std::string_view readPackets = R"lua(
local result={}
local count=0
for _,acquisition in ipairs(redis.call('ZREVRANGE',KEYS[1],0,99)) do
    local round='iot:debug:v4:acquisition:'..acquisition
    local metadata=redis.call('HGETALL',round)
    if #metadata==0 then
        redis.call('ZREM',KEYS[1],acquisition)
    else
        local ids=redis.call('ZRANGE',round..':packets',0,-1)
        if count+#ids>4096 then break end
        local packets={}
        for _,id in ipairs(ids) do
            local fields=redis.call('HGETALL','iot:debug:v4:packet:'..id)
            if #fields>0 then packets[#packets+1]={id,fields} end
        end
        result[#result+1]={acquisition,metadata,packets}
        count=count+#packets
    end
end
return result
)lua";
        const std::vector<std::string_view> keys{ key };
        const std::vector<std::string_view> args;
        const auto reply = co_await c.redis().eval(readPackets, keys, args);
        if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
            service::message::redis::throwValue("read debug packets", reply);
        }
        ruvia::BoxedArray<DeviceDebugAcquisitionDto> result(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : reply.array()) {
            if (row.kind() != ruvia::RedisValue::Kind::kArray || row.array().size() != 3) {
                continue;
            }
            auto& acquisition = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            acquisition.template set<"id">(row.array()[0].string());
            const auto metadata = row.array()[1].array();
            for (std::size_t index = 0; index + 1 < metadata.size(); index += 2) {
                const auto field = metadata[index].string();
                const auto value = metadata[index + 1].string();
                if (field == "started_at_ms") {
                    acquisition.template set<"startedAtMs">(value);
                }
                if (field == "finished_at_ms") {
                    acquisition.template set<"finishedAtMs">(value);
                }
                if (field == "last_packet_at_ms") {
                    acquisition.template set<"lastPacketAtMs">(value);
                }
                if (field == "state") {
                    acquisition.template set<"state">(value);
                }
                if (field == "device_id") {
                    acquisition.template set<"deviceId">(value);
                }
            }
            ruvia::BoxedArray<DeviceDebugPacketDto> packets(ruvia::ModelOptions{ .resource = c.arena() });
            for (const auto& packetRow : row.array()[2].array()) {
                const auto fields = packetRow.array()[1].array();
                std::string eventId(packetRow.array()[0].string());
                auto& packet = packets.emplace(ruvia::ModelOptions{ .resource = c.arena() });
                packet.template set<"id">(eventId);
                for (std::size_t index = 0; index + 1 < fields.size(); index += 2) {
                    const auto name = fields[index].string();
                    const auto value = fields[index + 1].string();
                    if (name == "acquisition_id") {
                        packet.template set<"acquisitionId">(value);
                    } else if (name == "device_id") {
                        packet.template set<"deviceId">(value);
                    } else if (name == "direction") {
                        packet.template set<"direction">(value);
                    } else if (name == "source") {
                        packet.template set<"source">(value);
                    } else if (name == "address") {
                        packet.template set<"address">(value);
                    } else if (name == "edge_node_id") {
                        packet.template set<"edgeNodeId">(value);
                    } else if (name == "edge_node_name") {
                        packet.template set<"edgeNodeName">(value);
                    } else if (name == "payload_hex") {
                        packet.template set<"payloadHex">(value);
                    } else if (name == "time_ms") {
                        packet.template set<"timeMs">(value);
                    } else if (name == "transport_status") {
                        packet.template set<"transportStatus">(value);
                    } else if (name == "response_status") {
                        packet.template set<"responseStatus">(value);
                    } else if (name == "parse_status") {
                        packet.template set<"parseStatus">(value);
                    } else if (name == "revision") {
                        packet.template set<"revision">(value);
                    } else if (name == "reply_to_packet_id") {
                        packet.template set<"replyToPacketId">(value);
                    } else if (name == "reason") {
                        packet.template set<"reason">(value);
                    } else if (name == "parsed_json") {
                        packet.template set<"parsedJson">(value);
                    }
                }
            }
            acquisition.template set<"packets">(std::move(packets));
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<void> setDebug(Context& c, std::string_view id, bool enabled) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::owner, c.userId);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery query(c.pool());
        query.update(service::device::entities::DeviceEntity::tableName())
            .set(service::device::entities::DeviceEntity::columnName<"debug_enabled">(), query.value(enabled))
            .set("updated_at", query.call("now"))
            .where((service::device::entities::DeviceEntity::column<"id">() == id && service::device::entities::DeviceEntity::column<"deleted_at">().isNull()).expression(query));
        (void)co_await transaction.execute(query);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "device", "updated", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
        const auto device = co_await detail(c, id);
        if (device.template get<"edgeNodeId">() && !device.template get<"edgeNodeId">()->view().empty()) {
            (void)co_await service::edge::EdgeService::queueSnapshot(c, device.template get<"edgeNodeId">()->view(), c.userId);
        }
    }

    template <typename Context>
    ruvia::Task<void> update(Context& c, std::string_view id, const SaveDeviceBody& body) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::owner, c.userId);
        ruvia::DbQuery currentQuery(c.pool());
        currentQuery
            .select({ DeviceAccessService::text(currentQuery, currentQuery.column(service::device::entities::DeviceEntity::columnName<"link_id">(), "d")), currentQuery.coalesce({ DeviceAccessService::text(currentQuery, currentQuery.column(service::device::entities::LinkEntity::columnName<"edge_node_id">(), "l")), currentQuery.value("") }), DeviceAccessService::text(currentQuery, currentQuery.column(service::device::entities::DeviceEntity::columnName<"protocol_config_id">(), "d")), DeviceAccessService::jsonText(currentQuery, currentQuery.column(service::device::entities::DeviceEntity::columnName<"protocol_params">(), "d"), "device_code"), currentQuery.column(service::device::entities::LinkEntity::columnName<"execution">(), "l"), DeviceAccessService::text(currentQuery, currentQuery.column(service::device::entities::DeviceEntity::columnName<"status">(), "d")) })
            .from(service::device::entities::DeviceEntity::tableName(), "d")
            .join(ruvia::DbJoinType::kInner, service::device::entities::LinkEntity::tableName(), currentQuery.binary(currentQuery.column(service::device::entities::LinkEntity::columnName<"id">(), "l"), ruvia::DbBinaryOperator::kEqual, currentQuery.column(service::device::entities::DeviceEntity::columnName<"link_id">(), "d")), "l")
            .where(andAll(currentQuery, currentQuery.binary(currentQuery.column(service::device::entities::DeviceEntity::columnName<"id">(), "d"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(currentQuery, id)), currentQuery.unary(ruvia::DbUnaryOperator::kIsNull, currentQuery.column(service::device::entities::DeviceEntity::columnName<"deleted_at">(), "d"))));
        const auto rows = co_await c.db().query(currentQuery);
        if (rows.empty()) {
            service::common::fail(18001, "设备不存在", 404);
        }
        co_await validate(c, body, false);

        const auto& current = rows.front();
        const std::string currentLinkId(
            current[0].value().value_or(std::string_view{})
        );
        const std::string currentEdgeNodeId(
            current[1].value().value_or(std::string_view{})
        );
        const std::string currentProtocolConfigId(
            current[2].value().value_or(std::string_view{})
        );
        const std::string currentExecution(
            current[4].value().value_or(std::string_view{})
        );
        const std::string requestedLinkId = str(body.template get<"linkId">());
        const std::string targetLinkId = requestedLinkId.empty() ? currentLinkId : requestedLinkId;
        const std::string targetProtocolConfigId = body.template get<"protocolConfigId">() ? str(body.template get<"protocolConfigId">()) : currentProtocolConfigId;
        ruvia::DbQuery targetQuery(c.pool());
        targetQuery
            .select(targetQuery.coalesce({ DeviceAccessService::text(targetQuery, targetQuery.column(service::device::entities::LinkEntity::columnName<"edge_node_id">(), "l")), targetQuery.value("") }))
            .from(service::device::entities::LinkEntity::tableName(), "l")
            .join(ruvia::DbJoinType::kInner, service::device::entities::ProtocolConfigEntity::tableName(), targetQuery.binary(targetQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"protocol">(), "p"), ruvia::DbBinaryOperator::kEqual, targetQuery.column(service::device::entities::LinkEntity::columnName<"protocol">(), "l")), "p")
            .where(andAll(targetQuery, targetQuery.binary(targetQuery.column(service::device::entities::LinkEntity::columnName<"id">(), "l"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(targetQuery, targetLinkId)), targetQuery.binary(targetQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"id">(), "p"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(targetQuery, targetProtocolConfigId)), targetQuery.unary(ruvia::DbUnaryOperator::kIsNull, targetQuery.column(service::device::entities::LinkEntity::columnName<"deleted_at">(), "l")), targetQuery.unary(ruvia::DbUnaryOperator::kIsNull, targetQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"deleted_at">(), "p"))));
        const auto target = co_await c.db().query(targetQuery);
        if (target.empty()) {
            service::common::fail(18003, "通道或设备类型不存在，或协议不一致", 400);
        }
        const std::string targetEdgeNodeId(target.front()[0].value().value_or(""));
        const bool targetEdge = !targetEdgeNodeId.empty();
        const bool connectionChanged = targetLinkId != currentLinkId || targetProtocolConfigId != currentProtocolConfigId;
        co_await ensureUnique(c, body, std::string(id));
        co_await validateRuntimeIdentity(c, body, std::string(id));

        ruvia::DbQuery update(c.pool());
        update.update(service::device::entities::DeviceEntity::tableName());
        bool changed = false;
        const auto assign = [&](std::string_view column, ruvia::DbQuery::Expr value) {
            update.set(column, value);
            changed = true;
        };
        if (body.template get<"name">()) {
            assign("name", update.value(body.template get<"name">()->view()));
        }
        if (targetLinkId != currentLinkId) {
            assign("link_id", DeviceAccessService::uuid(update, targetLinkId));
        }
        if (body.template get<"protocolConfigId">()) {
            assign("protocol_config_id", DeviceAccessService::uuid(update, targetProtocolConfigId));
        }
        if (body.template get<"groupId">()) {
            assign("group_id", DeviceAccessService::nullableUuid(update, body.template get<"groupId">()->view()));
        }
        if (body.template get<"status">()) {
            assign("status", update.value(body.template get<"status">()->view()));
        }
        auto protocolParams = update.column(service::device::entities::DeviceEntity::columnName<"protocol_params">());
        const auto jsonPath = [&](std::string_view key) {
            return update.cast(update.array({ DeviceAccessService::textKey(update, key) }), ruvia::DbTypeDefinition{ .dataType = ruvia::DbDataType::kText, .array = true });
        };
        const auto jsonValue = [&](std::string_view key, ruvia::DbQuery::Expr value) {
            protocolParams = update.call(
                "jsonb_set",
                { protocolParams, jsonPath(key), update.call("to_jsonb", { value }), DeviceAccessService::boolean(update, true) }
            );
            changed = true;
        };
        const auto jsonDocument = [&](std::string_view key, std::string_view value) {
            protocolParams = update.call(
                "jsonb_set",
                { protocolParams, jsonPath(key), update.cast(update.value(value), ruvia::DbDataType::kJsonb), DeviceAccessService::boolean(update, true) }
            );
            changed = true;
        };
        if (body.template get<"deviceCode">()) {
            jsonValue("device_code", DeviceAccessService::text(update, update.value(body.template get<"deviceCode">()->view())));
        }
        if (body.template get<"targetId">()) {
            jsonValue("target_id", DeviceAccessService::text(update, update.value(body.template get<"targetId">()->view())));
        } else if (connectionChanged) {
            protocolParams = update.binary(protocolParams, ruvia::DbBinaryOperator::kJsonDelete, DeviceAccessService::textKey(update, "target_id")),
            changed = true;
        }
        if (body.template get<"onlineTimeout">()) {
            jsonValue("online_timeout", update.cast(update.value(static_cast<std::int64_t>(*body.template get<"onlineTimeout">())), ruvia::DbDataType::kBigInt));
        }
        if (body.template get<"remoteControl">()) {
            jsonValue("remote_control", update.cast(update.value(static_cast<bool>(*body.template get<"remoteControl">())), ruvia::DbDataType::kBoolean));
        }
        if (body.template get<"modbusMode">()) {
            jsonValue("modbus_mode", DeviceAccessService::text(update, update.value(body.template get<"modbusMode">()->view())));
        }
        if (body.template get<"slaveId">()) {
            jsonValue("slave_id", update.cast(update.value(static_cast<std::int64_t>(*body.template get<"slaveId">())), ruvia::DbDataType::kBigInt));
        }
        if (body.template get<"timezone">()) {
            jsonValue("timezone", DeviceAccessService::text(update, update.value(body.template get<"timezone">()->view())));
        }
        std::string heartbeat;
        if (body.template get<"heartbeat">()) {
            heartbeat = packetJson(body.template get<"heartbeat">());
            jsonDocument("heartbeat", heartbeat);
        }
        std::string registration;
        if (targetEdge) {
            registration = R"({"mode":"OFF"})";
            jsonDocument("registration", registration);
        } else if (body.template get<"registration">()) {
            registration = packetJson(body.template get<"registration">());
            jsonDocument("registration", registration);
        }
        if (changed) {
            assign("protocol_params", protocolParams);
        }
        if (body.template get<"remark">()) {
            assign("remark", update.nullIf(DeviceAccessService::text(update, update.value(body.template get<"remark">()->view())), update.value("")));
        }

        {
            auto transaction = co_await c.db().beginTransaction();
            if (changed) {
                assign("updated_at", update.call("now"));
            }
            if (changed) {
                update.where(update.binary(update.column(service::device::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(update, id)));
            }
            if (changed) {
                (void)co_await transaction.execute(update);
            }
            co_await service::system::OutboxService::enqueueConfigEvent(transaction, "device", "updated", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
            co_await transaction.commit();
        }
        try {
            (void)co_await service::rpc::call(c, "telemetry", "project-device", std::string(id));
        } catch (...) {
            // PostgreSQL remains authoritative; startup hydration repairs Redis read models.
        }
        if (!currentEdgeNodeId.empty()) {
            (void)co_await service::edge::EdgeService::queueSnapshot(
                c,
                currentEdgeNodeId,
                c.userId
            );
        }
        if (!targetEdgeNodeId.empty() && targetEdgeNodeId != currentEdgeNodeId) {
            (void)co_await service::edge::EdgeService::queueSnapshot(
                c,
                targetEdgeNodeId,
                c.userId
            );
        }
    }

    template <typename Context>
    ruvia::Task<void> remove(Context& c, std::string_view id) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::owner, c.userId);
        ruvia::DbQuery currentQuery(c.pool());
        currentQuery
            .select({ DeviceAccessService::jsonText(currentQuery, currentQuery.column(service::device::entities::DeviceEntity::columnName<"protocol_params">(), "d"), "device_code"), currentQuery.coalesce({ DeviceAccessService::text(currentQuery, currentQuery.column(service::device::entities::LinkEntity::columnName<"edge_node_id">(), "l")), currentQuery.value("") }), DeviceAccessService::text(currentQuery, currentQuery.column(service::device::entities::DeviceEntity::columnName<"link_id">(), "d")), currentQuery.column(service::device::entities::LinkEntity::columnName<"execution">(), "l") })
            .from(service::device::entities::DeviceEntity::tableName(), "d")
            .join(ruvia::DbJoinType::kInner, service::device::entities::LinkEntity::tableName(), currentQuery.binary(currentQuery.column(service::device::entities::LinkEntity::columnName<"id">(), "l"), ruvia::DbBinaryOperator::kEqual, currentQuery.column(service::device::entities::DeviceEntity::columnName<"link_id">(), "d")), "l")
            .where(andAll(currentQuery, currentQuery.binary(currentQuery.column(service::device::entities::DeviceEntity::columnName<"id">(), "d"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(currentQuery, id)), currentQuery.unary(ruvia::DbUnaryOperator::kIsNull, currentQuery.column(service::device::entities::DeviceEntity::columnName<"deleted_at">(), "d"))));
        const auto rows = co_await c.db().query(currentQuery);
        if (rows.empty()) {
            service::common::fail(18001, "设备不存在", 404);
        }
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery removeQuery(c.pool());
        removeQuery.update(service::device::entities::DeviceEntity::tableName())
            .set(service::device::entities::DeviceEntity::columnName<"deleted_at">(), removeQuery.call("now"))
            .set(service::device::entities::DeviceEntity::columnName<"updated_at">(), removeQuery.call("now"))
            .where(removeQuery.binary(removeQuery.column(service::device::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(removeQuery, id)));
        (void)co_await transaction.execute(removeQuery);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "device", "deleted", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
        try {
            (void)co_await service::rpc::call(c, "telemetry", "erase-device", std::string(id));
        } catch (...) {
            // The next startup hydration removes stale Redis state for deleted devices.
        }
        if (!rows.front()[1].value().value_or(std::string_view{}).empty()) {
            (void)co_await service::edge::EdgeService::queueSnapshot(
                c,
                rows.front()[1].value().value_or(std::string_view{}),
                c.userId
            );
        }
    }

    // ===== 设备分组（合并入同一 DeviceService 类）=====

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<DeviceGroupItemDto>> listGroups(Context& c, bool withCount) {
        const auto actor = co_await deviceAccessService().actor(c, c.userId);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addVisibleGroupsCtes(query, actor);
        ruvia::DbQuery visible(query.resource());
        visible.select(visible.column("id")).from("visible_group");
        ruvia::DbQuery count(query.resource());
        count.select(count.aggregate("count", { count.star() }))
            .from("scoped_device", "scoped")
            .where(andAll(count, count.binary(count.column("group_id", "scoped"), ruvia::DbBinaryOperator::kEqual, count.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "g")), count.binary(count.column("access_rank", "scoped"), ruvia::DbBinaryOperator::kGreater, DeviceAccessService::integer(count, 0))));
        query.select({ DeviceAccessService::text(query, query.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "g")), query.column(service::device::entities::DeviceGroupEntity::columnName<"name">(), "g"), query.coalesce({ DeviceAccessService::text(query, query.column(service::device::entities::DeviceGroupEntity::columnName<"parent_id">(), "g")), query.value("") }), query.column(service::device::entities::DeviceGroupEntity::columnName<"status">(), "g"), query.column(service::device::entities::DeviceGroupEntity::columnName<"sort_order">(), "g"), query.coalesce({ query.column(service::device::entities::DeviceGroupEntity::columnName<"remark">(), "g"), query.value("") }), withCount ? query.subquery(count) : DeviceAccessService::integer(query, 0), query.call("iot_utc_timestamp", { query.column(service::device::entities::DeviceGroupEntity::columnName<"created_at">(), "g") }), query.call("iot_utc_timestamp", { query.column(service::device::entities::DeviceGroupEntity::columnName<"updated_at">(), "g") }), DeviceAccessService::text(query, query.column(service::device::entities::DeviceGroupEntity::columnName<"created_by">(), "g")) })
            .from(service::device::entities::DeviceGroupEntity::tableName(), "g")
            .where(andAll(query, query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">(), "g")), query.binary(query.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "g"), ruvia::DbBinaryOperator::kIn, query.subquery(visible))))
            .orderBy(query.column(service::device::entities::DeviceGroupEntity::columnName<"sort_order">(), "g"))
            .addOrderBy(query.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "g"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceGroupItemDto> result(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            fillGroup(result.emplace(ruvia::ModelOptions{ .resource = c.arena() }), row, actor);
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<DeviceGroupItemDto> groupDetail(Context& c, std::string_view id) {
        const auto actor = co_await deviceAccessService().actor(c, c.userId);
        ruvia::DbQuery query(c.pool());
        DeviceAccessService::addVisibleGroupsCtes(query, actor);
        ruvia::DbQuery visible(query.resource());
        visible.select(visible.column("id")).from("visible_group");
        ruvia::DbQuery count(query.resource());
        count.select(count.aggregate("count", { count.star() }))
            .from("scoped_device", "scoped")
            .where(andAll(count, count.binary(count.column("group_id", "scoped"), ruvia::DbBinaryOperator::kEqual, count.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "group_entry")), count.binary(count.column("access_rank", "scoped"), ruvia::DbBinaryOperator::kGreater, DeviceAccessService::integer(count, 0))));
        query.select({ DeviceAccessService::text(query, query.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "group_entry")), query.column(service::device::entities::DeviceGroupEntity::columnName<"name">(), "group_entry"), query.coalesce({ DeviceAccessService::text(query, query.column(service::device::entities::DeviceGroupEntity::columnName<"parent_id">(), "group_entry")), query.value("") }), query.column(service::device::entities::DeviceGroupEntity::columnName<"status">(), "group_entry"), query.column(service::device::entities::DeviceGroupEntity::columnName<"sort_order">(), "group_entry"), query.coalesce({ query.column(service::device::entities::DeviceGroupEntity::columnName<"remark">(), "group_entry"), query.value("") }), query.subquery(count), query.call("iot_utc_timestamp", { query.column(service::device::entities::DeviceGroupEntity::columnName<"created_at">(), "group_entry") }), query.call("iot_utc_timestamp", { query.column(service::device::entities::DeviceGroupEntity::columnName<"updated_at">(), "group_entry") }), DeviceAccessService::text(query, query.column(service::device::entities::DeviceGroupEntity::columnName<"created_by">(), "group_entry")) })
            .from(service::device::entities::DeviceGroupEntity::tableName(), "group_entry")
            .where(andAll(query, query.binary(query.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "group_entry"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(query, id)), query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">(), "group_entry")), query.binary(query.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "group_entry"), ruvia::DbBinaryOperator::kIn, query.subquery(visible))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17001, "设备分组不存在", 404);
        }
        DeviceGroupItemDto item(ruvia::ModelOptions{ .resource = c.arena() });
        fillGroup(item, rows.front(), actor);
        co_return item;
    }

    template <typename Context>
    ruvia::Task<void> createGroup(Context& c, const SaveDeviceGroupBody& body) {
        co_await validateParent(c, body, std::nullopt);
        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        const std::string name(body.template get<"name">()->view());
        const std::string parentId = body.template get<"parentId">() ? std::string(body.template get<"parentId">()->view()) : "";
        const std::string status = body.template get<"status">() ? std::string(body.template get<"status">()->view()) : "enabled";
        const std::int64_t sortOrder =
            body.template get<"sortOrder">() ? static_cast<std::int64_t>(*body.template get<"sortOrder">()) : 0;
        const std::string remark = body.template get<"remark">() ? std::string(body.template get<"remark">()->view()) : "";
        ruvia::DbQuery insert(c.pool());
        insert.insertInto(service::device::entities::DeviceGroupEntity::tableName(), { "id", "name", "parent_id", "status", "sort_order", "remark", "created_by" })
            .values({ DeviceAccessService::uuid(insert, id), insert.value(name), DeviceAccessService::nullableUuid(insert, parentId), insert.value(status), insert.value(sortOrder), insert.nullIf(DeviceAccessService::text(insert, insert.value(remark)), insert.value("")), DeviceAccessService::uuid(insert, c.userId) });
        (void)co_await c.db().execute(insert);
    }

    template <typename Context>
    ruvia::Task<void> updateGroup(Context& c, std::string_view id, const SaveDeviceGroupBody& body) {
        ruvia::DbQuery currentQuery(c.pool());
        currentQuery.select(currentQuery.column(service::device::entities::DeviceGroupEntity::columnName<"created_by">()))
            .from(service::device::entities::DeviceGroupEntity::tableName())
            .where(andAll(currentQuery, currentQuery.binary(currentQuery.column(service::device::entities::DeviceGroupEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(currentQuery, id)), currentQuery.unary(ruvia::DbUnaryOperator::kIsNull, currentQuery.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">()))));
        const auto rows = co_await c.db().query(currentQuery);
        if (rows.empty()) {
            service::common::fail(17001, "设备分组不存在", 404);
        }
        co_await requireGroupOwner(c, rows.front()[0].value().value_or(std::string_view{}));
        co_await validateParent(c, body, std::string(id));

        ruvia::DbQuery update(c.pool());
        update.update(service::device::entities::DeviceGroupEntity::tableName());
        bool changed = false;
        const auto assign = [&](std::string_view column, ruvia::DbQuery::Expr value) {
            update.set(column, value);
            changed = true;
        };
        if (body.template get<"name">()) {
            assign("name", update.value(body.template get<"name">()->view()));
        }
        if (body.template get<"parentId">()) {
            assign("parent_id", DeviceAccessService::nullableUuid(update, body.template get<"parentId">()->view()));
        }
        if (body.template get<"status">()) {
            assign("status", update.value(body.template get<"status">()->view()));
        }
        if (body.template get<"sortOrder">()) {
            assign("sort_order", update.value(static_cast<std::int64_t>(*body.template get<"sortOrder">())));
        }
        if (body.template get<"remark">()) {
            assign("remark", update.nullIf(DeviceAccessService::text(update, update.value(body.template get<"remark">()->view())), update.value("")));
        }
        if (!changed) {
            co_return;
        }
        update.set(service::device::entities::DeviceGroupEntity::columnName<"updated_at">(), update.call("now"))
            .where(update.binary(update.column(service::device::entities::DeviceGroupEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(update, id)));
        (void)co_await c.db().execute(update);
    }

    template <typename Context>
    ruvia::Task<void> removeGroup(Context& c, std::string_view id) {
        ruvia::DbQuery currentQuery(c.pool());
        currentQuery.select(currentQuery.column(service::device::entities::DeviceGroupEntity::columnName<"created_by">()))
            .from(service::device::entities::DeviceGroupEntity::tableName())
            .where(andAll(currentQuery, currentQuery.binary(currentQuery.column(service::device::entities::DeviceGroupEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(currentQuery, id)), currentQuery.unary(ruvia::DbUnaryOperator::kIsNull, currentQuery.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">()))));
        const auto rows = co_await c.db().query(currentQuery);
        if (rows.empty()) {
            service::common::fail(17001, "设备分组不存在", 404);
        }
        co_await requireGroupOwner(c, rows.front()[0].value().value_or(std::string_view{}));
        ruvia::DbQuery childGroups(c.pool());
        childGroups.select(DeviceAccessService::integer(childGroups, 1))
            .from(service::device::entities::DeviceGroupEntity::tableName())
            .where(andAll(childGroups, childGroups.binary(childGroups.column(service::device::entities::DeviceGroupEntity::columnName<"parent_id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(childGroups, id)), childGroups.unary(ruvia::DbUnaryOperator::kIsNull, childGroups.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">()))));
        ruvia::DbQuery childDevices(c.pool());
        childDevices.select(DeviceAccessService::integer(childDevices, 1))
            .from(service::device::entities::DeviceEntity::tableName())
            .where(andAll(childDevices, childDevices.binary(childDevices.column(service::device::entities::DeviceEntity::columnName<"group_id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(childDevices, id)), childDevices.unary(ruvia::DbUnaryOperator::kIsNull, childDevices.column(service::device::entities::DeviceEntity::columnName<"deleted_at">()))));
        ruvia::DbQuery used(c.pool());
        used.select(used.binary(used.exists(childGroups), ruvia::DbBinaryOperator::kOr, used.exists(childDevices)));
        const auto usedRows = co_await c.db().query(used);
        if (usedRows.front()[0].value().value_or(std::string_view{}) == "t") {
            service::common::fail(17004, "请先移除子分组和设备", 409);
        }
        ruvia::DbQuery removeQuery(c.pool());
        removeQuery.update(service::device::entities::DeviceGroupEntity::tableName())
            .set(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">(), removeQuery.call("now"))
            .set(service::device::entities::DeviceGroupEntity::columnName<"updated_at">(), removeQuery.call("now"))
            .where(removeQuery.binary(removeQuery.column(service::device::entities::DeviceGroupEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(removeQuery, id)));
        (void)co_await c.db().execute(removeQuery);
    }

  private:
    template <typename Context>
    ruvia::Task<service::device::DeviceCommandStatusDto> readCommandStatus(Context& context, std::string_view commandId) {
        const auto fields =
            co_await loadCommandStatus(context, commandId);
        if (fields.empty()) {
            service::common::fail(18012, "下发记录不存在", 404);
        }
        const auto deviceId = commandField(fields, "device_id");
        if (deviceId.empty()) {
            service::common::fail(18012, "下发状态数据无效", 500);
        }
        (void)co_await deviceAccessService().require(
            context,
            deviceId,
            service::device::DeviceAccessLevel::operate,
            context.userId
        );

        service::device::DeviceCommandStatusDto result(ruvia::ModelOptions{ .resource = context.arena() });
        fillCommandStatus(result, commandId, fields);
        co_return result;
    }

    static std::string commandActualValueField(std::size_t index, std::string_view name) {
        return "actual_value_" + std::to_string(index) + "_" + std::string(name);
    }

    static void fillCommandStatus(service::device::DeviceCommandStatusDto& result, std::string_view commandId, const std::vector<message::StreamField>& fields) {
        result.template set<"commandId">(commandId)
            .template set<"deviceId">(commandField(fields, "device_id"))
            .template set<"deviceCode">(commandField(fields, "device_code"))
            .template set<"protocol">(commandField(fields, "protocol"))
            .template set<"status">(commandField(fields, "status"));
        const auto reason = commandField(fields, "reason");
        if (!reason.empty()) {
            result.template set<"reason">(reason);
        }
        const auto createdAt = commandInteger(fields, "created_at_ms");
        if (createdAt != 0) {
            result.template set<"createdAtMs">(createdAt);
        }
        const auto completedAt = commandInteger(fields, "completed_at_ms");
        if (completedAt != 0) {
            result.template set<"completedAtMs">(completedAt);
        }
        const auto actualCount =
            std::clamp<std::int64_t>(commandInteger(fields, "actual_value_count"), 0, 8);
        if (actualCount != 0) {
            ruvia::BoxedArray<service::device::DeviceCommandActualValueDto> actualValues(
                ruvia::ModelOptions{ .resource = result.resource() }
            );
            for (std::int64_t index = 0; index < actualCount; ++index) {
                auto& actual = actualValues.emplace();
                actual.template set<"elementId">(
                          commandField(fields, commandActualValueField(index, "element_id"))
                )
                    .template set<"name">(commandField(fields, commandActualValueField(index, "name")))
                    .template set<"kind">(commandField(fields, commandActualValueField(index, "kind")))
                    .template set<"value">(commandField(fields, commandActualValueField(index, "value")))
                    .template set<"unit">(commandField(fields, commandActualValueField(index, "unit")));
            }
            result.template set<"actualValues">(std::move(actualValues));
        }
    }

    static std::string_view commandField(const std::vector<message::StreamField>& fields, std::string_view name) {
        for (const auto& value : fields) {
            if (value.name == name) {
                return value.value;
            }
        }
        return {};
    }

    static bool commandTerminalState(std::string_view state) {
        return state == "SUCCEEDED" || state == "REJECTED" || state == "UNKNOWN" ||
            state == "READBACK_MISMATCH" || state == "FAILED";
    }

    template <typename Context>
    static ruvia::Task<std::vector<message::StreamField>> loadCommandStatus(Context& context, std::string_view id) {
        ruvia::DbQuery statusQuery(context.pool());
        const auto createdAtMs = statusQuery.cast(
            statusQuery.binary(statusQuery.extract(ruvia::DbDatePart::kEpoch, statusQuery.column(service::command::entities::CommandOperationEntity::columnName<"created_at">())), ruvia::DbBinaryOperator::kMultiply, statusQuery.value(std::int64_t{ 1000 })),
            ruvia::DbDataType::kBigInt
        );
        const auto completedAtMs = statusQuery.cast(
            statusQuery.binary(statusQuery.extract(ruvia::DbDatePart::kEpoch, statusQuery.column(service::command::entities::CommandOperationEntity::columnName<"completed_at">())), ruvia::DbBinaryOperator::kMultiply, statusQuery.value(std::int64_t{ 1000 })),
            ruvia::DbDataType::kBigInt
        );
        statusQuery
            .select({ statusQuery.cast(statusQuery.column(service::command::entities::CommandOperationEntity::columnName<"device_id">()), ruvia::DbDataType::kText), statusQuery.column(service::command::entities::CommandOperationEntity::columnName<"device_code">()), statusQuery.column(service::command::entities::CommandOperationEntity::columnName<"protocol">()), statusQuery.column(service::command::entities::CommandOperationEntity::columnName<"status">()), statusQuery.column(service::command::entities::CommandOperationEntity::columnName<"reason">()), statusQuery.cast(createdAtMs, ruvia::DbDataType::kText), statusQuery.coalesce({ statusQuery.cast(completedAtMs, ruvia::DbDataType::kText), statusQuery.value("0") }) })
            .from(service::command::entities::CommandOperationEntity::tableName())
            .where(statusQuery.binary(statusQuery.column(service::command::entities::CommandOperationEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, statusQuery.cast(statusQuery.value(id), ruvia::DbDataType::kUuid)));
        const auto rows = co_await context.db().query(statusQuery);
        std::vector<message::StreamField> fields;
        if (rows.empty()) {
            co_return fields;
        }
        const std::string_view names[]{ "device_id", "device_code", "protocol", "status", "reason", "created_at_ms", "completed_at_ms" };
        for (std::size_t index = 0; index < 7; ++index) {
            fields.push_back({ std::string(names[index]), std::string(rows.front()[index].value().value_or(std::string_view{})) });
        }
        ruvia::DbQuery actualQuery(context.pool());
        const auto jsonKey = [&](std::string_view key) {
            return actualQuery.cast(actualQuery.value(key), ruvia::DbDataType::kText);
        };
        actualQuery
            .select({ actualQuery.binary(actualQuery.column("value", "a"), ruvia::DbBinaryOperator::kJsonGetText, jsonKey("elementId")), actualQuery.binary(actualQuery.column("value", "a"), ruvia::DbBinaryOperator::kJsonGetText, jsonKey("name")), actualQuery.binary(actualQuery.column("value", "a"), ruvia::DbBinaryOperator::kJsonGetText, jsonKey("kind")), actualQuery.binary(actualQuery.column("value", "a"), ruvia::DbBinaryOperator::kJsonGetText, jsonKey("value")), actualQuery.binary(actualQuery.column("value", "a"), ruvia::DbBinaryOperator::kJsonGetText, jsonKey("unit")) })
            .from(service::command::entities::CommandOperationEntity::tableName(), "operation")
            .joinFunction(
                ruvia::DbJoinType::kCross,
                actualQuery.call("jsonb_array_elements", { actualQuery.column(service::command::entities::CommandOperationEntity::columnName<"actual_values">(), "operation") }),
                {},
                "a",
                { .lateral = true, .withOrdinality = true, .columns = { { .name = "value" }, { .name = "idx" } } }
            )
            .where(actualQuery.binary(actualQuery.column(service::command::entities::CommandOperationEntity::columnName<"id">(), "operation"), ruvia::DbBinaryOperator::kEqual, actualQuery.cast(actualQuery.value(id), ruvia::DbDataType::kUuid)))
            .orderBy(actualQuery.column("idx", "a"));
        const auto actual = co_await context.db().query(actualQuery);
        fields.push_back({ "actual_value_count", std::to_string(actual.size()) });
        const std::string_view actualNames[]{ "element_id", "name", "kind", "value", "unit" };
        for (std::size_t index = 0; index < actual.size(); ++index) {
            for (std::size_t col = 0; col < 5; ++col) {
                fields.push_back({ "actual_value_" + std::to_string(index) + "_" + std::string(actualNames[col]), std::string(actual[index][col].value().value_or(std::string_view{})) });
            }
        }
        co_return fields;
    }

    static std::int64_t commandInteger(const std::vector<message::StreamField>& fields, std::string_view name) {
        const auto value = commandField(fields, name);
        std::int64_t result = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }

    static std::int64_t toInt(std::string_view value, std::int64_t fallback = 0) {
        return service::utils::parseInt64(std::optional<std::string_view>{ value })
            .value_or(fallback);
    }

    static std::optional<double> parseDouble(std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.remove_suffix(1);
        }
        if (value.empty()) {
            return std::nullopt;
        }
        return service::utils::decimal(value);
    }

    static double toDouble(std::string_view value, double fallback = 0) {
        return parseDouble(value).value_or(fallback);
    }

    static std::string edgeDeviceStatusKey(std::string_view nodeId, std::string_view deviceId) {
        return "iot:runtime:edge:" + std::string(nodeId) + ":device:" +
            std::string(deviceId);
    }

    static ruvia::DbQuery historyQuery(std::pmr::memory_resource* resource, std::string_view deviceId, std::string_view start, std::string_view end, std::int64_t pageSize, std::int64_t offset, std::int64_t page) {
        const auto timestamp = [](ruvia::DbQuery& query, std::string_view value) {
            return query.cast(query.value(value), ruvia::DbDataType::kTimestampTz);
        };
        ruvia::DbQuery counted(resource);
        const auto countedData = counted.column(service::device::entities::DeviceDataEntity::columnName<"data">(), "record");
        counted.select(counted.aggregate("count", { counted.star() }))
            .from(service::device::entities::DeviceDataEntity::tableName(), "record")
            .where(andAll(counted, counted.binary(counted.column(service::device::entities::DeviceDataEntity::columnName<"device_id">(), "record"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(counted, deviceId)), counted.binary(counted.column(service::device::entities::DeviceDataEntity::columnName<"report_time">(), "record"), ruvia::DbBinaryOperator::kGreaterEqual, timestamp(counted, start)), counted.binary(counted.column(service::device::entities::DeviceDataEntity::columnName<"report_time">(), "record"), ruvia::DbBinaryOperator::kLessEqual, timestamp(counted, end)), counted.binary(counted.call("jsonb_typeof", { DeviceAccessService::jsonValue(counted, countedData, "values") }), ruvia::DbBinaryOperator::kEqual, counted.value("object"))));

        ruvia::DbQuery filtered(resource);
        const auto filteredData = filtered.column(service::device::entities::DeviceDataEntity::columnName<"data">(), "record");
        filtered
            .select({ filtered.column(service::device::entities::DeviceDataEntity::columnName<"id">(), "record"), filtered.column(service::device::entities::DeviceDataEntity::columnName<"protocol">(), "record"), filtered.column(service::device::entities::DeviceDataEntity::columnName<"report_time">(), "record"), filtered.column(service::device::entities::DeviceDataEntity::columnName<"source">(), "record"), filtered.column(service::device::entities::DeviceDataEntity::columnName<"raw_payload_hex">(), "record"), filteredData })
            .from(service::device::entities::DeviceDataEntity::tableName(), "record")
            .where(andAll(filtered, filtered.binary(filtered.column(service::device::entities::DeviceDataEntity::columnName<"device_id">(), "record"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(filtered, deviceId)), filtered.binary(filtered.column(service::device::entities::DeviceDataEntity::columnName<"report_time">(), "record"), ruvia::DbBinaryOperator::kGreaterEqual, timestamp(filtered, start)), filtered.binary(filtered.column(service::device::entities::DeviceDataEntity::columnName<"report_time">(), "record"), ruvia::DbBinaryOperator::kLessEqual, timestamp(filtered, end)), filtered.binary(filtered.call("jsonb_typeof", { DeviceAccessService::jsonValue(filtered, filteredData, "values") }), ruvia::DbBinaryOperator::kEqual, filtered.value("object"))))
            .orderBy(filtered.column(service::device::entities::DeviceDataEntity::columnName<"report_time">(), "record"), ruvia::DbOrderDirection::kDesc)
            .addOrderBy(filtered.column(service::device::entities::DeviceDataEntity::columnName<"id">(), "record"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>(offset));

        ruvia::DbQuery normalized(resource);
        ruvia::DbQuery normalizedValues(resource);
        const auto point = normalizedValues.column("value", "point");
        const auto pointValue = DeviceAccessService::jsonValue(
            normalizedValues,
            point,
            "value"
        );
        const auto pointObject = andAll(
            normalizedValues,
            normalizedValues.binary(normalizedValues.call("jsonb_typeof", { point }), ruvia::DbBinaryOperator::kEqual, normalizedValues.value("object")),
            normalizedValues.binary(
                normalizedValues.call("jsonb_typeof", { pointValue }),
                ruvia::DbBinaryOperator::kEqual,
                normalizedValues.value("boolean")
            )
        );
        const auto pointBoolean = normalizedValues.cast(
            DeviceAccessService::jsonText(normalizedValues, point, "value"),
            ruvia::DbDataType::kBoolean
        );
        const auto pointNumber = normalizedValues.caseWhen(
            { { pointBoolean, DeviceAccessService::integer(normalizedValues, 1) } },
            DeviceAccessService::integer(normalizedValues, 0)
        );
        const auto pointPath = normalizedValues.cast(
            normalizedValues.array({ DeviceAccessService::textKey(normalizedValues, "value") }),
            ruvia::DbTypeDefinition{ .dataType = ruvia::DbDataType::kText, .array = true }
        );
        const auto normalizedPoint = normalizedValues.caseWhen(
            { { pointObject,
                normalizedValues.call("jsonb_set", { point, pointPath, normalizedValues.call("to_jsonb", { pointNumber }), DeviceAccessService::boolean(normalizedValues, false) }) } },
            point
        );
        normalizedValues
            .select(normalizedValues.aggregate("jsonb_object_agg", { normalizedValues.column("key", "point"), normalizedPoint }))
            .fromFunction(
                normalizedValues.call(
                    "jsonb_each",
                    { normalizedValues.coalesce(
                        { DeviceAccessService::jsonValue(
                              normalizedValues,
                              normalizedValues.column("data", "filtered"),
                              "values"
                          ),
                          normalizedValues.cast(normalizedValues.value("{}"), ruvia::DbDataType::kJsonb) }
                    ) }
                ),
                "point",
                { .lateral = true,
                  .columns = { { .name = "key" }, { .name = "value" } } }
            );
        normalized
            .select({ normalized.star("filtered"), normalized.alias(normalized.coalesce({ normalized.subquery(normalizedValues), normalized.cast(normalized.value("{}"), ruvia::DbDataType::kJsonb) }), "normalized_values") })
            .from(filtered, "filtered");

        ruvia::DbQuery query(resource);
        query.with("counted", counted)
            .with("filtered", filtered)
            .with("normalized", normalized);
        const auto total = query.coalesce({ query.subquery(counted), DeviceAccessService::integer(query, 0) });
        const auto item = query.call(
            "jsonb_build_object",
            { DeviceAccessService::textKey(query, "id"), query.column("id", "normalized"), DeviceAccessService::textKey(query, "protocol"), query.column("protocol", "normalized"), DeviceAccessService::textKey(query, "reportTime"), query.call("iot_utc_timestamp", { query.column("report_time", "normalized") }), DeviceAccessService::textKey(query, "source"), query.column("source", "normalized"), DeviceAccessService::textKey(query, "functionCode"), DeviceAccessService::jsonText(query, query.column("data", "normalized"), "function_code"), DeviceAccessService::textKey(query, "values"), query.column("normalized_values", "normalized"), DeviceAccessService::textKey(query, "rawPayloadHex"), query.coalesce({ query.column("raw_payload_hex", "normalized"), query.cast(query.value("[]"), ruvia::DbDataType::kJsonb) }) }
        );
        const std::array<ruvia::DbOrderTerm, 2> historyOrder{ { ruvia::DbOrderTerm{ query.column("report_time", "normalized"),
                                                                                    ruvia::DbOrderDirection::kDesc,
                                                                                    ruvia::DbNullsOrder::kDefault },
                                                                ruvia::DbOrderTerm{ query.column("id", "normalized"),
                                                                                    ruvia::DbOrderDirection::kDesc,
                                                                                    ruvia::DbNullsOrder::kDefault } } };
        const auto list = query.coalesce(
            { query.aggregate("jsonb_agg", { item }, false, historyOrder),
              query.cast(query.value("[]"), ruvia::DbDataType::kJsonb) }
        );
        const auto totalPages = query.cast(
            query.call("ceil", { query.binary(query.cast(total, ruvia::DbDataType::kNumeric), ruvia::DbBinaryOperator::kDivide, query.cast(query.value(pageSize), ruvia::DbDataType::kNumeric)) }),
            ruvia::DbDataType::kBigInt
        );
        query.select(query.cast(
                         query.call("jsonb_build_object", { DeviceAccessService::textKey(query, "list"), list, DeviceAccessService::textKey(query, "total"), total, DeviceAccessService::textKey(query, "page"), query.cast(query.value(page), ruvia::DbDataType::kBigInt), DeviceAccessService::textKey(query, "pageSize"), query.cast(query.value(pageSize), ruvia::DbDataType::kBigInt), DeviceAccessService::textKey(query, "totalPages"), totalPages }),
                         ruvia::DbDataType::kText
                     ))
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
            return query.nullIf(DeviceAccessService::jsonText(query, protocolParams, key), query.value(""));
        };
        const auto numericText = DeviceAccessService::jsonText(query, protocolParams, "online_timeout");
        const auto numericOnlineTimeout = query.caseWhen(
            { { query.binary(query.coalesce({ numericText, query.value("") }), ruvia::DbBinaryOperator::kRegex, query.value("^-?[0-9]{1,18}$")),
                query.cast(numericText, ruvia::DbDataType::kInteger) } }
        );
        const auto onlineTimeout = query.coalesce({ numericOnlineTimeout, DeviceAccessService::integer(query, 300) });
        const auto timezone = query.coalesce(
            { query.nullIf(DeviceAccessService::jsonText(query, protocolParams, "timezone"), query.value("")),
              query.value("+08:00") }
        );
        const auto edgeExecution = query.binary(query.column("execution", "l"), ruvia::DbBinaryOperator::kEqual, query.value("edge"));
        const auto rs485 = query.call(
            "lower",
            { query.coalesce({ DeviceAccessService::jsonText(query, endpoint, "rs485"), query.value("") }) }
        );
        const auto rs485Enabled = query.caseWhen(
            { { query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("true")),
                DeviceAccessService::boolean(query, true) },
              { query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("t")),
                DeviceAccessService::boolean(query, true) },
              { query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("1")),
                DeviceAccessService::boolean(query, true) },
              { query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("yes")),
                DeviceAccessService::boolean(query, true) },
              { query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("y")),
                DeviceAccessService::boolean(query, true) },
              { query.binary(rs485, ruvia::DbBinaryOperator::kEqual, query.value("on")),
                DeviceAccessService::boolean(query, true) } },
            DeviceAccessService::boolean(query, false)
        );

        ruvia::DbQuery functionCount(query.resource());
        const auto functions = functionCount.coalesce(
            { DeviceAccessService::jsonValue(functionCount, functionCount.column("config", "p"), "funcs"),
              functionCount.cast(functionCount.value("[]"), ruvia::DbDataType::kJsonb) }
        );
        const auto function = functionCount.column("value", "function");
        const auto elements = functionCount.coalesce(
            { DeviceAccessService::jsonValue(functionCount, function, "elements"),
              functionCount.cast(functionCount.value("[]"), ruvia::DbDataType::kJsonb) }
        );
        const auto responseElements = functionCount.coalesce(
            { DeviceAccessService::jsonValue(functionCount, function, "responseElements"),
              functionCount.cast(functionCount.value("[]"), ruvia::DbDataType::kJsonb) }
        );
        const auto elementCount = functionCount.call(
            "jsonb_array_length",
            { elements }
        );
        const auto responseElementCount = functionCount.call(
            "jsonb_array_length",
            { responseElements }
        );
        functionCount
            .select(functionCount.coalesce(
                { functionCount.aggregate(
                      "sum",
                      { functionCount.binary(elementCount, ruvia::DbBinaryOperator::kAdd, responseElementCount) }
                  ),
                  DeviceAccessService::integer(functionCount, 0) }
            ))
            .fromFunction(functionCount.call("jsonb_array_elements", { functions }), "function", { .columns = { { .name = "value" } } });
        const auto protocolElementCount = query.caseWhen(
            { { query.binary(query.column("protocol", "p"), ruvia::DbBinaryOperator::kEqual, query.value("Modbus")),
                query.call("jsonb_array_length", { query.coalesce({ DeviceAccessService::jsonValue(query, protocolConfig, "registers"), emptyArray }) }) },
              { query.binary(query.column("protocol", "p"), ruvia::DbBinaryOperator::kEqual, query.value("S7")),
                query.call("jsonb_array_length", { query.coalesce({ DeviceAccessService::jsonValue(query, protocolConfig, "areas"), emptyArray }) }) } },
            query.caseWhen({ { query.binary(query.column("protocol", "p"), ruvia::DbBinaryOperator::kEqual, query.value("SL651")), query.coalesce({ query.subquery(functionCount), DeviceAccessService::integer(query, 0) }) } }, query.call("jsonb_array_length", { query.coalesce({ DeviceAccessService::jsonValue(query, protocolConfig, "points"), emptyArray }) }))
        );

        query.select({ DeviceAccessService::text(query, query.column("id", "d")), query.column("name", "d"), DeviceAccessService::jsonText(query, protocolParams, "device_code"), DeviceAccessService::text(query, query.column("link_id", "d")), nullableJsonText("target_id"), DeviceAccessService::text(query, query.column("protocol_config_id", "d")), DeviceAccessService::text(query, query.column("group_id", "d")), query.column("status", "d"), onlineTimeout, DeviceAccessService::remoteControlEnabled(query, protocolParams), nullableJsonText("modbus_mode"), nullableJsonText("slave_id"), timezone, DeviceAccessService::jsonText(query, DeviceAccessService::jsonValue(query, protocolParams, "heartbeat"), "mode"), DeviceAccessService::jsonText(query, DeviceAccessService::jsonValue(query, protocolParams, "heartbeat"), "content"), DeviceAccessService::jsonText(query, DeviceAccessService::jsonValue(query, protocolParams, "registration"), "mode"), DeviceAccessService::jsonText(query, DeviceAccessService::jsonValue(query, protocolParams, "registration"), "content"), query.coalesce({ query.column("remark", "d"), query.value("") }), DeviceAccessService::text(query, query.column("created_by", "d")), query.call("iot_utc_timestamp", { query.column("created_at", "d") }), query.call("iot_utc_timestamp", { query.column("updated_at", "d") }), query.coalesce({ query.column("name", "l"), query.value("") }), query.coalesce({ DeviceAccessService::jsonText(query, endpoint, "mode"), query.value("") }), query.coalesce({ query.column("protocol", "l"), query.value("") }), query.column("name", "p"), query.column("protocol", "p"), query.nullIf(DeviceAccessService::jsonText(query, protocolConfig, "readInterval"), query.value("")), query.nullIf(DeviceAccessService::jsonText(query, protocolConfig, "storagePolicy"), query.value("")), protocolElementCount, query.column("access_rank", "d"), query.caseWhen({ { edgeExecution, DeviceAccessService::text(query, query.column("edge_node_id", "l")) } }), query.coalesce({ query.column("name", "en"), query.value("") }), query.coalesce({ query.column("imei", "en"), query.value("") }), query.caseWhen({ { edgeExecution, query.nullIf(DeviceAccessService::jsonText(query, endpoint, "transport"), query.value("")) } }), query.caseWhen({ { edgeExecution, query.nullIf(DeviceAccessService::jsonText(query, endpoint, "interface"), query.value("")) } }), query.caseWhen({ { edgeExecution, query.nullIf(DeviceAccessService::jsonText(query, endpoint, "mode"), query.value("")) } }), query.caseWhen({ { edgeExecution, query.nullIf(DeviceAccessService::jsonText(query, endpoint, "ip"), query.value("")) } }), query.caseWhen({ { edgeExecution, query.nullIf(DeviceAccessService::jsonText(query, endpoint, "port"), query.value("")) } }), query.caseWhen({ { edgeExecution, query.nullIf(DeviceAccessService::jsonText(query, endpoint, "baud_rate"), query.value("")) } }), query.caseWhen({ { edgeExecution, query.nullIf(DeviceAccessService::jsonText(query, endpoint, "data_bits"), query.value("")) } }), query.caseWhen({ { edgeExecution, query.nullIf(DeviceAccessService::jsonText(query, endpoint, "stop_bits"), query.value("")) } }), query.caseWhen({ { edgeExecution, query.nullIf(DeviceAccessService::jsonText(query, endpoint, "parity"), query.value("")) } }), query.caseWhen({ { edgeExecution, rs485Enabled } }), query.column("debug_enabled", "d"), query.column("debug_enabled", "l") });
    }

    template <typename Context, typename Row>
    static void fillItem(Context& c, DeviceItemDto& item, Row&& row, const DeviceActor& actor) {
        item.template set<"id">(row[0].value().value_or(std::string_view{}));
        item.template set<"name">(row[1].value().value_or(std::string_view{}));
        item.template set<"debugEnabled">(row[43].value().value_or("") == "t");
        item.template set<"linkDebugEnabled">(row[44].value().value_or("") == "t");
        item.template set<"deviceCode">(row[2].value().value_or(std::string_view{}));
        if (row[3].value().has_value()) {
            item.template set<"linkId">(row[3].value().value_or(std::string_view{}));
        }
        if (row[4].value().has_value()) {
            item.template set<"targetId">(row[4].value().value_or(std::string_view{}));
        }
        item.template set<"protocolConfigId">(row[5].value().value_or(std::string_view{}));
        if (row[6].value().has_value()) {
            item.template set<"groupId">(row[6].value().value_or(std::string_view{}));
        }
        item.template set<"status">(row[7].value().value_or(std::string_view{}));
        item.template set<"onlineTimeout">(toInt(row[8].value().value_or(std::string_view{})));
        item.template set<"remoteControl">(row[9].value().value_or(std::string_view{}) == "t");
        if (row[10].value().has_value()) {
            item.template set<"modbusMode">(row[10].value().value_or(std::string_view{}));
        }
        if (row[11].value().has_value()) {
            item.template set<"slaveId">(toInt(row[11].value().value_or(std::string_view{})));
        }
        item.template set<"timezone">(row[12].value().value_or(std::string_view{}));
        {
            DevicePacketDto heartbeat(ruvia::ModelOptions{ .resource = c.arena() });
            if (row[13].value().has_value()) {
                heartbeat.template set<"mode">(row[13].value().value_or(std::string_view{}));
            }
            if (row[14].value().has_value()) {
                heartbeat.template set<"content">(row[14].value().value_or(std::string_view{}));
            }
            item.template set<"heartbeat">(std::move(heartbeat));
        }
        if (!row[30].value().has_value()) {
            DevicePacketDto registration(ruvia::ModelOptions{ .resource = c.arena() });
            if (row[15].value().has_value()) {
                registration.template set<"mode">(row[15].value().value_or(std::string_view{}));
            }
            if (row[16].value().has_value()) {
                registration.template set<"content">(row[16].value().value_or(std::string_view{}));
            }
            item.template set<"registration">(std::move(registration));
        }
        item.template set<"remark">(row[17].value().value_or(std::string_view{}));
        item.template set<"createdBy">(row[18].value().value_or(std::string_view{}));
        item.template set<"createdAt">(row[19].value().value_or(std::string_view{}));
        item.template set<"updatedAt">(row[20].value().value_or(std::string_view{}));
        item.template set<"linkName">(row[21].value().value_or(std::string_view{}));
        item.template set<"linkMode">(row[22].value().value_or(std::string_view{}));
        item.template set<"linkProtocol">(row[23].value().value_or(std::string_view{}));
        item.template set<"protocolName">(row[24].value().value_or(std::string_view{}));
        item.template set<"protocolType">(row[25].value().value_or(std::string_view{}));
        if (row[26].value().has_value()) {
            if (const auto value = parseDouble(row[26].value().value_or(std::string_view{}))) {
                item.template set<"readInterval">(*value);
            }
        }
        if (row[27].value().has_value()) {
            item.template set<"storagePolicy">(row[27].value().value_or(std::string_view{}));
        }
        const auto capabilities = DeviceAccessService::capabilities(
            actor,
            DeviceAccessService::rank(row[29].value().value_or(std::string_view{})),
            row[9].value().value_or(std::string_view{}) == "t"
        );
        item.template set<"elementCount">(toInt(row[28].value().value_or(std::string_view{})));
        item.template set<"connected">(false);
        item.template set<"connectionState">("disconnected");
        item.template set<"elements">(ruvia::BoxedArray<DeviceElementDto>(
            ruvia::ModelOptions{ .resource = c.arena() }
        ));
        item.template set<"canEdit">(capabilities.canEdit);
        item.template set<"canDelete">(capabilities.canDelete);
        item.template set<"canShare">(capabilities.canShare);
        item.template set<"canCommand">(capabilities.canCommand);
        item.template set<"accessLevel">(capabilities.accessLevel);
        if (row[30].value().has_value()) {
            item.template set<"edgeNodeId">(row[30].value().value_or(std::string_view{}));
            item.template set<"edgeNodeName">(row[31].value().value_or(std::string_view{}));
            item.template set<"edgeNodeImei">(row[32].value().value_or(std::string_view{}));
        }
        if (row[33].value().has_value()) {
            item.template set<"edgeTransport">(row[33].value().value_or(std::string_view{}));
        }
        if (row[34].value().has_value()) {
            item.template set<"edgeInterface">(row[34].value().value_or(std::string_view{}));
        }
        if (row[35].value().has_value()) {
            item.template set<"edgeMode">(row[35].value().value_or(std::string_view{}));
        }
        if (row[36].value().has_value()) {
            item.template set<"edgeIp">(row[36].value().value_or(std::string_view{}));
        }
        if (row[37].value().has_value()) {
            item.template set<"edgePort">(toInt(row[37].value().value_or(std::string_view{})));
        }
        if (row[38].value().has_value()) {
            item.template set<"serialBaudRate">(toInt(row[38].value().value_or(std::string_view{})));
        }
        if (row[39].value().has_value()) {
            item.template set<"serialDataBits">(toInt(row[39].value().value_or(std::string_view{})));
        }
        if (row[40].value().has_value()) {
            item.template set<"serialStopBits">(toInt(row[40].value().value_or(std::string_view{})));
        }
        if (row[41].value().has_value()) {
            item.template set<"serialParity">(row[41].value().value_or(std::string_view{}));
        }
        if (row[42].value().has_value()) {
            item.template set<"serialRs485">(row[42].value().value_or(std::string_view{}) == "t");
        }
    }

    template <typename Context, typename Item>
    static ruvia::Task<void>
    fillLatest(Context& c, const std::map<std::string, Item*, std::less<>>& items) {
        if (items.empty()) {
            co_return;
        }
        auto pipeline = c.redis().pipeline();
        enum class ReplyKind { runtime,
                               latest,
                               edgeDevice };

        struct ReplyBinding {
            ReplyKind kind;
            Item* item;
        };

        std::vector<ReplyBinding> bindings;
        bindings.reserve(items.size() * 3);
        for (const auto& [id, item] : items) {
            (void)id;
            if (!item->template get<"deviceCode">()) {
                continue;
            }
            // The runtime hash also contains worker/session bookkeeping. The list only needs
            // these two fields, so HMGET avoids transferring and parsing the rest of the hash.
            pipeline.command("HMGET", service::telemetry::latest::runtimeKey(id), "connection_id", "last_report_at_ms");
            bindings.push_back({ ReplyKind::runtime, item });
            pipeline.hgetAll(service::telemetry::latest::latestKey(id));
            bindings.push_back({ ReplyKind::latest, item });
            if (item->template get<"edgeNodeId">() &&
                item->template get<"edgeTransport">() &&
                item->template get<"edgeTransport">()->view() == "tcp") {
                pipeline.command(
                    "HMGET",
                    edgeDeviceStatusKey(
                        item->template get<"edgeNodeId">()->view(),
                        item->template get<"id">()->view()
                    ),
                    "state",
                    "reason",
                    "client_count",
                    "last_activity_at_ms"
                );
                bindings.push_back({ ReplyKind::edgeDevice, item });
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

    template <typename Context>
    static ruvia::Task<void>
    fillCommandOperations(Context& c, const std::map<std::string, DeviceItemDto*, std::less<>>& items, std::optional<std::string_view> onlyDevice) {
        if (items.empty()) {
            co_return;
        }
        const auto makeWritable = [](ruvia::DbQuery& query,
                                     ruvia::DbQuery::Expr value) {
            const auto normalized = query.call("lower", { query.coalesce({ value, query.value("") }) });
            return query.caseWhen(
                { { query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("true")),
                    DeviceAccessService::boolean(query, true) },
                  { query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("t")),
                    DeviceAccessService::boolean(query, true) },
                  { query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("1")),
                    DeviceAccessService::boolean(query, true) },
                  { query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("yes")),
                    DeviceAccessService::boolean(query, true) },
                  { query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("y")),
                    DeviceAccessService::boolean(query, true) },
                  { query.binary(normalized, ruvia::DbBinaryOperator::kEqual, query.value("on")),
                    DeviceAccessService::boolean(query, true) } },
                DeviceAccessService::boolean(query, false)
            );
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
                query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::device::entities::DeviceEntity::columnName<"deleted_at">(), "d")),
                query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::device::entities::DeviceModelEntity::columnName<"deleted_at">(), "p"))
            );
            predicate = query.binary(
                predicate,
                ruvia::DbBinaryOperator::kAnd,
                query.binary(query.column(service::device::entities::DeviceModelEntity::columnName<"enabled">(), "p"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::boolean(query, true))
            );
            if (onlyDevice) {
                predicate = query.binary(
                    predicate,
                    ruvia::DbBinaryOperator::kAnd,
                    query.binary(query.column(service::device::entities::DeviceEntity::columnName<"id">(), "d"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(query, *onlyDevice))
                );
            }
            query.where(predicate);
        };

        ruvia::DbQuery modbus(c.pool());
        const auto modbusElement = modbus.column("element", "elements");
        const auto modbusRegisterType = DeviceAccessService::jsonText(
            modbus,
            modbusElement,
            "registerType"
        );
        ruvia::DbQuery modbusLabelOne(c.pool());
        const auto modbusLabelOneElement = modbusLabelOne.column("element", "elements");
        modbusLabelOne
            .select(DeviceAccessService::jsonText(modbusLabelOne, modbusLabelOne.column("value", "mapping"), "label"))
            .fromFunction(
                modbusLabelOne.call(
                    "jsonb_array_elements",
                    { modbusLabelOne.coalesce(
                        { DeviceAccessService::jsonValue(
                              modbusLabelOne,
                              DeviceAccessService::jsonValue(modbusLabelOne, modbusLabelOneElement, "dictConfig"),
                              "items"
                          ),
                          emptyArray(modbusLabelOne) }
                    ) }
                ),
                "mapping",
                { .columns = { { .name = "value" } } }
            )
            .where(modbusLabelOne.binary(
                DeviceAccessService::jsonText(modbusLabelOne, modbusLabelOne.column("value", "mapping"), "key"),
                ruvia::DbBinaryOperator::kEqual,
                modbusLabelOne.value("1")
            ))
            .limit(1);
        ruvia::DbQuery modbusLabelZero(c.pool());
        const auto modbusLabelZeroElement = modbusLabelZero.column("element", "elements");
        modbusLabelZero
            .select(DeviceAccessService::jsonText(modbusLabelZero, modbusLabelZero.column("value", "mapping"), "label"))
            .fromFunction(
                modbusLabelZero.call(
                    "jsonb_array_elements",
                    { modbusLabelZero.coalesce(
                        { DeviceAccessService::jsonValue(
                              modbusLabelZero,
                              DeviceAccessService::jsonValue(modbusLabelZero, modbusLabelZeroElement, "dictConfig"),
                              "items"
                          ),
                          emptyArray(modbusLabelZero) }
                    ) }
                ),
                "mapping",
                { .columns = { { .name = "value" } } }
            )
            .where(modbusLabelZero.binary(
                DeviceAccessService::jsonText(modbusLabelZero, modbusLabelZero.column("value", "mapping"), "key"),
                ruvia::DbBinaryOperator::kEqual,
                modbusLabelZero.value("0")
            ))
            .limit(1);
        const auto modbusPreset = modbus.caseWhen(
            { { modbus.binary(modbusRegisterType, ruvia::DbBinaryOperator::kEqual, modbus.value("COIL")),
                modbus.call(
                    "jsonb_build_array",
                    { modbus.call("jsonb_build_object", { DeviceAccessService::textKey(modbus, "label"), modbus.coalesce({ modbus.subquery(modbusLabelOne), DeviceAccessService::text(modbus, "1") }), DeviceAccessService::textKey(modbus, "value"), DeviceAccessService::text(modbus, "1") }),
                      modbus.call("jsonb_build_object", { DeviceAccessService::textKey(modbus, "label"), modbus.coalesce({ modbus.subquery(modbusLabelZero), DeviceAccessService::text(modbus, "0") }), DeviceAccessService::textKey(modbus, "value"), DeviceAccessService::text(modbus, "0") }) }
                ) } },
            emptyArray(modbus)
        );
        modbus
            .select({ modbus.column(service::device::entities::DeviceEntity::columnName<"id">(), "d"), DeviceAccessService::text(modbus, "MODBUS_WRITE"), DeviceAccessService::text(modbus, "写寄存器"), modbusElement, DeviceAccessService::integer(modbus, 1), modbus.column("element_position", "elements"), modbus.column("preset", "presets"), modbus.column("preset_position", "presets") })
            .from(service::device::entities::DeviceEntity::tableName(), "d")
            .join(ruvia::DbJoinType::kInner, service::device::entities::DeviceModelEntity::tableName(), andAll(modbus, modbus.binary(modbus.column(service::device::entities::DeviceModelEntity::columnName<"device_id">(), "p"), ruvia::DbBinaryOperator::kEqual, modbus.column(service::device::entities::DeviceEntity::columnName<"id">(), "d")), modbus.binary(modbus.column(service::device::entities::DeviceModelEntity::columnName<"protocol">(), "p"), ruvia::DbBinaryOperator::kEqual, modbus.value("Modbus"))), "p")
            .joinFunction(
                ruvia::DbJoinType::kCross,
                modbus.call("jsonb_array_elements", { modbus.coalesce({ DeviceAccessService::jsonValue(modbus, modbus.column(service::device::entities::DeviceModelEntity::columnName<"config">(), "p"), "registers"), emptyArray(modbus) }) }),
                {},
                "elements",
                { .lateral = true, .withOrdinality = true, .columns = { { .name = "element" }, { .name = "element_position" } } }
            )
            .joinFunction(
                ruvia::DbJoinType::kLeft,
                modbus.call("jsonb_array_elements", { modbusPreset }),
                DeviceAccessService::boolean(modbus, true),
                "presets",
                { .lateral = true, .withOrdinality = true, .columns = { { .name = "preset" }, { .name = "preset_position" } } }
            );
        addDeviceFilters(modbus);
        modbus.andWhere(makeWritable(
            modbus,
            DeviceAccessService::jsonText(modbus, modbusElement, "writable")
        ));

        ruvia::DbQuery s7(c.pool());
        const auto s7Element = s7.column("element", "elements");
        const auto s7Preset = s7.caseWhen(
            { { s7.binary(DeviceAccessService::jsonText(s7, s7Element, "dataType"), ruvia::DbBinaryOperator::kEqual, s7.value("BOOL")),
                s7.cast(s7.value("[{\"label\":\"1\",\"value\":\"1\"},"
                                 "{\"label\":\"0\",\"value\":\"0\"}]"),
                        ruvia::DbDataType::kJsonb) } },
            emptyArray(s7)
        );
        s7.select({ s7.column(service::device::entities::DeviceEntity::columnName<"id">(), "d"), s7.binary(s7.column("protocol", "p"), ruvia::DbBinaryOperator::kConcat, s7.value("_WRITE")), DeviceAccessService::text(s7, "写寄存器"), s7Element, DeviceAccessService::integer(s7, 2), s7.column("element_position", "elements"), s7.column("preset", "presets"), s7.column("preset_position", "presets") })
            .from(service::device::entities::DeviceEntity::tableName(), "d")
            .join(ruvia::DbJoinType::kInner, service::device::entities::DeviceModelEntity::tableName(), andAll(s7, s7.binary(s7.column(service::device::entities::DeviceModelEntity::columnName<"device_id">(), "p"), ruvia::DbBinaryOperator::kEqual, s7.column(service::device::entities::DeviceEntity::columnName<"id">(), "d")), s7.binary(s7.column(service::device::entities::DeviceModelEntity::columnName<"protocol">(), "p"), ruvia::DbBinaryOperator::kIn, s7.list({ s7.value("S7"), s7.value("MC"), s7.value("FINS"), s7.value("DLT645") }))), "p")
            .joinFunction(
                ruvia::DbJoinType::kCross,
                s7.call("jsonb_array_elements", { s7.coalesce({ DeviceAccessService::jsonValue(s7, s7.column(service::device::entities::DeviceModelEntity::columnName<"config">(), "p"), "areas"), DeviceAccessService::jsonValue(s7, s7.column("config", "p"), "points"), emptyArray(s7) }) }),
                {},
                "elements",
                { .lateral = true, .withOrdinality = true, .columns = { { .name = "element" }, { .name = "element_position" } } }
            )
            .joinFunction(ruvia::DbJoinType::kLeft, s7.call("jsonb_array_elements", { s7Preset }), DeviceAccessService::boolean(s7, true), "presets", { .lateral = true, .withOrdinality = true, .columns = { { .name = "preset" }, { .name = "preset_position" } } });
        addDeviceFilters(s7);
        s7.andWhere(makeWritable(s7, DeviceAccessService::jsonText(s7, s7Element, "writable")));

        ruvia::DbQuery sl651(c.pool());
        const auto function = sl651.column("function", "functions");
        const auto sl651Element = sl651.column("element", "elements");
        sl651.select({ sl651.column(service::device::entities::DeviceEntity::columnName<"id">(), "d"), DeviceAccessService::jsonText(sl651, function, "funcCode"), sl651.coalesce({ sl651.nullIf(DeviceAccessService::jsonText(sl651, function, "name"), sl651.value("")), DeviceAccessService::jsonText(sl651, function, "funcCode") }), sl651Element, sl651.binary(sl651.column("function_position", "functions"), ruvia::DbBinaryOperator::kAdd, DeviceAccessService::integer(sl651, 2)), sl651.column("element_position", "elements"), sl651.column("preset", "presets"), sl651.column("preset_position", "presets") })
            .from(service::device::entities::DeviceEntity::tableName(), "d")
            .join(ruvia::DbJoinType::kInner, service::device::entities::DeviceModelEntity::tableName(), andAll(sl651, sl651.binary(sl651.column(service::device::entities::DeviceModelEntity::columnName<"device_id">(), "p"), ruvia::DbBinaryOperator::kEqual, sl651.column(service::device::entities::DeviceEntity::columnName<"id">(), "d")), sl651.binary(sl651.column(service::device::entities::DeviceModelEntity::columnName<"protocol">(), "p"), ruvia::DbBinaryOperator::kEqual, sl651.value("SL651"))), "p")
            .joinFunction(
                ruvia::DbJoinType::kCross,
                sl651.call("jsonb_array_elements", { sl651.coalesce({ DeviceAccessService::jsonValue(sl651, sl651.column(service::device::entities::DeviceModelEntity::columnName<"config">(), "p"), "funcs"), emptyArray(sl651) }) }),
                {},
                "functions",
                { .lateral = true, .withOrdinality = true, .columns = { { .name = "function" }, { .name = "function_position" } } }
            )
            .joinFunction(
                ruvia::DbJoinType::kCross,
                sl651.call("jsonb_array_elements", { sl651.coalesce({ DeviceAccessService::jsonValue(sl651, function, "elements"), emptyArray(sl651) }) }),
                {},
                "elements",
                { .lateral = true, .withOrdinality = true, .columns = { { .name = "element" }, { .name = "element_position" } } }
            )
            .joinFunction(
                ruvia::DbJoinType::kLeft,
                sl651.call("jsonb_array_elements", { sl651.coalesce({ DeviceAccessService::jsonValue(sl651, sl651Element, "options"), emptyArray(sl651) }) }),
                DeviceAccessService::boolean(sl651, true),
                "presets",
                { .lateral = true, .withOrdinality = true, .columns = { { .name = "preset" }, { .name = "preset_position" } } }
            );
        addDeviceFilters(sl651);
        sl651.andWhere(sl651.binary(DeviceAccessService::jsonText(sl651, function, "dir"), ruvia::DbBinaryOperator::kEqual, sl651.value("DOWN")));
        sl651.andWhere(sl651.binary(
            sl651.coalesce({ DeviceAccessService::jsonText(sl651, sl651Element, "encode"), sl651.value("") }),
            ruvia::DbBinaryOperator::kNotEqual,
            sl651.value("JPEG")
        ));

        modbus.combine(ruvia::DbSetOperation::kUnionAll, s7)
            .combine(ruvia::DbSetOperation::kUnionAll, sl651);
        ruvia::DbQuery query(c.pool());
        query.with("command_element", modbus, { .columns = { "device_id", "operation_key", "operation_name", "element", "operation_position", "element_position", "preset", "preset_position" } });
        const auto element = query.column("element", "command_element");
        const auto preset = query.column("preset", "command_element");
        query.select({ DeviceAccessService::text(query, query.column("device_id", "command_element")), query.column("operation_key", "command_element"), query.column("operation_name", "command_element"), DeviceAccessService::jsonText(query, element, "id"), DeviceAccessService::jsonText(query, element, "name"), query.coalesce({ DeviceAccessService::jsonText(query, element, "unit"), query.value("") }), query.coalesce({ DeviceAccessService::jsonText(query, element, "registerType"), query.value("") }), query.coalesce({ DeviceAccessService::jsonText(query, element, "dataType"), query.value("") }), DeviceAccessService::jsonText(query, element, "size"), query.coalesce({ DeviceAccessService::jsonText(query, element, "encode"), query.value("") }), DeviceAccessService::jsonText(query, element, "length"), DeviceAccessService::jsonText(query, element, "digits"), DeviceAccessService::jsonText(query, preset, "label"), DeviceAccessService::jsonText(query, preset, "value"), query.column("operation_position", "command_element"), query.column("element_position", "command_element"), query.column("preset_position", "command_element") })
            .from("command_element")
            .orderBy(query.column("device_id", "command_element"))
            .addOrderBy(query.column("operation_position", "command_element"))
            .addOrderBy(query.column("operation_key", "command_element"))
            .addOrderBy(query.column("element_position", "command_element"))
            .addOrderBy(query.column("preset_position", "command_element"), ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kLast);
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
            if (!items.contains(deviceId)) {
                continue;
            }
            auto& operations = configured[deviceId];
            const auto operationKey = std::string(row[1].value().value_or(std::string_view{}));
            auto operation =
                std::find_if(operations.begin(), operations.end(), [&](const auto& value) {
                    return value.key == operationKey;
                });
            if (operation == operations.end()) {
                operations.push_back({ operationKey, std::string(row[2].value().value_or(std::string_view{})), {} });
                operation = std::prev(operations.end());
            }
            const auto elementId = std::string(row[3].value().value_or(std::string_view{}));
            auto element = std::find_if(operation->elements.begin(), operation->elements.end(), [&](const auto& value) {
                return value.id == elementId;
            });
            if (element == operation->elements.end()) {
                ElementData data;
                data.id = elementId;
                data.name = std::string(row[4].value().value_or(std::string_view{}));
                data.unit = std::string(row[5].value().value_or(std::string_view{}));
                data.registerType = std::string(row[6].value().value_or(std::string_view{}));
                data.dataType = std::string(row[7].value().value_or(std::string_view{}));
                if (row[8].value().has_value()) {
                    data.size = toInt(row[8].value().value_or(std::string_view{}));
                }
                data.encode = std::string(row[9].value().value_or(std::string_view{}));
                if (row[10].value().has_value()) {
                    data.length = toInt(row[10].value().value_or(std::string_view{}));
                }
                if (row[11].value().has_value()) {
                    data.digits = toInt(row[11].value().value_or(std::string_view{}));
                }
                operation->elements.push_back(std::move(data));
                element = std::prev(operation->elements.end());
            }
            if (row[12].value().has_value() && row[13].value().has_value()) {
                element->options.push_back(
                    { std::string(row[12].value().value_or(std::string_view{})), std::string(row[13].value().value_or(std::string_view{})) }
                );
            }
        }

        for (auto& [deviceId, operations] : configured) {
            const auto item = items.find(deviceId);
            if (item == items.end()) {
                continue;
            }
            ruvia::BoxedArray<DeviceCommandOperationDto> operationDtos(
                ruvia::ModelOptions{ .resource = c.arena() }
            );
            for (const auto& operation : operations) {
                auto& operationDto = operationDtos.emplace(ruvia::ModelOptions{ .resource = c.arena() });
                operationDto.template set<"name">(operation.name);
                ruvia::BoxedArray<DeviceCommandOperationElementDto> elementDtos(
                    ruvia::ModelOptions{ .resource = c.arena() }
                );
                for (const auto& element : operation.elements) {
                    auto& elementDto = elementDtos.emplace(ruvia::ModelOptions{ .resource = c.arena() });
                    elementDto.template set<"elementId">(element.id).template set<"name">(element.name).template set<"value">("");
                    if (!element.unit.empty()) {
                        elementDto.template set<"unit">(element.unit);
                    }
                    if (!element.registerType.empty()) {
                        elementDto.template set<"registerType">(element.registerType);
                    }
                    if (!element.dataType.empty()) {
                        elementDto.template set<"dataType">(element.dataType);
                    }
                    if (element.size) {
                        elementDto.template set<"size">(*element.size);
                    }
                    if (!element.encode.empty()) {
                        elementDto.template set<"encode">(element.encode);
                    }
                    if (element.length) {
                        elementDto.template set<"length">(*element.length);
                    }
                    if (element.digits) {
                        elementDto.template set<"digits">(*element.digits);
                    }
                    if (!element.options.empty()) {
                        ruvia::BoxedArray<DeviceCommandOptionDto> optionDtos(
                            ruvia::ModelOptions{ .resource = c.arena() }
                        );
                        for (const auto& option : element.options) {
                            optionDtos.emplace(ruvia::ModelOptions{ .resource = c.arena() }).template set<"label">(option.label).template set<"value">(option.value);
                        }
                        elementDto.template set<"options">(std::move(optionDtos));
                    }
                }
                operationDto.template set<"elements">(std::move(elementDtos));
            }
            item->second->template set<"commandOperations">(std::move(operationDtos));
        }
    }

    static std::string redisHashField(const ruvia::RedisValue& value, std::string_view field) {
        if (value.kind() != ruvia::RedisValue::Kind::kArray) {
            return {};
        }
        const auto& entries = value.array();
        for (std::size_t index = 0; index + 1 < entries.size(); index += 2) {
            if (entries[index].kind() != ruvia::RedisValue::Kind::kString ||
                entries[index + 1].kind() != ruvia::RedisValue::Kind::kString) {
                continue;
            }
            if (entries[index].string() == field) {
                return std::string(entries[index + 1].string());
            }
        }
        return {};
    }

    static std::string redisArrayField(const ruvia::RedisValue& value, std::size_t index) {
        if (value.kind() != ruvia::RedisValue::Kind::kArray) {
            return {};
        }
        const auto entries = value.array();
        if (index >= entries.size() || entries[index].kind() != ruvia::RedisValue::Kind::kString) {
            return {};
        }
        return std::string(entries[index].string());
    }

    static std::optional<ruvia::JsonValue> jsonField(const ruvia::JsonValue& object, std::string_view field) {
        if (!object.isObject()) {
            return std::nullopt;
        }
        std::optional<ruvia::JsonValue> result;
        const auto valid = ruvia::detail::visitJsonObjectFields(
            ruvia::detail::ResolvedPmrResourceTag{},
            object.view(),
            std::pmr::get_default_resource(),
            [&](std::string_view key, std::string_view value) {
                if (key == field) {
                    result = ruvia::JsonValue::parse(value);
                }
                return true;
            }
        );
        return valid ? result : std::nullopt;
    }

    static std::optional<std::string> jsonString(const ruvia::JsonValue& object, std::string_view field) {
        const auto value = object.template get<ruvia::String>(field);
        if (!value) {
            return std::nullopt;
        }
        return std::string(value->view());
    }

    static std::int64_t jsonInt(const ruvia::JsonValue& object, std::string_view field, std::int64_t fallback) {
        if (const auto value = object.template get<ruvia::Int64>(field)) {
            return static_cast<std::int64_t>(*value);
        }
        const auto raw = jsonField(object, field);
        if (!raw) {
            return fallback;
        }
        return service::utils::parseInt64(std::optional<std::string_view>{ raw->view() })
            .value_or(fallback);
    }

    static double jsonDouble(const ruvia::JsonValue& object, std::string_view field, double fallback) {
        const auto raw = jsonField(object, field);
        if (!raw) {
            return fallback;
        }
        return toDouble(raw->view(), fallback);
    }

    template <typename Item>
    static void applyRuntime(Item& item, const ruvia::RedisValue& reply) {
        const auto reportTime = redisArrayField(reply, 1);
        if (!reportTime.empty()) {
            const auto milliseconds = toInt(reportTime);
            if (milliseconds > 0) {
                item.template set<"reportTime">(
                    service::common::utcTimestampFromMilliseconds(milliseconds)
                );
            }
        }
        const bool connected = !redisArrayField(reply, 0).empty();
        item.template set<"connected">(connected);
        item.template set<"connectionState">(
            connected ? "connected" : "disconnected"
        );
    }

    template <typename Context, typename Item>
    static void applyEdgeRuntime(Context& c, Item& item, const ruvia::RedisValue& reply) {
        const auto state = redisArrayField(reply, 0);
        if (state.empty()) {
            return;
        }
        const bool connected = state == "connected" || state == "online";
        item.template set<"connected">(connected);
        item.template set<"connectionState">(
            connected ? "connected" : "disconnected"
        );
        EdgeStatusDto status(ruvia::ModelOptions{ .resource = c.arena() });
        status.template set<"state">(state);
        const auto reason = redisArrayField(reply, 1);
        if (!reason.empty()) {
            status.template set<"reason">(reason);
        }
        const auto clientCount = redisArrayField(reply, 2);
        if (!clientCount.empty()) {
            status.template set<"clientCount">(toInt(clientCount));
        }
        const auto lastActivity = redisArrayField(reply, 3);
        if (!lastActivity.empty()) {
            const auto milliseconds = toInt(lastActivity);
            if (milliseconds > 0) {
                status.template set<"lastActivityAt">(
                    service::common::utcTimestampFromMilliseconds(milliseconds)
                );
            }
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
        std::string value{ "-" };
        std::string unit;
        std::string dataType;
        std::string group;
        std::string encode;
    };

    template <typename Context, typename Item>
    static void applyLatestElements(Context& c, Item& item, const ruvia::RedisValue& reply) {
        if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
            return;
        }
        bool hasElementIds = false;
        std::set<std::string, std::less<>> elementIds;
        std::map<std::string, LatestElement, std::less<>> latest;

        const auto& entries = reply.array();
        for (std::size_t index = 0; index + 1 < entries.size(); index += 2) {
            if (entries[index].kind() != ruvia::RedisValue::Kind::kString ||
                entries[index + 1].kind() != ruvia::RedisValue::Kind::kString) {
                continue;
            }
            const auto field = entries[index].string();
            if (field == "_element_ids") {
                hasElementIds = true;
                const auto parsed = ruvia::JsonValue::parse(entries[index + 1].string());
                if (parsed && parsed->isObject()) {
                    (void)ruvia::detail::visitJsonObjectFields(
                        ruvia::detail::ResolvedPmrResourceTag{},
                        parsed->view(),
                        std::pmr::get_default_resource(),
                        [&](std::string_view key, std::string_view) {
                            if (!key.empty()) {
                                elementIds.emplace(key);
                            }
                            return true;
                        }
                    );
                }
                continue;
            }
            if (field.empty() || field.front() == '_') {
                continue;
            }
            const auto parsed = ruvia::JsonValue::parse(entries[index + 1].string());
            if (!parsed || !parsed->isObject()) {
                continue;
            }
            LatestElement element;
            element.id = jsonString(*parsed, "id").value_or(std::string(field));
            element.name = jsonString(*parsed, "name").value_or(element.id);
            element.dataType = jsonString(*parsed, "dataType").value_or("");
            element.value = service::telemetry::latest::canonicalPointText(
                jsonString(*parsed, "value").value_or("-"),
                element.dataType
            );
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
            if (hasElementIds && !elementIds.contains(id)) {
                continue;
            }
            elements.push_back(std::move(element));
        }
        std::sort(elements.begin(), elements.end(), [](const auto& left, const auto& right) {
            if (left.sort != right.sort) {
                return left.sort < right.sort;
            }
            return left.id < right.id;
        });
        ruvia::BoxedArray<DeviceElementDto> dtos(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        std::int64_t reportTime = 0;
        for (const auto& element : elements) {
            auto& dto = dtos.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            dto.template set<"id">(element.id)
                .template set<"name">(element.name)
                .template set<"value">(element.value)
                .template set<"unit">(element.unit)
                .template set<"scale">(element.scale)
                .template set<"decimals">(element.decimals);
            if (!element.group.empty()) {
                dto.template set<"group">(element.group);
            }
            if (!element.encode.empty()) {
                dto.template set<"encode">(element.encode);
            }
            reportTime = std::max(reportTime, element.observedAt);
        }
        item.template set<"elements">(std::move(dtos));
        if (reportTime > 0 && !item.template get<"reportTime">()) {
            item.template set<"reportTime">(
                service::common::utcTimestampFromMilliseconds(reportTime)
            );
        }
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
                        std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
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
        if (!packet) {
            return "";
        }
        std::string out = "{\"mode\":";
        appendJsonString(out, packet->template get<"mode">() ? packet->template get<"mode">()->view() : std::string_view("OFF"));
        if (packet->template get<"content">()) {
            out += ",\"content\":";
            appendJsonString(out, packet->template get<"content">()->view());
        }
        out.push_back('}');
        return out;
    }

    // 心跳/注册包内容校验（对应旧 SQL shape-check 的第 8、9 条，语义一致）
    static void validatePacket(const std::optional<DevicePacketBody>& packet) {
        if (!packet) {
            return;
        }
        const std::string_view mode =
            packet->template get<"mode">() ? packet->template get<"mode">()->view() : std::string_view("OFF");
        if (mode != "OFF" && mode != "HEX" && mode != "ASCII") {
            service::common::fail(18002, "设备参数无效", 400);
        }
        if (mode == "OFF") {
            return;
        }
        const std::string_view content =
            packet->template get<"content">() ? packet->template get<"content">()->view() : std::string_view{};
        if (content.empty()) {
            service::common::fail(18002, "设备参数无效", 400);
        }
        if (mode == "ASCII" && content.size() > 256) {
            service::common::fail(18002, "注册包或心跳包不能超过 256 字节", 400);
        }
        if (mode == "HEX") {
            std::string stripped;
            for (const char ch : content) {
                if (!std::isspace(static_cast<unsigned char>(ch))) {
                    stripped.push_back(ch);
                }
            }
            if (stripped.empty() || stripped.size() % 2 != 0) {
                service::common::fail(18002, "设备参数无效", 400);
            }
            if (stripped.size() / 2 > 256) {
                service::common::fail(18002, "注册包或心跳包不能超过 256 字节", 400);
            }
            for (const char ch : stripped) {
                if (!std::isxdigit(static_cast<unsigned char>(ch))) {
                    service::common::fail(18002, "设备参数无效", 400);
                }
            }
        }
    }

    // 扁平字段（必填/长度/枚举/范围/UUID/timezone）由声明式校验器保证；
    // 此处只做跨字段、依赖 DB 与协议相关的校验（保留 18002/18003 域码）。
    template <typename Context>
    ruvia::Task<void> validate(Context& c, const SaveDeviceBody& body, bool required) {
        validatePacket(body.template get<"heartbeat">());
        validatePacket(body.template get<"registration">());
        const auto linkId = str(body.template get<"linkId">());
        const auto configId = str(body.template get<"protocolConfigId">());
        if (required && linkId.empty()) {
            service::common::fail(18003, "请选择通道", 400);
        }
        if (required && configId.empty()) {
            service::common::fail(18003, "请选择设备类型", 400);
        }

        const auto& code = body.template get<"deviceCode">();
        if (code) {
            if (code->view().empty() || code->view().size() > 100) {
                service::common::fail(18002, "设备编码长度必须在 1 - 100 之间", 400);
            }
            for (const auto character : code->view()) {
                if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' && character != '_') {
                    service::common::fail(18002, "设备编码只能包含字母、数字、连字符和下划线", 400);
                }
            }
        }
        if (body.template get<"groupId">() && !body.template get<"groupId">()->view().empty()) {
            ruvia::DbQuery groupQuery(c.pool());
            groupQuery
                .select(DeviceAccessService::integer(groupQuery, 1))
                .from(service::device::entities::DeviceGroupEntity::tableName())
                .where(andAll(groupQuery, groupQuery.binary(groupQuery.column(service::device::entities::DeviceGroupEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(groupQuery, body.template get<"groupId">()->view())), groupQuery.unary(ruvia::DbUnaryOperator::kIsNull, groupQuery.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">()))));
            const auto group = co_await c.db().query(groupQuery);
            if (group.empty()) {
                service::common::fail(18003, "设备分组不存在", 400);
            }
        }
        if (configId.empty() || linkId.empty()) {
            co_return;
        }

        ruvia::DbQuery relationQuery(c.pool());
        relationQuery
            .select(relationQuery.column(service::device::entities::LinkEntity::columnName<"protocol">(), "l"))
            .from(service::device::entities::LinkEntity::tableName(), "l")
            .join(ruvia::DbJoinType::kInner, service::device::entities::ProtocolConfigEntity::tableName(), relationQuery.binary(relationQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"protocol">(), "p"), ruvia::DbBinaryOperator::kEqual, relationQuery.column(service::device::entities::LinkEntity::columnName<"protocol">(), "l")), "p")
            .where(andAll(relationQuery, relationQuery.binary(relationQuery.column(service::device::entities::LinkEntity::columnName<"id">(), "l"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(relationQuery, linkId)), relationQuery.binary(relationQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"id">(), "p"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(relationQuery, configId)), relationQuery.unary(ruvia::DbUnaryOperator::kIsNull, relationQuery.column(service::device::entities::LinkEntity::columnName<"deleted_at">(), "l")), relationQuery.unary(ruvia::DbUnaryOperator::kIsNull, relationQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"deleted_at">(), "p"))));
        const auto relation = co_await c.db().query(relationQuery);
        if (relation.empty()) {
            service::common::fail(18003, "通道或设备类型不存在，或协议不一致", 400);
        }
        const std::string configProtocol(relation.front()[0].value().value_or(""));
        if (configProtocol == "SL651" &&
            (packetEnabled(body.template get<"heartbeat">()) || packetEnabled(body.template get<"registration">()))) {
            service::common::fail(18002, "SL651 设备不支持配置注册包或心跳包", 400);
        }
        if (configProtocol == "DLT645" && code) {
            if (code->view().size() != 12 || !std::all_of(code->view().begin(), code->view().end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                })) {
                service::common::fail(18002, "DL/T645 表地址必须是 12 位数字，不足时左侧补零", 400);
            }
        }
        if (configProtocol == "SL651" && code) {
            if (code->view().size() != 10) {
                service::common::fail(18002, "SL651 遥测站地址必须是 10 位数字，不足时左侧补零", 400);
            }
            for (const auto character : code->view()) {
                if (!std::isdigit(static_cast<unsigned char>(character))) {
                    service::common::fail(18002, "SL651 设备编码必须是数字遥测站地址", 400);
                }
            }
        }
    }

    static bool packetEnabled(const std::optional<DevicePacketBody>& packet) {
        return packet && packet->template get<"mode">() && packet->template get<"mode">()->view() != "OFF";
    }

    template <typename Context>
    ruvia::Task<void> validateRuntimeIdentity(Context& c, const SaveDeviceBody& body, std::optional<std::string> excludedId) {
        const std::string excluded = excludedId.value_or(std::string(kNilUuid));
        const std::string inLinkId = str(body.template get<"linkId">());
        const std::string inTargetId = str(body.template get<"targetId">());
        const std::string inConfigId = str(body.template get<"protocolConfigId">());
        const std::string inSlaveId =
            body.template get<"slaveId">() ? std::to_string(static_cast<std::int64_t>(*body.template get<"slaveId">())) : "";
        const std::string inRegistration = packetJson(body.template get<"registration">());
        const std::string inHeartbeat = packetJson(body.template get<"heartbeat">());
        ruvia::DbQuery currentDevice(c.pool());
        currentDevice.select({ currentDevice.column(service::device::entities::DeviceEntity::columnName<"link_id">()), currentDevice.column(service::device::entities::DeviceEntity::columnName<"protocol_config_id">()), currentDevice.column(service::device::entities::DeviceEntity::columnName<"protocol_params">()) })
            .from(service::device::entities::DeviceEntity::tableName())
            .where(andAll(currentDevice, currentDevice.binary(currentDevice.column(service::device::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(currentDevice, excluded)), currentDevice.unary(ruvia::DbUnaryOperator::kIsNull, currentDevice.column(service::device::entities::DeviceEntity::columnName<"deleted_at">()))));
        ruvia::DbQuery candidateQuery(c.pool());
        candidateQuery.with("current_device", currentDevice);
        const auto currentParams = candidateQuery.column("protocol_params", "current");
        const auto candidateLink = candidateQuery.coalesce(
            { DeviceAccessService::nullableUuid(candidateQuery, inLinkId),
              candidateQuery.column("link_id", "current") }
        );
        const auto candidateTarget = candidateQuery.coalesce(
            { candidateQuery.nullIf(DeviceAccessService::text(candidateQuery, candidateQuery.value(inTargetId)), candidateQuery.value("")),
              DeviceAccessService::jsonText(candidateQuery, currentParams, "target_id"),
              candidateQuery.value("") }
        );
        const auto candidateConfig = candidateQuery.coalesce(
            { DeviceAccessService::nullableUuid(candidateQuery, inConfigId),
              candidateQuery.column("protocol_config_id", "current") }
        );
        const auto inputSlave = candidateQuery.nullIf(
            DeviceAccessService::text(candidateQuery, candidateQuery.value(inSlaveId)),
            candidateQuery.value("")
        );
        const auto storedSlave = DeviceAccessService::jsonText(candidateQuery, currentParams, "slave_id");
        const auto parseSlave = [&](ruvia::DbQuery::Expr value) {
            return candidateQuery.caseWhen(
                { { candidateQuery.binary(candidateQuery.coalesce({ value, candidateQuery.value("") }), ruvia::DbBinaryOperator::kRegex, candidateQuery.value("^-?[0-9]{1,18}$")),
                    candidateQuery.cast(value, ruvia::DbDataType::kInteger) } }
            );
        };
        const auto candidateSlave = candidateQuery.coalesce(
            { parseSlave(inputSlave), parseSlave(storedSlave), DeviceAccessService::integer(candidateQuery, 1) }
        );
        const auto defaultPacket = candidateQuery.cast(
            candidateQuery.value(R"({"mode":"OFF"})"),
            ruvia::DbDataType::kJsonb
        );
        const auto candidateRegistration = candidateQuery.coalesce(
            { candidateQuery.cast(candidateQuery.nullIf(DeviceAccessService::text(candidateQuery, candidateQuery.value(inRegistration)), candidateQuery.value("")), ruvia::DbDataType::kJsonb),
              DeviceAccessService::jsonValue(candidateQuery, currentParams, "registration"),
              defaultPacket }
        );
        const auto candidateHeartbeat = candidateQuery.coalesce(
            { candidateQuery.cast(candidateQuery.nullIf(DeviceAccessService::text(candidateQuery, candidateQuery.value(inHeartbeat)), candidateQuery.value("")), ruvia::DbDataType::kJsonb),
              DeviceAccessService::jsonValue(candidateQuery, currentParams, "heartbeat"),
              defaultPacket }
        );
        const auto registrationModeExpr = candidateQuery.call(
            "upper",
            { candidateQuery.coalesce({ DeviceAccessService::jsonText(candidateQuery, candidateRegistration, "mode"), candidateQuery.value("OFF") }) }
        );
        const auto registrationContent = candidateQuery.coalesce(
            { DeviceAccessService::jsonText(candidateQuery, candidateRegistration, "content"),
              candidateQuery.value("") }
        );
        const auto registrationKeyExpr = candidateQuery.caseWhen(
            { { candidateQuery.binary(registrationModeExpr, ruvia::DbBinaryOperator::kEqual, candidateQuery.value("OFF")),
                candidateQuery.value("OFF:") },
              { candidateQuery.binary(registrationModeExpr, ruvia::DbBinaryOperator::kEqual, candidateQuery.value("HEX")),
                candidateQuery.binary(
                    DeviceAccessService::text(candidateQuery, "HEX:"),
                    ruvia::DbBinaryOperator::kConcat,
                    candidateQuery.call(
                        "upper",
                        { candidateQuery.call(
                            "regexp_replace",
                            { registrationContent, candidateQuery.value("\\s"), candidateQuery.value(""), candidateQuery.value("g") }
                        ) }
                    )
                ) } },
            candidateQuery.binary(DeviceAccessService::text(candidateQuery, "ASCII:"), ruvia::DbBinaryOperator::kConcat, registrationContent)
        );
        const auto heartbeatModeExpr = candidateQuery.call(
            "upper",
            { candidateQuery.coalesce({ DeviceAccessService::jsonText(candidateQuery, candidateHeartbeat, "mode"), candidateQuery.value("OFF") }) }
        );
        candidateQuery
            .select({ candidateLink, DeviceAccessService::jsonText(candidateQuery, candidateQuery.column(service::device::entities::LinkEntity::columnName<"endpoint">(), "link"), "mode"), candidateQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"protocol">(), "protocol"), candidateTarget, candidateSlave, registrationModeExpr, registrationKeyExpr, heartbeatModeExpr })
            .fromFunction(candidateQuery.call("generate_series", { DeviceAccessService::integer(candidateQuery, 1), DeviceAccessService::integer(candidateQuery, 1) }), "base")
            .join(ruvia::DbJoinType::kLeft, "current_device", DeviceAccessService::boolean(candidateQuery, true), "current")
            .join(ruvia::DbJoinType::kInner, service::device::entities::LinkEntity::tableName(), andAll(candidateQuery, candidateQuery.binary(candidateQuery.column(service::device::entities::LinkEntity::columnName<"id">(), "link"), ruvia::DbBinaryOperator::kEqual, candidateLink), candidateQuery.unary(ruvia::DbUnaryOperator::kIsNull, candidateQuery.column(service::device::entities::LinkEntity::columnName<"deleted_at">(), "link"))), "link")
            .join(ruvia::DbJoinType::kInner, service::device::entities::ProtocolConfigEntity::tableName(), andAll(candidateQuery, candidateQuery.binary(candidateQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"id">(), "protocol"), ruvia::DbBinaryOperator::kEqual, candidateConfig), candidateQuery.unary(ruvia::DbUnaryOperator::kIsNull, candidateQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"deleted_at">(), "protocol"))), "protocol")
            .limit(1);
        const auto candidate = co_await c.db().query(candidateQuery);
        if (candidate.empty()) {
            co_return;
        }

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
            if (registrationMode != "OFF" || heartbeatMode != "OFF") {
                service::common::fail(18002, protocol == "SL651" ? "SL651 设备不支持配置注册包或心跳包" : "仅 TCP Server 设备支持配置注册包或心跳包", 400);
            }
        }
        if (protocol != "Modbus" && protocol != "S7" && protocol != "MC" && protocol != "FINS") {
            co_return;
        }

        ruvia::DbQuery siblingsQuery(c.pool());
        const auto siblingParams = siblingsQuery.column(service::device::entities::DeviceEntity::columnName<"protocol_params">(), "device");
        const auto siblingSlaveText = DeviceAccessService::jsonText(
            siblingsQuery,
            siblingParams,
            "slave_id"
        );
        const auto siblingSlave = siblingsQuery.coalesce(
            { siblingsQuery.caseWhen(
                  { { siblingsQuery.binary(siblingsQuery.coalesce({ siblingSlaveText, siblingsQuery.value("") }), ruvia::DbBinaryOperator::kRegex, siblingsQuery.value("^-?[0-9]{1,18}$")),
                      siblingsQuery.cast(siblingSlaveText, ruvia::DbDataType::kInteger) } }
              ),
              DeviceAccessService::integer(siblingsQuery, 1) }
        );
        const auto siblingRegistration = DeviceAccessService::jsonValue(
            siblingsQuery,
            siblingParams,
            "registration"
        );
        const auto siblingMode = siblingsQuery.call(
            "upper",
            { siblingsQuery.coalesce({ DeviceAccessService::jsonText(siblingsQuery, siblingRegistration, "mode"), siblingsQuery.value("OFF") }) }
        );
        const auto siblingContent = siblingsQuery.coalesce(
            { DeviceAccessService::jsonText(siblingsQuery, siblingRegistration, "content"),
              siblingsQuery.value("") }
        );
        const auto siblingKey = siblingsQuery.caseWhen(
            { { siblingsQuery.binary(siblingMode, ruvia::DbBinaryOperator::kEqual, siblingsQuery.value("OFF")),
                siblingsQuery.value("OFF:") },
              { siblingsQuery.binary(siblingMode, ruvia::DbBinaryOperator::kEqual, siblingsQuery.value("HEX")),
                siblingsQuery.binary(
                    DeviceAccessService::text(siblingsQuery, "HEX:"),
                    ruvia::DbBinaryOperator::kConcat,
                    siblingsQuery.call(
                        "upper",
                        { siblingsQuery.call(
                            "regexp_replace",
                            { siblingContent, siblingsQuery.value("\\s"), siblingsQuery.value(""), siblingsQuery.value("g") }
                        ) }
                    )
                ) } },
            siblingsQuery.binary(DeviceAccessService::text(siblingsQuery, "ASCII:"), ruvia::DbBinaryOperator::kConcat, siblingContent)
        );
        siblingsQuery
            .select({ siblingsQuery.column(service::device::entities::DeviceEntity::columnName<"name">(), "device"), siblingSlave, siblingsQuery.coalesce({ DeviceAccessService::jsonText(siblingsQuery, siblingParams, "target_id"), siblingsQuery.value("") }), siblingMode, siblingKey })
            .from(service::device::entities::DeviceEntity::tableName(), "device")
            .join(ruvia::DbJoinType::kInner, service::device::entities::ProtocolConfigEntity::tableName(), andAll(siblingsQuery, siblingsQuery.binary(siblingsQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"id">(), "config"), ruvia::DbBinaryOperator::kEqual, siblingsQuery.column(service::device::entities::DeviceEntity::columnName<"protocol_config_id">(), "device")), siblingsQuery.unary(ruvia::DbUnaryOperator::kIsNull, siblingsQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"deleted_at">(), "config"))), "config")
            .where(andAll(siblingsQuery, siblingsQuery.binary(siblingsQuery.column(service::device::entities::DeviceEntity::columnName<"link_id">(), "device"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(siblingsQuery, linkId)), siblingsQuery.binary(siblingsQuery.column(service::device::entities::DeviceEntity::columnName<"id">(), "device"), ruvia::DbBinaryOperator::kNotEqual, DeviceAccessService::uuid(siblingsQuery, excluded)), siblingsQuery.unary(ruvia::DbUnaryOperator::kIsNull, siblingsQuery.column(service::device::entities::DeviceEntity::columnName<"deleted_at">(), "device")), siblingsQuery.binary(siblingsQuery.column(service::device::entities::ProtocolConfigEntity::columnName<"protocol">(), "config"), ruvia::DbBinaryOperator::kEqual, siblingsQuery.value(protocol))))
            .orderBy(siblingsQuery.column(service::device::entities::DeviceEntity::columnName<"id">(), "device"));
        const auto siblings = co_await c.db().query(siblingsQuery);

        if (linkMode == "TCP Client") {
            if (targetId.empty()) {
                co_return;
            }
            for (const auto& sibling : siblings) {
                if (sibling[2].value().value_or(std::string_view{}) != targetId) {
                    continue;
                }
                const std::string name(sibling[0].value().value_or(std::string_view{}));
                if (protocol == "S7" || protocol == "MC" || protocol == "FINS") {
                    service::common::fail(
                        18006,
                        protocol + " 同一目标地址只能关联一个设备，冲突设备: " + name,
                        409
                    );
                }
                if (toInt(sibling[1].value().value_or(std::string_view{})) == slaveId) {
                    service::common::fail(
                        18006,
                        "Modbus 同一目标地址下 Slave ID 重复，冲突设备: " + name,
                        409
                    );
                }
            }
            co_return;
        }
        if (linkMode != "TCP Server") {
            co_return;
        }

        if (protocol == "Modbus" && !siblings.empty()) {
            if (registrationMode == "OFF") {
                service::common::fail(18006, "Modbus TCP Server 链路存在多个设备时必须配置注册包", 409);
            }
            for (const auto& sibling : siblings) {
                const std::string name(sibling[0].value().value_or(std::string_view{}));
                if (sibling[3].value().value_or(std::string_view{}) == "OFF") {
                    service::common::fail(
                        18006,
                        "Modbus TCP Server 链路存在未配置注册包的设备: " + name,
                        409
                    );
                }
                if (sibling[4].value().value_or(std::string_view{}) == registrationKey && toInt(sibling[1].value().value_or(std::string_view{})) == slaveId) {
                    service::common::fail(
                        18006,
                        "Modbus 同一链路和注册码下 Slave ID 重复，冲突设备: " + name,
                        409
                    );
                }
            }
            co_return;
        }

        if (protocol == "S7" || protocol == "MC" || protocol == "FINS") {
            for (const auto& sibling : siblings) {
                if (sibling[4].value().value_or(std::string_view{}) == registrationKey) {
                    service::common::fail(18006, protocol + " TCP Server 同一链路下注册码重复，冲突设备: " + std::string(sibling[0].value().value_or(std::string_view{})), 409);
                }
            }
        }
    }

    template <typename Context>
    ruvia::Task<void> ensureUnique(Context& c, const SaveDeviceBody& body, std::optional<std::string> excludedId) {
        const auto& name = body.template get<"name">();
        const auto& code = body.template get<"deviceCode">();
        if (!name && !code) {
            co_return;
        }
        const std::string nameValue = str(name);
        const std::string codeValue = str(code);
        const std::string linkValue = str(body.template get<"linkId">());
        const std::string excluded = excludedId.value_or(std::string(kNilUuid));
        ruvia::DbQuery query(c.pool());
        ruvia::DbQuery currentLink(c.pool());
        currentLink.select(currentLink.column(service::device::entities::DeviceEntity::columnName<"link_id">()))
            .from(service::device::entities::DeviceEntity::tableName())
            .where(currentLink.binary(currentLink.column(service::device::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(currentLink, excluded)))
            .limit(1);
        const auto nameMatch = andAll(
            query,
            query.binary(DeviceAccessService::text(query, nameValue), ruvia::DbBinaryOperator::kNotEqual, DeviceAccessService::text(query, "")),
            query.binary(query.column(service::device::entities::DeviceEntity::columnName<"name">()), ruvia::DbBinaryOperator::kEqual, query.value(nameValue))
        );
        const auto codeMatch = andAll(
            query,
            query.binary(DeviceAccessService::text(query, codeValue), ruvia::DbBinaryOperator::kNotEqual, DeviceAccessService::text(query, "")),
            query.binary(
                DeviceAccessService::jsonText(query, query.column(service::device::entities::DeviceEntity::columnName<"protocol_params">()), "device_code"),
                ruvia::DbBinaryOperator::kEqual,
                query.value(codeValue)
            ),
            query.binary(query.column(service::device::entities::DeviceEntity::columnName<"link_id">()), ruvia::DbBinaryOperator::kEqual, query.coalesce({ DeviceAccessService::nullableUuid(query, linkValue), query.subquery(currentLink) }))
        );
        query.select(DeviceAccessService::integer(query, 1))
            .from(service::device::entities::DeviceEntity::tableName())
            .where(andAll(query, query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::device::entities::DeviceEntity::columnName<"deleted_at">())), query.binary(query.column(service::device::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kNotEqual, DeviceAccessService::uuid(query, excluded)), query.binary(nameMatch, ruvia::DbBinaryOperator::kOr, codeMatch)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (!rows.empty()) {
            service::common::fail(18004, "设备名称已存在或同一链路的设备编码重复", 409);
        }
    }

    // ----- 设备分组私有工具 -----

    template <typename Row>
    static void fillGroup(DeviceGroupItemDto& item, const Row& row, const DeviceActor& actor) {
        item.template set<"id">(row[0].value().value_or(std::string_view{}));
        item.template set<"name">(row[1].value().value_or(std::string_view{}));
        item.template set<"parentId">(row[2].value().value_or(std::string_view{}));
        item.template set<"status">(row[3].value().value_or(std::string_view{}));
        item.template set<"sortOrder">(toInt(row[4].value().value_or(std::string_view{})));
        item.template set<"remark">(row[5].value().value_or(std::string_view{}));
        item.template set<"deviceCount">(toInt(row[6].value().value_or(std::string_view{})));
        item.template set<"createdAt">(row[7].value().value_or(std::string_view{}));
        item.template set<"updatedAt">(row[8].value().value_or(std::string_view{}));
        item.template set<"canShare">(
            actor.canGroupShare &&
            (actor.superadmin ||
             row[9].value().value_or(std::string_view{}) == actor.userId)
        );
    }

    template <typename Context>
    ruvia::Task<void> validateParent(Context& c, const SaveDeviceGroupBody& body, std::optional<std::string> currentId) {
        const auto& parent = body.template get<"parentId">();
        if (!parent || parent->view().empty()) {
            co_return;
        }
        if (!service::common::isUuid(parent->view())) {
            service::common::fail(17002, "上级分组必须是 UUID", 400);
        }
        if (currentId && parent->view() == *currentId) {
            service::common::fail(17003, "上级分组不能是自身", 409);
        }
        ruvia::DbQuery query(c.pool());
        query.select(DeviceAccessService::integer(query, 1))
            .from(service::device::entities::DeviceGroupEntity::tableName())
            .where(andAll(query, query.binary(query.column(service::device::entities::DeviceGroupEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(query, parent->view())), query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">()))))
            .limit(1);
        const auto exists = co_await c.db().query(query);
        if (exists.empty()) {
            service::common::fail(17003, "上级分组不存在", 400);
        }
    }

    template <typename Context>
    ruvia::Task<void> requireGroupOwner(Context& c, std::string_view ownerId) {
        if (c.userId == ownerId) {
            co_return;
        }
        ruvia::DbQuery roles(c.pool());
        roles.select(DeviceAccessService::integer(roles, 1))
            .from(service::device::entities::SysUserRoleEntity::tableName(), "ur")
            .join(ruvia::DbJoinType::kInner, service::device::entities::SysRoleEntity::tableName(), andAll(roles, roles.binary(roles.column(service::device::entities::SysRoleEntity::columnName<"id">(), "r"), ruvia::DbBinaryOperator::kEqual, roles.column(service::device::entities::SysUserRoleEntity::columnName<"role_id">(), "ur")), roles.binary(roles.column(service::device::entities::SysRoleEntity::columnName<"code">(), "r"), ruvia::DbBinaryOperator::kEqual, roles.value("superadmin")), roles.binary(roles.column(service::device::entities::SysRoleEntity::columnName<"status">(), "r"), ruvia::DbBinaryOperator::kEqual, roles.value("enabled")), roles.unary(ruvia::DbUnaryOperator::kIsNull, roles.column(service::device::entities::SysRoleEntity::columnName<"deleted_at">(), "r"))), "r")
            .where(roles.binary(roles.column(service::device::entities::SysUserRoleEntity::columnName<"user_id">(), "ur"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(roles, c.userId)))
            .limit(1);
        ruvia::DbQuery superadmin(c.pool());
        superadmin.select(superadmin.exists(roles));
        const auto rows = co_await c.db().query(superadmin);
        if (rows.front()[0].value().value_or(std::string_view{}) != "t") {
            service::common::fail(17005, "只能管理自己创建的设备分组", 403);
        }
    }

    static constexpr std::string_view kNilUuid = "00000000-0000-0000-0000-000000000000";
};

class DeviceShareService {
  public:
    static DeviceShareService& instance() {
        static thread_local DeviceShareService service;
        return service;
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<DeviceShareItemDto>> list(Context& c, std::string_view deviceId) {
        (void)co_await deviceAccessService().require(c, deviceId, DeviceAccessLevel::owner, c.userId);
        ruvia::DbQuery currentDevice(c.pool());
        currentDevice.select(currentDevice.column(service::device::entities::DeviceEntity::columnName<"group_id">()))
            .from(service::device::entities::DeviceEntity::tableName())
            .where(currentDevice.binary(currentDevice.column(service::device::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(currentDevice, deviceId)));
        ruvia::DbQuery ancestor(c.pool());
        ruvia::DbQuery ancestorRecursive(c.pool());
        ancestor.select({ ancestor.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "device_group"), ancestor.column(service::device::entities::DeviceGroupEntity::columnName<"parent_id">(), "device_group"), ancestor.column(service::device::entities::DeviceGroupEntity::columnName<"name">(), "device_group") })
            .from(service::device::entities::DeviceGroupEntity::tableName(), "device_group")
            .join(ruvia::DbJoinType::kInner, "current_device", ancestor.binary(ancestor.column("group_id", "current_device"), ruvia::DbBinaryOperator::kEqual, ancestor.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "device_group")), "current_device")
            .where(ancestor.unary(ruvia::DbUnaryOperator::kIsNull, ancestor.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">(), "device_group")));
        ancestorRecursive
            .select({ ancestorRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "parent"), ancestorRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"parent_id">(), "parent"), ancestorRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"name">(), "parent") })
            .from(service::device::entities::DeviceGroupEntity::tableName(), "parent")
            .join(ruvia::DbJoinType::kInner, "ancestor_group", ancestorRecursive.binary(ancestorRecursive.column("parent_id", "child"), ruvia::DbBinaryOperator::kEqual, ancestorRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "parent")), "child")
            .where(ancestorRecursive.unary(ruvia::DbUnaryOperator::kIsNull, ancestorRecursive.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">(), "parent")));
        ancestor.combine(ruvia::DbSetOperation::kUnionAll, ancestorRecursive);

        ruvia::DbQuery direct(c.pool());
        const auto directUser = direct.unary(ruvia::DbUnaryOperator::kIsNotNull, direct.column(service::device::entities::DeviceAccessGrantEntity::columnName<"user_id">(), "access_grant"));
        const auto directSubjectType = direct.caseWhen(
            { { directUser, DeviceAccessService::text(direct, direct.value("user")) } },
            DeviceAccessService::text(direct, direct.value("department"))
        );
        const auto directName = direct.caseWhen(
            { { directUser,
                direct.coalesce({ direct.nullIf(direct.column(service::device::entities::SysUserEntity::columnName<"nickname">(), "target_user"), direct.value("")), direct.column(service::device::entities::SysUserEntity::columnName<"username">(), "target_user"), direct.value("已删除用户") }) } },
            direct.coalesce({ direct.column(service::device::entities::SysDepartmentEntity::columnName<"name">(), "target_department"), direct.value("已删除部门") })
        );
        direct
            .select({ DeviceAccessService::text(direct, direct.column(service::device::entities::DeviceAccessGrantEntity::columnName<"id">(), "access_grant")), directSubjectType, direct.coalesce({ DeviceAccessService::text(direct, direct.column(service::device::entities::DeviceAccessGrantEntity::columnName<"user_id">(), "access_grant")), DeviceAccessService::text(direct, direct.column(service::device::entities::DeviceAccessGrantEntity::columnName<"department_id">(), "access_grant")) }), directName, direct.column(service::device::entities::DeviceAccessGrantEntity::columnName<"access_level">(), "access_grant"), DeviceAccessService::text(direct, direct.value("device")), DeviceAccessService::text(direct, direct.value("")), DeviceAccessService::text(direct, direct.value("")), DeviceAccessService::boolean(direct, false), direct.call("iot_utc_timestamp", { direct.column(service::device::entities::DeviceAccessGrantEntity::columnName<"created_at">(), "access_grant") }), direct.call("iot_utc_timestamp", { direct.column(service::device::entities::DeviceAccessGrantEntity::columnName<"updated_at">(), "access_grant") }) })
            .from(service::device::entities::DeviceAccessGrantEntity::tableName(), "access_grant")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::SysUserEntity::tableName(), direct.binary(direct.column(service::device::entities::SysUserEntity::columnName<"id">(), "target_user"), ruvia::DbBinaryOperator::kEqual, direct.column(service::device::entities::DeviceAccessGrantEntity::columnName<"user_id">(), "access_grant")), "target_user")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::SysDepartmentEntity::tableName(), direct.binary(direct.column(service::device::entities::SysDepartmentEntity::columnName<"id">(), "target_department"), ruvia::DbBinaryOperator::kEqual, direct.column(service::device::entities::DeviceAccessGrantEntity::columnName<"department_id">(), "access_grant")), "target_department")
            .where(direct.binary(direct.column(service::device::entities::DeviceAccessGrantEntity::columnName<"device_id">(), "access_grant"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(direct, deviceId)));

        ruvia::DbQuery inherited(c.pool());
        const auto inheritedUser = inherited.unary(
            ruvia::DbUnaryOperator::kIsNotNull,
            inherited.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"user_id">(), "group_access")
        );
        const auto inheritedSubjectType = inherited.caseWhen(
            { { inheritedUser, DeviceAccessService::text(inherited, inherited.value("user")) } },
            DeviceAccessService::text(inherited, inherited.value("department"))
        );
        const auto inheritedName = inherited.caseWhen(
            { { inheritedUser,
                inherited.coalesce({ inherited.nullIf(inherited.column(service::device::entities::SysUserEntity::columnName<"nickname">(), "inherited_user"), inherited.value("")), inherited.column(service::device::entities::SysUserEntity::columnName<"username">(), "inherited_user"), inherited.value("已删除用户") }) } },
            inherited.coalesce({ inherited.column(service::device::entities::SysDepartmentEntity::columnName<"name">(), "inherited_department"), inherited.value("已删除部门") })
        );
        inherited
            .select({ DeviceAccessService::text(inherited, inherited.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"id">(), "group_access")), inheritedSubjectType, inherited.coalesce({ DeviceAccessService::text(inherited, inherited.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"user_id">(), "group_access")), DeviceAccessService::text(inherited, inherited.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"department_id">(), "group_access")) }), inheritedName, inherited.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"access_level">(), "group_access"), DeviceAccessService::text(inherited, inherited.value("group")), DeviceAccessService::text(inherited, inherited.column("id", "ancestor")), inherited.column("name", "ancestor"), DeviceAccessService::boolean(inherited, true), inherited.call("iot_utc_timestamp", { inherited.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"created_at">(), "group_access") }), inherited.call("iot_utc_timestamp", { inherited.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"updated_at">(), "group_access") }) })
            .from(service::device::entities::DeviceGroupAccessGrantEntity::tableName(), "group_access")
            .join(ruvia::DbJoinType::kInner, "ancestor_group", inherited.binary(inherited.column("id", "ancestor"), ruvia::DbBinaryOperator::kEqual, inherited.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"group_id">(), "group_access")), "ancestor")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::SysUserEntity::tableName(), inherited.binary(inherited.column(service::device::entities::SysUserEntity::columnName<"id">(), "inherited_user"), ruvia::DbBinaryOperator::kEqual, inherited.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"user_id">(), "group_access")), "inherited_user")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::SysDepartmentEntity::tableName(), inherited.binary(inherited.column(service::device::entities::SysDepartmentEntity::columnName<"id">(), "inherited_department"), ruvia::DbBinaryOperator::kEqual, inherited.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"department_id">(), "group_access")), "inherited_department");
        direct.combine(ruvia::DbSetOperation::kUnionAll, inherited);
        ruvia::DbQuery query(c.pool());
        query.with("current_device", currentDevice)
            .with("ancestor_group", ancestor, { .recursive = true, .columns = { "id", "parent_id", "name" } })
            .with("device_share", direct, { .columns = { "id", "subject_type", "subject_id", "subject_name", "access_level", "source_type", "source_group_id", "source_group_name", "inherited", "created_at", "updated_at" } });
        query.select(query.star("device_share"))
            .from("device_share", "device_share")
            .orderBy(query.column("subject_type", "device_share"))
            .addOrderBy(query.column("subject_name", "device_share"))
            .addOrderBy(query.column("inherited", "device_share"))
            .addOrderBy(query.column("id", "device_share"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceShareItemDto> result(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.template set<"id">(row[0].value().value_or(std::string_view{}))
                .template set<"subjectType">(row[1].value().value_or(std::string_view{}))
                .template set<"subjectId">(row[2].value().value_or(std::string_view{}))
                .template set<"subjectName">(row[3].value().value_or(std::string_view{}))
                .template set<"accessLevel">(row[4].value().value_or(std::string_view{}))
                .template set<"sourceType">(row[5].value().value_or(std::string_view{}))
                .template set<"sourceGroupId">(row[6].value().value_or(std::string_view{}))
                .template set<"sourceGroupName">(row[7].value().value_or(std::string_view{}))
                .template set<"inherited">(row[8].value().value_or(std::string_view{}) == "t")
                .template set<"createdAt">(row[9].value().value_or(std::string_view{}))
                .template set<"updatedAt">(row[10].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<DeviceShareTargetDto>> targets(Context& c, std::string_view deviceId) {
        (void)co_await deviceAccessService().require(c, deviceId, DeviceAccessLevel::owner, c.userId);
        ruvia::DbQuery owner(c.pool());
        owner.select(owner.column(service::device::entities::DeviceEntity::columnName<"created_by">()))
            .from(service::device::entities::DeviceEntity::tableName())
            .where(owner.binary(owner.column(service::device::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(owner, deviceId)))
            .limit(1);
        ruvia::DbQuery users(c.pool());
        users.select({ DeviceAccessService::text(users, users.value("user")), DeviceAccessService::text(users, users.column(service::device::entities::SysUserEntity::columnName<"id">(), "target")), users.coalesce({ users.nullIf(users.column(service::device::entities::SysUserEntity::columnName<"nickname">(), "target"), users.value("")), users.column(service::device::entities::SysUserEntity::columnName<"username">(), "target") }) })
            .from(service::device::entities::SysUserEntity::tableName(), "target")
            .where(andAll(users, users.binary(users.column(service::device::entities::SysUserEntity::columnName<"status">(), "target"), ruvia::DbBinaryOperator::kEqual, users.value("enabled")), users.unary(ruvia::DbUnaryOperator::kIsNull, users.column(service::device::entities::SysUserEntity::columnName<"deleted_at">(), "target")), users.binary(users.column(service::device::entities::SysUserEntity::columnName<"id">(), "target"), ruvia::DbBinaryOperator::kNotEqual, users.subquery(owner))));
        ruvia::DbQuery departments(c.pool());
        departments.select({ DeviceAccessService::text(departments, departments.value("department")), DeviceAccessService::text(departments, departments.column(service::device::entities::SysDepartmentEntity::columnName<"id">(), "department")), departments.column(service::device::entities::SysDepartmentEntity::columnName<"name">(), "department") })
            .from(service::device::entities::SysDepartmentEntity::tableName(), "department")
            .where(andAll(departments, departments.binary(departments.column(service::device::entities::SysDepartmentEntity::columnName<"status">(), "department"), ruvia::DbBinaryOperator::kEqual, departments.value("enabled")), departments.unary(ruvia::DbUnaryOperator::kIsNull, departments.column(service::device::entities::SysDepartmentEntity::columnName<"deleted_at">(), "department"))));
        users.combine(ruvia::DbSetOperation::kUnionAll, departments);
        ruvia::DbQuery query(c.pool());
        query.with("share_target", users, { .columns = { "subject_type", "subject_id", "subject_name" } })
            .select(query.star("share_target"))
            .from("share_target", "share_target")
            .orderBy(query.column("subject_type", "share_target"))
            .addOrderBy(query.column("subject_name", "share_target"))
            .addOrderBy(query.column("subject_id", "share_target"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceShareTargetDto> result(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.template set<"subjectType">(row[0].value().value_or(std::string_view{})).template set<"subjectId">(row[1].value().value_or(std::string_view{})).template set<"subjectName">(row[2].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<void> replace(Context& c, std::string_view deviceId, const ReplaceDeviceSharesBody& body) {
        auto decision =
            co_await deviceAccessService().require(c, deviceId, DeviceAccessLevel::owner, c.userId);
        auto shares = normalize(body);

        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery deviceQuery(c.pool());
        deviceQuery.select(DeviceAccessService::text(deviceQuery, deviceQuery.column(service::device::entities::DeviceEntity::columnName<"created_by">())))
            .from(service::device::entities::DeviceEntity::tableName())
            .where(andAll(deviceQuery, deviceQuery.binary(deviceQuery.column(service::device::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(deviceQuery, deviceId)), deviceQuery.unary(ruvia::DbUnaryOperator::kIsNull, deviceQuery.column(service::device::entities::DeviceEntity::columnName<"deleted_at">()))))
            .lock({ .mode = ruvia::DbRowLock::kUpdate });
        const auto deviceRows = co_await transaction.query(deviceQuery);
        if (deviceRows.empty()) {
            service::common::fail(18001, "设备不存在", 404);
        }
        const std::string ownerId(deviceRows.front()[0].value().value_or(std::string_view{}));

        co_await validateTargets(transaction, shares, ownerId);

        ruvia::DbQuery remove(c.pool());
        remove.deleteFrom(service::device::entities::DeviceAccessGrantEntity::tableName())
            .where(remove.binary(remove.column(service::device::entities::DeviceAccessGrantEntity::columnName<"device_id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(remove, deviceId)));
        (void)co_await transaction.execute(remove);
        for (const auto& share : shares) {
            const std::string userId = share.subjectType == "user" ? share.subjectId : "";
            const std::string departmentId =
                share.subjectType == "department" ? share.subjectId : "";
            const auto grantId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
            ruvia::DbQuery insert(c.pool());
            insert.insertInto(service::device::entities::DeviceAccessGrantEntity::tableName(), { "id", "device_id", "user_id", "department_id", "access_level", "granted_by" })
                .values({ DeviceAccessService::uuid(insert, grantId), DeviceAccessService::uuid(insert, deviceId), DeviceAccessService::nullableUuid(insert, userId), DeviceAccessService::nullableUuid(insert, departmentId), insert.value(share.accessLevel), DeviceAccessService::uuid(insert, decision.actor.userId) });
            (void)co_await transaction.execute(insert);
        }
        const auto auditId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        const auto shareCount = static_cast<std::int64_t>(shares.size());
        ruvia::DbQuery audit(c.pool());
        audit.insertInto(service::device::entities::SecurityAuditLogEntity::tableName(), { "id", "actor_user_id", "action", "resource_type", "resource_id", "outcome", "details" })
            .values({ DeviceAccessService::uuid(audit, auditId), DeviceAccessService::uuid(audit, decision.actor.userId), audit.value("device.share.replace"), audit.value("device"), DeviceAccessService::uuid(audit, deviceId), audit.value("success"), audit.call("jsonb_build_object", { DeviceAccessService::textKey(audit, "share_count"), audit.cast(audit.value(shareCount), ruvia::DbDataType::kInteger) }) });
        (void)co_await transaction.execute(audit);
        co_await transaction.commit();
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<DeviceShareItemDto>> listGroup(Context& c, std::string_view groupId) {
        (void)co_await deviceAccessService().requireGroupOwner(c, groupId, c.userId);
        ruvia::DbQuery query(c.pool());
        const auto isUser = query.unary(ruvia::DbUnaryOperator::kIsNotNull, query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"user_id">(), "access_grant"));
        const auto subjectType = query.caseWhen(
            { { isUser, DeviceAccessService::text(query, query.value("user")) } },
            DeviceAccessService::text(query, query.value("department"))
        );
        const auto subjectName = query.caseWhen(
            { { isUser,
                query.coalesce({ query.nullIf(query.column(service::device::entities::SysUserEntity::columnName<"nickname">(), "target_user"), query.value("")), query.column(service::device::entities::SysUserEntity::columnName<"username">(), "target_user"), query.value("已删除用户") }) } },
            query.coalesce({ query.column(service::device::entities::SysDepartmentEntity::columnName<"name">(), "target_department"), query.value("已删除部门") })
        );
        query.select({ DeviceAccessService::text(query, query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"id">(), "access_grant")), subjectType, query.coalesce({ DeviceAccessService::text(query, query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"user_id">(), "access_grant")), DeviceAccessService::text(query, query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"department_id">(), "access_grant")) }), subjectName, query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"access_level">(), "access_grant"), DeviceAccessService::text(query, query.value("group")), DeviceAccessService::text(query, query.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "target_group")), query.column(service::device::entities::DeviceGroupEntity::columnName<"name">(), "target_group"), DeviceAccessService::boolean(query, false), query.call("iot_utc_timestamp", { query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"created_at">(), "access_grant") }), query.call("iot_utc_timestamp", { query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"updated_at">(), "access_grant") }) })
            .from(service::device::entities::DeviceGroupAccessGrantEntity::tableName(), "access_grant")
            .join(ruvia::DbJoinType::kInner, service::device::entities::DeviceGroupEntity::tableName(), query.binary(query.column(service::device::entities::DeviceGroupEntity::columnName<"id">(), "target_group"), ruvia::DbBinaryOperator::kEqual, query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"group_id">(), "access_grant")), "target_group")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::SysUserEntity::tableName(), query.binary(query.column(service::device::entities::SysUserEntity::columnName<"id">(), "target_user"), ruvia::DbBinaryOperator::kEqual, query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"user_id">(), "access_grant")), "target_user")
            .join(ruvia::DbJoinType::kLeft, service::device::entities::SysDepartmentEntity::tableName(), query.binary(query.column(service::device::entities::SysDepartmentEntity::columnName<"id">(), "target_department"), ruvia::DbBinaryOperator::kEqual, query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"department_id">(), "access_grant")), "target_department")
            .where(query.binary(query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"group_id">(), "access_grant"), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(query, groupId)))
            .orderBy(subjectType)
            .addOrderBy(subjectName)
            .addOrderBy(query.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"id">(), "access_grant"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceShareItemDto> result(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.template set<"id">(row[0].value().value_or(std::string_view{}))
                .template set<"subjectType">(row[1].value().value_or(std::string_view{}))
                .template set<"subjectId">(row[2].value().value_or(std::string_view{}))
                .template set<"subjectName">(row[3].value().value_or(std::string_view{}))
                .template set<"accessLevel">(row[4].value().value_or(std::string_view{}))
                .template set<"sourceType">(row[5].value().value_or(std::string_view{}))
                .template set<"sourceGroupId">(row[6].value().value_or(std::string_view{}))
                .template set<"sourceGroupName">(row[7].value().value_or(std::string_view{}))
                .template set<"inherited">(false)
                .template set<"createdAt">(row[9].value().value_or(std::string_view{}))
                .template set<"updatedAt">(row[10].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<DeviceShareTargetDto>> groupTargets(Context& c, std::string_view groupId) {
        (void)co_await deviceAccessService().requireGroupOwner(c, groupId, c.userId);
        ruvia::DbQuery owner(c.pool());
        owner.select(owner.column(service::device::entities::DeviceGroupEntity::columnName<"created_by">()))
            .from(service::device::entities::DeviceGroupEntity::tableName())
            .where(owner.binary(owner.column(service::device::entities::DeviceGroupEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(owner, groupId)))
            .limit(1);
        ruvia::DbQuery users(c.pool());
        users.select({ DeviceAccessService::text(users, users.value("user")), DeviceAccessService::text(users, users.column(service::device::entities::SysUserEntity::columnName<"id">(), "target")), users.coalesce({ users.nullIf(users.column(service::device::entities::SysUserEntity::columnName<"nickname">(), "target"), users.value("")), users.column(service::device::entities::SysUserEntity::columnName<"username">(), "target") }) })
            .from(service::device::entities::SysUserEntity::tableName(), "target")
            .where(andAll(users, users.binary(users.column(service::device::entities::SysUserEntity::columnName<"status">(), "target"), ruvia::DbBinaryOperator::kEqual, users.value("enabled")), users.unary(ruvia::DbUnaryOperator::kIsNull, users.column(service::device::entities::SysUserEntity::columnName<"deleted_at">(), "target")), users.binary(users.column(service::device::entities::SysUserEntity::columnName<"id">(), "target"), ruvia::DbBinaryOperator::kNotEqual, users.subquery(owner))));
        ruvia::DbQuery departments(c.pool());
        departments.select({ DeviceAccessService::text(departments, departments.value("department")), DeviceAccessService::text(departments, departments.column(service::device::entities::SysDepartmentEntity::columnName<"id">(), "department")), departments.column(service::device::entities::SysDepartmentEntity::columnName<"name">(), "department") })
            .from(service::device::entities::SysDepartmentEntity::tableName(), "department")
            .where(andAll(departments, departments.binary(departments.column(service::device::entities::SysDepartmentEntity::columnName<"status">(), "department"), ruvia::DbBinaryOperator::kEqual, departments.value("enabled")), departments.unary(ruvia::DbUnaryOperator::kIsNull, departments.column(service::device::entities::SysDepartmentEntity::columnName<"deleted_at">(), "department"))));
        users.combine(ruvia::DbSetOperation::kUnionAll, departments);
        ruvia::DbQuery query(c.pool());
        query.with("share_target", users, { .columns = { "subject_type", "subject_id", "subject_name" } })
            .select(query.star("share_target"))
            .from("share_target", "share_target")
            .orderBy(query.column("subject_type", "share_target"))
            .addOrderBy(query.column("subject_name", "share_target"))
            .addOrderBy(query.column("subject_id", "share_target"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeviceShareTargetDto> result(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.template set<"subjectType">(row[0].value().value_or(std::string_view{})).template set<"subjectId">(row[1].value().value_or(std::string_view{})).template set<"subjectName">(row[2].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<void> replaceGroup(Context& c, std::string_view groupId, const ReplaceDeviceSharesBody& body) {
        auto actor = co_await deviceAccessService().requireGroupOwner(c, groupId, c.userId);
        auto shares = normalize(body);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery groupQuery(c.pool());
        groupQuery.select(DeviceAccessService::text(groupQuery, groupQuery.column(service::device::entities::DeviceGroupEntity::columnName<"created_by">())))
            .from(service::device::entities::DeviceGroupEntity::tableName())
            .where(andAll(groupQuery, groupQuery.binary(groupQuery.column(service::device::entities::DeviceGroupEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(groupQuery, groupId)), groupQuery.unary(ruvia::DbUnaryOperator::kIsNull, groupQuery.column(service::device::entities::DeviceGroupEntity::columnName<"deleted_at">()))))
            .lock({ .mode = ruvia::DbRowLock::kUpdate });
        const auto groupRows = co_await transaction.query(groupQuery);
        if (groupRows.empty()) {
            service::common::fail(17001, "设备分组不存在", 404);
        }
        co_await validateTargets(transaction, shares, groupRows.front()[0].value().value_or(std::string_view{}));

        ruvia::DbQuery remove(c.pool());
        remove.deleteFrom(service::device::entities::DeviceGroupAccessGrantEntity::tableName())
            .where(remove.binary(remove.column(service::device::entities::DeviceGroupAccessGrantEntity::columnName<"group_id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(remove, groupId)));
        (void)co_await transaction.execute(remove);
        for (const auto& share : shares) {
            const std::string userId = share.subjectType == "user" ? share.subjectId : "";
            const std::string departmentId =
                share.subjectType == "department" ? share.subjectId : "";
            const auto grantId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
            ruvia::DbQuery insert(c.pool());
            insert.insertInto(service::device::entities::DeviceGroupAccessGrantEntity::tableName(), { "id", "group_id", "user_id", "department_id", "access_level", "granted_by" })
                .values({ DeviceAccessService::uuid(insert, grantId), DeviceAccessService::uuid(insert, groupId), DeviceAccessService::nullableUuid(insert, userId), DeviceAccessService::nullableUuid(insert, departmentId), insert.value(share.accessLevel), DeviceAccessService::uuid(insert, actor.userId) });
            (void)co_await transaction.execute(insert);
        }
        const auto auditId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        const auto shareCount = static_cast<std::int64_t>(shares.size());
        ruvia::DbQuery audit(c.pool());
        audit.insertInto(service::device::entities::SecurityAuditLogEntity::tableName(), { "id", "actor_user_id", "action", "resource_type", "resource_id", "outcome", "details" })
            .values({ DeviceAccessService::uuid(audit, auditId), DeviceAccessService::uuid(audit, actor.userId), audit.value("device_group.share.replace"), audit.value("device_group"), DeviceAccessService::uuid(audit, groupId), audit.value("success"), audit.call("jsonb_build_object", { DeviceAccessService::textKey(audit, "share_count"), audit.cast(audit.value(shareCount), ruvia::DbDataType::kInteger) }) });
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
        if (!body.template get<"shares">()) {
            service::common::fail(18010, "分享列表不能为空", 400);
        }
        std::vector<NormalizedShare> shares;
        shares.reserve(body.template get<"shares">()->size());
        std::set<std::string, std::less<>> uniqueSubjects;
        for (const auto& item : *body.template get<"shares">()) {
            if (!item.template get<"subjectType">() || !item.template get<"subjectId">() || !item.template get<"accessLevel">()) {
                service::common::fail(18010, "分享对象参数不完整", 400);
            }
            NormalizedShare share{ std::string(item.template get<"subjectType">()->view()),
                                   std::string(item.template get<"subjectId">()->view()),
                                   std::string(item.template get<"accessLevel">()->view()) };
            if (share.subjectType != "user" && share.subjectType != "department") {
                service::common::fail(18010, "分享对象类型无效", 400);
            }
            if (!service::common::isUuid(share.subjectId)) {
                service::common::fail(18010, "分享对象 ID 必须是 UUID", 400);
            }
            if (share.accessLevel != "view" && share.accessLevel != "operate") {
                service::common::fail(18010, "设备访问级别无效", 400);
            }
            if (!uniqueSubjects.emplace(share.subjectType + ":" + share.subjectId).second) {
                service::common::fail(18010, "分享对象不能重复", 400);
            }
            shares.emplace_back(std::move(share));
        }
        return shares;
    }

    static ruvia::Task<void> validateTargets(ruvia::DbTransaction& transaction, const std::vector<NormalizedShare>& shares, std::string_view ownerId) {
        for (const auto& share : shares) {
            if (share.subjectType == "user") {
                if (share.subjectId == ownerId) {
                    service::common::fail(18010, "不能向资源所有者重复授权", 400);
                }
                ruvia::DbQuery targetQuery;
                targetQuery.select(DeviceAccessService::integer(targetQuery, 1))
                    .from(service::device::entities::SysUserEntity::tableName())
                    .where(andAll(targetQuery, targetQuery.binary(targetQuery.column(service::device::entities::SysUserEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(targetQuery, share.subjectId)), targetQuery.binary(targetQuery.column(service::device::entities::SysUserEntity::columnName<"status">()), ruvia::DbBinaryOperator::kEqual, targetQuery.value("enabled")), targetQuery.unary(ruvia::DbUnaryOperator::kIsNull, targetQuery.column(service::device::entities::SysUserEntity::columnName<"deleted_at">()))))
                    .limit(1);
                const auto target = co_await transaction.query(targetQuery);
                if (target.empty()) {
                    service::common::fail(18010, "包含不存在或已禁用的用户", 400);
                }
            } else {
                ruvia::DbQuery targetQuery;
                targetQuery.select(DeviceAccessService::integer(targetQuery, 1))
                    .from(service::device::entities::SysDepartmentEntity::tableName())
                    .where(andAll(targetQuery, targetQuery.binary(targetQuery.column(service::device::entities::SysDepartmentEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, DeviceAccessService::uuid(targetQuery, share.subjectId)), targetQuery.binary(targetQuery.column(service::device::entities::SysDepartmentEntity::columnName<"status">()), ruvia::DbBinaryOperator::kEqual, targetQuery.value("enabled")), targetQuery.unary(ruvia::DbUnaryOperator::kIsNull, targetQuery.column(service::device::entities::SysDepartmentEntity::columnName<"deleted_at">()))))
                    .limit(1);
                const auto target = co_await transaction.query(targetQuery);
                if (target.empty()) {
                    service::common::fail(18010, "包含不存在或已禁用的部门", 400);
                }
            }
        }
    }
};

inline DeviceShareService& deviceShareService() {
    return DeviceShareService::instance();
}

inline DeviceService& deviceService() {
    return DeviceService::instance();
}

} // namespace service::device
