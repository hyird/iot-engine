# SQL ORM 迁移与能力边界

核对版本：Ruvia `13eba75f3b8317a9fe4b5e27ca70554a3e887cab`，从 `6357a680e527c9776eadd1ee4e6674b9012d4808` 升级。

Redis 继续按业务需要混用 Repository、原生命令和 Lua。新版将 Redis 实体与 SQL 实体分离，因此同步将 Worker 快照的两处映射和 VPN 调度记录改为 `RUVIA_REDIS_ENTITY` / `RUVIA_REDIS_COLUMN`；键名、字段、过期和消息确认机制不变。

## 已完成的迁移

上一轮迁移了 18 处单表查询、检查和创建操作，涉及认证、用户、协议配置和链路。本轮继续迁移 23 处 SQL 操作：

| 服务 | 本轮操作 | 数量 |
| --- | --- | --- |
| `service/modules/system/auth/auth.service.h` | 权限检查、用户角色查询、JSON 权限展开与去重 | 3 |
| `service/modules/system/user/user.service.h` | 列表与详情联表投影、用户角色查询、更新、软删除 | 5 |
| `service/modules/system/dept/dept.service.h` | 列表与详情联表投影、递归循环检查、更新、软删除 | 5 |
| `service/modules/system/role/role.service.h` | 列表、详情、创建、更新、软删除、使用检查、权限展开 | 7 |
| `service/modules/protocol/protocol.service.h` | 数据库端 JSON 合并更新、软删除 | 2 |
| `service/modules/link/link.service.h` | 软删除 | 1 |

认证、用户、角色和部门这四个服务的 SQL 访问已全部通过实体 Repository / QueryBuilder 执行。查询结果使用实体字段或类型化投影读取，不再按结果列下标手动映射。

用户和部门的联表查询分别使用所属实体文件中的 `UserDetails`、`DepartmentDetails`；角色权限展开使用 `RolePermission`，不为投影伪造数据表。用户、角色和部门实体补充实际读取和更新的时间字段、枚举类型名及默认表达式，角色补充实际读写的 JSONB 权限字段。历史 Schema/迁移未改写。

数据库 `now()`、软删除条件、JSON 合并、空字符串与 NULL 的区别、分页及排序语义保留。用户和角色绑定仍在同一事务中更新；协议和链路写入继续与 Outbox 事件共用事务。角色权限改为应用端生成 JSON 后参数化写入，保留空值、长度和逗号校验，并验证了引号、反斜杠、中文及重复权限。

## 上一轮缺口的核对结果

以下能力均已在本次固定版本的公开接口中找到，不能继续笼统列为“不支持”：

| 原缺口 | 新接口及本项目使用情况 |
| --- | --- |
| 表达式更新 | `DbExpressions` 与 `update(predicate, DbAssignment...)`；已用于数据库时间、NULL、JSON 合并。 |
| 写入返回值映射 | `insertReturning`、`updateReturning`、`deleteReturning`、`upsertReturning`；接口已具备，本轮操作不需要强行加入 RETURNING。 |
| 选择字段与类型化投影 | `select(DbSelection...)`、`getMany<Output>()` 和 `RUVIA_DB_PROJECTION`；已用于角色及用户、部门列表/详情。 |
| 任意读取联查、表函数、CTE、集合与窗口查询 | `join<Entity>`、`joinFunction`、`with`、`joinCte`、`combine`、表达式窗口函数等；已用于权限展开和部门递归检查。 |
| 自定义 Upsert | `DbUpsertOptions::updateExpressions` 和 `updateWhere`；普通 Upsert 的表达式与业务更新条件缺口已补齐。 |
| PostgreSQL 枚举与默认表达式元数据 | `DbColumnOptions::enumName` 和 `defaultExpression`；已补充系统管理实体，未改写历史迁移。 |

## 仍需保留 DbQuery 的具体边界

此处仅描述 Repository 层限制；Ruvia 的低层 `DbQuery` 能完成这些 SQL 操作。

| 尚存边界 | 实际受影响调用与保留原因 |
| --- | --- |
| Repository 的普通更新及 `updateReturning` 条件仍要求 `DbPredicate`，没有接收任意 `DbExpression` 条件的重载；QueryBuilder 的表达式 WHERE 只用于读取。 | 链路 `update` 需要在同一 UPDATE 内以 `IS DISTINCT FROM` 比较 JSONB、名称和状态，并根据影响行数决定是否写 Outbox。不能退化为应用端先查后写。简单 ID/状态条件的更新已经迁移，不能说所有条件更新都不支持。 |
| 写入 API 没有 `UPDATE ... FROM` / JOIN 或 `INSERT ... SELECT` 入口，也没有把写入语句组合进 CTE 的 Repository 构建器。新增 `with` 接收的是读取 QueryBuilder。 | Gateway 的 `claimCommand` 依赖跨表状态和截止时间的原子认领；命令超时更新使用 `UPDATE ... FROM`；遥测持久化包含输入集、排序/窗口、插入、最新值和状态更新组成的写入 CTE。不能用多次 Repository 调用拆散原有快照和原子边界。 |

依据：固定版本 `ruvia-web/include/ruvia/web/db/DbRepository.h`、`DbExpressions.h`、`DbPredicate.h`、`DbProjection.h`、`DbEntity.h`。

其他模块仍有尚未迁移的普通查询及复杂投影，不能全部归因为框架缺口。本轮没有宣称全仓库 SQL 已完成 ORM 迁移；也没有为绕开边界引入兼容宏、包装接口或修改 Ruvia 源码。

## 验证

- Windows Release 构建通过，依赖源码 HEAD 与 CMake 锁定 SHA 一致。
- 独立 PostgreSQL/Redis 环境的 `system-orm-integration.ts`、`business-orm-integration.ts`、`live-query-integration.ts` 通过。
- 覆盖权限 JSON 转义和重复值、多层部门循环拒绝与合法祖先调整、联表 NULL 和 UTC 时间输出、账号状态与令牌、数据库失败后的用户/角色事务回滚、协议 JSON 合并及 SSE 权限撤销。
- CTest 29 项中 28 项通过；`gb28181-sip` 失败信息为 `GB28181 IPv6 UDP catalog datagram was not delivered`。媒体及媒体代理测试通过，不能继续沿用上一轮的监听绑定失败结论。
- `git diff --check` 通过。本次未运行 Linux 构建，未提交、发布或部署。

2026-09-17：通信方式恢复为 HTTP/SSE 后，旧 `live-query-integration.ts` 已删除，其全局通知及系统管理 WS/SSE 假设不再适用；现行验证使用 `auth-http-sse-integration.ts`、`device-http-sse-integration.ts` 及恢复 HTTP 的系统/业务 ORM 集成测试。
