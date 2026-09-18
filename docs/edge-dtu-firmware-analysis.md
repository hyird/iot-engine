# SUMMIT DTU 固件配置拆解

后续已完成关键转发路径的二进制控制流分析，确认 TCP 通道复用、Server 默认广播、可选客户端定向及串口多通道分发。见 [二进制分析报告](edge-dtu-binary-analysis.md)。本文保留最初页面分析阶段的证据边界。

## 样本与方法

- 分析日期：2026-09-17。
- 样本：`SUMMIT_DTU_IoT_R21.02-V2.7_D20260420.bin`，5,767,965 字节。
- SHA-256：`ff6f09aaeba6f237868f45526fc11806ffa03f94c91a576580f7a8cb16930529`。
- 镜像包含 U-Boot uImage 头，名称为 `MIPS OpenWrt Linux-5.4.238`。
- 在偏移 `1905228`（十进制）发现 SquashFS 4.0，使用 XZ 压缩，文件系统记录长度为 3,727,482 字节。
- 使用本地 `rdsquashfs` 静态解包，跳过设备、套接字、FIFO 和符号链接。没有执行固件程序、连接样本内的服务器或刷写设备。
- 分析产物位于 `build/summit-dtu-analysis/`。原始页面为 `rootfs/www/luci-static/resources/view/luci-app-dtu/overview.js`；按分号换行的阅读副本为 `dtu-overview.readable.js`，不是可替代原页面的格式化源码。

## 已确认的配置模型

固件没有在页面上直接使用“南向/北向”分区，而是提供一个串口与 A/B/C/D 四个网络通道。每个网络通道可选择连接方式和转发目标。因此，功能本质是通道之间的转发，不能限定成“一个串口对应一个 TCP 服务器”。

证据来源：`/etc/config/tas_dtu` 和 `/www/luci-static/resources/view/luci-app-dtu/overview.js`。

| 配置 | 字段 | 页面中的含义 |
| --- | --- | --- |
| 串口 | `UART uart1` | 波特率、数据位、校验位、停止位 |
| 网络通道 | `SOCKET socket_0` 至 `socket_3` | A、B、C、D 四个通道 |
| 连接角色 | `socket_type` | `0` Client，`1` Server，`2` 禁用 |
| 客户端工作模式 | `socket_dtu_mode` | `0` 禁用，`1` 普通 TCP/UDP，`2` MQTT，`3` DTU Cloud；A/B/C 还提供 `5` HTTP |
| 传输类型 | `socket_mode` | `0` TCP，`1` UDP |
| 地址及端口 | `socket_address`、`socket_port` | Client 目标地址及端口；Server 使用监听端口，页面显示 LAN 地址 |
| 本地端口 | `socket_local_port` | Client 本地端口，默认 `0` |
| 转发目标 | `socket_dtu_route` | `0` A、`1` B、`2` C、`3` D、`4` 串口 |

`socket_dtu_route` 在 Client 和 Server 模式下均可配置，是网络通道互相转发的直接证据。页面没有限制各通道的目标地址必须不同。

串口编码不是参数的字面值：`uart_bit=1` 表示 8 位，`uart_stop=0` 表示 1 位停止位，`uart_parity=0` 表示无校验。样本串口默认 9600/8/N/1。

## 按南向、北向理解

用户要求的南向是设备接入端，北向是接收透传数据的 TCP 服务器。可表达为：

```text
南向设备接入                          北向服务器连接
串口                         ─┐
TCP Client：节点连接设备      ─┼─ 节点内转发规则 ─ TCP Client → 目标服务器
TCP Server：设备连接节点     ─┘

平台：配置南向、北向与转发关系；不承担透传数据中转。
```

例如，南向用 A 通道接设备，北向用 B 通道连接目标服务器。页面允许将 A 的转发目标设成 B，将 B 的转发目标设成 A，表达双向转发关系。南向 A 可以是 Client，也可以是 Server。这是依据页面字段作出的配置解释，尚未通过目标固件运行验证。

样本出厂配置中，A 为普通 TCP Client，D 为 TCP Server、监听端口为 10000；A 与 D 的转发目标均为串口（`4`）。因此样本默认配置并不是 A 与 D 直接互转的示例。

## 其他相关配置

| 功能 | 字段 | 观察结果 |
| --- | --- | --- |
| 重连 | `socket_reconnect_delay_s` | 样本通道默认 3 秒；页面新值默认 1 秒 |
| 组帧等待 | `socket_recv_delay_ms` | 默认 100 毫秒 |
| 单包长度 | `socket_dtu_packet_len` | 样本为 10240；页面只读字段默认 2048，不能据此断言运行上限 |
| TCP 保活 | `socket_keepalive_en`、`socket_keepalive_idle_s`、`socket_keepalive_intvl_s`、`socket_keepalive_cnt` | 样本启用，参数 40 秒、10 秒、2 次 |
| 空闲断开 | `socket_recv_timeout_s` | `0` 表示禁用 |
| 注册包 | `socket_dtu_id_mode` 等 | 关闭、连接时发送、随数据发送、两者都发送；可选自定义、IMEI、ICCID |
| 应用心跳 | `socket_dtu_keepalive_time_s` 等 | 可配置间隔和 ASCII/HEX 内容，样本默认关闭 |
| Server 客户端策略 | `socket_client_keep` | 拒绝连接或踢出不活跃客户端 |
| Server 连接数量 | `socket_connect_max` | 样本值为 1；该字段未在本次提取的页面中提供编辑入口 |
| 转换 | `socket_tcphex_en`、`socket_tcpmodbus_en` | 可选 HEX、Modbus TCP/RTU 转换，样本关闭 |
| 串口缓存 | `socket_fifo_send_flag`、`cache_en` | 缓存开关与重启保留；页面明确异常掉电不保证保留 |

基础原样透传应与注册包、心跳及协议转换区分，不能默认附加这些内容或沿用第三方出厂服务器地址。

## TCP 共享的证据边界

- 可以确定：TCP 也可以充当南向；网络通道可指向另一个网络通道；Client/Server 是通道属性。
- 可以确定：页面每个网络通道只有一个 `socket_dtu_route`，不是目标列表。多个通道在配置层面可以选中同一个目标，但页面没有说明运行时合流和回包分发规则。
- 不能据此确定：多设备共享同一 TCP 会话时如何区分来源；北向回包是广播、定向、最近请求匹配还是其他策略；与主动协议采集共用同一设备连接时怎样仲裁。
- 串口必须由节点内一个运行对象实际持有；不能由采集和透传两个对象同时打开。TCP 监听器或已有连接的复用也需要明确连接归属和分发规则，不能仅因协议是 TCP 就认定可任意共享字节流。

实际执行文件 `/usr/sbin/at_cmd` 为 MIPS32r2 ELF，已无节表。静态字符串包含 `socket_recv_flag`、`socket_dtu_route`、`connect_num` 与客户端保留策略日志，佐证配置由程序使用，但不能替代完整控制流或实际通信验证。

## 对边缘节点设计的修正

应分别建模南向接入、北向连接、转发关系，界面属于边缘节点 DTU 配置，不应只作为串口调试功能的扩展。

1. 南向支持串口、TCP Client、TCP Server；串口配置物理参数，TCP 配置设备目标或监听地址。
2. 北向配置目标服务器、端口、连接参数。
3. 转发规则引用两端通道，明确双向关系；避免直接复制第三方编号配置。
4. 物理串口独占；TCP 的监听器、会话复用及多客户端回包策略在实现前单独确定。
5. 平台通过 HTTP 保存和下发配置；节点直接执行数据转发。无需为配置页面额外建立实时订阅。
6. 必须验证断线重连、配置持久化、平台离线时持续转发、背压、部分写入和配置替换时的资源清理。

本次完成固件静态配置分析，未修改平台或固件的 DTU 业务实现，未执行硬件透传验收。
