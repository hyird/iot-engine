# 设备与查询响应模型

设备通信属于南向层。北向 API 只能通过 `iot:commands` 向南向层发送意图，不能
直接取得或调用具体的 `DeviceAdapter`。南向层的所有输出也必须先写入 Redis
Stream，不能直接回调北向层。

## 通信模式

设备支持以下模式：

- `push`：仅主动上报；
- `query_response`：由平台查询后响应；
- `hybrid`：同时支持主动上报和平台查询。

设备协议与通信模式分离。初始协议适配器接口应能覆盖 HTTP、MQTT、Modbus TCP
和自定义 TCP，但实际协议按后续需求逐个实现。

## 适配器接口职责

`DeviceAdapter` 负责：

- 判断是否支持目标协议和操作；
- 从 `iot:commands` 获取并解析命令；
- 与设备建立连接并发送查询；
- 解析设备响应；
- 把成功、失败或超时结果写入 `iot:responses`；
- 只有写入响应 Stream 成功后才能确认命令消息。

适配器禁止直接写数据库。

## 命令状态

```text
queued -> dispatched -> responded
                    -> timed_out
                    -> failed
queued             -> cancelled
```

状态变化由 Redis 事件驱动，数据库消费者负责持久化最终状态。

## 异步查询

异步查询立即返回 `202 Accepted` 和 `command_id`。客户端通过命令查询接口或
SSE/WebSocket 获取最终状态。异步查询是默认方式，适合慢设备、离线设备和重试。

## 等待式查询

等待式 HTTP API 仍然先向 `iot:commands` 执行 `XADD`。响应也必须进入
`iot:responses`，完成持久化后才能通知等待中的 Ruvia 协程。

HTTP 等待超时不取消设备命令。迟到响应仍需正常进入 Redis 并写入数据库。

## 周期查询

调度器根据设备的 `query_interval` 生成普通查询命令并写入 `iot:commands`。
周期查询和人工查询使用相同的处理链路，不单独建立数据库写入路径。
