# iot-engine 目标架构

本文档是当前架构决策的唯一说明。项目采用单进程运行，但北桥与南桥在 Worker、状态和资源所有权上保持逻辑解耦。

## 1. 总体结构

```text
单进程 iot-engine
├── Northbridge Worker Pool
│   ├── HTTP/API
│   ├── Redis 消费与发布
│   ├── PostgreSQL/TimescaleDB
│   └── 配置投影与遥测持久化
└── Southbridge Worker Pool
    ├── TCP Server/TCP Client
    ├── 连接、协议 Session 与设备路由
    ├── Modbus/S7/SL651 协议运行时
    ├── 协议独立的上报、查询、控制与响应处理
    └── Redis 消费与发布
```

Redis 是南北桥之间唯一的运行时通信边界。南桥不读取或写入 PostgreSQL。所有与南桥
运行状态相关的消息和临时状态都必须按 `worker_id` 分区；一个消息从进入南桥到完成、
失败或进入死信，始终属于最初拥有 socket 的 Worker，禁止跨 Worker 消费、转移 Pending
或共享同一个运行时 Stream。

## 2. CPU 与 Worker 分配

进程启动时读取逻辑 CPU 数量，总线程预算由北桥和南桥平分：

```cpp
const auto cpu = std::max(2u, std::thread::hardware_concurrency());
const auto northWorkerCount = (cpu + 1) / 2;
const auto southWorkerCount = cpu / 2;
```

CPU 数为奇数时，多出的一个 Worker 分配给北桥。允许通过配置覆盖自动计算结果，但默认不创建超过 CPU 总数的业务 Worker。

每个 Worker 是一个完整、独立的事件循环线程。Worker 内可以并发运行多个协程，但同一时刻只在所属线程上访问本地运行状态。

### 2.1 Northbridge Worker

每个北桥 Worker 拥有：

- 一个事件循环线程；
- 一个 Redis 连接；
- 一个 PostgreSQL 连接；
- HTTP 请求处理能力；
- Redis Stream 消费能力；
- 独立的遥测落库批次。

### 2.2 Southbridge Worker

每个南桥 Worker 拥有：

- 一个事件循环线程；
- 一个 Redis 连接；
- 分配给该 Worker 的 TCP socket；
- socket、设备和协议运行状态；
- 一个只负责触发 deadline 的 `TimerScheduler`；
- 各协议完全独立的 Session、业务调度器和响应匹配状态。

南桥 Worker 不创建 PostgreSQL 连接。

## 3. 全协程异步约束

所有 Worker 热路径必须使用协程异步接口。Worker 内禁止：

- `future.get()`、`future.wait()`；
- `condition_variable::wait()`；
- `thread::sleep_for()`；
- 同步 socket、Redis 或 PostgreSQL 调用；
- 在 Worker 内调用 `thread.join()`；
- 通过固定间隔忙轮询等待任务。

允许的等待方式包括：

- `co_await async_read/async_write`；
- `co_await redis.command`；
- `co_await db.query`；
- `co_await timer`；
- `co_await` 异步 mailbox 或 one-shot signal。

Worker 没有任务时应挂起，由新任务、网络事件或定时器唤醒。进程监督器只能在所有事件循环停止后回收线程，不能让业务 Worker 同步等待其他线程退出。

### 3.1 每个 Worker 一个计时设施

每个 Worker 只能创建一个 `TimerScheduler`，底层始终最多挂起一个异步 timer。socket、设备和协议运行时不得分别创建独立 timer。

`TimerScheduler` 只是 Worker 级基础设施，不是跨协议业务调度器。它只保存 `deadline -> callback/token`，不知道设备队列、功能码、读写顺序、响应类型或命令完成条件。是否登记 deadline、deadline 到期后执行什么动作，完全由链路重连逻辑或对应协议 Session 决定。

各运行对象只向统一调度器登记自身协议确实需要的 deadline：

- Modbus Session 的周期采集、`in-flight` 和写后回读超时；
- S7 Session 的会话协商、周期采集、exchange 和写后回读超时；
- TCP Client 重连；
- Modbus/S7 TCP Server 模式配置启用时的注册、心跳和离线判定；
- Modbus/S7 可选的广播 `DiscoveryWindow`；
- Redis Pending 重试；
- 批量落库刷新；
- 配置重载防抖和协议维护。

SL651 只登记自身真实需要的组帧、多包重组和可选下发命令超时。不得因为 Worker 存在统一计时设施，就给 SL651 创建注册、心跳、轮询、广播、通用 `in-flight` 或写后回读任务。

调度器使用最小堆或时间轮保存任务，只把最近的 deadline 设置到底层异步 timer：

```text
register/cancel deadline
-> 更新统一调度结构
-> 必要时重新设置最近到期时间
-> co_await timer
-> 执行所有已到期任务
-> 设置下一个最近 deadline
```

定时任务使用稳定时钟计算超时；业务时间仍使用 UTC 系统时间。取消使用 token/generation，不依赖销毁回调。socket 重连或配置更新后，旧 `session_epoch`/generation 的到期任务必须直接丢弃。

一次到期事件过多时，调度器应设置单轮处理预算，达到预算后主动让出执行权并继续处理，避免定时任务饿死 TCP 和 HTTP I/O。

## 4. 南桥运行状态层级

南桥运行状态分为四层：

```text
Worker
└── Link
    └── Socket
        └── Device
```

### 4.1 Worker 级

Worker 负责承载多个独立 socket。TCP Server 接受连接后，以随机或轮询方式把连接分配给一个南桥 Worker。连接一旦分配，在断开前不得迁移。

TCP Client 的每个目标连接在创建时分配 Worker，断线重连必须返回原 Worker。

### 4.2 Link 级

链路级状态只负责：

- Listener 启停；
- socket 集合；
- 链路配置重载；
- 连接数、客户端 `IP:Port` 和状态汇总；
- 协议支持时的广播 fan-out 与广播结果汇总。

普通查询、写入、回读和响应匹配不得使用整个 `link_id` 作为串行化范围。

### 4.3 Socket 级

每个 socket 独立拥有统一的传输状态，以及由当前协议创建的 `ProtocolSession`：

- `connection_id` 与 `session_epoch`；
- 协议需要时的注册、心跳和在线状态；
- 绑定的设备集合；
- 接收组帧缓冲；
- 协议自定义的会话、响应匹配或多包重组状态；
- 在 Worker 统一 `TimerScheduler` 中登记的超时和重试 deadline。

同一链路下的不同 socket 可以并发工作。一个 socket 变慢不得阻塞其他 socket。

### 4.4 Device 级

每台设备必须具有唯一 `device_code`，并维护通用设备信息：

- 设备协议配置；
- 设备时区；
- 当前 socket 路由。

Modbus/S7 可以在各自的协议运行时中为设备附加读、写、轮询或控制队列；SL651 不得因为共用 `DeviceRuntime` 而被强制创建这些队列。

设备被当前协议识别并绑定 socket 后，运行时路由至少包含：

```text
device_code -> worker_id + connection_id + session_epoch
```

`session_epoch` 用于拒绝重连前产生的过期指令和响应。

## 5. 协议独立运行时与时序

系统不设置跨协议的通用业务调度框架。统一边界只到 Worker 执行环境、Link 传输、socket 所有权和 deadline 计时设施；任务队列、`in-flight`/exchange、轮询、响应匹配和命令完成条件由每种协议独立定义。

- Modbus 和 S7 支持周期读取、主动查询和写入；
- SL651 以设备主动上报为主，只在配置了协议命令时产生主动请求；
- 协议适配器负责构建协议支持的请求、匹配响应和定义命令完成条件；
- Modbus/S7 各自的 socket 调度器负责队列、公平性、超时和同一 socket 的串行化；
- SL651 直接处理上行报文，不经过 Modbus/S7 的主动读写调度器。

禁止创建包含 `readQueue`、`writeQueue`、`poll`、`readback`、`heartbeat`、`registration` 和 `discovery` 全套字段的通用协议 Session。公共传输层只负责把连接事件和原始字节交给当前协议 Session；命令、轮询、探测、注册和 deadline 回调通过可选 capability 接口接入。每个协议只实例化自己支持的接口与状态。

### 5.1 Modbus

Modbus 周期采集由南桥 Poll Scheduler 产生并实际发送。北桥只负责保存和发布采集配置。

用户主动查询或控制由北桥写入 Redis 指令队列，设备所属南桥 Worker 消费后使用本地 socket 发送。

#### 5.1.1 Socket 级 in-flight

`in-flight` 属于 socket，而不是设备或整个链路。一个 DTU socket 后面的多个 Modbus 从站共享同一个 `in-flight`。

Modbus TCP 响应按以下字段匹配：

```text
transaction_id + unit_id + function_code
```

Modbus RTU 响应按以下字段匹配：

```text
slave_id + function_code
```

异常响应使用去除异常位后的功能码匹配。

#### 5.1.2 写后强制回读

每个写任务必须携带对应的回读定义。调度顺序固定为：

```text
Write -> Readback -> Write -> Readback
```

禁止连续处理两个写任务。写入和对应回读构成一个不可插入的 socket 级事务序列：

```text
Idle
-> SendingWrite
-> WaitingWriteResponse
-> SendingReadback
-> WaitingReadbackResponse
-> VerifyValue
-> Complete / Retry / DeadLetter
-> Idle
```

即使写响应超时，也必须执行回读：

- 回读值符合预期：任务成功，并记录 `write_ack_missing`；
- 回读值不符合预期：按策略重试写入；
- 回读也超时：释放 `in-flight`，进入重试或死信。

写后回读完成前，同一 socket 不得发送其他协议请求。其他 socket 不受影响。

### 5.2 S7

S7 socket 必须先完成协议会话建立，才能进入普通读写调度：

```text
TCP Connected
-> COTP CR/CC
-> Setup Communication
-> Session Ready
-> Read Var / Write Var
```

COTP 建连和 PDU 协商都属于 socket 状态，不能放到设备级队列中。协商完成后记录 PDU 长度、PDU Reference 和会话能力。

S7 请求响应使用 `PDU Reference` 匹配。第一版每个 socket 同时只允许一个 S7 exchange，即使 PLC 协商结果允许多个并行作业，也不启用流水线并发；只有在独立压力测试证明时序正确后才能增加窗口。

S7 读取必须根据协商 PDU 长度对 Area 项进行合并和分批。一个逻辑读任务可以包含多个顺序执行的 `Read Var` 帧，全部完成后才生成一次统一结果。

S7 写任务同样执行强制回读：

```text
必要时预读（BOOL/位级读改写）
-> Write Var
-> Read Var 回读
-> 比较写入目标值
```

预读只用于构造安全的位级写入，不能代替写后的验证回读。写响应超时后仍尝试回读，并采用与 Modbus 相同的 `write_ack_missing`、重试和死信语义。

### 5.3 SL651

SL651 只运行在 TCP Server 链路上，以设备主动上报为主要数据方向：

```text
Socket 收到报文
-> 立即写入所属 Worker 的 Redis 原始报文 Stream
-> 同一 Worker 按 socket 接收顺序处理
-> 按 SL651 帧边界组帧
-> CRC/长度/控制字符校验
-> 从协议报文读取设备编码
-> 多包按设备编码 + 功能码在 socket Session 内重组
-> 按功能码和要素配置解析
-> 重组完成后生成一条统一遥测消息
-> 发布到所属 Worker 的 Redis 已解析消息 Stream
```

SL651 不使用 Modbus/S7 的注册码和心跳包配置，也不运行周期 Poll Scheduler。设备身份来自合法 SL651 帧中的设备编码。

SL651 必须支持粘包、拆包以及协议定义的多包报文重组。每次 socket 原始读取都必须先写入 `iot:channel:packet:raw:worker:<worker_id>`，成功后才允许进入协议解析；每个 Worker 独立顺序处理自己的 Stream，不得跨 Worker 消费。多包重组状态属于 socket，超时按最后一个有效分包到达时间刷新；收到同设备、同功能码的新图片首包时，立即丢弃未完成旧图片。只有全部分包收齐后才发布一条已解析消息。

SL651 不创建通用 `readQueue`/`writeQueue`。如果某个功能码支持下发，由 SL651 运行时维护可选的下发命令状态，并通过设备当前 socket 发送。上行主动报文可以在命令等待期间到达，适配器必须先按设备编码、功能码、方向和协议序号判断它是命令响应还是独立上报，不能把任意上行帧当作当前命令的响应。

SL651 下发命令的完成条件由功能码定义，不套用通用写后回读流程：

- 收到并校验与命令对应的同功能码响应，或协议定义的成功/失败确认码，才能完成当前命令；
- 不得为 SL651 伪造不存在的读功能或强制查询回读；
- 没有响应语义的命令只能标记为已发送但未确认，不能返回“设备已确认成功”。

## 6. 广播探测

广播是可选的链路级协议能力，只能由 TCP Server 模式且配置支持主动探测的 Modbus/S7 运行时创建。普通 Link 不强制持有广播队列或 `DiscoveryWindow`；TCP Client 直连模式也不运行注册广播。SL651 通过合法上报帧中的设备编码识别设备，不进行注册广播。

```text
Link Broadcast
├── Socket A
├── Socket B
└── Socket C
```

链路广播协调器生成统一 `broadcast_id`，并把广播任务 fan-out 到该链路的所有目标 socket。

每个 socket 仍独立遵守自己的时序：

- Modbus/S7 socket 已存在 `in-flight`/exchange 时不得中断当前请求；
- socket 空闲后才能发送广播；
- 响应由所属 Worker 解析；
- 链路协调器在广播窗口内汇总所有 socket 的响应。

一个 DTU socket 的一次探测可能返回多个设备响应，因此广播期间 socket 使用 `DiscoveryWindow`，允许接收多个合法响应。普通查询仍只匹配一个响应。广播结果必须分别交给 Modbus 或 S7 适配器验证，不能仅凭任意返回字节注册设备。

## 7. Redis 职责

Redis 保存以下类型的数据：

- 南桥每次 socket 接收的原始字节，先按 Worker 写入有界 Stream，报文使用大写十六进制字符串；
- 统一解析消息与遥测事件；
- 北桥下发给南桥的查询和控制指令；
- 南桥返回的指令执行结果；
- 链路、socket、Session 和设备当前状态；
- 配置快照和配置变更通知；
- 有界重试状态和死信。

设备管理实时读模型固定使用两类可读 Hash：

```text
iot:state:device:<device_code>
  device_id / device_code / state / state_reason
  worker_id / connection_id / session_epoch（在线时存在）
  last_report_at_ms / updated_at_ms

iot:latest:device:<device_code>:element:<element_id>
  element_id / element_name / value / unit / protocol
  observed_at_ms / updated_at_ms / source / data
```

设备状态 key 常驻：启动时所有未删除设备先写为 `offline`，没有连接不等于没有设备。要素 key 按协议配置建立；尚无历史值时 `value` 固定为 `-`。每个要素 Hash 的 `data` 字段保存与 PostgreSQL `data JSONB` 同构的完整 JSON 文档，其他扁平字段用于无需解析 JSON 的批量读取和时间比较。Redis 本身没有 PostgreSQL `JSONB` 类型，因此不依赖 RedisJSON 模块。连接建立只更新状态和路由字段，断线时条件校验 `connection_id` 后删除路由字段并保留 `offline` 状态，避免旧连接的迟到清理覆盖重注册的新连接。

设备管理页的设备、链路和协议配置仍来自 PostgreSQL 事实表；连接状态和最新要素值只读 Redis，不允许列表请求逐设备扫描 TimescaleDB 历史表。协议配置定义展示全集与顺序，Redis 只覆盖已经产生的最新值。

Redis Key 使用可读的业务语义。当前状态使用 Hash，可靠消息使用有界 Stream。
Session、事件和死信必须设置明确的生命周期或容量限制，不能无限增长。

### 7.1 Worker-affine 消息队列

所有南桥运行时消息都使用生产者—Redis Stream—消费者模型，并按 socket 所属 Worker
隔离。禁止创建由多个南桥 Worker 共同竞争的 raw、parsed、command、egress、result、
event、config notification 或 dead-letter Stream。

固定的 Worker 分区 Key 如下：

```text
iot:channel:packet:raw:worker:<worker_id>
iot:channel:packet:parsed:worker:<worker_id>
iot:channel:command:worker:<worker_id>:high
iot:channel:command:worker:<worker_id>:normal
iot:channel:socket:egress:worker:<worker_id>
iot:channel:command:result:worker:<worker_id>
iot:channel:link:event:worker:<worker_id>
iot:channel:control:worker:<worker_id>
iot:channel:config:worker:<worker_id>
iot:channel:dead-letter:worker:<worker_id>
```

每条运行时消息必须携带 `message_id`、`created_at_ms`；有关联来源时同时携带
`causation_id`。涉及 socket 的消息必须携带 `connection_id + session_epoch`。队列容量、
Pending 恢复、最大尝试次数和死信策略必须明确。

各阶段的所有权固定如下：

| 阶段 | 生产者 | Stream | 消费者 |
| --- | --- | --- | --- |
| 网络上行 | Worker N 的 TCP Runtime | `packet:raw:worker:N` | Worker N 的协议解析消费者 |
| 解析结果 | Worker N 的协议 Session | `packet:parsed:worker:N` | 北桥持久化消费者为该分区建立的消费者组 |
| 设备命令 | 北桥命令服务 | `command:worker:N:high/normal` | Worker N 的协议命令消费者 |
| 网络下行 | Worker N 的协议 Session | `socket:egress:worker:N` | Worker N 的 TCP 发送消费者 |
| 命令结果 | Worker N 的协议 Session | `command:result:worker:N` | 北桥命令结果消费者 |
| 链路事件 | Worker N 的 TCP Runtime | `link:event:worker:N` | Worker N 的状态投影消费者 |
| 配置通知 | 北桥配置投影器 | `config:worker:N` | Worker N 的配置消费者 |
| 最终失败 | Worker N 的任一消费者 | `dead-letter:worker:N` | 运维、审计或人工处理消费者 |

TCP Runtime 收到字节后只能 `XADD packet:raw:worker:N`，不能把同一个内存对象直接交给
协议 Session 形成绕过 Redis 的快速路径。协议消费者通过 `XREADGROUP` 先恢复自己的
Pending，再读取新消息；协议动作和下游消息生产成功后才允许 `XACK + XDEL`。同样，
协议 Session 不能直接调用 `tcp.send()`，必须生产 `socket:egress:worker:N`，由本 Worker
的网络消费者完成实际发送。

可靠性语义统一为至少一次投递加幂等消费。消费者在数据库事务提交或下游 Stream
生产成功前不得 ACK。重试只能重新进入原 Worker 的队列，其他 Worker 不得 `XCLAIM`、
读取或接管该消息。Worker N 退出后，其 Pending 只允许由恢复后的 Worker N 处理；如果
原 socket 已不存在，则以 `connection_id + session_epoch` 判定为过期并进入 Worker N 的
死信，不能重放到新连接。

北桥根据 `iot:state:device:<device_code>` 中当前的 `worker_id + connection_id +
session_epoch`，直接把命令投递到目标 Worker 队列，不先写入一个由所有南桥 Worker
竞争的全局命令队列。设备重新连接到 Worker M 后，新命令只进入 M；旧 Worker N 中的
命令不得迁移。广播必须显式地为每个目标 Worker 生产独立子任务，并在北桥或链路协调层
按父 `message_id` 聚合，不能由一个 Worker 访问其他 Worker 的 socket。

设备状态和逐要素最新值属于北桥全局读模型，不是南桥内部队列，因此继续使用
`iot:state:device:<device_code>` 与 `iot:latest:device:<device_code>:element:<element_id>`。
版本化配置快照同样是不可变公共数据；只有配置通知 Stream 按 Worker 分区。所有其他
Worker 内部状态必须在 Key 中包含 `worker:<worker_id>`。

## 8. 北桥持久化

南桥解析成功后只写 Redis，不写 PostgreSQL：

```text
Southbridge
-> Redis telemetry Stream
-> Northbridge consumer group
-> PostgreSQL/TimescaleDB transaction
-> COMMIT
-> Redis 设备状态与逐要素最新值 pipeline
-> XACK + XDEL
```

北桥为每个 `packet:parsed:worker:N` 分区分别建立持久化消费者组。一个分区可以映射到
任一北桥 Worker 执行数据库写入，但南桥消息本身不改分区，也不重新发布到其他南桥
Worker。北桥 Worker 数量与南桥 Worker 数量不同时，使用稳定的取模映射分配分区。

数据库不是另起一套 `iot_*` 命名，而是以 `iot-manager` 的现有表为基线：`sys_department`、`sys_role`、`sys_user`、`sys_user_role`、`link`、`protocol_config`、`device_group`、`device`、`device_data`。原表的核心字段和 JSONB 边界必须保留；本项目只在其上增加 UUIDv7、所有权、约束和南北桥运行所需字段。全部业务主键使用 UUIDv7，不能恢复整数兼容列或重复影子表。

`device` 保留原版 `protocol_params JSONB` 作为设备协议参数真源。设备编码、目标 ID、在线超时、远程控制、Modbus 模式、Slave ID、设备时区、心跳包和注册包都放在该对象中；`id/name/link_id/protocol_config_id/group_id/status` 等稳定关系保持强类型列。设备编码通过 JSONB 表达式唯一索引保证全局唯一。

`device_data` 保留原版“一条完整协议结果一行”的设计和 `id/device_id/link_id/protocol/data/report_time/created_at` 核心字段，再扩充 `connection_id/source/occurred_at/raw_payload_hex`。动态协议结果统一保存在 `data JSONB`；`raw_payload_hex` 是 JSONB 字符串数组，单包结果为一项，多包结果按协议包序保存多项。一个完整多包图片只能生成一行，不能按分包写多行。索引覆盖 `device_id + report_time`、`link_id + report_time` 和 `device_id + data.function_code + report_time`。不得为每个协议要素动态增加数据库列。

每个北桥 Worker 独立攒批，默认满足任一条件即写入：

- 达到 100 条；
- 等待达到 200ms。

数据库写入失败时不得 ACK。Pending 消息按有界策略重试，超过次数后进入死信。数据库使用 `device_data.id`（消息 UUIDv7）实现幂等，避免 Redis 重试导致重复数据。

设备最新状态和每个要素都必须独立比较 `observed_at_ms`，旧报文晚到不能覆盖较新的设备状态或其他要素。多点位分批采集时，每个要素保留自己的最后值，不能用“设备最后一条报文”替换整台设备的值集合。

进程启动时，北桥在南桥启动前执行一次可恢复投影：清理旧设备状态/最新值 key，为全部未删除设备写入 `offline + -`，再从 TimescaleDB 按 `device_id + element_id` 回填最后一条历史值。之后只由解析消息增量更新 Redis；设备管理请求不再访问遥测历史表。

## 9. 配置同步与启动顺序

PostgreSQL 是配置事实来源，但只有北桥访问。北桥完成配置事务后，把版本化配置投影到 Redis，再通知南桥加载。

```text
Northbridge CRUD
-> PostgreSQL COMMIT
-> 更新 Redis 配置快照
-> 发布 config_changed(version)
-> Southbridge Workers 加载新版本
-> 每个 Worker 发布 applied version
```

配置快照使用内容签名去重：运行内容未变化（例如删除一个已经禁用的设备）不得制造空版本。所有北桥 Worker 通过 PostgreSQL advisory lock 串行投影，完整写完版本化 Hash/Set 后才原子切换 active-version。每个南桥 Worker 将已应用版本写入 `iot:state:runtime-config:worker:<index>`；active-version 表示“已发布”，全部 Worker 状态一致才表示“已应用”。

北桥另有且只有一个配置对账协程。CRUD 提交后只设置进程内 dirty 标记，不允许每个
HTTP 请求各自重建完整快照；对账协程在 100ms 内合并并串行投影，空闲时每 5 秒以
PostgreSQL 为真源校验一次。新版本完成原子切换后，投影器为每个南桥 Worker 分别向
`iot:channel:config:worker:N` 生产轻量通知。即时 Redis 通知失败时 CRUD 已提交结果仍然
成功返回，dirty 标记保证 Redis 恢复后补齐快照。南桥同时轮询 active-version，因此
配置通知 Stream 只是低延迟通知，消费后必须 `XACK + XDEL`，不能成为唯一可靠来源。
Redis 配置丢失时由北桥重建，南桥不得绕过边界直接查询数据库。

设备、链路、协议变更只重载内容发生变化的链路，不能重启无关协议或无关 socket。被任一未删除设备引用的链路或协议配置禁止删除，API 返回明确的 409；必须先删除关联设备。

进程启动顺序：

```text
1. 数据库迁移
2. 启动 Northbridge Worker Pool
3. 北桥从 PostgreSQL 重建 Redis 配置快照
4. 北桥建立全部设备离线状态并从 TimescaleDB 回填逐要素最新值
5. 标记配置和实时读模型 ready
6. 启动 Southbridge Worker Pool
7. 南桥加载配置、发布 applied version 并启动链路
```

## 10. 时间规则

- PostgreSQL 会话时区固定为 UTC；
- 数据库时间字段使用 `TIMESTAMPTZ`；
- API 和 Redis 消息使用带 `Z` 或明确 UTC 偏移的 ISO 8601 时间；
- 每台设备独立保存 `timezone`；
- 设备报文时间先按设备时区解析，再转换为 UTC；
- 设备时区不得改变数据库或连接会话时区。

## 11. 协议边界

三种协议只复用 Worker 执行环境、Link 传输和 socket 所有权，不复用任务调度、会话状态或命令完成条件：

| 能力 | Modbus | S7 | SL651 |
| --- | --- | --- | --- |
| TCP Server | 支持 | 支持 DTU/桥接模式 | 唯一允许模式 |
| TCP Client | 支持 | 支持 PLC 直连模式 | 不支持 |
| Server 模式注册码/心跳 | 按配置支持 | 按配置支持 | 不支持 |
| 周期主动读取 | 支持 | 支持 | 不支持 |
| 主动写入 | 支持 | 支持 | 按功能码配置 |
| 下发完成判定 | 写响应加寄存器回读 | 写响应加 Area 回读 | 同功能码响应或协议确认码 |
| 普通响应关联 | 事务号/从站/功能码 | PDU Reference | 设备编码/功能码/方向/序号 |
| 主动广播探测 | Server 模式按配置支持 | Server 模式按配置支持 | 不支持 |
| 多响应窗口 | 探测时支持 | 探测时支持 | 不适用 |
| 多包重组 | RTU/TCP 组帧 | TPKT/COTP 分帧 | 协议多包重组 |

所有协议唯一必须实现的是最小传输会话接口：

```text
onSocketConnected
consumeBytes
onSocketDisconnected
```

其他行为按 capability 组合，不能放进所有协议都必须实现的基类：

```text
CommandCapability        -> executeCommand
PollingCapability        -> buildPoll / scheduleNextPoll
DiscoveryCapability      -> startDiscovery / consumeDiscoveryResponse
RegistrationCapability   -> consumeRegistration / refreshHeartbeat
DeadlineCapability       -> onDeadline(token)
```

`TimerScheduler` 只在 Worker 内唤醒 token；只有声明 `DeadlineCapability` 的 Session 才接收该 token，并由协议自己解释其含义。协议不支持的命令必须在进入 Worker 指令队列前按 capability 拒绝，不能发送给 Session 后再依赖空实现返回失败。

具体组合固定如下：

| Session | 必须能力 | 可选能力 | 明确禁止 |
| --- | --- | --- | --- |
| Modbus | 传输、命令、响应关联 | 轮询、Server 注册/心跳、Server 探测、deadline | SL651 多包语义 |
| S7 | 传输、会话协商、命令、响应关联 | 轮询、Server 注册/心跳、Server 探测、deadline | Modbus 事务号语义 |
| SL651 | 传输、上报解析、多包重组 | 功能码下发、重组/下发 deadline | 轮询、注册码/心跳、广播探测、通用读写队列、写后回读 |

这里的 capability 是装配和入队校验依据，不是一个包含所有虚函数的“大接口”。不得用默认空实现、`not_supported` 分支或无意义字段让协议伪装成支持某项能力。

`buildRead`、`buildWrite`、`buildReadback`、`matchResponse` 和 `consumeDiscoveryResponse` 不属于跨协议基础接口，只能存在于支持该语义的 Modbus/S7 内部。SL651 的 `consumeBytes` 必须始终先处理主动上报，不依赖通用主动请求调度器。

适配器输出统一结果，但保留协议原始语义：

```text
message_id
device_id
device_code
link_id
connection_id
session_epoch
protocol
direction
message_type
observed_at
received_at
values
raw_payload_hex[]
quality
state_reason
```

所有原始报文使用大写十六进制字符串；统一结果中的原始报文字段始终是数组，单包也不使用标量兼容格式。协议时间按设备 `timezone` 解析后转换成 UTC；无法确定设备时间时使用接收时间并明确标记时间来源。

北桥持久化只消费统一结果，不包含 Modbus、S7 或 SL651 的协议解析逻辑。

## 12. 实施要求

目标实现需要独立的 Northbridge Worker Pool 和 Southbridge Worker Pool。Southbridge Worker 必须拥有自己的事件循环和 Redis 上下文，不能借用某个 Web Worker，也不能通过同步客户端补齐功能。

南桥共享实现的边界只能包括：Worker 生命周期、异步 TCP 传输、socket 所有权、
Worker 分区 Redis 生产与消费、配置版本加载和 deadline 计时设施。公共层不得拥有
Poll Scheduler、通用读写队列、通用 `in-flight` 或统一命令状态机。网络层、协议层、
状态投影层之间也不得通过进程内快捷调用绕过已定义的 Stream。协议 Session、业务队列、
解析、响应关联、重试和完成判定必须位于 `modbus`、`s7`、`sl651` 各自子模块中。
新增协议时先声明 capability，再接入其真实支持的路径，禁止通过填充无意义的通用字段
或空实现满足接口。

Ruvia 需要提供可供非 HTTP Worker 使用的通用异步 Worker Context，使北桥和南桥复用同一套协程、Redis 和生命周期能力，同时保持各自独立的资源所有权。
