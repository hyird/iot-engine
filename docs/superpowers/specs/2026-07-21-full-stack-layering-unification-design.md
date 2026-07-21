# 全栈分层统一（垂直切片重构）设计

- 日期：2026-07-21
- 范围：全栈分层一致性；不改业务功能，只统一架构分层与端到端链路
- 执行路线：垂直切片（按模块端到端重构），逐片可独立验证

## 背景与问题

架构层面（南北向逻辑分离 + Redis Streams 强制边界）已有清晰文档，真正"杂乱"的
是**分层与链路的一致性没有落实**：同一套系统里并存两种写法。

### 后端偏差

`system/*` 与 `northbridge/link` 采用 **typed 风格**：`*.types.h` 定义
ModelObject DTO 与 `Body/Query/Params`，`*.schema.h` 定义校验器，`*.service.h`
返回强类型 DTO、入参用强类型 Body、DB 直连在内部，`*.controller.h` 只做编排。

而 `northbridge/device`、`northbridge/device-group`、`northbridge/protocol` 采用
**raw-JSON 风格**：service 直接接收 `ruvia::JsonValue& payload`，在 SQL 里用
`jsonb_build_object` 拼装 JSON，返回裸 `std::string`，绕过 typed schema/DTO 层。
`device-group` 甚至缺少 `schema.h` 与 `types.h`。

| service | raw-json 迹象 | typed 迹象 | 结论 |
|---|---|---|---|
| system/user, role, dept, auth | 0 | 多 | canonical ✅ |
| northbridge/link | 0 | 14 | canonical ✅ |
| northbridge/device | 12 | 0 | 偏差 ❌ |
| northbridge/device-group | 7 | 0（缺 schema/types） | 偏差 ❌ |
| northbridge/protocol | 8 | 0 | 偏差 ❌ |

### 前端偏差

`system/*`、`iot/link`、`login` 采用 **五件套**：`index.tsx` +
`*.api.ts` / `*.schema.ts` / `*.service.ts` / `*.types.ts`。

而 `iot/device` 用 `Device.tsx`（非 `index.tsx`），并把 device-group 文件混放；
`iot/protocol` 无 `index.tsx`，入口是 `S7Config.tsx`(1737 行)、
`ModbusConfig.tsx`(1361 行)、`SL651Config.tsx`(790 行) 等巨型单文件。

## 统一分层规范（canonical）

### 后端 · 每个业务模块 = 四件套

| 文件 | 职责 | 硬约束 |
|---|---|---|
| `*.types.h` | ModelObject DTO + `Body/Query/Params` 类型 | 只定义数据形状，无逻辑 |
| `*.schema.h` | `RUVIA_VALIDATE_JSON/QUERY/PARAM` 校验器 | 绑定 types；必填/长度/取值范围/枚举/格式/数组数量/分页/ID 全覆盖 |
| `*.service.h` | 单例 service；返回强类型 DTO、入参强类型 Body；DB 直连在内部 | 禁止 `JsonValue& payload`；禁止用 `jsonb_build_object` 拼业务 JSON；禁止返回裸 `std::string` 作为业务数据 |
| `*.controller.h` | 路由 + `requirePermission` + `c.req().valid<Body>()` + `service::common::ok<Response>` 包壳 | 只做编排，无 SQL / 业务逻辑 |

判定基准文件：`service/modules/system/user/{user.types,user.schema,user.service,user.controller}.h`。

**不新增 repository 层**：沿用 `system/*` 已确立的"service 内直连 DB"，只统一现有
层次，不引入新层（YAGNI）。`southbridge` 属通信/消息层，另有 `runtime` / `network`
/ `protocol` / `queue` 结构，**不套用**本规范；其现存的
`runtime_config.repository.h` 是通信层特例，保留。

### 前端 · 每个页面模块 = 五件套

| 文件 | 职责 |
|---|---|
| `*.types.ts` | namespace 类型 + `queryKeys` |
| `*.schema.ts` | zod schema（必填/长度/枚举/ID 等，与后端逐条对齐） |
| `*.api.ts` | endpoints；请求前一律 `schema.parse()` |
| `*.service.ts` | react-query hooks（`useQuery` / `useMutation`） |
| `index.tsx` | 页面入口；子组件命名 `PascalCase.tsx`、局部 hooks 命名 `useXxx.ts`，同目录 |

判定基准文件：`web/pages/system/user/*`。

## 偏差 → 目标 映射

| 模块 | 现状 | 目标 |
|---|---|---|
| 后端 `device` | raw-JSON（收 `JsonValue`、SQL 拼 jsonb、返回 string） | 转 typed：补全 `device.types.h`/`device.schema.h` 的 DTO，service 返回 DTO、入参 Body |
| 后端 `device-group` | raw-JSON，缺 schema/types，独立模块 | **并入 `device/` 模块**（同目录 `device-group.{types,schema,service,controller}.h`），转 typed |
| 后端 `protocol` | raw-JSON（有 schema/types 但 service 用 JsonValue） | 转 typed：service 改用 Body/DTO |
| 前端 `iot/device` | `Device.tsx` 非 index、group 文件混放 | `index.tsx` 为入口；device 与 group **保持同一模块**，按五件套归位（`device.*` + `device-group.*` + 子组件 `DeviceFormModal.tsx` 等） |
| 前端 `iot/protocol` | 无 index，S7/Modbus/SL651 巨型单文件 | `index.tsx` 为入口；每个协议编辑器拆为子目录（`modbus/`、`s7/`、`sl651/`）+ 分段子组件；共享逻辑（`grouping.ts`、`useProtocolImportExport.ts`、`SortableGroup.tsx`）保留但归位 |

device / device-group 在前端本就是**同一页面**（左侧分组树 `DeviceGroupPanel` +
右侧设备平面表格），因此前端保持合并、后端并入同一模块。

## 执行顺序（垂直切片）

每个切片端到端重构一个模块的整条链路（前端 schema → api → 后端 controller →
service → DB），可独立交付与验证。

1. **切片 0 — 规范落地**：写入本 spec，并在 `docs/design-patterns.md` 增补"统一分层
   规范"章节，作为后续判定依据。
2. **切片 1 — device(+group)**：后端 device/device-group typed 化并合并模块 → 前端
   `iot/device` 归位。一次交付、独立验证。
3. **切片 2 — protocol**：后端 protocol typed 化 → 前端 `iot/protocol` 巨型文件拆分。

## 不改行为的保证

- 转 typed **只改数据的产出与校验方式，不改 SQL 语义、字段集合、权限码、错误码、
  分页语义**；DTO 字段与现有 jsonb 输出**逐字段对齐**，包含 `can_edit`、
  `can_delete`、`can_share`、`can_command`、`access_level` 等 ACL 字段与
  `connected`、`connectionState` 等实时字段。
- 前后端校验规则改动时**双向核对**（AGENTS.md）：必填、长度、取值范围、枚举、格式、
  数组数量、分页、ID 约束保持一致，并核对数据库字段限制。
- 时间字段维持 UTC 语义（`TIMESTAMPTZ`、带 `Z` 的 ISO 8601、设备 `timezone` 解析
  语义）不变。
- 前端拆分只搬运/切分组件，**不改交互与提交 payload**；复用现有 `FormModal`、
  `PageContainer`、Ant Design 组件与工具类，不新增一次性抽象。
- 南北向不新增绕过 Redis 的进程内调用；南向不新增直写 DB 的路径。

## 验证

- 后端每片：`cmake --build build` 通过。
- 前端每片：`bun run typecheck`、`bun run lint`、`bun run build`、`git diff --check`
  通过；改动文件通过 Biome 格式检查。
- 契约对齐：对每个改造端点，比对改造前后的响应 JSON 字段集合与校验错误，确保一致。
- 布局：`iot/device`、`iot/protocol` 页面验证固定表头、横向/纵向滚动、窄屏与分页位置；
  若认证阻碍页面检查，明确说明未覆盖范围。

## 非目标（YAGNI）

- 不引入 repository 层。
- 不改造 `southbridge` 通信层结构。
- 不做与统一分层无关的重构（新增功能、告警引擎、开放 API 等，见 rewrite-plan 阶段 5）。
- 不改数据库 schema 与迁移。
