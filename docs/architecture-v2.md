# 平台架构切换

兼容边界仅限已部署 EdgeNode。平台 API、内部消息和 Redis 运行模型统一切换，不提供双读、双写或旧 API 别名。EdgeNode 的协议版本协商、配置、任务消息、终端及旧固件 token 下载路径继续保留。

## 指令

`POST /v1/device/:id/commands` 必须传 `idempotency_key` UUID。同一调用方复用同一键且参数相同时返回原指令；换设备或参数返回 409。开放接口使用相同要求。前端自动生成键，API 调用方须保存该键供网络重试使用。

PostgreSQL 的 `command_request`、`command_operation`、`command_attempt` 是持久化事实。Redis 仅投递，不再保存第二份指令状态。接受请求及通知在同一事务内落盘。命令队列满时拒绝新增，不裁剪已接受的工作。节点控制使用独立队列，不受其他管理任务裁剪影响。

状态：`ACCEPTED`、`DISPATCHING`、`AWAITING_RESULT`、`SUCCEEDED`、`REJECTED`、`UNKNOWN`、`READBACK_MISMATCH`、`FAILED`。`UNKNOWN` 表示是否执行尚不能确认；后续有效结果可将其确认。等待接口把 UNKNOWN 视为当前尝试已结束，界面提示核对设备而非重发。

一次请求可以包含多个独立指令，不承诺设备上的事务原子性。当前每条指令仅安排一次物理投递尝试，60 秒后仍未确认则结束。发送前认领、发送超时或服务进程中断均不自动重发旧节点控制。Redis/网络故障无法提供设备端恰好执行一次保证。

Webhook 改为 `device.command.accepted`、`device.command.updated`。载荷中的 `command` 包含 `id`、`status`、`reason`、`elements`、`actual_values`；不再将未知结果压成一个 success 布尔值。已有订阅在数据库迁移中转换事件名称，接收方需同步更新。

## 设备及 SL651

设备 UUID 用于缓存、在线截止时间、权限、历史数据和下发记录。协议地址保留在设备配置和原始报文中。同一链路的地址必须唯一，不同链路可以同码。

SL651 测站地址采用 10 位十进制规范文本，原有短地址在迁移中左补零，BCD 报文地址保持相同。新 API 校验 10 位地址。解码先按接入链路选会话，再用测站地址找到 UUID；下发反向使用设备配置的测站地址。运行时拒绝同链路归一化后的重复地址，避免把 `1` 和 `0000000001` 默认为两个站。Edge 物理端点冲突校验继续执行。

## 实例及分区

每次进程启动产生新的实例 UUID。连接寻址为实例 UUID、Worker 序号、会话 epoch。重启不会重新占用前一进程的连接地址。遥测、指令结果和 Edge 入站采用 64 个固定逻辑分区，服务 Worker 数变化不改变分区名称。

采集链路使用 Redis 15 秒租约，每 5 秒刷新，发送前验证所有权。新实例不清除其他实例的实时状态；接管后旧连接必须重连。配置投影完成后，通过运行时事件唤醒各实例的所有采集 Worker；5 秒租约维护不再重复加载配置。启动、重连和 60 秒恢复期限会重新核对当前版本，修复被裁剪或丢失的唤醒提示。Redis 消费者包含实例 UUID；恢复先读自己的 pending，其他消费者的消息至少空闲 60 秒才接管。

Outbox 使用独立 PostgreSQL LISTEN 连接接收事务提交通知，启动和重连后补查持久化待发记录。空闲队列不再按 100 毫秒派发查询；未来可用记录、锁竞争及失败重试各自安排期限。指令结果处理按最近尝试截止时间唤醒，不再执行空闲 250 毫秒扫描。零行更新不会产生查询变更事件。统计采集、租约续期、协议采集周期和丢失提示恢复仍有明确用途的计时器。

这提供平台侧所有权检查，不等于在任意网络分区下对没有 fencing 能力的物理设备实现严格单写事务。公网入口、TCP Server 流量切换、VPN/GB28181 等服务的部署归属仍需在部署配置中落实。

## 切换步骤

1. 停止所有旧平台进程及写入方，备份 PostgreSQL 和 Redis。不要让新旧平台同时使用相同运行库。
2. 使用新程序 `--migrate-only` 执行迁移。若既有 SL651 地址补零后在同链路冲突，迁移会失败；先明确这些设备的实际接入范围，不自动删记录。
3. 使用 `DATABASE_URL`、`REDIS_URL` 运行 `bun ops/import-legacy-commands.ts` 预览，再加 `--apply --offline` 导入仍存在的旧 Redis 指令记录。重复执行不会重复导入。旧 Redis 指令仅保留约 24 小时，已过期的记录无法凭空恢复。
4. 旧 SUCCESS 导入为 SUCCEEDED；旧 PENDING/FAILED 导入为 UNKNOWN，原字段保存到导入请求中。旧控制任务不重放。旧节点管理任务仍可按其原协议处理。
5. 同步上线新前端及开放接口调用方，再启动新平台。遥测历史表继续使用原设备 UUID；新缓存从数据库回填。旧 Worker 队列不再消费，可在备份核验后清理，不提供长期兼容读取。
6. 检查节点重新建立会话、采集数据新鲜度、指令与 outbox 状态。数据库已升级时，不应直接启动旧程序；回滚需要恢复配套备份和旧前端。

## 验证

`ctest --test-dir build -C Release --output-on-failure` 覆盖原有协议及新增测站地址隔离、实例地址、命令未知状态和队列边界用例。

`bun tests/architecture-integration.ts` 默认使用隔离端口：HTTP 55102、PostgreSQL 55439（iot_architecture / architecture_test）、Redis 56439。可通过 `TEST_BASE_URL`、`ARCHITECTURE_DATABASE_URL`、`ARCHITECTURE_REDIS_URL` 指定其他本机隔离实例。运行前应在隔离目录启动迁移完成的新程序。测试通过真实 HTTP、SQL 和 Redis 验证并发幂等、状态持久化、自然到期未知结果及迟到确认、重复结果和队列背压；不会使用默认生产端口。

2026-09-09 收尾验证新增 `outbox-notification-integration.ts`、`config-notification-integration.ts` 和 `live-query-multi-instance-integration.ts`，通过空闲无派发轮询、提交与回滚、未来期限、锁释放、通知连接重建、双实例配置广播、丢失提示恢复及暂停读取的 4 MiB 完整 SSE 快照用例。前端类型检查、lint、构建及 8 项测试（328 个断言）通过。生产基线已部署到 `1109161`（Ruvia `83292260`）；本次事件唤醒收尾尚未部署，详见 `architecture-cutover-plan.md`。

## 边缘节点流量

应用心跳默认间隔从 5 秒调整到 300 秒；服务端以存活的 WebSocket 会话判断在线，每 20 秒只在平台内部刷新 Redis 租约，并检查遗漏的队列唤醒。空队列不产生下行报文。按间隔计算，空闲应用心跳次数降为原来的 1/60；这不是总流量实测值，WebSocket Ping/Pong、遥测及业务任务仍产生流量。

新节点可在遥测内携带设备状态，减少重复上报；旧节点继续通过原有 heartbeat_ack 路径补齐状态。协议扩展使用可选字段，旧固件可继续连接和执行任务。本次平台测试不等同于旧硬件实测或蜂窝流量计费测量。

隔离测试应用停止后，运行 `bun tests/architecture-cutover.ts` 验证旧 Redis 指令导入及实际 SL651 迁移 SQL；它使用与接口测试相同的隔离端口。2026-09-05 本地结果：全新数据库 37 项迁移成功，25/25 C++ 测试通过，HTTP/Redis/PostgreSQL 集成及切换测试通过，前端类型检查、终端协议测试和生产构建通过。未部署生产，未执行硬件 E2E 或全量压力测试。
