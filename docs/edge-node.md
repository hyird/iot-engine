# 边缘节点对齐与多平台扩展设计

本文定义 `iot-engine` 边缘节点的功能基线、OpenWrt 运行形态、nanopb 线协议以及多平台
路由规则。实现顺序固定为：先对齐 `iot-manager` 已有功能，再开放多平台，最后完成各
OpenWrt 目标平台的可安装制品验证。

## 1. 参考基线

功能基线来自 `D:\Workspace\iot-manager` 中删除边缘节点构建目标前的提交
`53e6008e338618a4a57e6ed4c1f3ffbf3150375f`。后续提交
`26719b11ca1f6e7de632839980f0fa61cfa85c77` 为修复主程序 MinGW 构建，删除了整个
`edgenode/` 目录，但平台端管理 API、WebSocket 契约和管理页面仍保留。

对齐的是旧实现向用户提供的功能，不复制已知缺陷：

- 旧节点只能配置一个 `platform_url`，新节点的数据模型从第一版保留平台命名空间；
- 旧 Modbus 控制在收到写响应后立即报告成功，后续回读只刷新数据。新节点必须以实际
  回读值作为最终结果；
- 旧 S7 边缘节点只有轮询采集，没有写入与写后回读。新节点与中心侧 S7 完成条件一致；
- 按部署要求，节点从 HTTPS 地址建立加密升级连接但不校验服务端证书链和主机名；该模式不能防止中间人
  攻击，生产网络必须依赖专网/VPN、平台独立凭据和消息来源审计降低风险；
- 旧网络配置只适配 NetworkManager、systemd-networkd、netplan 和 ifupdown。OpenWrt
  必须通过 UCI/ubus 修改网络，不能直接套用这些文件写入逻辑；
- 旧平台以进程内回调直接处理 Agent 数据。`iot-engine` 中边缘会话、协议结果、命令和
  持久化仍通过 Redis Stream 解耦，不增加绕过 Redis 的快速路径。

## 2. 功能对齐清单

| 功能域 | `iot-manager` 删除前能力 | `iot-engine` 对齐要求 |
| --- | --- | --- |
| 节点注册 | SN + model 自注册，平台审批，也可由 UI 预创建 | 外部身份改为 15 位 IMEI；保留自注册、预创建、批准、拒绝和重新注册审计；凭据按平台独立签发 |
| 在线状态 | hello、心跳、超时离线、版本、能力和运行信息 | 保留；状态写 Redis，事实和审计写 PostgreSQL，时间统一 UTC |
| 节点管理 | 列表、重命名、删除、重新同步、摘要卡片 | 保留，接入现有 RBAC、分页和统一错误返回 |
| 端点管理 | 以太网/串口端点，启停，关联设备检查 | 保留；端点 ID 改为 UUID，前后端校验一致 |
| 以太网模式 | TCP Server、TCP Client | 保留；SL651 仅 Server，Modbus/S7 按协议能力限制 |
| 串口模式 | Modbus RTU，通道和波特率 | 保留，并补齐数据位、停止位、校验位和 RS485 参数 |
| SL651 | 本地组帧与解析，解析失败时可原始透传 | 复用当前南桥 SL651 语义，支持拆包、粘包、多包、CRC 和命令响应判断 |
| Modbus | TCP/RTU 轮询、寄存器合并、缩放、控制、快速回读 | 复用当前南桥 Modbus 定义；最终控制结果必须比较写入目标和回读值 |
| S7 | TSAP/Rack-Slot 连接和周期读取 | 补齐为当前中心侧同等的握手、分批读取、写入、BOOL 预读和强制回读 |
| 离线续传 | SQLite 缓存，100 条一批，ACK 后删除，超时重发 | 不使用数据库；完整 nanopb 消息按平台写入 `/tmp/edgenode`，进程重启可恢复，设备重启/断电后清空 |
| 原始透传 | 设备原始数据上报和指定客户端下发 | 保留，但必须带来源平台、端点、连接 epoch 和消息 ID |
| 配置同步 | 版本号、应用成功/失败、失败回滚、离线排队 | 保留；改为分块事务同步、摘要校验和每平台独立版本 |
| 端点状态 | 连接状态、客户端列表、错误、最后活动时间 | 保留；平台只能看到自己配置的端点和设备 |
| 网络管理 | DHCP/静态地址、桥创建/删除、成员口调整 | 保留功能面；OpenWrt 使用 UCI/ubus，并加入失联自动回滚 |
| 在线终端 | 单 PTY，会话打开、输入、缩放、关闭、压缩输出 | 保留但默认关闭；显式授权、短期会话票据、完整审计，同一节点一次一个写会话 |
| L2 运维 | EtherType `0x88B5` 发现和本地 TCP 隧道 | 保留为可选包特性；发现和控制载荷同样使用 nanopb，隧道默认只允许本机 SSH 端口 |
| 事件审计 | 注册、上线、配置、网络配置、离线等最近事件 | 保留并增加平台、消息 ID、配置版本、结果和失败阶段 |

“对齐完成”必须由自动化测试逐项证明，不能只以页面存在或 WebSocket 已连接作为结论。

待审批节点保持同一条 WebSocket 长连接并按配置周期发送应用层心跳。平台批准后在该连接
上返回 `HelloAck`，节点直接进入已注册会话，不以反复断开重连轮询审批状态。

## 3. 运行结构

```text
平台 iot-engine（单进程）
├── Northbridge
│   ├── 边缘节点/端点管理 API 与 RBAC
│   ├── PostgreSQL 配置事实、注册关系和审计
│   ├── 配置投影、命令创建和遥测持久化
│   └── 管理端页面
├── Edge Gateway（连接归属固定到一个 Worker）
│   ├── HTTP/HTTPS 升级后的二进制会话与 nanopb 编解码
│   ├── 节点认证、心跳和配置同步
│   ├── 上行消息写入 Redis Stream
│   └── 从 Redis Stream 消费下行配置和命令
└── Redis
    ├── 边缘上行遥测、原始报文、状态与结果
    ├── 边缘下行配置、命令和终端控制
    └── Pending、重试和死信

OpenWrt 节点 edgenode
├── PlatformSession[platform_id]（固化引导平台 + 最多三个动态平台）
├── ConfigStore（分平台 tmpfs nanopb staging/active）
├── EndpointRuntime（TCP Server/TCP Client/Serial）
├── ProtocolRuntime（SL651/Modbus/S7）
├── Outbox[platform_id]（tmpfs nanopb 消息文件 + 内存索引）
├── NetworkManager（UCI/ubus + 失联回滚）
├── TerminalManager（默认关闭）
└── L2Maintenance（可选）
```

平台端边缘连接仍遵守 `docs/architecture.md` 的 Redis 边界：网络会话不得直接写
PostgreSQL，也不得直接调用北桥业务服务。一个连接建立后记录
`gateway_worker_id + connection_id + session_epoch`，重连前的命令和 ACK 不得作用于
新会话。

## 4. nanopb 线协议

### 4.1 版本与生成方式

- 使用 `nanopb` `0.4.9.1`，上游 tag commit 为
  `cad3c18ef15a663e30e3e43e3a752b66378adec1`；
- `.proto` 是协议事实来源；平台副本位于 `service/modules/edge/edge.proto`；
- OpenWrt 节点源码、节点测试、协议副本及预生成 nanopb 文件只由独立的
  `openwrt-dtu-packages/net/edgenode` 包维护；本仓库不再保存节点实现；
- 平台 CMake 从平台副本生成 `.pb.c/.pb.h`，不提交平台生成文件；
- 设备程序只编译 `pb_common.c`、`pb_encode.c`、`pb_decode.c` 和生成文件；
- 构建依赖 nanopb 生成器、Python 和 protobuf host 工具，设备运行时不依赖完整
  Google protobuf；
- 禁止使用 protobuf map、`google.protobuf.Any`、`Struct` 或设备端无边界的动态数组。

### 4.2 传输与封装

平台配置和管理界面只接受 HTTP/HTTPS 基础地址。节点内部把该地址转换为对应的
WebSocket Upgrade 连接并追加 `/edge/v1/connect`；每个 Binary Message 只包含一个
nanopb `Envelope`，消息边界就是 Envelope 边界，不再套 JSON 或 base64。

`Envelope` 至少包含：

- `protocol_version`：线协议主版本；
- `message_id`：16 字节 UUIDv7；
- `node_id`：平台批准后签发的节点 UUID；
- `platform_id`：节点本地连接配置的稳定 UUID；
- `session_epoch`：每次成功握手递增；
- `created_at_ms`：UTC epoch milliseconds；
- `causation_id`：响应、结果和 ACK 对应的原消息 ID；
- `sequence`：同一会话内单调递增序号；
- `payload`：`oneof` 业务消息。

单个 Envelope 上限为 16 KiB。解码前先检查 WebSocket message 大小，nanopb 的
`max_size`、`max_length` 和 `max_count` 必须与平台端校验一致，超限消息直接拒绝并写
审计，不得截断。

### 4.3 消息类别

第一阶段必须覆盖以下 protobuf payload：

- `Hello` / `HelloAck` / `EnrollmentPending` / `EnrollmentRejected`；
- `Heartbeat` / `HeartbeatAck`；
- `CapabilityReport` / `EndpointStatusReport` / `EventReport`；
- `ConfigBegin` / `ConfigItem` / `ConfigCommit` / `ConfigApplied` /
  `ConfigRejected`；
- `TelemetryBatch` / `TelemetryAck`；
- `RawPacket` / `RawPacketAck`；
- `CommandRequest` / `CommandProgress` / `CommandResult`；
- `NetworkConfigRequest` / `NetworkConfigResult`；
- `TerminalOpen` / `TerminalData` / `TerminalResize` / `TerminalClose`；
- `Error` / `Ping` / `Pong`。

配置不以一个巨大快照装入内存。平台先发 `ConfigBegin(revision, item_count, sha256)`，
再逐条发送带顺序号和摘要的 `ConfigItem`，最后发送 `ConfigCommit`。`ConfigItem` 使用
`oneof` 表达端点、设备、Modbus 寄存器、S7 Area、SL651 功能和要素。节点将 item 写入
`/tmp/edgenode/<platform_id>/staging/`，每个文件直接保存一条 nanopb `ConfigBegin`、
`ConfigItem` 或 `ConfigCommit`。全部收齐且摘要一致后通过目录 rename 原子切换 active
revision。进程重启从 tmpfs 恢复，设备重启后平台按 IMEI 重新注册并重发完整配置。

每个 `ConfigItem.sha256` 是将该 item 的 `sha256` 字段清空后进行 nanopb 确定性编码所得
字节的 SHA-256；revision 摘要是按 `index` 顺序拼接所有 32 字节 item 摘要后再次计算
SHA-256。单个 revision 最多 512 个 item，超限在 `ConfigBegin` 阶段拒绝。

遥测使用有上限的批次；平台 ACK 返回已接收的消息 ID。每个 outbox `.pb` 文件就是一条
完整 nanopb `Envelope`，节点收到 ACK 后才删除对应平台目录中的文件。进程重启重新扫描
tmpfs 并续传；设备重启或断电会清空 tmpfs。

### 4.4 协议兼容

- 只允许在 protobuf 中新增可选字段或新增 field number；禁止复用已发布的 number；
- 删除字段必须保留为 `reserved`；
- 握手交换支持的主/次版本和能力位；主版本不兼容时拒绝会话；
- 平台不得根据 Agent 软件版本猜测字段，必须根据协商能力下发；
- 配置内容使用类型化 protobuf，不把完整协议配置 JSON 塞进 `bytes` 字段规避 Schema；
- 原始设备报文使用 `bytes`，不得 base64。

## 5. 平台侧数据模型

平台至少新增以下事实表：

### 5.1 `edge_node`

- `id UUID PRIMARY KEY`；
- `imei VARCHAR(15)`，活动记录唯一，写入前执行数字格式和 Luhn 校验；
- `model`、`name`、`version`；
- `auth_status`：`pending/approved/rejected`；
- `capabilities JSONB`、`runtime JSONB`；
- `expected_config_version`、`applied_config_version`；
- `config_status`、`config_error`；
- `last_seen`、`connected_at`、`last_config_sync_at`、
  `last_config_applied_at`，全部 `TIMESTAMPTZ`；
- 审批人、审批时间和软删除字段。

平台不保存可恢复的明文 enrollment token。批准后使用独立节点凭据；凭据轮换和吊销要有
审计。

### 5.2 `edge_endpoint`

- `id UUID PRIMARY KEY`、`edge_node_id UUID`；
- `name`、`transport`、`mode`、`protocol`、`status`；
- 以太网地址/端口；
- 串口通道、波特率、数据位、停止位、校验位和 RS485 参数；
- 创建、更新时间与软删除字段。

设备继续使用现有 `device.link_id` 关联统一链路模型。边缘端点在配置投影时转换成
`LinkDefinition`，不再保留旧项目的 `link_id = 0` 哨兵值。现有 `link.agent_*` 占位字段
在迁移前必须核对用途，不能直接删除。

### 5.3 `edge_event` 与凭据表

事件表记录节点、事件类型、级别、消息、结构化 detail 和 UTC 时间。节点凭据、注册挑战
和吊销状态单独存储，不能混入 capabilities/runtime JSONB。

实时在线、连接路由和端点状态写 Redis；PostgreSQL 只保存事实、最近状态快照和审计，
页面不得逐节点扫描历史遥测。

## 6. 单平台功能对齐阶段

第一阶段虽然 Schema 中已经存在 `platform_id`，运行配置只允许一个 enabled 平台，直到
以下验收全部通过：

1. IMEI/model 自注册、平台审批、凭据签发、重连和吊销；
2. 节点列表、状态、版本、能力、事件、重命名、删除和重新同步；
3. 以太网与串口端点 CRUD、状态和配置版本回报；
4. SL651、Modbus、S7 采集结果与中心侧同一协议配置产生等价数据；
5. Modbus/S7 控制使用实际回读值决定成功、不一致、超时和离线；
6. 平台断开期间数据进入分平台 tmpfs 队列，进程重启后仍可续传且不会跨平台 ACK；
7. UCI 网络配置、桥管理和失联自动回滚；
8. 受控在线终端与完整审计；
9. 可选 L2 发现/维护；
10. OpenWrt 安装、启停、升级、卸载保留配置和回滚验证。

南向 DTU 调度和北向上报必须解耦。节点固定每 1000 ms 对已启用的 Modbus/S7 设备执行
一次采集循环，并在同一周期处理排队的写命令；平台配置的 `report_interval_sec` 只决定
最新采集值何时进入该平台的 outbox，不能降低设备读写频率。不同平台可配置不同上报
间隔，调度器始终使用设备配置的 `origin_platform_id` 选择 outbox。

Modbus 与 S7 写入都必须经过“写请求、写响应校验、强制读回、目标值与实际值比较”四个
阶段。S7 在握手、读取、写入或读回阶段只要设备在超时内无响应，就必须关闭底层 TCP
连接；下一次 1 秒周期重新建立 TCP，并重新执行 COTP Connection Request/Confirm 与
S7 Setup Communication。不得在无响应后复用旧 TCP 会话，因为 PLC 端可能仍保留半开
连接或不确定的 PDU/作业状态。

只有这些能力全部完成，才把 `max_enabled_platforms` 从 1 放开。

## 7. 多平台扩展

### 7.1 本地平台配置

固化引导平台为 `https://i.a-z.xin`，不能通过 UCI 覆盖。其他平台由引导平台授权的命令
使用 UCI CLI 新建或删除，每个平台连接是独立 section，包含：

- 本地 `platform_id`；
- HTTP/HTTPS `url` 和启用状态；当前部署明确不校验 HTTPS 服务端证书；
- enrollment token 或已签发凭据的引用，密钥文件权限必须是 `0600`；
- `priority`；
- outbox 容量和重连策略。

节点全局网络、固件和平台配置命令只接受固化引导平台；动态平台不能取得这些权限。

动态端点与设备配置不写入 UCI 或数据库，只以 nanopb 消息写入分平台 tmpfs 目录；进程
重启恢复 active 配置，设备重启后平台根据 IMEI 重新下发完整配置。

### 7.2 来源归属

每一条下发配置在节点本地都带不可变的 `origin_platform_id`：

```text
(origin_platform_id, remote_endpoint_id) -> local endpoint runtime
(origin_platform_id, remote_device_id)   -> local device runtime
telemetry/command result/raw packet      -> origin_platform_id outbox
```

硬性规则：

- 哪个平台创建的端点和设备，其采集数据、原始报文、状态和命令结果只发回该平台；
- 一个平台的 Telemetry ACK 只能删除该平台 outbox 中明确列出的消息；
- 断线重连、配置回滚和凭据轮换不能改变历史记录的来源平台；
- 平台不能查询、控制、删除或占用另一平台的设备；
- 节点自身的 CPU、内存、磁盘等公共健康信息可发给所有已批准平台，但不得包含其他平台
  的端点、设备、地址或队列明细；
- 日志字段包含 `platform_id`，但不记录 token、私钥或完整凭据。

### 7.3 物理资源冲突

串口、监听端口、目标 PLC 会话和网卡是节点全局物理资源。应用配置前建立资源声明表：

```text
serial:/dev/ttyS1
listen:tcp:0.0.0.0:6001
client:tcp:192.168.1.10:102
network-owner
terminal-writer
```

第一版不允许跨平台共享物理采集端点。新配置与其他平台已生效资源冲突时，只拒绝本次
平台 revision，保留它的上一版本以及其他平台全部运行状态。错误必须返回冲突资源的
匿名化标识，不泄露另一平台名称或设备配置。

### 7.4 节点全局操作

网络配置只接受固化引导平台的请求。其他平台只能读取脱敏后的接口
能力。在线终端同一时刻只允许一个写会话，按平台权限和显式抢占策略仲裁；默认不允许
抢占。升级、重启、恢复出厂设置等节点全局操作后续使用相同所有权模型，不能采用“最后
下发者获胜”。

## 8. OpenWrt 包

OpenWrt 安装包、进程、服务和 UCI 配置名统一为 `edgenode`：

```text
/usr/sbin/edgenode
/etc/init.d/edgenode
/etc/config/edgenode
/etc/edgenode/credentials/
/usr/share/edgenode/NOTICE
```

- 使用标准 OpenWrt package `Makefile`，由 OpenWrt SDK 为目标架构生成 `.ipk`；
- 使用 procd 监督，支持 respawn、reload、资源限制和干净停止；
- 所有 OpenWrt 设置变更都通过 UCI CLI 完成，并使用标准 reload 接口生效；
- 动态依赖显式声明，nanopb 源码编入包内，不要求设备安装 protobuf；
- 分平台 outbox 直接写 `/tmp/edgenode/<platform_id>/outbox/*.pb`，不写 flash；
- 每次写入前检查 tmpfs。预计剩余低于总容量 15% 时，从所有平台 outbox 中按修改时间
  滚动删除最旧消息，直至恢复 15% 空闲；不得删除 active/staging 配置；
- 进程重启保留 tmpfs 配置与未 ACK 数据；系统升级、设备重启或断电后由平台重新同步；
- 网络配置应用前保存 UCI 备份，启动失联看门狗。新配置在限定时间内无法重新连接
  `network_owner` 时自动恢复并 reload network；
- shell、L2 隧道和远程网络修改分别提供编译/运行开关，默认最小权限；
- 进程降权运行；需要串口、原始套接字或网络配置时通过专用用户组、ubus ACL 和最小
  capability 授权，不长期以无限制 root 身份运行。

首批 CI 至少覆盖一个 64 位 ARM musl SDK 和一个 MIPS/musl SDK。具体设备 target/
subtarget 在产出可安装 `.ipk` 前必须由实际硬件确认，不能用宿主 Linux 二进制冒充
OpenWrt 包。

## 9. Redis 消息边界

边缘连接按 Gateway Worker 分区，建议新增：

```text
iot:channel:edge:ingress:worker:<worker_id>
iot:channel:edge:egress:worker:<worker_id>
iot:channel:edge:config:worker:<worker_id>
iot:channel:edge:command:worker:<worker_id>
iot:channel:edge:result:worker:<worker_id>
iot:channel:edge:event:worker:<worker_id>
iot:channel:edge:dead-letter:worker:<worker_id>
```

消息携带 `message_id`、`platform_id`、`edge_node_id`、`connection_id`、
`session_epoch`、`created_at_ms` 和可选 `causation_id`。遥测最终转换为现有
`ParsedDeviceMessage` 再进入统一持久化路径；边缘网关不能直接写 `device_data`。

下行命令先根据 Redis 节点状态解析当前 Gateway Worker 和 session epoch，再投递到该
Worker 的 Stream。旧会话 Pending 不迁移到新 Worker；新连接建立后由北桥根据业务命令
状态决定重新投递，而不是由网关猜测。

## 10. 验收与发布门槛

### 10.1 协议与单元测试

- nanopb 与平台 protobuf 实现进行双向 golden vector 测试；
- 未知字段、缺字段、超限长度、错误 oneof、截断消息和版本不兼容测试；
- 配置 begin/item/commit 的丢包、重复、乱序、摘要错误和重启后重新同步测试；
- 每个平台独立 ACK、重试、死信和容量淘汰测试；
- UUID、序列号、时间、端口、串口和各协议元素约束前后端一致。

### 10.2 功能与故障测试

- 旧功能对齐清单逐项通过；
- 两个平台同时在线，各自下发不同端点，数据和命令结果零串流；
- 一个平台断线时另一平台不受影响，断线平台恢复后只续传自己的数据；
- 两个平台争用串口/端口时后到配置明确失败，既有采集不中断；
- Modbus/S7 写 ACK 成功但回读不一致时最终结果必须失败并带实际值；
- 配置应用、网络 reload、进程重启和设备断电后均能由平台重发恢复到确定状态；
- 网络配置导致平台失联时自动回滚；
- 单平台 outbox 达到配置上限或 tmpfs 低于 15% 触发滚动清理时必须产生可见告警，不能
  静默丢数据。

### 10.3 制品验证

- 使用 OpenWrt SDK 实际生成 `.ipk`，检查架构、依赖、文件权限和 conffiles；
- 在真实设备执行安装、首次注册、审批、配置、采集、控制、重启、升级和卸载测试；
- 验证 procd 状态、ubus 状态、UCI reload、HTTPS 加密升级连接和日志脱敏；
- 平台端仍需完成 Linux、macOS、Windows 三平台配置、构建和 CTest；
- 没有实际硬件验证的 target 必须标记为“仅完成交叉编译”，不能宣称可部署。
