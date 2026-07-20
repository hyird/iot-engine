# HTTP API 草案

所有路径使用 `/api/v1` 前缀。错误响应使用统一 JSON Envelope，并包含可追踪的
`request_id`。

## 健康检查

```text
GET /api/v1/health/live
GET /api/v1/health/ready
```

`live` 只检查进程是否运行；`ready` 检查 Redis、TimescaleDB 和必需消费者状态。

## 设备

```text
GET    /api/v1/devices
POST   /api/v1/devices
GET    /api/v1/devices/{device_id}
PATCH  /api/v1/devices/{device_id}
DELETE /api/v1/devices/{device_id}
```

设备列表只能返回当前用户创建或被分享的设备。设备详情、遥测、命令和修改接口都
必须在后端执行设备级权限检查。

## 设备分享

```text
GET    /api/v1/devices/{device_id}/shares
POST   /api/v1/devices/{device_id}/shares
PATCH  /api/v1/devices/{device_id}/shares/{subject_type}/{subject_id}
DELETE /api/v1/devices/{device_id}/shares/{subject_type}/{subject_id}
```

新增分享支持在一次请求中同时选择多个用户和多个部门：

```json
{
  "subjects": [
    {
      "type": "user",
      "id": "019f...",
      "access": "viewer"
    },
    {
      "type": "user",
      "id": "019f...",
      "access": "editor"
    },
    {
      "type": "department",
      "id": "019f...",
      "access": "viewer"
    }
  ]
}
```

`type` 只能是 `user` 或 `department`，`access` 只能是 `viewer` 或 `editor`。
批量写入必须在同一个数据库事务中完成；任意目标无效时整个请求失败，避免出现
部分分享成功。

这些接口只允许设备创建者调用。通过个人或部门获得权限的用户，无论权限级别
如何，都不能调用分享接口或删除设备。

分享列表统一返回两种目标：

```json
{
  "items": [
    {
      "subject_type": "user",
      "subject_id": "019f...",
      "display_name": "张三",
      "access": "editor"
    },
    {
      "subject_type": "department",
      "subject_id": "019f...",
      "display_name": "生产一部",
      "access": "viewer"
    }
  ]
}
```

## 主动上报

```text
POST /api/v1/devices/{device_id}/telemetry
```

API 校验请求后执行 `XADD iot:telemetry`，成功返回 `202 Accepted`。API 不直接写
TimescaleDB。

## 遥测查询

```text
GET /api/v1/devices/{device_id}/telemetry
GET /api/v1/devices/{device_id}/metrics/{metric}
```

查询参数建议包含：

- `from` 和 `to`；
- `limit` 和分页游标；
- `bucket` 聚合窗口；
- `aggregation`，例如 `avg`、`min`、`max`、`last`。

## 设备查询命令

异步接口：

```text
POST /api/v1/devices/{device_id}/queries
GET  /api/v1/commands/{command_id}
```

等待式接口：

```text
POST /api/v1/devices/{device_id}/queries:wait?timeout_ms=3000
```

两种接口都必须先写入 `iot:commands`。等待式接口不能绕过 Redis 直接访问设备。
创建者和具有相应操作权限的编辑用户可以发起查询；只读用户只能查看已持久化的
查询结果。

## 实时通知

首选 SSE：

```text
GET /api/v1/events?device_id={device_id}
```

用于向前端推送命令状态和新遥测通知。实时通知不是数据库可靠性的组成部分；刷新
页面后仍应从 TimescaleDB 查询权威历史数据。

## 队列运维

```text
GET  /api/v1/queue/status
GET  /api/v1/queue/dead-letters
POST /api/v1/queue/dead-letters/{stream_id}:retry
```

运维接口必须受管理员权限保护，并记录审计日志。
