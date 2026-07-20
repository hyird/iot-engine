# iot-manager 参考基线

## 来源

重写参考源码位于：

```text
D:\Workspace\iot-manager
```

分析基于当前本地源码。旧项目保持只读，不在重写过程中直接修改。

## 重写原则

`iot-engine` 是 `iot-manager` 的业务重写，不是仅替换 Web 框架的机械移植：

- 继承已经验证的业务概念、前端交互和协议语义；
- 对外 API 是否保持完全兼容，由 API 兼容清单逐项决定；
- Drogon 替换为 Ruvia 0.1.3；
- 南北向直接调用替换为 Redis Streams；
- PostgreSQL/TimescaleDB 继续作为事实数据源，并采用 JSONB 优先设计；
- 消除旧实现中的跨模块回调、隐式共享状态和直接数据库快速路径；
- 新旧系统可以并行运行并执行数据迁移和结果核对。

## 旧项目功能盘点

### 系统管理

- JWT 登录、刷新、退出和当前用户；
- 用户、角色、部门、菜单；
- 用户—角色和角色—菜单关联；
- `module:resource:action` 格式的权限代码；
- 后端动态菜单和前端动态路由；
- 前端页面与权限注册中心。

### IoT 资源

- 链路管理；
- 设备类型/协议配置；
- 设备和树形设备分组；
- 本地链路和 Agent 接入；
- 设备静态信息、实时信息、历史数据和指令下发；
- 创建者、设备分享和部门分享；
- ETag 和分页。

### 协议与南向接入

- SL651；
- Modbus TCP/RTU；
- Siemens S7；
- GB28181 视频接入；
- TCP Server/Client 链路；
- Agent/边缘节点和端点；
- 设备注册、连接状态、轮询、指令优先级和应答匹配。

### 数据与事件

- PostgreSQL JSONB 设备数据；
- TimescaleDB hypertable；
- 压缩策略、归档表和小时连续聚合；
- 实时数据缓存；
- WebSocket 推送；
- 领域事件；
- 告警规则、模板、记录、确认和恢复；
- AccessKey、开放 API、Webhook 和调用日志。

### 运维

- 首页统计和系统监控；
- Agent 在线状态、能力、配置版本与事件；
- 数据库迁移框架；
- 运行模块的启动和停止编排。

## 建议直接继承的设计

### 模块生命周期

旧项目的 `ApplicationModule` 定义了清晰的：

```text
configure -> registerHandlers -> start -> stop
```

新项目保留这一概念，但实现不依赖 Drogon。Ruvia 启动钩子负责驱动模块管理器，
每个模块自行管理连接、后台任务和停止过程。

### 协议生命周期

继续使用以下三个状态，不能把“数据库已有配置”等同于“设备可以通信”：

- `configured`：配置存在且可读取；
- `bound`：链路/session 已与运行对象绑定；
- `runtime_activated`：轮询、收发和解析已经可运行。

适配器继续保留初始化、重载、连接变化、数据输入和维护 tick 等生命周期语义。

### 协议无关轮询

保留旧项目中的通用轮询概念：

- 周期调度；
- 单设备周期状态；
- 多步骤轮询；
- 失败重试和连续失败降频；
- 控制命令后的快速读取窗口；
- 同一链路或 DTU 下的错峰；
- 高优先级控制任务和普通轮询任务。

新实现的调度结果必须写入 `iot:commands`，不能直接调用协议适配器。

### JSONB 协议配置

保留 `protocol_config.config` 和设备协议参数使用 JSONB 的思路。不同协议通过版本化
JSON Schema 或等价校验器验证，避免为每个寄存器或协议字段增加数据库列。

### 静态与实时数据分离

保留旧前端的组合方式：

- 静态设备数据可使用 ETag 和较长缓存；
- 实时状态通过独立 API 或 SSE 更新；
- 前端按设备 ID 合并两部分；
- 历史 JSONB 可直接透传，减少无意义的解析和重新序列化。

### 动态页面和权限注册

保留页面注册中心、懒加载、动态菜单和 `module:resource:action` 权限代码。前端按钮
权限只用于用户体验，后端仍必须执行 RBAC 和设备 ACL 判定。

## 必须改变的内部设计

### 南北向边界

旧项目中的 Controller/Service 到 `ProtocolDispatcher` 的直接调用、协议回调和
进程内事件分发不能用于跨越新系统的南北向边界。

新系统统一为：

```text
北向查询/控制 -> iot:commands -> 南向适配器
南向主动数据 -> iot:telemetry -> 数据消费者
南向命令结果 -> iot:responses -> 数据消费者/北向通知
```

进程内领域事件仍可用于同一层内部的低风险通知，但不能代替跨层 Redis 消息。

### 数据库写入

旧协议运行时存在直接保存设备数据和命令结果的路径。新系统规定南向层不直接写
TimescaleDB，只有 Redis 数据消费者可以持久化和确认消息。

### 设备分享

旧项目使用 `view/control`，并逐步把用户/部门分享目标放入 JSONB。新项目按当前
需求重新定义：

- 创建者拥有全部权限；
- 只有创建者可以分享；
- 被分享者不能删除或二次分享；
- 分享级别为 `viewer/editor`；
- ACL 主体和权限级别保留关系列，扩展命令策略使用 JSONB。

新项目继续支持旧项目已有的用户和部门两种分享目标。若个人授权与部门授权同时
命中，按 `editor > viewer` 合并有效权限；分享和撤销仍只允许设备创建者执行。
分享界面和批量 API 允许在同一次操作中混合选择多个用户与多个部门。

### 超级管理员

旧项目允许超级管理员访问所有资源。新设计默认不允许管理员静默取得设备业务
权限；如需要该能力，应采用强审计的 break-glass 流程。

## 旧表到新模型的初步映射

| iot-manager | iot-engine | 处理方式 |
|---|---|---|
| `sys_user` | `users` | 保留身份与状态，迁移密码哈希前确认算法兼容性 |
| `sys_role` | `roles` | 保留 RBAC |
| `sys_menu` | `menus` | 保留动态菜单和权限代码 |
| `sys_department` | `departments` | 保留组织树 |
| `link` | `links` | 保留业务字段，连接配置优先 JSONB |
| `protocol_config` | `protocol_configs` | 保留版本化 JSONB 配置 |
| `device_group` | `device_groups` | 保留树形分组 |
| `device` | `devices` | 保留创建者和协议参数，重整 JSONB |
| `device_share` | `device_acl` | 按新权限规则转换 |
| `device_data` | `telemetry_events` | 转换为 Redis Envelope 对应的 JSONB 事件 |
| `alert_rule` | `alert_rules` | 保留 JSONB 条件 |
| `alert_record` | `alert_records` | 保留状态和 JSONB 详情 |
| `agent_node` | `edge_nodes` | 保留能力、运行态和配置版本 |
| `agent_endpoint` | `edge_endpoints` | 保留南向端点概念 |
| `open_access_key` | `access_keys` | 保留设备范围和动作范围 |
| `open_webhook` | `webhooks` | 保留事件订阅、签名和重试 |
| `open_access_log` | `integration_logs` | 保留 JSONB 请求/响应审计 |

## 前端参考范围

首期前端应参考并逐步迁移：

- 登录与动态导航；
- 首页；
- 用户、角色、部门、菜单；
- 链路、边缘节点、协议配置；
- 设备卡片、分组、拓扑、分享、历史数据和命令面板；
- 告警；
- 开放接入；
- GB28181。

React 19、TanStack Query、Redux Toolkit、Ant Design、ECharts 和 Biome 可以继续
使用。迁移时优先复用业务类型和交互，不直接复制与旧 API 强耦合的请求代码。

## 仍需形成明确决策的兼容项

- 新 API 是否保留旧 `/api/device` 等路径，或只提供 `/api/v1`；
- 旧整数 ID 是否原样保留，还是新资源使用 UUID；
- 超级管理员是否允许访问所有设备；
- AccessKey 和 Webhook 是否进入首期；
- GB28181 是否进入首期；
- 旧 `device_data` 和归档数据的迁移时间范围；
- 登录令牌和密码哈希是否要求无感迁移；
- 前端是否要求界面与操作路径完全兼容。

这些项目应在编码前进入兼容矩阵，不能在实现过程中隐式决定。
