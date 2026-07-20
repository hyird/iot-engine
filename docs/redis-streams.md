# Redis Streams 设计

## Stream 清单

| Stream | 生产者 | 消费者组 | 用途 |
|---|---|---|---|
| `iot:telemetry` | 接入 API、设备适配器 | `telemetry-writers` | 主动上报数据 |
| `iot:commands` | HTTP API、定时调度器 | 按协议划分的 gateway group | 设备查询命令 |
| `iot:responses` | 设备协议适配器 | `response-writers` | 查询成功、失败或超时结果 |
| `iot:telemetry:dlq` | 遥测消费者 | 运维工具 | 无法处理的遥测消息 |
| `iot:commands:dlq` | 命令消费者 | 运维工具 | 无法发送的命令 |
| `iot:responses:dlq` | 响应消费者 | 运维工具 | 无法处理的响应 |

## 通用 Envelope

Redis Stream 使用少量可路由字段，业务内容放入 JSON `payload` 字段：

```json
{
  "schema_version": 1,
  "event_id": "019f...",
  "event_type": "telemetry.reported",
  "device_id": "sensor-01",
  "correlation_id": null,
  "occurred_at": "2026-07-20T08:00:00Z",
  "produced_at": "2026-07-20T08:00:01Z",
  "payload": {}
}
```

必须携带 `schema_version`，以支持未来兼容迁移。时间使用 UTC RFC 3339。
消费者将 `payload` 原样或标准化后写入 PostgreSQL JSONB，避免为不同设备型号
反复修改表结构。

## 遥测消息

```json
{
  "schema_version": 1,
  "event_id": "019f...",
  "event_type": "telemetry.reported",
  "device_id": "sensor-01",
  "occurred_at": "2026-07-20T08:00:00Z",
  "payload": {
    "metrics": {
      "temperature": 24.6,
      "humidity": 61.2
    },
    "attributes": {
      "site": "taipei-1"
    }
  }
}
```

## 查询命令

```json
{
  "schema_version": 1,
  "event_type": "device.query.requested",
  "command_id": "019f...",
  "correlation_id": "019f...",
  "device_id": "meter-01",
  "protocol": "modbus_tcp",
  "operation": "read_metrics",
  "timeout_ms": 3000,
  "payload": {
    "metrics": ["voltage", "current"]
  }
}
```

## 查询响应

成功、设备错误和超时都必须写入 `iot:responses`：

```json
{
  "schema_version": 1,
  "event_id": "019f...",
  "event_type": "device.query.responded",
  "command_id": "019f...",
  "correlation_id": "019f...",
  "device_id": "meter-01",
  "status": "success",
  "occurred_at": "2026-07-20T08:00:02Z",
  "payload": {
    "metrics": {
      "voltage": 220.4,
      "current": 1.8
    }
  }
}
```

## 确认和重试

1. `XREADGROUP` 批量读取；
2. 校验 Envelope 和业务 payload；
3. 开启数据库事务；
4. 幂等写入；
5. 提交数据库事务；
6. 执行 `XACK`；
7. 可在确认后执行 `XDEL`，但默认依靠 `MAXLEN` 保留短期审计数据。

建议初始参数：批量 100、阻塞 1 秒、Pending 超时 30 秒、最大重试 5 次。
重试次数需要保存到消息或独立的 Redis Hash 中。
