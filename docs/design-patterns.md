# 设计模式建议

## 总体风格：模块化单体

项目采用单进程部署，但内部按模块隔离：

```text
api / modules / protocols / network / queue / database / security
```

每个模块拥有明确接口和生命周期，最终静态链接到同一个 `iot-engine`。这比一开始
拆微服务更适合当前目标：部署简单、调试方便，同时保留未来拆分南向服务的可能。

## 1. Ports and Adapters

这是后端最重要的结构模式。

业务代码依赖抽象端口，而不是依赖 Ruvia、hiredis、libpq 或具体设备协议：

```cpp
class CommandPublisher;
class TelemetryRepository;
class DeviceRepository;
class AuthorizationPolicy;
class ProtocolAdapter;
```

基础设施提供实现：

```text
RedisCommandPublisher       -> CommandPublisher
PostgresDeviceRepository    -> DeviceRepository
ModbusAdapter               -> ProtocolAdapter
S7Adapter                   -> ProtocolAdapter
```

适用位置：

- `service/api` 调用应用服务接口；
- `service/modules` 保存业务用例；
- `service/queue` 实现消息端口；
- `service/database` 实现 repository；
- `service/protocols` 实现设备协议端口。

不要为每个简单函数建立接口。只有存在基础设施边界、多个实现或需要隔离测试时才
创建端口。

## 2. Event-Driven Architecture

南北向跨边界通信使用 Redis Streams：

```text
command -> iot:commands
telemetry -> iot:telemetry
response -> iot:responses
```

事件必须包含稳定的 Envelope、唯一事件 ID、schema 版本、发生时间和关联 ID。

进程内可以使用直接函数调用完成同一模块内部的普通逻辑；不要建立第二套通用
EventBus 来绕过 Redis，也不要用观察者回调跨越南北向边界。

## 3. CQRS Lite

使用轻量命令查询分离，不采用完整 CQRS 框架：

- Command：改变状态或访问设备，先进入 Redis；
- Query：从 PostgreSQL/TimescaleDB 查询已经持久化的数据；
- Command Response：通过 Redis 回来，持久化后通知等待者。

```text
POST query/control -> Redis command
GET device/history -> PostgreSQL/TimescaleDB
```

不采用 Event Sourcing。Redis Stream 不是永久业务事实库，PostgreSQL 才是权威
历史数据源。

## 4. Strategy + Registry

不同设备协议使用策略模式，通过注册表按协议选择适配器：

```cpp
class ProtocolAdapter {
public:
    virtual std::string_view protocol() const = 0;
    virtual Task handleCommand(const DeviceCommand&) = 0;
    virtual void onBytes(ConnectionId, ByteSpan) = 0;
};
```

```text
ProtocolRegistry
├── modbus -> ModbusAdapter
├── s7     -> S7Adapter
├── sl651  -> Sl651Adapter
└── gb28181-> Gb28181Adapter
```

注册在进程启动时完成，运行期不允许静默覆盖同名协议。适配器只负责协议和设备
通信，不直接写数据库。

## 5. Explicit State Machine

连接、命令和消费者状态不要散落为多个布尔值，应使用显式状态机。

设备运行时：

```text
configured -> bound -> activated
                    -> degraded
                    -> disconnected
```

查询命令：

```text
queued -> dispatched -> responded
                    -> timed_out
                    -> failed
       -> cancelled
```

Redis 消费：

```text
received -> validated -> persisted -> acknowledged
                    -> retrying
                    -> dead_lettered
```

状态转换函数同时验证允许的来源状态，并产生结构化审计信息。

## 6. Process Manager

查询—响应不是一个同步函数调用，而是跨 Redis 和设备连接的长流程。使用
`CommandProcessManager` 按 `correlation_id` 管理：

- 命令创建；
- 设备发送；
- 应答关联；
- HTTP 等待式查询通知；
- 超时；
- 迟到响应；
- 重试和最终失败。

等待式 HTTP 请求只是该流程的一个观察者。HTTP 超时不能删除命令状态，也不能
阻止迟到响应入队和落库。

## 7. Idempotent Consumer / Inbox

Redis Streams 提供至少一次投递，因此所有数据库消费者必须幂等。

推荐使用事件收件箱：

```sql
CREATE TABLE message_inbox (
    consumer       TEXT NOT NULL,
    event_id       UUID NOT NULL,
    stream_id      TEXT NOT NULL,
    processed_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (consumer, event_id)
);
```

消费者在同一个数据库事务内：

1. 插入 `message_inbox`；
2. 写入业务数据；
3. 提交事务；
4. 执行 `XACK`。

收件箱主键冲突表示该事件已经处理，可以安全确认。遥测表自身仍保留 `event_id`
唯一约束作为第二道保护。

## 8. Repository + Unit of Work

Repository 隐藏 SQL 和 JSONB 映射：

```text
UserRepository
DeviceRepository
DeviceAclRepository
TelemetryRepository
CommandRepository
```

一次业务操作涉及多个 repository 时，由显式 `UnitOfWork`/事务对象保证原子性，
例如：

- 批量分享用户和部门；
- 写入响应并更新命令状态；
- 消费消息并记录 inbox；
- 删除设备并清理 ACL。

不要使用 Active Record，领域对象不负责连接数据库或执行 SQL。

## 9. Policy / Specification

RBAC 与设备 ACL 使用策略对象集中判定，不在各 Controller 中复制条件：

```cpp
authorization.canViewDevice(user, device);
authorization.canEditDevice(user, device);
authorization.canShareDevice(user, device);
authorization.canExecute(user, device, operation);
```

策略内部组合：

- 平台 RBAC；
- 设备创建者；
- 用户分享；
- 部门分享；
- `editor > viewer` 权限合并；
- JSONB 命令策略；
- 授权到期时间。

Controller 只调用策略并处理允许/拒绝结果，不能自行拼接 ACL SQL。

## 10. Middleware / Decorator

Ruvia middleware 处理横切逻辑：

- request ID；
- JWT 认证；
- RBAC 前置检查；
- 访问日志；
- 限流；
- 安全响应头；
- 统一错误映射。

设备 ACL 依赖具体资源 ID，通常在应用服务或资源策略中执行，不强行全部放进通用
middleware。

## 11. Cache-Aside

以下数据可使用 Redis cache-aside：

- 用户角色和权限代码；
- 设备静态信息；
- 用户/部门的设备有效权限；
- 协议配置快照。

PostgreSQL 是事实来源。写入数据库事务提交后再失效缓存。部门成员变化必须失效
相关用户的设备 ACL 缓存。

实时遥测队列和权限缓存使用不同 key 前缀，不能混用生命周期策略。

## 12. Retry + Circuit Breaker + Bulkhead

设备和外部系统不稳定，需要组合使用：

- 有上限的指数退避和随机抖动；
- 设备或端点级熔断器；
- 每个链路/协议独立的容量上限；
- 高优先级控制命令与普通轮询的独立额度；
- Redis、PostgreSQL 和 Webhook 的独立连接池。

这可以避免一台异常设备、一个慢协议或一个失败 Webhook 拖垮整个单进程。

业务错误、权限拒绝和不可恢复的协议错误不应自动重试。

## 13. Managed Module Lifecycle

参考 `iot-manager` 的模块生命周期：

```text
configure -> register -> start -> ready -> stop
```

模块管理器负责依赖顺序和失败回滚。例如：

```text
配置 -> 日志 -> 数据库 -> Redis -> 消费者 -> 协议 -> HTTP ready
```

停止时执行反向顺序，先停止接收新请求和新命令，再排空当前批次，最后关闭连接。
后台线程必须由模块拥有，禁止 detached thread。

## 推荐组合

首期采用以下最小组合即可：

| 优先级 | 模式 | 用途 |
|---|---|---|
| 必须 | 模块化单体 | 单进程但保持模块边界 |
| 必须 | Ports and Adapters | 隔离框架、Redis、数据库和协议 |
| 必须 | Redis 事件驱动 | 南北向唯一通信通道 |
| 必须 | Strategy + Registry | 多协议适配 |
| 必须 | State Machine | 设备连接和命令生命周期 |
| 必须 | Idempotent Consumer | 防止重复消费 |
| 必须 | Policy | RBAC 与设备 ACL |
| 建议 | CQRS Lite | 命令入队、查询数据库 |
| 建议 | Process Manager | 查询响应关联和超时 |
| 建议 | Repository + Unit of Work | SQL 隔离和事务一致性 |
| 建议 | Cache-Aside | 权限和静态配置缓存 |
| 后续 | Circuit Breaker/Bulkhead | 高设备量下的故障隔离 |

## 不建议

- 一开始拆成多个微服务；
- 完整 Event Sourcing；
- 为所有类创建接口和抽象工厂；
- 大量全局 Singleton/Service Locator；
- 用一个通用 EventBus 隐藏模块依赖；
- Controller 直接执行 SQL；
- Controller 直接调用协议 Adapter；
- 协议 Adapter 直接写 TimescaleDB；
- 用 detached thread 管理后台任务；
- 把所有字段都放进 JSONB，导致所有权和权限失去约束。
