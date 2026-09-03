# iot-engine VPN 核心设计

## 1. 设计目标

Windows 客户端通过 iot-engine 访问 Edge 节点所在局域网设备：

- LAN 设备无需安装客户端或修改默认网关。
- Edge 支持 NAT、4G 和无公网入站连接。
- 真实 LAN 和虚拟 LAN 映射网段全局唯一且不重叠。
- iot-engine 统一管理 Peer、虚拟地址、路由、权限、撤销和审计。
- VPN 数据面不进入现有 WebSocket、Redis 或 Protobuf 队列。
- Edge 固件不引入完整用户态 VPN 守护进程。

## 2. 方案结论

| 项目 | 方案 |
| --- | --- |
| VPN 数据面 | WireGuard over UDP |
| VPN 控制面 | HTTPS API + 现有 Edge WSS/Protobuf |
| 中心节点 | Linux 上的 iot-engine 内置 WireGuard Hub/Server |
| Windows 客户端 | 官方 WireGuard Windows 客户端 + 标准 `.conf`，首期不运行 Hub |
| Edge 数据面 | Linux 内核 WireGuard，固件使用 `kmod-wireguard` |
| 网络模型 | IPv4、L3、Hub-and-Spoke |
| LAN 映射 | 虚拟网段到真实 LAN 等长映射，默认 Edge NAT |
| 首期不做 | L2、广播、mDNS、全流量代理、客户端互访、IPv6、P2P 直连 |

不要把 IP 数据包封装进 WSS/Protobuf，避免 TCP-in-TCP 和队头阻塞。

## 3. 总体拓扑

```text
                         控制面
       Web/HTTPS <------> iot-engine <------> Edge WSS/Protobuf
                              |
                         WireGuard Hub
                         UDP :51820
                         /          \
                        /            \
             Windows WireGuard     Edge WireGuard
                 100.96.0.10/32      100.96.0.20/32
                                           |
                              172.31.10.0/24 虚拟 LAN
                                           |
                                  Edge DNAT + MASQUERADE
                                           |
                              192.168.10.0/24 真实 LAN
                                           |
                                      LAN 设备
```

首期所有 Peer 都连接 Linux Hub。Windows 只需要 Hub Endpoint；Edge 通过主动连接和 `PersistentKeepalive` 适应 NAT。

## 4. 跨平台分层

```text
跨平台 iot-engine Core
  - VPN 数据模型、API、RBAC、审计
  - CIDR 校验和地址分配
  - 配置生成与期望状态收敛
  - Edge WSS/Protobuf 控制面

IWireGuardController
  ├─ Linux：内核 WireGuard + Generic Netlink/UAPI
  ├─ Windows：外部官方 WireGuard Runtime/服务适配
  └─ 其他平台：不支持时显式返回 unsupported_platform
```

| 平台 | Hub 数据面 | 说明 |
| --- | --- | --- |
| Linux | 内核 WireGuard | 生产首选 |
| Windows | 官方 WireGuard 客户端 | 首期仅作为客户端 |
| Edge/OpenWrt | 内核 WireGuard + nftables | 必须支持 |
| macOS/其他平台 | 暂不承诺 | 需要独立适配器 |

跨平台核心不得直接包含 Linux Netlink、Windows 驱动头文件或平台 shell 命令。Linux Hub 是首期唯一生产数据面；Windows Hub 作为后续平台适配项，不进入 MVP。

## 5. 地址和 LAN 映射

### 5.1 地址池

```text
VPN Overlay：100.96.0.0/11，每个 Peer 分配 /32
虚拟 LAN 池：172.31.0.0/16
Hub：100.96.0.1
```

首期只有一个平台级 WireGuard Server，固定网络名为 `iot-server`，不在 EdgeNode
页面创建额外 VPN 网络。创建或修改桥接映射时拒绝 Overlay、虚拟 LAN、服务端网段及
其他映射之间的重叠；真实 LAN 和虚拟 LAN 的前缀长度必须相同，并在所有 EdgeNode
之间全局唯一。

### 5.2 首期 NAT 映射

```text
真实 LAN：192.168.10.0/24
虚拟 LAN：172.31.10.0/24

172.31.10.50:502 -> 192.168.10.50:502
```

虚拟网络号由平台自动分配，默认按真实网络号映射；用户只允许修改虚拟网络号，不能
修改桥接接口、真实 LAN、掩码或 NAT 模式。虚拟地址和真实地址使用相同主机位偏移：

```text
virtual_ip = virtual_network_base + host_offset
real_ip    = real_network_base    + host_offset
```

Edge 配置：

- VPN 接口到虚拟 LAN 的 DNAT。
- 到真实 LAN 的 MASQUERADE/SNAT。
- 使用 conntrack 保证响应流量返回 Windows。
- 默认拒绝未声明的 VPN 到 LAN 流量。

首期要求真实网段和虚拟网段前缀长度相同。后续可增加 routed mode，由现场网关配置回程路由，从而保留客户端源地址。

### 5.3 AllowedIPs

- Windows Peer：仅包含用户被授权的虚拟 LAN 网段。
- Edge Peer：仅包含该 Edge 负责的虚拟 LAN 网段。
- Peer Overlay 地址唯一，使用 `/32`。
- 不自动生成 `0.0.0.0/0`，不做全流量代理。
- AllowedIPs 由服务端生成，客户端不可任意修改后作为可信配置。
- 客户端配置默认包含当前账户可访问的全部 Edge 虚拟 LAN，不提供手工选择设备。
- 客户端 `AllowedIPs` 只负责本机路由；Hub 还必须按客户端 Overlay `/32` 和授权虚拟网段
  配置 nftables 转发 ACL，客户端自行扩大 `AllowedIPs` 不能越权。

## 6. WireGuard 运行时

### 6.1 iot-engine Linux Hub

iot-engine 在 Linux 服务端维护 `wg` 接口：

- 默认监听 UDP `51820`。
- 以数据库 `vpn_network` 表中固定的 `iot-server` 记录作为 Hub 配置源。
- 首次运行自动生成 Hub 私钥和公钥并回写表；私钥不进入 API、Protobuf 或普通日志。
- 为每个 Windows/Edge Peer 配置公钥、地址和 AllowedIPs。
- 使用 `IWireGuardController` 做幂等创建、更新、删除和状态读取。
- Linux 实现通过 WireGuard Generic Netlink/UAPI 和 rtnetlink 配置内核接口，不启动 `wg`、`ip` 或其他 shell 子进程。
- Hub 只处理 WireGuard 数据面，不读取内层业务数据包。

```text
create_interface(name, private_key, listen_port)
set_interface(...)
add_peer(...)
update_peer(...)
remove_peer(...)
get_runtime_status(...)
```

### 6.2 Linux/Edge

- 固件仅加入 `kmod-wireguard` 及目标内核依赖。
- Edge Agent 通过内核控制接口配置 WireGuard、路由和 nftables。
- 不加入 `wireguard-go`、`wg-quick`、LuCI VPN 页面或独立 VPN daemon。
- Edge 私钥在本地生成和保存。

### 6.3 Windows 客户端

- 客户端使用官方 WireGuard Windows 客户端，导入标准 `.conf`。
- iot-engine 只生成配置，不复制 Windows WireGuard 数据面。
- Windows Hub 不属于 MVP；后续如需要，再增加独立的 Windows Runtime 适配器。

标准配置：

```ini
[Interface]
PrivateKey = <client-private-key>
Address = 100.96.0.10/32
MTU = 1280

[Peer]
PublicKey = <hub-public-key>
Endpoint = vpn.example.com:51820
AllowedIPs = 172.31.10.0/24
PersistentKeepalive = 25
```

## 7. 控制面

现有 Edge WSS/Protobuf 只承载：

- VPN 能力和版本。
- Hub 公钥、Endpoint、Edge 地址。
- 路由映射和 NAT 模式。
- 配置版本、应用结果、错误和状态。
- 撤销、暂停、重新同步通知。

不承载 IP 数据包，也不复用现有 egress queue。

建议在 `service/features/edge/edge.proto` 增加：

```protobuf
message VpnCapabilities {
  bool supports_vpn = 1;
  string wireguard_version = 2;
  string agent_version = 3;
}

message VpnRoute {
  string route_id = 1;
  string virtual_cidr = 2;
  string target_cidr = 3;
  string mode = 4;       // nat or routed
  string nat_mode = 5;   // masquerade or none
  bool enabled = 6;
}

message VpnConfigRequest {
  uint64 config_version = 1;
  string hub_public_key = 2;
  string hub_endpoint = 3;
  uint32 hub_listen_port = 4;
  string edge_address = 5;
  repeated VpnRoute routes = 6;
}

message VpnConfigResult {
  uint64 config_version = 1;
  bool applied = 2;
  string error_code = 3;
  string error_message = 4;
}
```

私钥不得进入 Protobuf。Edge 配置必须支持版本、幂等、重试和失败回滚。

## 8. 数据模型

从当前迁移版本 0028 之后追加 VPN 迁移。

### `vpn_network`

```text
id, name, overlay_cidr, hub_private_key, hub_public_key,
hub_endpoint, hub_listen_port, status,
created_by, created_at, updated_at
```

### `vpn_peer`

```text
id, network_id, peer_type,
edge_node_id, user_id, name,
public_key, assigned_ipv4, status,
config_revision, last_handshake_at,
created_at, updated_at, revoked_at
```

### `vpn_route`

```text
id, network_id, edge_peer_id,
lan_interface, target_cidr, virtual_cidr,
mode, nat_mode, status, enabled,
last_error, created_by, created_at, updated_at
```

### `vpn_access_rule`

```text
peer/user/role, route, protocol, port_range,
action, priority, enabled
```

### `vpn_enrollment`

```text
token_hash, network_id, allowed_routes,
expires_at, used_at, created_by
```

数据库不得保存 Hub、Edge 或 Windows 私钥明文。

## 9. API 和权限

```text
GET/POST/PATCH/DELETE /v1/vpn/networks
GET/POST/PATCH/DELETE /v1/vpn/routes
GET/POST              /v1/vpn/peers
POST                  /v1/vpn/peers/{id}/revoke
POST                  /v1/vpn/peers/{id}/rotate-key
POST                  /v1/vpn/enrollments
POST                  /v1/vpn/client/enroll
GET                   /v1/vpn/client/config
GET                   /v1/vpn/sessions
GET                   /v1/vpn/diagnostics
```

权限：

```text
iot:vpn:query
iot:vpn:add
iot:vpn:edit
iot:vpn:delete
iot:vpn:enroll
iot:vpn:revoke
iot:vpn:diagnose
```

VPN 权限独立于设备控制权限，默认 deny。

## 10. 注册和收敛流程

### Windows Peer

```text
使用默认 iot-server / 自动生成 Edge LAN 映射
        -> 创建一次性 enrollment token
        -> Windows 本地生成密钥
        -> 提交 token + 公钥
        -> 服务端分配 /32、创建 Peer、更新 Hub
        -> 返回配置参数
        -> 导入官方 WireGuard 客户端
```

### Edge Peer

```text
Edge 通过现有身份认证连接 WSS
        -> 上报 VPN 能力和公钥
        -> 服务端读取桥接 LAN，自动生成等长虚拟映射
        -> 用户可调整虚拟网络号
        -> 服务端生成期望配置
        -> WSS/Protobuf 下发配置版本
        -> Edge 应用 WireGuard/路由/NAT
        -> 返回应用结果和实际状态
```

服务端分别收敛：

```text
数据库期望状态 -> Hub WireGuard
数据库期望状态 -> Edge WSS/Protobuf -> Edge Agent
实际状态       -> 诊断/审计
```

Peer 撤销时删除 Hub Peer、使 enrollment 失效，并下发 Edge 新配置。

## 11. 安全和许可证边界

- Hub 私钥只保存在 `vpn_network` 的受限服务端配置字段，不写入 API、Protobuf 或普通日志；
  Edge 和 Windows 私钥不写入业务数据库。
- enrollment token 只保存哈希、一次使用、短时有效。
- AllowedIPs、NAT 和访问规则由服务端生成。
- Edge 和 Hub 管理面默认不可通过 VPN 访问。
- 所有配置下载、撤销、轮换写入审计日志。
- Linux Hub 和 Edge 通过系统 WireGuard 接口运行；iot-engine 不集成 WireGuard GPL 源码、不静态链接 WireGuard 实现。
- Windows 首期只依赖用户独立安装的官方客户端；Windows Hub Runtime、驱动、签名和许可证不进入 MVP。
- 当前仓库已有 FFmpeg/x264 GPL 依赖和 FAAC LGPL 依赖；VPN 不能解决现有媒体依赖的许可证问题。

## 12. 固件约束

首期 Edge 固件只允许增加：

```text
kmod-wireguard + 内核依赖
Edge Agent VPN 控制逻辑
现有 firewall4/nftables 规则支持
```

禁止增加：

```text
wireguard-go
Go runtime
wg-quick
独立 VPN daemon
LuCI VPN 页面
```

必须在 canonical `immortalwrt-dtu` checkout 中实测：镜像增量、ipk 大小、RAM、CPU、启动时间、4G 重连和升级余量。任何指标超过产品预算都不能进入默认固件。

## 13. 实施阶段

### Phase 0：固件可行性

验证目标内核 WireGuard、nftables、NAT、尺寸和性能。

### Phase 1：服务端

实现数据库迁移、CIDR/地址分配、Hub 控制器、Peer/route API 和配置生成。

### Phase 2：Edge

实现 Protobuf 扩展、Edge 私钥、本地 WireGuard、路由、DNAT、MASQUERADE、回滚和状态上报。

### Phase 3：Windows/管理端

完成配置下载、enrollment、Peer 管理、权限和诊断页面；客户端使用官方 WireGuard。

### Phase 4：发布

完成密钥轮换、撤销、长稳测试、容量测试、许可证清单和固件 size gate。

## 14. MVP 验收

1. Edge 的 `192.168.10.0/24` 映射为 `172.31.10.0/24`。
2. Windows 导入 iot-engine 生成的标准配置。
3. Windows 与 Hub 通过 UDP WireGuard 建立握手。
4. Edge 通过 WSS/Protobuf 应用 WireGuard、路由和 NAT。
5. Windows 访问 `172.31.10.50:502`，实际到达 `192.168.10.50:502`。
6. Edge、Hub 重启后自动恢复配置。
7. Peer 撤销后访问立即失效。
8. 未授权网段和端口保持拒绝。
