# 分层统一 · 切片 0+1（device + device-group 端到端）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `device` 与 `device-group` 从 raw-JSON 风格重构为 typed schema/DTO 风格并合并为同一后端模块，前端 `iot/device` 归位为五件套，端到端不改业务行为。

**Architecture:** 后端每模块四件套（`types.h` 定义 ModelObject DTO/Body/Query/Params，`schema.h` 定义校验器，`service.h` 返回强类型 DTO、DB 直连在内部，`controller.h` 只做编排）。device-group 作为设备子资源并入 `service/modules/northbridge/device/` 目录。前端每模块五件套（`index.tsx` + `*.api/schema/service/types.ts`）。

**Tech Stack:** C++23 header-only + Ruvia（`RUVIA_REQUEST_MODEL` / `RUVIA_RESPONSE_MODEL` / `RUVIA_VALIDATE_JSON|QUERY|PARAM`）；PostgreSQL；React 19 + Vite + zod + @tanstack/react-query + antd6。

## Global Constraints

- 编译目录只能用根目录 `build/`：`cmake -S . -B build && cmake --build build`。
- 不改数据库 schema/迁移、不改权限码、不改错误码、不改分页语义、不改 SQL 语义与字段集合。
- 时间字段维持 UTC（`TIMESTAMPTZ`、带 `Z` 的 ISO 8601、设备 `timezone` 解析语义）不变。
- 前后端校验规则一致：必填、长度、取值范围、枚举、格式、数组数量、分页、ID 约束逐条对齐，并核对数据库字段限制。
- 不新增 repository 层；不新增南北向绕过 Redis 的调用；不动 southbridge。
- 无应用级单测框架：每个后端任务的验证 = `cmake --build build` 通过 + 契约字段清单逐项核对；前端任务验证 = `bun run typecheck && bun run lint && bun run build && git diff --check`。
- 复用现有 `FormModal`、`PageContainer`、Ant Design 组件与工具类，不新增一次性抽象。
- 只 stage 本任务涉及的文件提交；保留工作区中他人/其他任务的在途改动，不顺手格式化无关文件。

**判定基准模板文件（照抄其结构）：**
- 后端：`service/modules/system/user/{user.types,user.schema,user.service,user.controller}.h`、`service/modules/northbridge/link/link.{types,service,controller}.h`
- 前端：`web/pages/system/user/*`、`web/pages/iot/link/*`

---

## Task 0: 落地"统一分层规范"到 design-patterns.md

**Files:**
- Modify: `docs/design-patterns.md`（在文末追加一节）

**Interfaces:**
- Produces: 一份可被后续任务引用的"统一分层规范"章节，含后端四件套/前端五件套的硬约束表。

- [ ] **Step 1: 追加规范章节**

在 `docs/design-patterns.md` 末尾追加以下内容（若已存在同名小节则替换）：

```markdown
## 统一分层规范（canonical layering）

### 后端业务模块 = 四件套

| 文件 | 职责 | 硬约束 |
|---|---|---|
| `*.types.h` | `RUVIA_REQUEST_MODEL` / `RUVIA_RESPONSE_MODEL` 定义 Body/Query/Params/DTO | 只定义数据形状，无逻辑 |
| `*.schema.h` | `RUVIA_VALIDATE_JSON/QUERY/PARAM` 校验器 | 绑定 types；必填/长度/取值范围/枚举/格式/数组数量/分页/ID 全覆盖 |
| `*.service.h` | 单例 service；返回强类型 DTO、入参强类型 Body；DB 直连在内部 | 禁止 `JsonValue& payload`；禁止用 `jsonb_build_object` 拼业务 JSON；禁止返回裸 `std::string` 作为业务数据 |
| `*.controller.h` | 路由 + `requirePermission` + `c.req().valid<Body>()` + `service::common::ok<Response>` 包壳 | 只做编排，无 SQL / 业务逻辑 |

判定基准：`service/modules/system/user/*`、`service/modules/northbridge/link/*`。
不新增 repository 层。`southbridge` 通信层不套用本规范。

### 前端页面模块 = 五件套

| 文件 | 职责 |
|---|---|
| `*.types.ts` | namespace 类型 + `queryKeys` |
| `*.schema.ts` | zod schema（与后端逐条对齐） |
| `*.api.ts` | endpoints；请求前一律 `schema.parse()` |
| `*.service.ts` | react-query hooks |
| `index.tsx` | 页面入口；子组件 `PascalCase.tsx`、局部 hooks `useXxx.ts`，同目录 |

判定基准：`web/pages/system/user/*`、`web/pages/iot/link/*`。
同一页面的子资源（如设备分组之于设备）保持同一模块，不拆分。
```

- [ ] **Step 2: 提交**

```bash
git add -f docs/design-patterns.md
git commit -m "docs: add canonical layering standard section"
```

---

## Task 1: device-group 后端转 typed 并并入 device 模块

先做 device-group（逻辑简单，作为 typed 转换的样板）。目标：删除 `service/modules/northbridge/device-group/`，在 `service/modules/northbridge/device/` 下新建 `device-group.{types,schema,service,controller}.h`，命名空间保持 `service::device_group`，路由与错误码完全不变。

**Files:**
- Create: `service/modules/northbridge/device/device-group.types.h`
- Create: `service/modules/northbridge/device/device-group.schema.h`
- Create: `service/modules/northbridge/device/device-group.service.h`
- Create: `service/modules/northbridge/device/device-group.controller.h`
- Delete: `service/modules/northbridge/device-group/device-group.service.h`
- Delete: `service/modules/northbridge/device-group/device-group.controller.h`
- Modify: `service/server.cpp`（更新 include 路径）

**Interfaces:**
- Consumes: `service::common::{ok,fail,operation}`、`service::middleware::{requireAuth,requirePermission}`、`service::common::{isUuid,nextUuidV7,dbParams}`。
- Produces: `service::device_group::deviceGroupService()`，方法：
  - `list(Context&, bool withCount) -> Task<ruvia::List<DeviceGroupItemDto>>`
  - `detail(Context&, string_view id) -> Task<DeviceGroupItemDto>`
  - `create(Context&, const SaveDeviceGroupBody&) -> Task<void>`
  - `update(Context&, string_view id, const SaveDeviceGroupBody&) -> Task<void>`
  - `remove(Context&, string_view id) -> Task<void>`
  - DTO 字段（与旧 jsonb 逐字段对齐）：`id,name,parent_id,status,sort_order,remark,deviceCount,created_at,updated_at`。

- [ ] **Step 1: 先看现有 controller 确认路由/权限码/错误码**

Run: `sed -n '1,200p' service/modules/northbridge/device-group/device-group.controller.h`
记录：路由前缀、每个方法的 `requirePermission("...")` 权限码、`list` 的 `withCount` 查询参数来源、错误码（17001/17002/17003/17004/17005）。**这些在重构后必须逐字不变。**

- [ ] **Step 2: 写 `device-group.types.h`**

```cpp
#pragma once

#include <ruvia/web/Model.h>

namespace service::device_group {

RUVIA_REQUEST_MODEL(DeviceGroupIdParams, RUVIA_FIELD(id, ruvia::String));

RUVIA_REQUEST_MODEL(DeviceGroupListQuery,
                    RUVIA_FIELD_NAME("withCount", withCount, ruvia::Bool, RUVIA_DEFAULT(false)));

RUVIA_REQUEST_MODEL(SaveDeviceGroupBody, RUVIA_FIELD(name, ruvia::String),
                    RUVIA_FIELD_NAME("parent_id", parentId, ruvia::String),
                    RUVIA_FIELD(status, ruvia::String),
                    RUVIA_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
                    RUVIA_FIELD(remark, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceGroupItemDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD_NAME("parent_id", parentId, ruvia::String),
                     RUVIA_FIELD(status, ruvia::String),
                     RUVIA_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
                     RUVIA_FIELD(remark, ruvia::String),
                     RUVIA_FIELD_NAME("deviceCount", deviceCount, ruvia::Int64),
                     RUVIA_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceGroupListResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String),
                     RUVIA_FIELD(data, ruvia::List<DeviceGroupItemDto>));
RUVIA_RESPONSE_MODEL(DeviceGroupDetailResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, DeviceGroupItemDto));

} // namespace service::device_group
```

> `parent_id` 在旧输出里可能是 `null`。保持 DTO 为 `ruvia::String` 并在 fill 时对 NULL 用空字符串，或按基准模板 `link` 处理 NULL 的方式一致处理（见 Step 4 fill 注释）。核对旧前端 `device-group.types.ts` 对 `parent_id` 的读取，确保 `null` 与 `""` 语义一致；若前端区分二者，改用可空处理（见 Step 4 备注）。

- [ ] **Step 3: 写 `device-group.schema.h`**（把旧 service 内联校验搬为声明式校验器 + 保留跨表校验在 service）

```cpp
#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/id.validation.h"
#include "service/modules/northbridge/device/device-group.types.h"

namespace service::device_group {

class SaveDeviceGroupValidator final : public ruvia::Middleware<SaveDeviceGroupValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        SaveDeviceGroupBody,
        RUVIA_RULE(name, RUVIA_MIN(1, "分组名称长度必须在 1 - 100 之间"),
                   RUVIA_MAX(100, "分组名称长度必须在 1 - 100 之间")),
        RUVIA_RULE(status, RUVIA_ONE_OF("设备分组参数无效", "enabled", "disabled")),
        RUVIA_RULE_NAME("sort_order", sortOrder, RUVIA_MIN(0, "设备分组参数无效")))
};

class CreateDeviceGroupValidator final : public ruvia::Middleware<CreateDeviceGroupValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        SaveDeviceGroupBody,
        RUVIA_RULE(name, RUVIA_REQUIRED("分组名称不能为空"),
                   RUVIA_MIN(1, "分组名称长度必须在 1 - 100 之间"),
                   RUVIA_MAX(100, "分组名称长度必须在 1 - 100 之间")),
        RUVIA_RULE(status, RUVIA_ONE_OF("设备分组参数无效", "enabled", "disabled")),
        RUVIA_RULE_NAME("sort_order", sortOrder, RUVIA_MIN(0, "设备分组参数无效")))
};

class DeviceGroupListQueryValidator final : public ruvia::Middleware<DeviceGroupListQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(DeviceGroupListQuery)
};

class DeviceGroupIdParamsValidator final : public ruvia::Middleware<DeviceGroupIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(DeviceGroupIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

} // namespace service::device_group
```

> `parent_id` 的"必须是 UUID / 不能是自身 / 上级必须存在"是**跨状态与跨表校验**（依赖 `currentId` 与 DB），保留在 service 里（见 Step 4），不进声明式 schema。错误码保持 17002/17003。

- [ ] **Step 4: 写 `device-group.service.h`**（DTO 化 list/detail；create/update 读 typed Body；保留 requireOwner 与跨表校验）

以旧 `device-group.service.h` 为源，逐方法改写：
- `list`/`detail`：把 `jsonb_build_object(...)::text` 改为 `SELECT` 明确列，`fill` 进 `DeviceGroupItemDto`。列顺序：`id::text, name, COALESCE(parent_id::text,''), status, sort_order, COALESCE(remark,''), <deviceCount>, created_at::text, updated_at::text`。`deviceCount` 用旧 `countExpr`（`withCount ? 子查询 : 0`）。
- `create(c, const SaveDeviceGroupBody& body)`：先 `co_await validateRelations(c, body, std::nullopt)`（只保留 parent_id 跨表/自身校验），再 INSERT，参数改为绑定 `body.name()->view()`、`body.parentId()`、`body.status()`、`body.sortOrder()`、`body.remark()`，用 `NULLIF/COALESCE` 语义与旧 SQL 一致（parent_id 空→NULL，status 缺省→'enabled'，sort_order 缺省→0，remark 空→NULL）。
- `update`：保留 `requireOwner` + `validateRelations(c, body, id)`，UPDATE 用 `CASE WHEN <字段存在>` 语义。**注意**：typed Body 需能区分"字段缺省"与"显式空值"，用 `body.xxx()` 是否有值判断（`RUVIA_REQUEST_MODEL` 未提供的字段读取为 empty optional）。若某字段在 Body 中不可区分 present/absent，则改写 UPDATE 为"仅在 optional 有值时拼接 `SET col=$n`"，与 `user.service.h` 的 `append` lambda 同款（照抄 `service/modules/system/user/user.service.h:157-177` 的动态 SET 拼接）。
- `remove`：逻辑与错误码（17004）不变。
- `requireOwner`：照抄旧实现（错误码 17005）。

保留私有方法 `validateRelations(c, body, currentId)`，只做 parent_id 的：非空时必须 UUID（17002）、不能等于 currentId（17003）、上级必须存在（17003）。

**契约核对清单（Step 6 用）：** 输出 DTO 字段集合 = `{id,name,parent_id,status,sort_order,remark,deviceCount,created_at,updated_at}`，与旧 `jsonb_build_object` 完全一致（含 `deviceCount` 驼峰 key）。

- [ ] **Step 5: 写 `device-group.controller.h`**（照抄旧 controller 的路由/权限码，替换为 typed 调用）

以旧 `device-group.controller.h` 为源，保留：路由前缀、`AuthMiddleware`、每方法 `requirePermission("<原权限码>")`。把 handler 改为：
- `list`：`const auto& q = c.req().valid<DeviceGroupListQuery>(); co_return c.json(service::common::ok<DeviceGroupListResponse>(c, co_await deviceGroupService().list(c, *q.withCount())));`
- `detail`：`ok<DeviceGroupDetailResponse>`。
- `create`：`co_await deviceGroupService().create(c, c.req().valid<SaveDeviceGroupBody>()); co_return c.json(service::common::operation(c, "创建成功"));`（沿用旧成功文案）
- `update`：`c.req().valid<SaveDeviceGroupBody>()` + `operation(c, "更新成功")`。
- `remove`：`operation(c, "删除成功")`。
- 路由绑定校验器：create 用 `CreateDeviceGroupValidator`，update 用 `SaveDeviceGroupValidator`，list 用 `DeviceGroupListQueryValidator`，带 id 的用 `DeviceGroupIdParamsValidator`。

> 成功文案与 `withCount` 参数名以旧 controller 为准，Step 1 已记录。

- [ ] **Step 6: 更新 server.cpp include 并删除旧目录**

在 `service/server.cpp` 把 `#include ".../device-group/device-group.controller.h"` 改为 `.../device/device-group.controller.h`；确认控制器注册处类名不变。

```bash
git rm service/modules/northbridge/device-group/device-group.controller.h \
       service/modules/northbridge/device-group/device-group.service.h
```

- [ ] **Step 7: 构建**

Run: `cmake --build build`
Expected: 成功，无 error/warning 回归。

- [ ] **Step 8: 契约核对**

对照 Step 4 清单，逐字段确认新 DTO 输出 key 集合与旧 jsonb 一致；确认 5 个错误码文案与 HTTP 状态未变。

- [ ] **Step 9: 提交**

```bash
git add service/modules/northbridge/device/device-group.*.h service/server.cpp
git commit -m "refactor(device-group): typed DTO layering, merge into device module"
```

---

## Task 2: device 后端输出侧转 typed DTO（list/detail/options/realtime）

先转风险最低的读路径，输入侧留到 Task 3。

**Files:**
- Modify: `service/modules/northbridge/device/device.types.h`（新增全部 DTO）
- Modify: `service/modules/northbridge/device/device.service.h`（4 个读方法 DTO 化）
- Modify: `service/modules/northbridge/device/device.controller.h`（读方法包壳）

**Interfaces:**
- Produces:
  - `DeviceItemDto`（30 字段，见下）、`DeviceOptionDto`、`DeviceRealtimeDto`、`DevicePageDataDto`（`list/total`）、及对应 `*Response`。
  - service：`list -> Task<DevicePageDataDto>`、`detail -> Task<DeviceItemDto>`、`options -> Task<ruvia::List<DeviceOptionDto>>`、`realtime -> Task<DevicePageDataDto>`（沿用旧 `{list,total}` 包装，total 为行数）。

- [ ] **Step 1: 在 `device.types.h` 定义输出 DTO**

字段清单**逐字段对齐** `device.service.h` 的 `itemExpression()`（30 字段）：
`id,name,device_code,link_id,target_id,protocol_config_id,group_id,status,online_timeout,remote_control,modbus_mode,slave_id,timezone,heartbeat,registration,remark,created_by,created_at,updated_at,link_name,link_mode,link_protocol,protocol_name,protocol_type,read_interval,storage_interval,element_count,connected,connectionState,elements,can_edit,can_delete,can_share,can_command,access_level`。

```cpp
#pragma once

#include <ruvia/web/Model.h>

namespace service::device {

RUVIA_REQUEST_MODEL(DeviceIdParams, RUVIA_FIELD(id, ruvia::String));

// heartbeat / registration 嵌套对象
RUVIA_RESPONSE_MODEL(DevicePacketDto, RUVIA_FIELD(mode, ruvia::String),
                     RUVIA_FIELD(content, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceItemDto,
                     RUVIA_FIELD(id, ruvia::String), RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD_NAME("device_code", deviceCode, ruvia::String),
                     RUVIA_FIELD_NAME("link_id", linkId, ruvia::String),
                     RUVIA_FIELD_NAME("target_id", targetId, ruvia::String),
                     RUVIA_FIELD_NAME("protocol_config_id", protocolConfigId, ruvia::String),
                     RUVIA_FIELD_NAME("group_id", groupId, ruvia::String),
                     RUVIA_FIELD(status, ruvia::String),
                     RUVIA_FIELD_NAME("online_timeout", onlineTimeout, ruvia::Int64),
                     RUVIA_FIELD_NAME("remote_control", remoteControl, ruvia::Bool),
                     RUVIA_FIELD_NAME("modbus_mode", modbusMode, ruvia::String),
                     RUVIA_FIELD_NAME("slave_id", slaveId, ruvia::Int64),
                     RUVIA_FIELD(timezone, ruvia::String),
                     RUVIA_FIELD(heartbeat, DevicePacketDto),
                     RUVIA_FIELD(registration, DevicePacketDto),
                     RUVIA_FIELD(remark, ruvia::String),
                     RUVIA_FIELD_NAME("created_by", createdBy, ruvia::String),
                     RUVIA_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_FIELD_NAME("updated_at", updatedAt, ruvia::String),
                     RUVIA_FIELD_NAME("link_name", linkName, ruvia::String),
                     RUVIA_FIELD_NAME("link_mode", linkMode, ruvia::String),
                     RUVIA_FIELD_NAME("link_protocol", linkProtocol, ruvia::String),
                     RUVIA_FIELD_NAME("protocol_name", protocolName, ruvia::String),
                     RUVIA_FIELD_NAME("protocol_type", protocolType, ruvia::String),
                     RUVIA_FIELD_NAME("read_interval", readInterval, ruvia::Double),
                     RUVIA_FIELD_NAME("storage_interval", storageInterval, ruvia::Double),
                     RUVIA_FIELD_NAME("element_count", elementCount, ruvia::Int64),
                     RUVIA_FIELD(connected, ruvia::Bool),
                     RUVIA_FIELD_NAME("connectionState", connectionState, ruvia::String),
                     RUVIA_FIELD(elements, ruvia::List<ruvia::String>),
                     RUVIA_FIELD_NAME("can_edit", canEdit, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_delete", canDelete, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_share", canShare, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_command", canCommand, ruvia::Bool),
                     RUVIA_FIELD_NAME("access_level", accessLevel, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceOptionDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD_NAME("device_code", deviceCode, ruvia::String),
                     RUVIA_FIELD_NAME("can_edit", canEdit, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_delete", canDelete, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_share", canShare, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_command", canCommand, ruvia::Bool),
                     RUVIA_FIELD_NAME("access_level", accessLevel, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceRealtimeDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(connected, ruvia::Bool),
                     RUVIA_FIELD_NAME("connectionState", connectionState, ruvia::String),
                     RUVIA_FIELD(elements, ruvia::List<ruvia::String>),
                     RUVIA_FIELD_NAME("can_edit", canEdit, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_delete", canDelete, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_share", canShare, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_command", canCommand, ruvia::Bool),
                     RUVIA_FIELD_NAME("access_level", accessLevel, ruvia::String));

RUVIA_RESPONSE_MODEL(DevicePageDataDto, RUVIA_FIELD(list, ruvia::List<DeviceItemDto>),
                     RUVIA_FIELD(total, ruvia::Int64));
RUVIA_RESPONSE_MODEL(DeviceRealtimePageDto, RUVIA_FIELD(list, ruvia::List<DeviceRealtimeDto>),
                     RUVIA_FIELD(total, ruvia::Int64));

RUVIA_RESPONSE_MODEL(DevicePageResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, DevicePageDataDto));
RUVIA_RESPONSE_MODEL(DeviceDetailResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, DeviceItemDto));
RUVIA_RESPONSE_MODEL(DeviceOptionsResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String),
                     RUVIA_FIELD(data, ruvia::List<DeviceOptionDto>));
RUVIA_RESPONSE_MODEL(DeviceRealtimeResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, DeviceRealtimePageDto));

} // namespace service::device
```

> **确认点（Step 之前先验证）：** `RUVIA_FIELD(elements, ruvia::List<ruvia::String>)` 旧输出恒为 `[]`；`heartbeat`/`registration` 旧输出是原样透传 DB jsonb 对象（`{mode,content}` 或仅 `{mode}`）。若 `DevicePacketDto` 固定二字段会在 `content` 缺省时**多输出** `"content":""`，与旧透传不一致——**先用 psql 取一行现网 `heartbeat` jsonb 确认字段集合**：`SELECT heartbeat, registration FROM iot_device LIMIT 5;`。若存在只有 `{mode}` 的记录，改为在 fill 时按实际 jsonb 组装（保留缺省不补 content），或让 DTO `content` 用可空处理。契约一致优先于形状统一。
> **另注：** `read_interval`/`storage_interval` 在旧 jsonb 里可能为 `null`（config 无 `readInterval/pollInterval/storageInterval` key 时 COALESCE 为 NULL）。确认 `ruvia::Double` 字段在值缺失时输出 `null` 而非 `0`，与旧契约一致；若 DTO 无法输出 null，则对这两个字段保留可空处理。

- [ ] **Step 2: DTO 化 `list`/`detail`/`options`/`realtime`**

把 `itemExpression()` 从 `jsonb_build_object` 改为一个 **SELECT 列表达式**（同名列、同计算逻辑：`read_interval`/`storage_interval`/`element_count` 的 CASE 原样保留），去掉外层 jsonb 包装，改为逐列 `SELECT ... FROM iot_device d JOIN iot_link l ... JOIN iot_protocol_config p ...`，然后写 `fill(c, DeviceItemDto&, row)` 逐列赋值。`heartbeat`/`registration` 两个 jsonb 列 `::text` 取出后按 Step 1 确认结果填入 `DevicePacketDto`（或原样透传方案）。

- `list` -> 组 `DevicePageDataDto{list, total=行数}`（沿用旧 `page()` 的 total=行数语义）。
- `detail` -> 单个 `DeviceItemDto`，空则 `fail(18001,"设备不存在",404)`。
- `options` -> `ruvia::List<DeviceOptionDto>`（固定 `can_*`/`access_level` 常量同旧）。
- `realtime` -> `DeviceRealtimePageDto`（固定字段同旧）。
- 删除私有 `page()` 与 `itemExpression()` 中不再需要的字符串拼接（若 detail/list 复用列表达式，抽成 `static std::string itemColumns()` 返回 SELECT 列串）。

**契约核对清单：** list/detail 输出 30 key 全集一致；options 8 key；realtime 9 key；分页包装 `{list,total}` 不变。

- [ ] **Step 3: controller 读方法包壳**

`device.controller.h` 里 list/detail/options/realtime 改为 `c.json(service::common::ok<...Response>(c, co_await deviceService().xxx(...)))`；权限码、路由、`AuthMiddleware` 不变。detail 路由加 `DeviceIdParamsValidator`（若旧未加则保持旧行为，Step 先确认）。

- [ ] **Step 4: 构建 + 契约核对**

Run: `cmake --build build` → 成功。逐项核对 Step 2 清单。

- [ ] **Step 5: 提交**

```bash
git add service/modules/northbridge/device/device.types.h \
        service/modules/northbridge/device/device.service.h \
        service/modules/northbridge/device/device.controller.h
git commit -m "refactor(device): typed DTOs for read endpoints"
```

---

## Task 3: device 后端输入侧转 typed Body（create/update）

最高风险任务。目标：controller 收 `SaveDeviceBody`（typed），service 读 `body.xxx()` 取代 `payload.get<>()`；扁平字段规则进 `device.schema.h` 声明式校验；嵌套 heartbeat/registration 的 HEX 校验与跨设备冲突检测保留在 service，但从 typed Body 取值。**逐条保留** 现有全部校验规则与错误码（18002/18003/18004/18005/18006）。

**Files:**
- Modify: `service/modules/northbridge/device/device.types.h`（新增 Body）
- Modify: `service/modules/northbridge/device/device.schema.h`（新增校验器）
- Modify: `service/modules/northbridge/device/device.service.h`（create/update/validate/ensureUnique/validateRuntimeIdentity 改读 typed Body）
- Modify: `service/modules/northbridge/device/device.controller.h`（create/update 用 typed Body + 校验器）

**Interfaces:**
- Consumes: Task 2 的 DTO；`service::device` 命名空间。
- Produces: `SaveDeviceBody`（含嵌套 `DevicePacketBody{mode,content}`）；service `create(c, const SaveDeviceBody&)`、`update(c, id, const SaveDeviceBody&)`。

- [ ] **Step 1: 定义 `SaveDeviceBody`**（在 `device.types.h`）

```cpp
RUVIA_REQUEST_MODEL(DevicePacketBody, RUVIA_FIELD(mode, ruvia::String),
                    RUVIA_FIELD(content, ruvia::String));

RUVIA_REQUEST_MODEL(SaveDeviceBody, RUVIA_FIELD(name, ruvia::String),
                    RUVIA_FIELD_NAME("device_code", deviceCode, ruvia::String),
                    RUVIA_FIELD_NAME("link_id", linkId, ruvia::String),
                    RUVIA_FIELD_NAME("target_id", targetId, ruvia::String),
                    RUVIA_FIELD_NAME("protocol_config_id", protocolConfigId, ruvia::String),
                    RUVIA_FIELD_NAME("group_id", groupId, ruvia::String),
                    RUVIA_FIELD(status, ruvia::String),
                    RUVIA_FIELD_NAME("online_timeout", onlineTimeout, ruvia::Int64),
                    RUVIA_FIELD_NAME("remote_control", remoteControl, ruvia::Bool),
                    RUVIA_FIELD_NAME("modbus_mode", modbusMode, ruvia::String),
                    RUVIA_FIELD_NAME("slave_id", slaveId, ruvia::Int64),
                    RUVIA_FIELD(timezone, ruvia::String),
                    RUVIA_FIELD(heartbeat, DevicePacketBody),
                    RUVIA_FIELD(registration, DevicePacketBody),
                    RUVIA_FIELD(remark, ruvia::String));
```

- [ ] **Step 2: `device.schema.h` 声明式校验器**（扁平规则，逐条对应旧 `validate()` 的 SQL shape 检查）

规则清单（**逐条保留**，错误文案沿用 18002 系列）：
1. `name`：create 必填、长度 1–100；update 非必填但给出时 1–100。
2. `status`：`ONE_OF("enabled","disabled")`。
3. `online_timeout`：数字、范围 1–86400。
4. `timezone`：正则 `^([+-](0[0-9]|1[0-3]):[0-5][0-9]|[+-]14:00)$`（用 `RUVIA_CUSTOM` + 复用/新增校验函数）。
5. `modbus_mode`：null 或 `ONE_OF("TCP","RTU")`。
6. `slave_id`：null 或数字范围 1–247。
7. create 时 `link_id`/`protocol_config_id` 必填。
8. `link_id`/`protocol_config_id`/`group_id`：给出时必须 UUID（可用 `RUVIA_CUSTOM(service::common::isUuidField)`）。

写 `CreateDeviceValidator`（含必填项）与 `UpdateDeviceValidator`（去必填）两个 `RUVIA_VALIDATE_JSON`，绑定 `SaveDeviceBody`。id 参数用 `DeviceIdParamsValidator`。照抄 `user.schema.h` 结构。

> heartbeat/registration 的 mode∈{OFF,HEX,ASCII}、HEX content 必须偶数长十六进制、非 OFF 必须非空 content —— 这些是**跨字段/条件校验**，无法用扁平声明式规则表达，保留在 service（Step 3）。

- [ ] **Step 3: service 改读 typed Body**（保留全部剩余校验）

以现有 `device.service.h` 为源，做以下等价替换（**不改判定逻辑与错误码**）：
- `validate(c, payload, required)` → `validate(c, const SaveDeviceBody& body, bool required)`：
  - 删除已被 Step 2 声明式覆盖的 shape 检查；**保留**：heartbeat/registration 的 mode+content HEX/ASCII 条件校验（把旧 SQL 里第 8、9 条判定改写为读 `body.heartbeat()`/`body.registration()` 的 `mode()`/`content()` 的 C++ 校验；HEX 用现有 `service::utils` 或本地 helper 做去空白+偶数长+`[0-9A-Fa-f]` 判定，语义与旧正则一致）；保留 link/protocol relation 查询与一致性校验（18003）、device_code 规则（含 SL651 数字/≤10 位）、group_id 存在性校验。
  - relation 查询的 `link_id`/`protocol_config_id` 从 `body.linkId()`/`body.protocolConfigId()` 取。
- `ensureUnique(c, body, excludedId)`：`name`/`device_code` 从 `body` 取。
- `validateRuntimeIdentity(c, body, excludedId)`：该方法核心是**基于 payload 字段的 DB 查询**。把 `WITH body AS (SELECT $1::jsonb ...)` 保留，但 `$1` 由 `body` 序列化而来会破坏 typed 目标。改为：把 CTE 里 `body.value->>'link_id'` 等替换为**绑定参数**（`link_id/target_id/protocol_config_id/slave_id/registration/heartbeat` 六个值从 typed body 传入，registration/heartbeat 以 `{mode,content}` 组回 jsonb 文本参数或拆成 mode/content 两参数）。逻辑与冲突判定（18006 各分支）一字不改。
- `create(c, const SaveDeviceBody& body)`：INSERT 改为绑定 typed 参数（取代 `$1::jsonb` + `value->>`），字段缺省语义保持（online_timeout→300、remote_control→TRUE、timezone→'+08:00'、heartbeat/registration→`{"mode":"OFF"}`、status→'enabled'、空串→NULL）。`publishConfigEvent(... "created" ...)` 不变。
- `update(c, id, const SaveDeviceBody& body)`：保留 created_by owner 校验、link_id/protocol_config_id 不可改校验（18003）；UPDATE 用"仅 optional 有值才拼 SET col=$n"的动态拼接（同 `user.service.h` 的 append lambda），取代 `CASE WHEN body.value ? 'x'`。`publishConfigEvent(... "updated" ...)` 不变。

**校验规则保留核对表（Step 5 用）：** 18002(name/参数)、18002(timezone/modbus/slave/heartbeat/registration/device_code/SL651)、18003(relation/link不可改/type不可改/group)、18004(name/code 唯一)、18005(owner)、18006(S7/Modbus 各冲突分支) —— 逐条在新代码里能指到对应位置。

- [ ] **Step 4: controller create/update 用 typed Body**

```cpp
ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
    co_await service::middleware::requirePermission(c, "<原权限码>");
    co_await deviceService().create(c, c.req().valid<SaveDeviceBody>());
    co_return c.json(service::common::operation(c, "创建成功"));
}
```
update 同理（`valid<SaveDeviceBody>()` + id 参数）。路由绑定 `CreateDeviceValidator`/`UpdateDeviceValidator`。权限码/成功文案以旧 controller 为准。

- [ ] **Step 5: 构建 + 逐条核对校验保留表**

Run: `cmake --build build` → 成功。对照 Step 3 核对表逐条确认；确认 `create`/`update` 缺省值与旧一致。

- [ ] **Step 6: 提交**

```bash
git add service/modules/northbridge/device/device.types.h \
        service/modules/northbridge/device/device.schema.h \
        service/modules/northbridge/device/device.service.h \
        service/modules/northbridge/device/device.controller.h
git commit -m "refactor(device): typed request bodies and validators for write endpoints"
```

---

## Task 4: 前端 `iot/device` 归位五件套

目标：`Device.tsx` → `index.tsx`；device 与 device-group 保持同一模块但文件命名规整；契约字段与后端 Task 2/3 对齐；交互与提交 payload 不变。

**Files:**
- Rename: `web/pages/iot/device/Device.tsx` → `web/pages/iot/device/index.tsx`
- Modify: 路由引用（`web/routes/*` 中指向 `Device.tsx` 的 import）
- Review/Modify: `device.types.ts`、`device.schema.ts`、`device.api.ts`、`device.service.ts`、`device-group.types.ts`、`DeviceFormModal.tsx`、`DeviceGroupFormModal.tsx`、`DeviceGroupPanel.tsx`（仅在后端契约变化处对齐；无变化则不动）

**Interfaces:**
- Consumes: 后端 `/api/devices*`、`/api/device-groups*` 的 DTO（Task 2/3 后字段集合不变）。

- [ ] **Step 1: 定位路由引用**

Run: `grep -rn "iot/device/Device\|from './Device'\|device/Device" web`
记录所有 import 点。

- [ ] **Step 2: 重命名并修正 import**

```bash
git mv web/pages/iot/device/Device.tsx web/pages/iot/device/index.tsx
```
把 Step 1 的 import 改为指向 `iot/device`（目录默认解析 `index.tsx`）。组件内部若有 `export default function Device`，保留组件名不变，仅改文件名与 import 路径。

- [ ] **Step 3: 契约对齐核对**

因 Task 2/3 保证字段集合不变，`device.types.ts` 的 `Device.Item` 等类型**应无需改动**；逐字段比对后端 DTO key 与前端 type key，若有历史不一致（如后端补了 `access_level` 前端缺）在此补齐。`device.schema.ts` 的 zod 规则与后端 `device.schema.h` 校验器逐条比对（name 1–100、status 枚举、online_timeout 1–86400、timezone 正则、slave_id 1–247、modbus_mode 枚举、device_code 规则、group_id/link_id/protocol_config_id UUID）。不一致处以"双向核对"原则同步（AGENTS.md）。

- [ ] **Step 4: 验证**

Run: `bun run typecheck && bun run lint && bun run build && git diff --check`
Expected: 全部通过。

- [ ] **Step 5: 页面自检（若认证允许）**

本地打开设备页，确认：分组树 + 平面表格、固定表头、横向/纵向表内滚动、窄屏、分页位置正常，控制台无报错。若认证阻碍，明确记录未覆盖范围。

- [ ] **Step 6: 提交**

```bash
git add web/pages/iot/device web/routes
git commit -m "refactor(web/device): normalize module to index.tsx + five-file layout"
```

---

## 完成标准（切片 1）

- device、device-group 后端均为四件套 typed 风格，无 `JsonValue& payload`、无 `jsonb_build_object` 业务拼装、无裸 string 业务返回。
- device-group 已并入 `northbridge/device/` 目录；旧 `device-group/` 目录删除。
- 前端 `iot/device` 入口为 `index.tsx`，五件套齐整，device/group 同模块。
- 所有响应字段集合、权限码、错误码、分页语义、校验规则与重构前逐条一致。
- `cmake --build build` 与 `bun run typecheck/lint/build` 全绿。

## 后续（另立计划）

- 切片 2 = `protocol`：后端 protocol typed 化 + 前端 `iot/protocol` 巨型文件（S7 1737 / Modbus 1361 / SL651 790）拆分为 `modbus/`、`s7/`、`sl651/` 子目录 + 分段子组件，入口 `index.tsx`。落地切片 1 后新开 `docs/superpowers/plans/2026-..-layering-unification-slice-2-protocol.md`。
