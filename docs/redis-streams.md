# Redis 通道与运行状态

Redis 只承担进程内模块之间的消息通道和短期运行状态，不保存业务主数据。南桥、
北桥是同一进程中的逻辑模块，因此 key 不包含 `south` 或 `north`。

## 消息通道

| Key | 类型 | 用途 |
|---|---|---|
| `iot:channel:config` | Stream | 链路、协议、设备配置变更 |
| `iot:channel:packet:raw` | Stream | 设备原始上行报文 |
| `iot:channel:packet:parsed` | Stream | 已解析设备消息 |
| `iot:channel:command` | Stream | 北桥下发命令 |
| `iot:channel:protocol:task:high` | Stream | 注册、响应等高优先级任务 |
| `iot:channel:protocol:task:normal` | Stream | 普通查询任务 |
| `iot:channel:link:event` | Stream | 链路状态变化通知 |
| `iot:channel:dead-letter` | Stream | 达到重试上限的消息 |

所有 `message_id`、`causation_id` 和业务资源 ID 都使用 UUIDv7。原始报文只使用
大写十六进制字符串字段：原始包为 `payload_hex`，解析消息保留
`raw_payload_hex`；不保存二进制转义串，也不兼容旧 `payload` 字段。

消费者成功处理消息后执行 `XACK` 和 `XDEL`。配置消息消费完成后同样立即删除，
不会把 Stream 当长期审计库。普通 Stream 默认最多 10000 条，链路事件和死信默认
最多 1000 条。

## 可读运行状态

运行状态采用“一个对象一个 key”，避免一个大 Hash 混放所有连接或协议组：

| Key | 类型 | 内容 |
|---|---|---|
| `iot:state:link:<link_uuid>` | Hash | 链路名、协议、模式、连接数、端点和精确状态 |
| `iot:state:session:<connection_id>` | Hash | 远端 `ip:port`、链路 UUID、DTU、注册状态和时间 |
| `iot:state:protocol:queue-depth:<group_key>` | String | 该协议组当前排队任务数 |
| `iot:state:protocol:inflight:<group_key>` | Hash | 当前查询的 Stream、消息、连接、超时和 Modbus 匹配字段 |

`group_key` 表达调度顺序域，例如一个 DTU 或
`link:<link_uuid>:discovery`。同组任务严格串行；High Stream 总是优先读取，但一个
查询进入 in-flight 后会立即检查上行报文，避免高优先级任务破坏查询—响应时序。

in-flight Hash 使用字段名直接表达含义，例如：

```text
token                 019f...
stream                iot:channel:protocol:task:high
entry_id              1721...-0
message_id            019f...
connection_id         019f...-12
protocol              Modbus
deadline_at_ms        1784600000000
transport             TCP
transaction_id        12
unit_id               1
function_code         3
```

## 容量与清理

- High 队列最多 2048 条，Normal 队列最多 10000 条。
- 每个 `group_key` 最多排队 256 条，容量判断和入队由 Lua 原子完成。
- 查询完成、超时转死信或任务失败时，任务、in-flight 和 queue-depth 原子清理。
- session 在连接断开时删除，服务启动时只清理上次残留的 session。
- Redis 不保存长期 session/event 历史；持久业务数据仍写 PostgreSQL。
