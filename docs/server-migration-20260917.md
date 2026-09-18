# 2026-09-17 生产服务器迁移

## 范围与结果

- 来源：`103.236.69.112`；目标：`43.142.33.210`；域名：`i.a-z.xin`。
- 用户修改 DNS，并放行目标服务器 TCP 443。
- 迁移现有生产制品 `225719b9e097d5e356a3e6e63a65677a2d89798b`，未重新构建或发布代码。
- 新机保留 `/opt/iot/server`、同版本 `web/`、固件、配置及 `/opt/iot/releases/225719b/` 制品和许可证。
- 新机 `iot.service` 为 `active/running`，验收时重启次数为 0；旧机 `iot.service` 已停止并禁用自启。
- 旧机原有 TimescaleDB、Redis 及其他业务仍保留。

## 存储与配置

- 按用户要求沿用 Compose，直接使用官方滚动镜像标签，不使用镜像代理。
- `/opt/timescaledb/compose.yaml` 使用 `timescale/timescaledb:latest-pg18`，数据目录为 `/opt/timescaledb/data`。
- `/opt/redis/compose.yaml` 使用 `redis:latest`，数据及配置分别位于 `/opt/redis/data`、`/opt/redis/config`。
- 新机 PostgreSQL 为 18.6；恢复的数据库 TimescaleDB 扩展版本仍为 2.29.1，未执行扩展升级。
- 停止旧平台后，用 `pg_basebackup` 配合 WAL 流生成完整物理备份。旧机共享集群包含其他业务库，因此新机恢复的是完整集群，未修改旧机这些业务库。
- Redis 通过一致性 RDB 快照恢复，随后重新生成 AOF，再重启容器验证持久化。
- Compose 增加容器健康检查；`iot-storage.service` 负责启动两组 Compose，`iot.service` 等待存储与平台防火墙服务。
- 数据库、Redis 和业务 HTTP 入口分别保持回环监听 `5432`、`6379`、`3000`；数据库时区为 UTC。
- `.env` 的 SIP/RTP 公网地址改为新 IP。VPN 网络的 Hub 地址由旧 IP 改为新 IP，8 个未撤销 Peer 的配置版本递增，原有密钥保留。
- 迁移了平台 SIP 防护和媒体 HTTP 监听防护规则，未修改新机已有的 `wg0` 隧道。

## HTTPS

- 旧通配符证书已过期，在新机为 `i.a-z.xin` 重新申请证书，新私钥在新机生成。
- 新证书到期日为 `2026-12-16`，存放于 `/etc/letsencrypt/live/i.a-z.xin/`。
- 已启用 `certbot-renew.timer`，部署钩子在配置检查成功后重载 Nginx；`certbot renew --dry-run --cert-name i.a-z.xin` 通过。
- HTTP 保留 ACME 验证路径，其余请求跳转 HTTPS；HTTPS 代理至 `127.0.0.1:3000`，保留 WebSocket、SSE 与下载配置。

## 验收证据

- PostgreSQL 备份归档 SHA-256 校验通过；解包后 `pg_verifybackup` 通过。
- 启动平台前，51 张业务表行数逐表一致，52 条迁移记录的 ID 和校验和完全一致。
- 恢复时包含 14 个边缘节点、9 个设备、307,576 条设备数据、78 个固件记录、1 个用户。
- 183 个平台文件 SHA-256 一致；通过新机 HTTPS 逐项读取的 25 个前端资源与源文件字节一致。
- 运行二进制 SHA-256：`12e273df395349f92e8e8a60b73ebe6d91ebc19b94c3f411db7f2d5abcd63de7`。
- 从旧机访问新 IP 的 HTTPS 返回 200，证书校验结果为 0；实际浏览器正确显示登录页面。
- `/internal/health/ready` 返回 `ready`；Redis AOF 已启用，写入状态正常；两个存储容器均为 `healthy`。
- 通过 Redis 有效会话 TTL 确认 12 个节点在线；剩余 2 个节点的最后在线时间为 9 月 4 日、9 月 14 日，均早于本次迁移。
- 验收时设备数据已增长至 307,599 条；3 个 VPN 配置任务成功，3 个 Peer 在新机完成近期握手。
- 平台启动后的日志未发现 `fatal`、`panic`、`segmentation`、`failed`、`error`，服务重启次数为 0。
- 未执行用户登录后的全业务页面、实际视频播放及 Windows 客户端交互测试；本次复用现有生产制品，未运行新的构建与 CTest。

## 备份与清理

- 旧机完整备份保留在 `/opt/iot/backups/migration-20260917/`，包含 PostgreSQL 物理备份、Redis 快照、逐表数量、迁移校验和与文件清单。
- 新机迁移临时备份及校验目录由用户的另一个任务清理；本任务确认后不再重建该目录。
- 本次临时 SSH 授权及传输密钥已移除。
- 新平台已接收新写入，若需要回迁，必须先停止新平台并迁移新增数据，不能直接启用旧库作为当前生产库。
