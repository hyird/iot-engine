# 采集协议层

平台直采的 Modbus、S7、SL651 共用协议定义、会话创建和运行接口。协议对象由实际接入连接的 Collector Worker 持有，协议层不执行数据库、Redis 或网络 I/O。

## 职责

| 位置 | 职责 |
| --- | --- |
| `service/features/collector/collector.config.h` | 不可变协议定义：连接能力、指令分组、任务传输标记、应答跟踪和调试归属策略 |
| `service/features/collector/collector.protocol.h` | 会话和工厂接口、连接能力校验、设备筛选、命令值校验与分组、整轮采集结果合并 |
| `service/features/collector/<protocol>/<protocol>.protocol.h` | 协议握手、帧解析、编解码、地址识别、协议状态转换，以及是否允许保留传输连接 |
| `service/features/collector/engine/engine.runtime.h` | 当前 Worker 的工厂注册与会话管理、按协议策略制定重载计划、调试归属筛选、指令和定时事件派发 |
| `service/features/collector/collector.runtime.h` | 执行协议动作、接入当前 Worker 的网络和消息队列、持久化与回执协调 |

## 统一契约

- `ProtocolDefinition` 是只读定义，不保存运行状态。指令准备和具体协议工厂使用同一份定义。
- `ProtocolSessionFactory::createSession` 统一检查协议、连接方式和快照，按链路、协议、目标筛选设备，再创建具体会话；工厂声明的命令、轮询能力须有对应会话接口。
- `ProtocolSession` 接收连接、报文、断开和持久化完成事件。命令与截止时间分别通过 `CommandCapabilitySession`、`DeadlineCapabilitySession` 接入。
- 协议只输出 `ProtocolAction`；网络发送、连接关闭、定时器、设备归属、解析结果发布和指令完成由运行层执行。解析结果继续使用 `ParsedDeviceMessage`，持久化确认与设备应答保持各自语义。
- `CommandLayout::WritableElements` 将每个写入点位组织成独立任务；`CompleteFunction` 保留完整功能的全部要素。平台直采和边缘命令准备使用同一分组函数。

## 保留的协议差异

- Modbus TCP 可以在重载时保留 TCP 连接并继承事务编号；Modbus RTU 和 S7 仍重建相关目标连接。重载计划同时检查变更前后的目标设备，任一设备不允许保留连接时选择重连。
- Modbus 的 Unit ID 与 RTU 地址、SL651 的上下行站号偏移，由所属协议工厂识别。调试筛选仍受链路、目标和已绑定设备限制；多设备歧义时不指定设备。
- SL651 原始上行片段只归链路，完整帧处理提供设备身份后才能进入设备视图；主动上报的回复不作为等待应答的请求。Modbus、S7 保留发送后的应答跟踪。
- 轮询队列、S7 握手、Modbus 写后回读、SL651 多包重组与应答模式仍由各自会话维护。统一接口不改变这些协议状态机。

## 扩展方式

新增协议时，在统一协议定义中声明实际支持的能力，并实现对应协议目录中的会话与工厂，在 Collector Worker 的装配入口注册。地址识别和连接重用策略通过工厂接口提供，不在 Collector Worker 或协议引擎中增加协议名称判断。

新增协议还需要独立完成管理 API 校验、数据库配置投影、设备展示、告警要素和边缘固件接入。现有 JSON 配置、Redis 快照与 Protobuf 字段属于已部署契约，本次统一运行接口不改变这些格式，也不代表新增协议已经接入。

## 验证

`tests/collector-protocol-test.cpp` 覆盖工厂筛选和拒绝非法连接、共享连接的调试归属、上下行站号、指令分组，以及三种现有协议的报文、读写、优先级、重载、迟到应答和实际 TCP 生命周期。后端修改执行 Release 构建及 CTest；实际设备和固件联调应单独记录。
