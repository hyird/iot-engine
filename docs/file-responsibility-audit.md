# 文件职责整改记录

## 目标与验收

按照 `AGENTS.md` 完成整个仓库的文件职责与内容一致性整改，范围包括 `service/`、`web/`、`clients/` 和相关构建、测试、文档。本文用于追踪证据和剩余工作，不以已有改动代替全仓验收。

验收必须覆盖真实调用、存储读写、实体使用、依赖方向与循环、Worker 状态和生命周期，以及接口、历史迁移、设备协议和已部署固件的兼容性。目录和字符串检查只能作为辅助证据。

## 已实施，等待最终验收

- Edge protocol::outbound 只装配显式传入的消息 UUID、时间戳和平台 ID，service/runtime 调用方负责生成运行输入。删除协议内 randomUuidV7Bytes 的线程局部随机引擎及 nowMs 时钟读取函数；终端 ID 也由 gateway runtime 生成。所有信封调用和测试同步参数，保留协议版本、会话 epoch/sequence 和二进制 UUID 格式；平台配置的可变存储仍待整改。

- Modbus/S7/SL651 parsedAction 不再调用 UUID 生成器，Collector runtime 在 PublishParsed 分支发布前分配 messageId；causationId 保持协议解析结果。SL651 改用会话内 publicationToken 关联待发布报告，接口、engine 和真实测试调用同步变更，Redis 发布成功后才回传完成令牌并生成协议确认。UUID 生成器本身和进程身份仍待后续整改。

- Webhook Header 校验归 AccessPayloadValidator.validateHeaders：解码键与字符串值、按大小写敏感的同名键保留最后值，再检查 ASCII token、CR/LF 和 15 个大小写不敏感保留字段。删除 service 中用于该校验的 DbQuery，创建/更新在原位置调用无 I/O 校验；持久化仍使用原 JSON，未改变存储格式。

- 开放接入新增 open_access.schema.h 的 AccessPayloadValidator，集中实际调用的字符串/整数/布尔/UUID、权限集合、事件类型、对象字段与 URL 校验。service 保持原调用顺序、权限和数据库业务检查，存储 JSON 数组解码仍留在 service；Header 校验已改为 schema 内无 I/O 实现，详见后续验证记录。

- LinkPayloadValidator 新增 Edge 端点参数、节点 ID 与状态检查，原方法调用位置、串口默认值、Asio IPv4 解析和错误语义保持不变；service 保留节点审批/能力、串口和网口可用性、监听地址与已查询接口匹配，以及持久化事务。实际业务集成增加 12 个 Edge 参数/节点检查顺序用例并确认无通道写入。

- 链路必填字段、端点/目标列表读取和 Collector TCP 配置校验迁入 link.schema.h 的 LinkPayloadValidator，IPv4 检查为私有方法；service 在原位置调用，保留错误码、校验顺序和重复目标冲突语义，数据库唯一性、权限和事务仍归 service。

- 告警规则/模板请求解析、UUID/字段检查、条件数组和阈值/离线/变化率/位索引校验归 alert.schema.h 的 AlertPayloadValidator。service 仅调用实际需要的解析入口，设备/协议存在性、名称唯一性与持久化留在 service；删除 validateConditionsForTest 条件编译转发，测试直接执行实际 schema 校验。

- protocol.service.h 的请求字段读取、结构校验和精确数字解析迁入 protocol.schema.h 的 ProtocolPayloadValidator，无 I/O 辅助函数保持私有，仅公开实际调用的字段解析与校验入口。service 保留权限、现有协议类型查询、名称唯一性、事务和更新后业务操作，并在原位置调用 schema 校验，保留更新时使用已有协议类型及原错误检查顺序。结构测试同步检查 schema 中的实现与 service 的实际引用。

- 协议导入导出拆分：页面私有 useProtocolImportExport 负责文件选择、读取、下载、提示与 DOM 清理；schema 的 parseProtocolImport 先校验整份文件；service 的 useProtocolConfigImport 负责全协议名称冲突、逐项保存、失败汇总和缓存刷新，loadProtocolExportData 返回去除标识与时间戳的导出数据；导入结果和导出类型归 types。保留文件格式、命名后缀、逐项失败继续、校验错误提示及下载文件名。

- 协议页面私有 useFilterableGroupOptions、寄存器/区域卡片 CSS Grid 及三个表单输入样式常量从 protocol.service.ts 移至 index.tsx，删除服务层导出与页面导入。Hook 内容、所有样式值与使用位置保持原样，service 不再依赖 CSSProperties/useMemo。

- 协议全量分页订阅从 protocol.api.ts 移至 protocol.service.ts 的 getAllProtocolConfigs，更新列表查询、配置选择和保存前查询三个调用方；API 保留单页 HTTP/SSE 接入。页大小、过滤参数、总页数回退计算、switchMap 订阅切换与合并顺序保持原样。设备命令状态按 64 个 ID 分批用于控制 HTTP 请求行长度，仍归 API 接入职责。

- GB28181 管理入口合并至 GbControlService.executeOperation：投影查询、请求校验、按现有 owner token 发布 Redis 控制指令、等待回执、超时和取消围栏统一归 service，私有辅助函数同步迁移。删除 runtime 中 GbControlHandler 声明与实现，更新 server.cpp；原 Collector 会话执行仍在 runtime。健康响应读取公开 SDK 适配状态，不引入 service 对 runtime 的引用；原有 owner 校验、15 秒期限、取消及回执删除顺序不变。

- VpnControlHandler 及仅由它使用的状态 JSON 编码从 vpn.runtime.h 移至 vpn.service.h，入口改为 VpnControlService.executeOperation，编码成为私有方法；server.cpp 注册同步更新。备用 Hub 配置和平台 ID 显式只读，控制服务不持有连接、计时器或会话；周期性协调、启停和 Worker 归属仍由 VpnHubRuntime 管理。RPC 操作名与状态响应字段不变。

- TelemetryProjectionHandler 与 CommandPreparationHandler 从 runtime 删除，操作入口分别合并到既有 TelemetryService.executeProjection 和 PreparationService.executeOperation；告警刷新入口归 alert.service.h 的 metadata::executeRefreshOperation。AccessOperationHandler 改为 AccessOperationService.executeOperation；同步 server.cpp 注册，RPC 操作名、参数、取消检查和错误响应保持不变，未保留转发类。清除 alert.runtime.h 重复 include。

- EdgeControlHandler 从 edge.runtime.h 移至 edge.service.h，改名为 EdgeControlService::executeOperation，更新 server.cpp 的 RPC 注册。配置快照与网络/固件/日志控制的请求校验、封包及下发归 service；迁移后与原处理类逐字比对，除类名和入口名外一致。删除全仓无引用的 kParsedStreamPrefix 与 kCommandResultStreamPrefix，未改变现行队列键。

- 协议配置 OFFSET 边界计算移除 std::stoll，复用严格整数解析得到的值；integerFieldValid 与边界计算共用完整消费、位数及范围检查，缺省 byteOffset 仍为 0。新增 protocol-offset-integration.ts，纳入默认隔离数据库测试入口，覆盖创建/更新、elements/responseElements、缺省值、临界范围、指数数字、错误类型和失败更新不写入。

- Collector 的 ProtocolRuntime 改名为 ProtocolSessionFactory，Modbus/S7/SL651 的 Runtime 改为 SessionFactory，注册表与实际调用、测试装配同步改名。核对三个实现仅公开协议能力与 createSession，实际连接生命周期仍由 ProtocolEngine 和 CollectorRuntime 驱动；未引入新抽象或兼容别名。

- Collector 的协议能力位、输入、动作、命令和解析后的命令参数归 collector.types.h；collector.protocol.h 保留会话接口、校验及编解码行为。engine.types.h 改为直接包含 collector.types.h，避免纯类型依赖协议实现。此次仅移动既有声明，字段、默认值、命名空间与处理逻辑不变。

- 模块结构检查增加直接声明 std::thread、std::jthread、asio::io_context、ruvia::EventLoopPool 的检测，覆盖此前 LinkService 自建出站线程。回归样例区分运行对象、线程 ID、注释和 Worker HTTP 客户端调用。该检查按声明文本工作，不宣称覆盖类型别名、间接创建或替代实际生命周期审查。

- 将通用整数解析 parseInt64 从 common/http.h 移入 utils/number.h，更新各层实际调用及直接 include，删除旧定义而不保留转发。函数体与原实现逐字比对一致，仍使用 from_chars 检查完整消费与溢出；HTTP 分页使用新归属。删除全仓无调用的 dbParams 和 HTTP 头中因此失效的数据库类型/容器 include。

- Collector 的一分钟命令发送期限和 `iot:v2:command:sent:` 去重认领归 CollectorCommandService.reserveTransmission，结果类型归 collector.types.h。该标记在发送前 SET NX EX、一天内阻止重复认领，实际设备结果不写入此键，因此按幂等认领锁保留，未新增实体。runtime 保留过期失败处理、重复消息确认、本地待执行状态及连接驱动；键、标记值、TTL、Redis 错误传播和原有发送顺序不变。

- 清除未被任何生产源码调用的旧队列契约：protocolTaskDepthKey、protocolInflightKey、sessionStateKey 及三个仅由其引用的前缀。删除 addGroupedBounded、acknowledgeGroupedAndDelete 和仅验证这两段废弃实现的模拟 Redis/测试注册；现行命令流、确认和队列操作未改写，旧固件所用终端和下发协议仍保留。

- EdgeNode 页面不再拼接终端地址或直接创建 WebSocket：openTerminalSocket 归 edge_node.api.ts，经所属 service 暴露；保留 disposed 检查、一次性票据编码、HTTP/HTTPS 对应 ws/wss、arraybuffer 与原有 Protobuf/UI 生命周期。EdgeNode 和设备分组树的组装由 API 移入各自 service；设备原始分组查询改称 getDeviceGroups，更新全部调用，未改变节点顺序、缺失父节点处理或计数字段。
- 前端结构检查新增页面不得直接创建 WebSocket、EventSource 或 fetch 请求的规则，配合已有 service/API 导入边界检查。

- 全仓引用核对后删除 `utils/redis.h` 中五个无调用模板：claimHash、completeInflightTask、eraseMatchingIfFieldValue、incrementWithExpiry，以及仅由废弃函数调用的 eraseHashIfFieldValue；未改写现存调用的 Redis 操作。
- Windows 连接服务的 IPC 命令入口由 handle 改为 executeCommand，更新原生服务入口和全部行为测试；平台来源校验 isPlatformOrigin 只用于连接服务，将其移入该实现的匿名命名空间，删除 HTTP 适配头的无关声明。命令字符串、IPC 格式、平台来源规则、服务状态与产品身份不变。

- LinkService 公网 IP 查询改用 `server.cpp` 注册的 Worker HTTP 客户端 `link-public-ip`，删除自建 Asio io_context、线程、OneShot 回调及缓存互斥锁。保留 ip.sb HTTP 查询、五分钟缓存、200 状态校验、空白裁剪与字符限制、查询失败返回旧缓存；响应读取限 64 KiB，使用框架连接及请求超时。

- 登录失败的次数上限和窗口时长从 `auth.entity.h` 移至 `auth.service.h` 的 `LoginRateLimiter`；Redis 键映射保留在 entity。
- 认证令牌签发与验签归 `AuthTokenService`，载荷归 `auth.types.h`，异常归 `auth.error.h`；删除原 `service/utils/jwt.h`，更新 middleware 和测试。签发者、受众、载荷字段、配置键、有效期和错误处理保持原有行为。
- `session.protocol.h` 更名为 `session.entity.h`，明确 Redis String 会话记录及过期索引映射的框架限制；删除旧文件并更新引用。
- Edge 会话过期的 Redis 操作归 `session.service.h`，live runtime 调用所属 service，保留调度与启停。
- Edge 下发的会话查询和所属 Worker 路由从 transport 移至 session service。对应消息格式、流键及无 I/O 解码归 `common/message.h`。
- 删除 Edge service 未使用的认证 middleware include，避免认证实现归位后形成 features 到 modules 的跨层依赖。
- 结构检查增加对实体、类型、schema、config 和 protocol 中协程等待的检查。原生 Redis 和自定义映射检查仍需继续完善。
- 报文日志从 `common/log.cpp` 移至 `features/packet_log/packet_log.transport.cpp`，配置与解析归 `packet_log.config.h`、级别及报文上下文归 `packet_log.types.h`。通用日志行输出归 `middleware/log.h`；更新全部调用及 CMake 输入，删除原 common 日志文件，保留线程局部 logger、独立队列、轮转及清理行为。
- gateway 的设备日志结果和日志级别结果持久化归 `GatewayService`，Redis 标量键映射归 `gateway.entity.h`。固件来源类型归 `gateway.types.h`，保留已有终端票据请求类型。
- 结构检查增加纯定义文件对 service/runtime/controller/transport 的传递依赖检查。
- 终端子组件的 service 集中处理票据消费、节点会话查询、终端登记和续期、输入队列、输出读取与写入、序号确认、失败关闭和释放。gateway runtime 只传递 `ConnectionIdentity`，保留 WebSocket 收发与等待调度；终端 Redis 标量映射归 `terminal.entity.h`，队列键归公共消息契约。
- 终端输入队列的容量检查、追加与首次过期设置改为单次 Lua，消除原先 RPUSH/EXPIRE/RPOP 的并发交错，保持满时拒绝新消息、已有消息顺序和原过期窗口。
- 用户明确确认保留现有 entity 规范，允许实际使用的自定义 Redis 存储映射。验收仍须证明实际存储对象和读写用途；业务策略、队列、锁、通知与通用键名工具不能伪装为实体。
- gateway 的认证缓存读取与 pending 状态处理归 `GatewayService::loadEnrollment`；`EnrollmentRecord` 实际保存并解析 nodeId/status，runtime 不再读取或解析 Redis 标量。首次缺失生成 pending 身份、后续缺失保留已有身份，维持原行为。
- GB28181 配置 Hash 的 11 个实际存储字段集中为 `ConfigProjectionRecord`，编解码归 entity，配置发布和读取归 `GbProjectionService`；runtime 调用 service。保留固定键、字段名和解析默认值，记录 Ruvia Redis ORM 自动添加前缀及实体 ID、无法映射原键的具体限制。投影快照类型归 `gb28181.types.h`。
- access 的 Webhook 已完成目标集合映射归 `DeliveryProgressRecord`；查询与结果消息/进度的原子写入归 `DeliveryService`，保留原 Lua、去重标记、七天 TTL 和唤醒顺序。投递 DTO、HTTP 响应类型归 types；流键契约从 transport 归 `common/message.h`。
- access 的实时数据 Hash 解码归 `LatestValuesRecord`，保留动态字段、配置集合、空值、BOOL 标准化与无效字段处理；数据读取、筛选排序和设备/图片/命令消息内容组装归 `DeliveryService`。新增身份覆盖防护、图片载荷及无效图片回退行为断言。
- access 的事件发布、批量管线发布和审计消息发布从 transport 归 service；`AccessOperationHandler` 的刷新和审计业务接入同归 service。telemetry、alert、messaging 更新为依赖 access service，transport 只保留 HTTP/TLS 适配，发布 Lua、容量、TTL 和唤醒语义保持不变。
- 前端持有 Axios 实例与拦截器的 `utils/http.ts`、持有共享 SSE 连接的 `utils/snapshot-request.ts` 迁入 `lib/`，同步入口、各模块 API 和会话测试引用；不保留旧转发文件，纯快照组合与 SSE 解析工具保留在 utils。未改变请求和刷新行为。
- Windows 公共目录定位函数同时服务安装所有者标识和加密会话状态，`stateDirectory` 改为 `productDataDirectory`，同步 IPC 和状态存储调用；保留 ProgramData/IotEngineVpn、owner.sid、state.dpapi 及加密身份。状态文件读写实现仍归服务侧。
- operations 的预期 Worker 数量改为从 Ruvia 注册的只读 `ServiceWorkerTopology` 获取，server 使用实际装配数量注册；移除 operations 对全局诊断访问器的依赖，并删除诊断对象中已无用途的 Worker 数量字段。未注册配置时仍返回原有不可用状态；common 中的诊断运行对象尚未完成迁移。
- GB28181 的控制所有权查询、取消标记、回复与消息确认、投影完成与消息确认归 `GbControlService`；runtime 调用 service，保留原子 Lua 和 TTL。全仓确认无调用后删除 `deleteOwnerIfMatches`，不保留占位兼容方法。
- Edge 投影的租约认领、续期、所有权验证、释放、失效流发现与恢复认领、清理和带租约校验的消息确认归 `EdgeProjectionService`。恢复描述类型归 `edge.types.h`；TaskScope、心跳调度、当前恢复租约集合和失效处理仍由 runtime 持有。保留原 Lua、认领规则、恢复扫描间隔和消息确认顺序，更新恢复集成测试的脚本来源。
- operations 的就绪结果与 Worker 诊断快照归 `operations.types.h`；alert 的规则、模板输入类型归 `alert.types.h`。这些类型不执行 I/O，查询、输入解析和业务校验继续由 service 承担，输出格式与校验语义保持不变。
- 结构检查补充新 Ruvia 的 `RUVIA_REDIS_ENTITY` 定义位置和所属组件使用检查；原生 Hash 方法和项目 Hash 辅助函数统一要求在 service 中调用，并检查所属 entity 文件。该检查不能证明 Lua、多键原子行为、自定义映射完整性或实际运行语义，仍需专项审查与行为验证。
- GB28181 的所有权租约认领、续期、释放集中到 `GbControlService`，消除两处重复续期实现；保持原 Lua、15 秒 Redis 租约、3 秒 Redis 超时、本地截止时间及失效处理。独立 Redis 测试覆盖不存在的租约不能续期、错误所有者不能认领、合法所有者替换、旧所有者不能续期/释放新租约及 TTL 范围。
- GB28181 控制结果缓存的 String 映射归 `ControlResultRecord`，读取及写入编码归 service。按第一个换行分隔状态和载荷，保留无分隔符时的 ok 默认值及载荷剩余换行；仍使用原子回复/确认 Lua，未改变缓存 TTL。
- GB28181 投影发布、修复 Lua、完成回执读取及发布结果解析归 `GbControlService`，发布结果类型归 types，排序 ID 校验归 protocol；runtime 保留停止信号、等待及重试期间的原始排序 ID。更新完整 Worker 集成测试的脚本来源，新增独立 Redis 用例验证发布去重、sent 标记丢失、已删除消息修复保留首次排序 ID，以及非法 ID 不产生新消息。

- 本轮将 GB28181 控制执行的原子认领和取消状态读取归入 `GbControlService`，认领结果归 `gb28181.types.h`。原 Lua 中所有者校验、取消检查、Redis 时间截止判断、NX 认领与 600 秒 TTL 保持原样；runtime 保留本地连接归属检查、SDK 执行和完成结果重试，未提前确认消息。新增 `tests/gb-control-claim-integration.ts` 使用独立 Redis 验证实际生产 Lua，覆盖过期、取消、所有者替换、重复执行排除及认领过期时间，测试通过（`build/responsibility-gb-claim-integration.log`）。

- 控制命令下发的所有者读取复用 `GbControlService::ownerTokenFor`，所有者检查与 XADD/EXPIRE 原子发布归 `publishControlCommand`；runtime 保留消息等待、取消及异常处理顺序。独立 Redis 用例验证归属替换和所有者缺失时不追加消息、成功时完整保留字段及 600 秒过期时间（`build/responsibility-gb-dispatch-integration.log`）。

- GB28181 设备与媒体流投影的无 I/O 编解码从 runtime 归入 `gb28181.protocol.h` 的 `projection_protocol`，全部实际调用同步使用新归属；保持消息版本、字段、时间格式、默认值及通道/记录数量上限。projector 测试增加消息字段、设备/通道名称和观众数还原，以及超量通道拒绝断言。

- GB28181 必填文本/整数、有限数值范围、云台动作校验及远端地址拆分从 runtime 归入 control protocol；功能启用状态检查归 `GbControlService`。全部调用同步更新，原错误码、错误信息与校验顺序保持不变。

- `web/utils/query.ts` 中的 QueryClient 单例移入唯一使用它的 TanStackQueryProvider，保持应用级单例及原缓存配置，不新增转发文件；utils 只保留查询键和 URL 参数生成。SSE 流读取、取消与 reader 生命周期移入 `web/lib/sse.ts`，纯事件解码及类型保留 `web/utils/sse.ts`，更新 snapshot-request 与实际集成测试引用。

- 前端播放器目录 `web/lib/gb28181` 改为 `web/lib/flv_player`：实际代码只适配 FLV 解复用、JV4/WebCodecs 解码与音视频渲染，不含 GB28181 API 或设备业务。保留原实现及第三方入口，更新页面引用并删除空旧目录。跨页面分页 DTO `PageParams`/`PaginatedResult` 归入 `web/types/pagination.ts`，全部类型调用同步更新；原 utils 文件保留分页参数校验，不转发旧类型。已核实 DeviceCard 在设备及 EdgeNode 页面复用，表单校验与权限 Hook 有实际跨页面用途，无需机械迁入单一页面。

- SIP 预览启动/停止结果从 SipServer 嵌套定义归入 `sip.types.h`，媒体服务器端口和能力信息从 ZlmSdk 嵌套定义归入 `media.types.h`；类型名明确为 SipPreviewStartResult、SipPreviewStopResult、MediaServerPorts、MediaCapabilities。更新 transport 实现、runtime 及监督器真实调用，不保留旧别名；成员、默认值及对外序列化字段不变。连接、SDK 句柄与回调生命周期仍归 transport。

- 已复核当前固定 Ruvia 的 `web/db/DbEntity.h`，DbColumnOptions 实际支持 enumName/defaultExpression。按 Schema 的 outbox_event 定义，补齐 modules/system/outbox 的 VARCHAR 长度和显式数据类型，并为模块及 messaging 各自实体补齐 payload、occurred_at、available_at、attempts 默认表达式；不改历史迁移或物理列。旧“框架不支持默认表达式”的结论不再适用，其他实体仍需逐项核对。

- 回查 auth 自定义 Redis 映射发现此前 LoginFailureCounter 只有键名方法，仍不够完整；现实体同时定义实际计数值和字符串解码，锁定判断使用该映射。畸形/空计数仍按原逻辑回退 0，限次和原子递增/首次过期策略仍归 service。新增实际 HTTP 登录与 Redis 状态集成测试验证存储值、第五次失败锁定、已锁定不递增/延长窗口，以及记录清除后的新窗口。

- GB28181 runtime 的 11 个纯响应组装函数迁入 protocol，通过公开 ModelOptions 接收内存分配器，不依赖 WebWorkerContext；原模型字段与字符串 JSON 保持不变。删除无实际调用的 playUrlsJson、previewStartJson、previewStopJson、actionJson 及未用类型别名。健康检查仍由 runtime 读取 SDK 状态，再调用纯转换。首次编译发现模型不接受裸 resource 指针，已改为公开 ModelOptions 构造后重验。

- 删除只承担 Redis 投影发布的 `edge.transport.h`：原子租约校验、队列容量限制、追加与 Worker 唤醒归 Edge service，流键和消息种类归 common/message 契约。gateway/runtime 和真实 Worker 隔离测试改为使用 service；Lua 提取测试同步来源。Worker 隔离测试新增实际 service 所需 edgenode-protocol/OpenSSL 依赖和 LARGE 编译配置，未保留转发头或旧实现。

- Edge 元数据动态 Redis Hash 的 Device 记录、节点/目录映射、键名与长度前缀编解码从 service 归 entity；实际写入与在线窗口换算仍归 service，保留原子整节点替换和全部存储字段。实体明确记录 ORM 无法直接表达动态设备字段及原键的限制。元数据单测直接包含 entity，并移除已经无用途的协议生成和 OpenSSL 链接依赖，验证该映射不需要业务 service。

- GB28181 投影在线/离线事件的 Redis 所有者查询归入 GbProjectionService；runtime 两个消费调用同步委托。在线必须匹配当前所有者，离线允许键缺失但不得覆盖新所有者的规则保持原样。

- 新增 `tests/entity-schema-audit.ts`，以本地独立数据库 information_schema 对照实际实体宏。首轮覆盖 125 个实体/1393 个字段，发现 489 项缺失默认表达式、33 项缺失命名类型、43 项长度差异，无缺失物理列或空值差异。按目录值补齐 23 个文件的 522 个字段映射，复跑为 0 项（`build/responsibility-entity-schema-audit-final.log`、`build/entity-schema-audit.json`）。不改物理表或迁移，保留现有字段与其他任务改动。审计不证明所有实体被实际使用，不比较已有默认表达式等价性、主键、精度或完整数据类型，这些仍是待验收项；初始化所用服务二进制来自最近成功构建，config/Schema 未变，因此只用于目录对照，不宣称新实体运行行为验证完成。

- 实体目录审计扩展为比较实际主键、显式/推断数据类型和 NUMERIC 精度。125 个实体/1393 个字段中新增发现 DeptEntity.sort_order 推断 BIGINT 与物理 INTEGER 不符，已补明确 kInteger 元数据，保留宽整数值容器与 API 表示以避免内部收窄截断。复查 0 差异（`build/responsibility-entity-types-audit-final.log`）。本次没有新增主键或精度差异；审计仍不验证已有默认表达式等价性和真实调用覆盖。SL651 缺失成员仍未补齐，未重复运行已知会失败的完整编译，部门改动的完整服务构建与业务回归仍待补齐。

- 默认表达式审计不再只检查存在性：从实体提取表达式，在独立数据库的事务临时表中按物理列类型解析，并使用 pg_get_expr 与真实列默认表达式比较；不插入数据，临时表提交即删除。125 个实体/1393 个字段、507 个默认表达式全部通过（`build/responsibility-entity-defaults-audit.log`、`build/entity-schema-audit.json`），补齐此前已有默认表达式未验证的缺口。实际查询使用覆盖和完整业务回归仍未完成。

- RPC 消费运行层的结果缓存查询、请求认领、取消检查及回执/通知/输入确认原子写入归 `RpcReceiptService`；结果 JSON 使用 `RpcReplyRecord` 实际映射，锁与取消信号不伪装实体。runtime 保留处理器选择、停止令牌、超时和重试循环，原查询顺序、错误信息、TTL 和 Lua 不变。相关 service 目标编译、access/architecture 两项测试及独立 Redis 回执集成通过（`build/responsibility-rpc-storage-targets.log`、`build/responsibility-rpc-storage-focused-ctest.log`、`build/responsibility-rpc-storage-integration.log`）；当前完整服务器回归仍待补齐。

## 未完成事项

- 链路 Edge 通道的串口/TCP 参数、节点 ID 格式和状态字段检查已归入 schema；依赖已查询节点/接口的业务约束留在 service，仍需最终复查。开放接入 validateHeaders 已从数据库查询改为 schema 内 JSON 遍历，34 组实际 API 与原 PostgreSQL 表达式对照已通过，仍纳入全仓最终复查。

- 协议导入导出职责已拆分，浏览器导入/同名处理/重复文件/校验失败已验证；导出落盘文件、窄屏可达性、全部协议及部分保存失败仍未覆盖，其余表单展示配置与类型归属继续审查。

- Edge、Telemetry、命令准备、告警刷新和 Access 的 RPC 操作入口已归 service；VPN 控制入口也已归 service；GB28181 管理入口已归 GbControlService，Collector 会话执行仍需最终复查。
- 全面清查 runtime 和 transport 中业务存储操作，重点包括 GB28181 会话操作、Edge 投影认领与恢复、access 的事件发布及审计处理；gateway 票据、认证、终端状态和 GB28181 配置已完成迁移，仍需最终复查。
- 清查所有 Redis 存储对象及其所属实体，检查自定义映射的实际使用、类型和存储语义；区分记录、缓存与队列、锁、通知。Collector 的发送认领现已归属 service；其余 Redis 原生命令仍需按实际用途逐项复查。
- 协议请求结构校验已迁入 schema；其他模块的 schema/service 边界仍需全仓复查。
- 检查其余共享目录、service/types/config 中职责混放和语义命名；确认没有无用实体、占位、转发文件或生成产物混入。`common/uuid.h` 的有状态生成器和进程身份归属待处理。
- 对前端和 Windows 客户端作内容与实际调用审查；现有结构测试通过不足以宣称完成。
- 完善原生 Redis 数据访问及自定义存储映射的结构检查和必要行为测试，不能用固定文件豁免掩盖问题。
- 完成对应 Release 构建、CTests、前端质量检查和生产构建、涉及页面的实际验证、Windows 客户端 CMake 验证及最终差异检查。

## 当前验证证据（2026-09-15）

- Edge 出站信封运行输入显式化：完整 Release 通过（build/responsibility-edge-envelope-release-complete.log），Edge 协议测试通过（build/responsibility-edge-envelope-focused.log），包括输入时间/消息 ID/平台 ID 原样装配及旧固件协议契约；全套 CTest 30/31（build/responsibility-edge-envelope-ctest.log），仅 GB28181 SIP 失败。实际 RPC、Edge owner/应用恢复集成通过（build/responsibility-edge-envelope-integration.log）。初次遗漏终端 UUID 辅助函数调用已同步；后续并发编译占用 server.obj，待进程结束重试通过。平台配置存储和公共 UUID 生成器的归属仍未完成。

- Modbus scale 回归失败已定位为测试夹具缺列：RuntimeRepositoryScaleDb 未提供新查询读取的 link/device debug_enabled，导致越界先于 scale 解析。补齐两个布尔列并使非预期异常输出真实原因，未改变生产解析规则。完整 Release 通过（build/responsibility-scale-fixture-release.log），collector-protocol 独立通过（build/responsibility-scale-fixture-focused.log），全套 CTest 恢复 30/31（build/responsibility-scale-fixture-ctest.log），仅 GB28181 SIP 失败。全仓 diff --check 本轮通过，此前其他改动中的行尾空格已不再出现。

- 公共 UUID 调用边界检查已加入 architecture-test，完整 Release 通过（build/responsibility-identity-boundary-release.log），新增检查通过。当前完整 CTest 29/31（build/responsibility-identity-boundary-ctest.log）：collector-protocol 报 runtime repository accepted a Modbus scale with trailing bytes，GB28181 IPv6 UDP catalog 未送达。前者需继续核对当前并行改动后的实现与测试，不能沿用历史 30/31。全仓 diff --check 仍发现 web/pages/iot/link/index.tsx:35 行尾空格，本轮未改该文件。

- 新增 collector-publication-integration.ts，独立测试库/API/Redis 与动态本地 TCP 端口中创建 SL651 M2 配置和设备。真实 Collector 接收报告，独立 XREAD 观察原始发布 UUIDv7，正常报告落库；临时把测试遥测流替换为错误类型后 700ms 内无确认，恢复原流并在同一连接重传后得到确认。集成通过（build/responsibility-collector-publication-verified.log），fixture 已退出。最初测试补齐 storagePolicy，随后纠正把派生 UUIDv8 误作原始消息 ID 的断言及 Bun XREAD 返回对象格式，业务代码未因此修改。该用例覆盖实际单包 M2 发布失败恢复，未替代全部协议、多包及进程重启验收。

- 协议发布身份分离：完整 Release 通过（build/responsibility-protocol-publication-release-complete.log），CTest 30/31（build/responsibility-protocol-publication-ctest-final.log），仅 GB28181 SIP 测试失败。collector-protocol 实际执行协议引擎测试通过，包括 SL651 多包图像、工作模式、独立报告发布完成及新增未知/重复完成令牌不确认；协议解析结果不再自行分配 UUID。初次 Redis 编解码往返测试缺少运行阶段 ID，已在测试转换前填入固定消息 ID 后重跑通过。生产 Redis 发布故障路径与全部协议的现场端到端回归未在本轮新增执行，不能据本次单元测试宣称 UUID 专项完成。

- Webhook Header 无 I/O 校验：完整 Release 通过（build/responsibility-webhook-headers-release.log），CTest 30/31（build/responsibility-webhook-headers-ctest.log），仅 GB28181 IPv6 UDP catalog 未送达。业务集成通过（build/responsibility-webhook-headers-integration-complete.log）：34 组 Header 与原 PostgreSQL 正则/jsonb_each 表达式对照一致，覆盖 15 个保留名的大写形式、转义键、重复键最后值、CR/LF、字面反斜杠、非字符串及合法 Unicode/制表符值，并逐组验证存储内容或拒绝后原值保持。最初两次测试端参数编码失败，改为文本参数再显式转 jsonb 后重跑通过；业务实现未因测试失败修改语义。

- 开放接入 schema：完整 Release 通过（build/responsibility-open-schema-release.log）；业务集成通过（build/responsibility-open-schema-integration.log），新增 9 个 Webhook 非法部分更新用例，并通过后续原配置/类型查询断言。CTest 30/31（build/responsibility-open-schema-ctest.log）；本轮完整套件的 gb28181-sip 在 ZLMediaKit 服务器绑定阶段失败；独立重跑成功越过绑定阶段，但仍因 IPv6 UDP catalog 未送达失败（build/responsibility-open-schema-gb-retry.log），两次失败阶段分别记录。未修改 GB 组件或系统端口配置。

- 链路 Edge schema：完整 Release 通过（build/responsibility-link-edge-schema-release.log），CTest 30/31（build/responsibility-link-edge-schema-ctest.log），仅原有 GB28181 IPv6 UDP 接收失败。实际业务集成通过（build/responsibility-link-edge-schema-integration-final.log），新增 12 个串口/TCP 参数及合法端点进入节点审批检查的用例，无通道写入。首次集成因测试使用内部字段名失败，按 DTO 已有 JSON 名称修正后重跑通过，未改变公开 API。

- 链路 schema 迁移：完整 Release 通过（build/responsibility-link-schema-release-final.log），CTest 30/31（build/responsibility-link-schema-ctest-final.log），仅 GB28181 IPv6 UDP catalog 接收失败。初次 link-service 结构测试因只读取旧 service 文件失败，已同步读取 schema 并验证 service 调用，重跑通过。实际 business-orm 集成通过（build/responsibility-link-schema-integration.log），覆盖链路创建/列表/详情/更新及端点数字 JSON、设备连接编辑保持共享链路，同时开放接入与协议等既有业务回归通过。

- 告警 schema 归位：首次构建发现缺少 set 头已修复；同时工作区 Edge 分块调用曾缺少定义，定义补齐后完整 Release 通过（build/responsibility-alert-schema-release-final.log）。告警实际条件与结构检查通过，全量 CTest 30/31（build/responsibility-alert-schema-ctest.log），仅 IPv6 UDP 失败；隔离告警 ORM 集成通过（build/responsibility-alert-schema-integration.log），覆盖阈值、变化率、位条件、恢复、回执、JSON 类型与变化存储。git diff --check 通过。

- 后端协议 schema 归位：Release 通过（build/responsibility-protocol-schema-release.log），CTest 30/31（build/responsibility-protocol-schema-ctest.log），仅 GB28181 IPv6 UDP 失败。业务 ORM 与 SL651 OFFSET 隔离集成通过（build/responsibility-protocol-schema-integration.log），实际验证 Modbus 创建/部分更新、指数数字标准化、精确小数越界拒绝，以及 OFFSET 34 组创建/更新输入和失败不写入。git diff --check 通过。

- 协议浏览器验证：使用隔离 PostgreSQL/Redis/API 与生产前端构建，在 1280×900 视口实际打开 SL651 配置、要素编辑和分组筛选，验证无匹配收起、匹配恢复、取消不保存。同名 JSON 连续导入两次，分别生成“(导入)”与“(导入) 2”，页面实时显示；第二项 null 的文件提示“第 2 项必须是对象”，数据库仍只有原记录及两条成功导入记录（build/responsibility-protocol-browser-evidence.log）。导出页面提示“已导出 1 条配置”，但下载事件等待超时，未验证落盘文件。临时视口已恢复、浏览器页已关闭、隔离进程退出为 0。319px 默认面板存在内容截断，本轮未证明窄屏右侧功能可达，也未覆盖全部协议或导入保存部分失败。

- 协议导入导出拆分：锁定 Bun 的 typecheck、lint、生产构建通过，导入校验/前端结构/SL651 配置测试 11/11（build/responsibility-protocol-transfer-*.log）；修改文件 Biome 格式检查及 git diff --check 通过。新增测试覆盖完整文件校验、顺序、错误条目位置、非法 JSON/空内容/协议不匹配；未新增文件选择、下载或部分保存失败的浏览器证据。

- 协议私有 UI 归位：锁定 Bun 的 typecheck、lint、生产构建、修改文件 Biome 格式检查通过；前端结构/SL651 配置测试 9/9（build/responsibility-protocol-ui-*.log）；git diff --check 通过。此次仅改变定义位置，未改变样式值与布局，没有新增浏览器视觉验收证据。

- 前端协议分页归位：锁定 Bun 的 typecheck、lint、生产构建、两个修改文件 Biome 格式检查及 git diff --check 通过（build/responsibility-protocol-pagination-*.log）。SnapshotStream、前端结构与 SL651 配置测试 14/14（build/responsibility-protocol-pagination-tests-final.log）；初次测试命令未使用 ./ 路径导致无匹配，已纠正并实际执行。此次只移动查询组装，未改页面布局；未新增多页真实浏览器验收。

- GB28181 管理入口归位：完整 Release 通过（build/responsibility-gb-control-service-release-complete.log）；projector 测试同步补齐媒体适配实现与 ZLMediaKit/日志/加密依赖，首次缺头文件及后续链接失败日志保留。CTest 30/31（build/responsibility-gb-control-service-ctest.log），仍为 IPv6 UDP 未送达。GB28181 实际多 Worker 集成通过（build/responsibility-gb-control-service-integration-dynamic-port.log）：HTTP 改名到原连接 Worker、持久化后回复、错误归属令牌拒绝、投影乱序/重试、回执重建、租约丢失清理。
- GB28181 集成夹具原固定 UDP 55133 落入本机 Windows 保留区间 55076–55175，导致启动失败；改由操作系统分配 UDP 端口并验证 TCP 同端口可绑定，通过 GB_TEST_SIP_PORT 传给模拟设备，结束后恢复环境变量。未修改系统网络配置；该修复与 IPv6 收包失败是不同问题。git diff --check 通过。

- VPN 控制服务归位：Release 通过（build/responsibility-vpn-control-release.log），CTest 30/31（build/responsibility-vpn-control-ctest.log），剩余 GB28181 IPv6 UDP 失败。独立 VPN 桌面 API 集成全部通过（build/responsibility-vpn-control-integration.log），覆盖在线归属、撤销地址复用、并发复用、迟到断开隔离、重复重连、注册令牌原子消费、权限与路由变更以及禁用网络和幂等撤销；这不是实际 WireGuard 系统隧道或固件配置验收。git diff --check 通过。

- Telemetry/命令准备/告警刷新/Access 操作入口归位：Release 通过（build/responsibility-rpc-operations-release.log）；CTest 30/31（build/responsibility-rpc-operations-ctest.log），仍为 gb28181-sip IPv6 UDP 失败。系统 ORM、实时查询及 SL651 OFFSET 隔离测试通过（build/responsibility-rpc-operations-system.log）；RPC 锁隔离、订阅与 Worker 消息、Edge 崩溃恢复/租约丢失就绪检测通过（build/responsibility-rpc-operations-integration.log）。git diff --check 通过。

- Edge 控制业务归位：Release 通过（build/responsibility-edge-control-service-release.log），CTest 30/31（build/responsibility-edge-control-service-ctest.log），剩余 GB28181 IPv6 UDP 失败。隔离 RPC、Edge owner 恢复/租约排他/围栏及拒绝 SCAN 后恢复就绪测试通过（build/responsibility-edge-control-service-rpc.log）。该集成覆盖装配与消息恢复，不覆盖真实固件网络配置或升级；控制处理函数除命名外逐字一致。git diff --check 通过。

- 协议整数解析修复：Release 通过（build/responsibility-protocol-integers-release.log），CTest 30/31（build/responsibility-protocol-integers-ctest.log），protocol-service 已恢复通过，剩余 gb28181-sip IPv6 UDP 未送达。实际 PostgreSQL/Redis/API 隔离集成通过（build/responsibility-protocol-integers-integration.log），34 组 OFFSET 输入分别验证创建和更新，覆盖两个元素字段；git diff --check 通过。

- 协议会话工厂命名整改：完整 Release 通过（build/responsibility-session-factory-release.log），Collector 协议/状态及 architecture 检查通过。全量 CTest 首次 27/31（build/responsibility-session-factory-ctest.log），三个 GB28181 用例出现媒体服务器绑定失败；单独复测 2/3（build/responsibility-session-factory-media-recheck.log），media 与 media-proxy 通过，sip 仍为 IPv6 UDP 未送达，绑定失败未持续复现。protocol-service 整数解析检查仍未解决；不将分次结果表述为一次全量通过。git diff --check 通过。

- Collector 类型归位：完整 Release 重试通过（build/responsibility-collector-types-release-retry.log）；首次构建写入 server.obj 被拒绝访问，当时有其他编译/链接进程，保留失败日志，不将其认定为源码错误。CTest 29/31（build/responsibility-collector-types-ctest.log），Collector 与结构检查通过，protocol-service 整数解析检查及 gb28181-sip IPv6 UDP 仍失败；git diff --check 通过。

- 新模块线程/事件循环检查经 MSVC Release 编译及 architecture CTest 验证通过（`build/responsibility-module-executors-build-final.log`、`build/responsibility-module-executors-test.log`）。首次构建发现当前 MSVC 不提供 regex_constants::multiline，已改为逐行检测并保留原失败日志。全量 Release 通过（`build/responsibility-module-executors-release.log`），CTest 29/31（`build/responsibility-module-executors-ctest.log`），仍失败于 SL651 std::stoll 检查及 IPv6 UDP；本轮未放宽这两项检查。

- 整数解析归位的首轮构建发现 command.service.h 三处简写 common::parseInt64，已补齐（`build/responsibility-numeric-utility-release.log`）；源码核对确认无旧调用、无 dbParams 残留，函数体与 HEAD 原定义一致，差异检查通过。修复后完整 Release 通过（`build/responsibility-numeric-utility-release-final.log`）；CTest 29/31（`build/responsibility-numeric-utility-ctest.log`），原有 SL651 解析检查和 IPv6 UDP 用例仍失败，其余通过。

- Collector 发送认领归位后完整 Release 通过（`build/responsibility-command-reservation-release.log`）；collector-state 行为检查通过（`build/responsibility-command-reservation-focused.log`）：60000ms 到期且不请求 Redis、59999ms 可认领、完整 SET/NX/EX 参数保持、重复返回与 Redis 错误拒绝。此测试注入 Redis 回执，不宣称已完成真实设备发送或重启恢复集成。全量 CTest 29/31（`build/responsibility-command-reservation-ctest.log`），仍失败于同期 SL651 std::stoll 检查及 IPv6 UDP 目录报文；未跳过失败项。差异检查通过。

- 旧队列遗留清理后完整 Release 通过（`build/responsibility-retired-queue-release.log`）；全量 CTest 29/31（`build/responsibility-retired-queue-ctest.log`），失败为同期 SL651 std::stoll 检查和 GB28181 IPv6 UDP 目录报文未送达。SIP 单独复跑仍失败（`build/responsibility-retired-queue-sip-recheck.log`）。独立 Python UDP 回环对照不使用项目代码：IPv4 收到 14 字节，IPv6 ::1 发出 14 字节后接收超时（`build/responsibility-udp-loopback-probe.log`）；该现象说明失败不限于 SIP，尚未确定具体系统网络原因，未修改网络/防火墙或跳过用例。差异检查通过。

- 通过 Codex 浏览器在一次性本地 PostgreSQL/Redis/API 上补做生产前端页面验证（`build/page-review-fixture.ts`、`build/responsibility-page-review-fixture.log`）：登录成功；设备页面展示“审查设备父组 (0)”和“审查设备子组 (0)”，选择子组后按钮显示对应名称；EdgeNode 页面展示父子组，表单新建“浏览器新增节点组”成功，未刷新页面即在分组查询面板中看到新组。截图确认父子缩进和面板实际可见。验证仅覆盖零设备计数、分组展示/选择/新增及订阅更新，不覆盖非零汇总、真实固件或终端连接。浏览器页已关闭，回查 55124/55122/55459/56459 无监听，测试环境已退出。

- 页面/API 职责整改后，锁定 Bun 的类型检查、lint、生产构建及五个修改文件的 Biome 格式检查通过（`build/responsibility-page-api-typecheck.log`、`build/responsibility-page-api-lint.log`、`build/responsibility-page-api-build.log`、`build/responsibility-page-api-format.log`）。11 项相关测试通过（`build/responsibility-page-api-tests.log`），新增真实本地 WebSocket 上游验证从 service 调用创建连接、带特殊字符的票据完整解码及二进制帧接收；分组视图和终端 Protobuf 原有测试通过。本批未做浏览器页面或真实设备终端验证，也未借这些测试声称分组树所有边界已完整覆盖。

- 未使用 Redis 模板清理后，后端完整 Release 通过（`build/responsibility-unused-redis-release.log`），全量 CTest 30/31 通过，仍由同期 SL651 的 std::stoll 触发 protocol-service 失败（`build/responsibility-unused-redis-ctest.log`）；未跳过或放宽检查。Windows 客户端通过统一 CMake 入口完成 Release、5/5 测试、WinUI 控件行为和打包文件验证（`build/responsibility-windows-command-build.log`）；没有安装或发布制品。实际调用复查：WinUI main 注入 pipeRequest 至 ConnectionController，UI 未引入服务和 WireGuard 实现；共享 files/data_protection/json 分别实现基础文件、DPAPI 与 JSON 操作，连接状态仍由 service 持有。差异检查通过。

- 公网 IP 本地 HTTP 行为测试通过（`build/responsibility-link-public-ip-test.log`）：IPv4、分块 IPv6、非 200、无效/空正文、连接中断、响应超限、同实例缓存不重复请求及异步完成仍在原 Worker。测试启动独立回环 HTTP 上游和两 Worker 的测试 App，不访问 ip.sb；未等待五分钟验证缓存过期后的失败回退。两轮完整 Release 通过（`build/responsibility-link-http-release.log`、`build/responsibility-link-http-tests-build.log`）。全量 CTest 30/31 通过（`build/responsibility-link-http-ctest.log`）；唯一失败是 `protocol-service` 检查发现同期 SL651 配置校验新增 `std::stoll`（`protocol.service.h` 的 byteOffset/length 计算）。已确认“补全SL651协议M1至M4”任务仍在本工作区活动，本批未改写该校验或放宽测试；不能宣称全量验收通过。差异检查通过。

- 诊断迁移的保留策略已落实：计数 `updated_at` 由活跃 Worker 的采集周期刷新，先于 Redis 指标查询；清理只针对其他进程超过保留期未活跃的记录，排除本进程所有 Worker。默认 30 天、每小时执行，`OUTBOX_REPLAY_COUNTER_RETENTION_DAYS` 可设 1–3650 天，配置示例同步更新；Policy 默认值归 messaging.config。完整 Release 与 CTest 30/30 通过（`build/responsibility-counter-retention-release-final.log`、`build/responsibility-counter-retention-ctest.log`）。保留/清理、新库与升级、漂移/失败恢复、重启归零和事务故障/并发集成全部通过（`build/responsibility-counter-retention-integration.log`），包括无新重放时活跃记录续期、过期外部进程记录删除和较新记录保留。诊断运行状态迁出、跨层计数交接及配套生命周期验收完成；全仓其余未完成项仍列于上方。

- 新增真实服务器迁移验收模式 `Run-SystemOrmIntegration.ps1 -ReplayCounterMigration` 与分阶段测试 `replay-counter-migration-integration.ts`。在同一隔离数据库上多次停止/启动本次二进制，验证新表列与默认值、带已有计数的进程重启归零、0046 状态升级到 0047、重复启动不改迁移执行记录、篡改校验和启动失败且不自动修复、DDL 名称冲突失败且不写成功记录、移除冲突后恢复及再次幂等启动。各阶段比较历史迁移记录和业务哨兵数据，均保持不变。完整流程通过（`build/responsibility-replay-counter-migration-final.log`）；测试先前对定长 checksum 空格填充的比较错误已修正。仅操作本运行器创建的隔离实例，未连接部署数据库。

- 公共诊断运行状态已迁出：`RuntimeDiagnostics` 归 observability.service，ComponentState/ComponentStatus/AlertStatus 归 observability.types，更新生命周期装配、messaging、Edge 及测试引用；删除 thread_local 诊断指针、设置/查找函数与启动设置代码。Worker 隔离测试改为创建带显式诊断依赖的 Edge 组件，验证各 Worker 的独立实例信息。完整 Release 和 CTest 30/30 通过（`build/responsibility-observability-location-release.log`、`build/responsibility-observability-location-ctest.log`）；新二进制的重放计数故障/并发集成和完整 RPC/Edge 恢复集成均通过（`build/responsibility-observability-location-metrics.log`、`build/responsibility-observability-location-rpc.log`），包含租约失效 readiness。重放计数的新迁移升级、重启归零与旧实例记录清理仍待完成。

- 重放计数数据库交接已实现：追加迁移 0047（未改历史迁移内容），modules 与 features 各自定义实际使用的 OutboxReplayCounterEntity；同一事务执行死信重放与计数 upsert，后台按本进程/Worker 读取。Worker 编号在启动阶段写入所属 Worker 的拓扑元数据。首次编译 API 调用错误已修正；第二次构建遇到其他编译占用对象文件，重试后完整 Release 通过（`build/responsibility-replay-counter-release-retry.log`），CTest 30/30（`build/responsibility-replay-counter-ctest.log`）。新二进制通过原重放行为测试及新增计数写入失败回滚测试，实体审计 127 个实体、1399 列、零差异（`build/responsibility-replay-counter-integration.log`）。完整迁移验收与重启/清理尚未完成，不能作为发布结论。

- 新增 `tests/outbox-replay-metrics-integration.ts` 在独立数据库及 Redis 上验证重放计数的真实故障行为。数据库失败通过只匹配测试事件 ID 的触发器注入，Redis 故障窗口通过 `CLIENT PAUSE WRITE` 模拟，均有清理。成功、重复、数据库失败、Redis 暂停恢复和并发幂等五类检查通过（`build/responsibility-outbox-replay-metrics-concurrent.log`）。本批未修改生产计数实现或数据库迁移；这是公共诊断状态整改的行为基线，不表示该整改完成。

- 消息、传输及运行层的公开数据定义归位：StreamWake/StreamPublication 移入 messaging.types；WireGuard Peer/RuntimeStatus 移入 wireguard.types，HubConfig 移入 wireguard.config；防火墙 ClientAccess/Result 移入 firewall.types；TCP LinkState 移入 tcp.types；WebhookUrl 和 DeviceRouteSnapshot 分别移入现有 access.types、device.types。字段、默认值和命名空间未变，原 transport/runtime 引用真实定义，没有兼容副本。Netlink 报文、内核回复等私有系统适配类型保留在 transport。完整 Release 与 CTest 30/30 通过（`build/responsibility-transport-types-release.log`、`build/responsibility-transport-types-ctest.log`）；未因纯定义移动重复全部外部集成，本批未覆盖 Linux 实机 WireGuard/nftables。

- 最新完整 Release 构建通过（`build/responsibility-post-mapping-release.log`），完整 CTest 30/30 通过（`build/responsibility-post-mapping-ctest.log`，新增 collector-state 后共 30 项）。本次 SIP 也通过，但未认定此前间歇性 IPv6/SDK 绑定失败的根因已经消除。
- 使用本次新服务器二进制，系统 ORM、实时查询与权限撤销、登录失败限流集成通过，实体与真实数据库审计为 125 个实体、1393 列、零差异（`build/responsibility-post-mapping-orm-integration.log`）。RPC、跨 Worker 通知、数据库锁隔离、幂等、写入失败回滚和完整 Edge 恢复/租约失效 readiness 测试通过（`build/responsibility-post-mapping-rpc-integration.log`）。补齐了此前因 SL651 缺失定义而未完成的实体元数据、RPC、Collector、网关映射及 Edge 诊断注入的整体编译与相关集成覆盖。
- GB28181 集成首次在完成回执恢复检查后异常退出（`build/responsibility-post-mapping-gb-integration.log`），未记录明确断言信息；对应 PostgreSQL 日志有客户端异常断开和进程异常终止，根因未定。确认旧实例退出后，新的独立环境完整复验通过（`build/responsibility-post-mapping-gb-integration-retry.log`），覆盖注册提交顺序、连接所有者路由、错误 Collector 拒绝、DB 阻塞顺序、迟到投影、回执重建和租约失效后的 SIP 拒绝及持久化离线。本次通过不抹去首次失败证据。

- 第一批改动的 Windows Release 构建成功，日志：`build/responsibility-release.log`。之后仍有代码变更，不能代表最终状态。
- 第一轮 CTest：25/29 通过，日志：`build/responsibility-ctest-initial.log`。Edge service 测试的旧源文件位置断言已更新，等待重建复验；三个 GB28181 媒体相关测试报告 SDK 绑定服务器失败，原因尚未查明。
- 第二轮 Windows Release 构建成功，日志：`build/responsibility-release-2.log`。第二轮 CTest 为 28/29，日志：`build/responsibility-ctest-2.log`，Edge 及两个媒体测试通过。剩余 `gb28181-sip` 在 IPv6 UDP 目录查询处失败，单独复跑仍失败（`build/responsibility-sip-recheck.log`）；尚未完成原因定位。
- 删除无用认证 include 后，现有 architecture 可执行文件对当前源码的检查通过；新增检查仍须重建验证。
- 使用锁定的 Bun 1.3.14 运行前端及 Windows 客户端结构测试，8/8 通过，日志：`build/responsibility-frontend-client-audit.log`。
- 本地 PATH 中 Bun 为 1.4.0；本任务使用 CMakeCache 指定的 `build/bun-1.3.14/bun-windows-x64/bun.exe`。
- Redis 集成测试指定的 `127.0.0.1:56439` 未监听，Edge 会话集成测试尚未运行；不得改用生产 Redis。
- 报文日志与 gateway Release 构建通过；补全 gateway 已有类型后的最终重建也通过，日志为 `build/responsibility-logging-release.log`、`build/responsibility-gateway-release.log`、`build/responsibility-gateway-release-final.log`。
- 本轮 CTest 28/29，日志 `build/responsibility-gateway-ctest.log`。新增结构检查、报文日志行为测试及相关媒体测试通过，SIP IPv6 UDP 目录查询仍失败。
- 本轮锁定 Bun 安装检查无依赖改动，前端类型检查、lint 和生产构建通过，日志分别为 `build/responsibility-bun-install.log`、`build/responsibility-web-typecheck.log`、`build/responsibility-web-lint.log`、`build/responsibility-web-build.log`。未修改页面布局，尚未进行全仓页面内容验收。
- 终端状态测试通过独立启动的本地 Redis 验证生产 Lua，6/6 通过（43 个断言），日志 `build/responsibility-terminal-state-final.log`。覆盖空闲续期、所有者替换、历史协议无序号状态、失败关闭和队列溢出；不会使用生产 Redis。
- 终端状态测试文件按项目 Biome 配置通过 stdin 格式化并验证；根配置只包括 `web/**/*`，直接传入 tests 路径会被忽略。
- 终端迁移第一轮及队列原子化后的最终 Release 构建通过（`build/responsibility-terminal-release.log`、`build/responsibility-terminal-release-final.log`）。当前 CTest 28/29（`build/responsibility-terminal-ctest-final.log`），Edge 测试通过，SIP 本次在 SDK 绑定阶段失败；前几次也出现 IPv6 目录查询失败，两个现象均需定位，不能将其统称为已确认的环境问题。
- 认证缓存整改最终 Release 构建通过，CTests 28/29，包含认证记录解析测试；日志 `build/responsibility-enrollment-release-final.log`、`build/responsibility-enrollment-ctest.log`。
- SIP IPv6 测试现已分别断言数据报送达、SIP 解析和 Catalog 内容，实际失败在数据报未送达。独立 .NET UDP socket 对照验证：IPv4 回环成功接收 `iot-loopback-check`，IPv6 `::1` 发送成功但接收超时。此项复现不依赖 iot-engine、Ruvia 或 ZLMediaKit，表明当前主机存在 IPv6 回环阻碍；尚未在其他环境完成 SIP IPv6 验证，未跳过测试、修改系统网络或宣称完整通过。
- 系统 TCP/UDP 排除端口段不同，偶发共享 TCP/UDP 端口预留失败和 SDK 绑定失败仍需分别确认；当前只读取系统配置，未更改排除范围。
- GB28181 配置职责拆分及快照类型归位后的 Release 构建通过（`build/responsibility-gb-config-release-final.log`）；CTest 28/29（`build/responsibility-gb-config-ctest.log`），唯一失败仍为 SIP IPv6 UDP 数据报未送达，未跳过该测试。
- access 投递进度整改的最终 Release 构建通过（`build/responsibility-access-progress-release-final.log`），结构、access 与 Webhook 测试通过；完整 CTest 28/29（`build/responsibility-access-progress-ctest.log`），剩余 `gb28181-sip`。首次编译暴露的流键声明依赖已通过将契约归入 common 修复，未添加 transport 反向依赖。
- access 实时数据映射和载荷组装迁移的 Release 构建通过（`build/responsibility-access-payload-release.log`），新增载荷行为断言通过；完整 CTest 28/29（`build/responsibility-access-payload-ctest.log`），唯一失败仍为 SIP IPv6 数据报未送达。结构检查与 `git diff --check` 通过。
- access 事件和审计发布迁移的最终 Release 构建通过（`build/responsibility-access-publication-release-final.log`）；CTest 28/29（`build/responsibility-access-publication-ctest.log`），唯一失败仍为 SIP IPv6 数据报未送达。结构检查及差异空白检查通过。首次构建读到未完成更新的 include，已补齐全部调用方后重新验证。
- Windows 客户端通过既有 CMake 的 `windows-native-tests` 入口完成原生构建和 5/5 CTest（`build/responsibility-windows-native.log`）；尚未完成 WinUI/安装包全流程与全部客户端内容审查。
- 前端请求层迁移后，锁定 Bun 1.3.14 的类型检查、lint、生产构建、14 个修改文件 Biome 格式检查通过（`build/responsibility-http-*-final.log`）；前端架构、快照流及会话刷新测试通过（`build/responsibility-http-tests.log`）。首次检查遗漏的快照请求相对引用已修复后重验。
- Windows 改名后的 CMake `windows-client` 完整构建、5/5 原生测试、打包 WinUI 登录/设备/选择/应用/搜索/注销自测和文件校验通过（`build/responsibility-windows-client-final.log`）；`windows-installer` 生成安装包成功（`build/responsibility-windows-installer.log`），未安装或发布。客户端结构测试 3/3 通过（`build/responsibility-client-architecture.log`）。这些结果不代替全部客户端实际调用审查或安装事务实机验证。
- Worker 拓扑配置分离后的 Release 构建通过（`build/responsibility-worker-topology-release.log`）。CTest 本轮 26/29（`build/responsibility-worker-topology-ctest.log`），结构与 Worker 隔离测试通过；三个 GB28181 媒体/SIP 测试均在 ZLMediaKit 绑定服务器阶段失败，尚未解决本机端口绑定及 IPv6 验证问题。
- GB28181 控制存储操作迁移及新 Ruvia 固定提交下的完整 Release 构建通过（`build/responsibility-gb-control-release.log`）；CTest 28/29（`build/responsibility-gb-control-ctest.log`），唯一失败为 SIP IPv6 数据报未送达。两个媒体测试本轮通过，不代表偶发端口绑定问题已解决。
- Edge 恢复集成测试使用本次独立启动且结束后关闭的 Redis 验证，通过旧所有者恢复、活动租约排除和所有权 fencing（`build/responsibility-edge-recovery-integration.log`），未连接生产 Redis。首轮编译遇到其他进程占用 server.obj，已在占用结束后重建。
- Edge 恢复迁移的后续重建再次遇到对象文件占用；确认所有实际 cl/MSBuild 构建进程结束后，最终 Release 验证通过（`build/responsibility-edge-recovery-release-verified.log`）。CTest 28/29（`build/responsibility-edge-recovery-ctest.log`），仍有 SIP 测试失败，未宣称全通过。
- operations/alert 类型迁移在补齐 alert service 的显式 types 引用后，最终 Release 构建通过（`build/responsibility-module-types-release-final.log`）；CTest 28/29（`build/responsibility-module-types-ctest.log`），唯一失败仍为 SIP IPv6 数据报未送达。结构和差异检查通过。
- Redis 实体宏与 Hash 访问检查补全后，Release 构建及结构检查通过（`build/responsibility-storage-check-release.log`）；CTest 28/29（`build/responsibility-storage-check-ctest.log`），剩余 SIP 测试失败。
- GB28181 所有权租约迁移的 Release 构建和独立 Redis 集成测试通过（`build/responsibility-gb-owner-release.log`、`build/responsibility-gb-owner-integration.log`）；CTest 28/29（`build/responsibility-gb-owner-ctest.log`），唯一失败仍为 SIP IPv6 数据报未送达。
- GB28181 控制结果缓存映射迁移的 Release 构建通过（`build/responsibility-gb-result-release.log`）；CTest 28/29（`build/responsibility-gb-result-ctest.log`），唯一失败仍为 SIP IPv6 数据报未送达。
- GB28181 投影发布职责迁移的 Release 构建及独立 Redis 修复测试通过（`build/responsibility-gb-projection-publish-release.log`、`build/responsibility-gb-projection-integration.log`）；CTest 28/29（`build/responsibility-gb-projection-publish-ctest.log`），唯一失败仍为 SIP IPv6 数据报未送达。完整 `gb28181-worker-integration.ts` 仅同步了来源引用，尚未在完整数据库/服务 fixture 下复跑。

- 控制认领迁移后的 Release 构建通过（`build/responsibility-gb-claim-release.log`），CTest 28/29（`build/responsibility-gb-claim-ctest.log`），唯一失败为 SIP IPv6 UDP 数据报未送达；未跳过失败项。独立 Redis 控制认领测试通过，完整数据库/服务集成仍待复验。

- 控制下发职责迁移的 Release 构建通过（`build/responsibility-gb-dispatch-release.log`）；独立 Redis 认领及发布测试通过；CTest 28/29（`build/responsibility-gb-dispatch-ctest.log`），唯一失败仍为 SIP IPv6 UDP 数据报未送达。runtime 中的投影编解码和请求校验仍需按用途审查归位，Redis 调用迁移不代表该文件已完成整改。

- 投影编解码迁移的 Release 构建通过（`build/responsibility-gb-codec-release.log`）；新增 projector 行为测试通过，完整 CTest 28/29（`build/responsibility-gb-codec-ctest.log`），唯一失败仍为 SIP IPv6 UDP 数据报未送达。差异空白检查通过。

- 本轮补齐实际服务集成验证：`Run-RpcIntegration.ps1 -Gb28181` 在独立 PostgreSQL/TimescaleDB、Redis、两个 Service Worker 和两个 Collector Worker 下通过全部断言（`build/responsibility-gb-full-integration.log`），覆盖注册回复晚于提交、元数据保留、HTTP 所属连接路由、错误 Collector 拒绝、数据库停顿排序、迟到投影防覆盖、过期回执及去重标记恢复、租约失效后的 SIP fencing 与持久化离线。此前“完整 GB28181 fixture 未复跑”的缺口已补齐；此结果不代替仍失败的 IPv6 CTest。
- `Run-SystemOrmIntegration.ps1` 的系统 ORM 与 live-query 测试通过（`build/responsibility-system-full-integration.log`），覆盖角色权限、参数绑定、部门联表及 UTC/空值、递归环拒绝、用户角色替换、数据库失败回滚、软删除、权限码、外部提交驱动 SSE、重连快照及权限撤销关闭订阅。仅使用独立本地数据库，夹具进程已由运行器关闭。
- Edge 会话测试改用统一 `architecture-fixture` 的 Redis 地址，支持隔离端口，并在本轮独立 Redis 上通过会话所有权、续期、断开及过期通知断言（`build/responsibility-edge-session-integration.log`），补齐原固定端口未监听造成的验证缺口。

- 双 Service Worker 的 RPC 与 Edge 完整应用恢复测试通过（`build/responsibility-rpc-full-integration.log`）：命令持久化、Worker 独立消费/回复、连接归属、指标聚合及 readiness 恢复、并发幂等、写入失败回滚、流删除后恢复、SSE 与 Collector 独立消息接收、数据库锁隔离、旧 Edge 所有者恢复与 fencing、SCAN 被拒后的恢复，以及 projector 租约丢失导致 readiness 失败。运行器完成清理并退出 0；这些验证覆盖当前真实服务调用，而不只是 Lua 提取执行。

- 请求校验归位后的 Release 构建通过（`build/responsibility-gb-validation-release.log`），CTest 28/29（`build/responsibility-gb-validation-ctest.log`），唯一失败为 SIP IPv6 数据报未送达。完整 GB28181 集成首次在 ZLMediaKit 绑定端口 51227 时收到 permission denied，尚未执行断言；夹具清理退出后独立重试全部通过（`build/responsibility-gb-validation-integration-retry.log`）。重试成功不代表偶发 SDK 端口绑定问题已解决。差异检查通过。

- 前端共享目录整改通过锁定 Bun 的类型检查、lint、生产构建（`build/responsibility-web-shared-{typecheck,lint,build}.log`），5 个修改文件 Biome 格式检查、12 项前端架构/快照/会话测试（`build/responsibility-web-shared-tests.log`），以及使用新 SSE 读取实现的完整 RPC/Edge 应用恢复测试（`build/responsibility-web-shared-rpc.log`）。差异检查通过；未涉及页面布局变化。

- 播放器目录及公共分页类型归位通过锁定 Bun 类型检查、lint、生产构建（`build/responsibility-public-types-{typecheck,lint,build}.log`）、前端结构测试、37 个当前修改/新增前端文件 Biome 格式检查及差异检查。此次没有修改播放逻辑或页面布局，未执行实际视频源播放验证。

- SIP/媒体公共结果类型归位的 Release 构建通过（`build/responsibility-gb-transport-types-release.log`）；CTest 28/29（`build/responsibility-gb-transport-types-ctest.log`），唯一失败仍为 SIP IPv6 UDP 数据报未送达。旧嵌套类型引用已清理，差异检查通过；本批未改动控制、SDK 或数据库行为，未重复完整服务集成。

- Outbox 元数据补齐后 Release 构建通过（`build/responsibility-outbox-mapping-release.log`）；完整 outbox 通知集成通过（`build/responsibility-outbox-mapping-integration.log`），覆盖空队列不轮询、提交唤醒/回滚不发布、未来期限唤醒、锁释放恢复、监听重连补偿、指数退避和死信边界。CTest 28/29（`build/responsibility-outbox-mapping-ctest.log`），仍有 SIP IPv6 失败，未宣称全通过。

- auth 计数映射完善后 Release 构建通过（`build/responsibility-auth-mapping-release.log`）；新增登录限流集成与完整系统 ORM 集成通过（`build/responsibility-auth-mapping-integration.log`），CTest 28/29（`build/responsibility-auth-mapping-ctest.log`），仍有 SIP 测试失败。差异检查通过。

- 响应组装归位最终 Release 构建及完整 GB28181 服务集成通过（`build/responsibility-gb-response-release-final.log`、`build/responsibility-gb-response-integration.log`），CTest 28/29（`build/responsibility-gb-response-ctest.log`），唯一失败仍为 SIP IPv6 UDP 数据报未送达。差异检查通过。

- Edge 投影发布归位在补齐测试构建依赖后 Release 构建通过（`build/responsibility-edge-publish-release-final.log`）；Worker 隔离及结构检查通过，完整 RPC/Edge 恢复集成通过（`build/responsibility-edge-publish-integration.log`），包括旧所有者恢复、活动租约排除、fencing、SCAN 拒绝恢复与租约失效 readiness。CTest 28/29（`build/responsibility-edge-publish-ctest.log`），唯一失败仍为 SIP IPv6 UDP 数据报未送达。差异检查通过。

- Edge 元数据映射归位的 Release 构建通过（`build/responsibility-edge-metadata-mapping-release.log`），直接依赖实体的元数据编解码测试通过；CTest 28/29（`build/responsibility-edge-metadata-mapping-ctest.log`），唯一失败仍为 SIP IPv6 数据报未送达。差异检查通过。

- GB28181 所有者查询归位本批完整 Release 未通过（`build/responsibility-gb-owner-query-release.log`）：工作区同期新增的 SL651 改动引用了尚未定义的 stationHeaders_、ambiguousDevices_、recentResponses_ 和 appendReportActions。保留该文件改动，未回退或替写。GB28181 projector/SIP 两个目标单独构建及 projector CTest 通过（`build/responsibility-gb-owner-query-targets.log`、`build/responsibility-gb-owner-query-focused-ctest.log`）；完整服务构建与集成验证仍待源文件恢复可编译后补齐，不使用旧服务器二进制宣称本批集成通过。

- 全仓实体元数据补齐后，完整 Release 仍因同期 SL651 未声明成员失败（`build/responsibility-entity-metadata-release.log`）；已成功重建的 10 项相关结构、配置、设备、协议、链路、遥测、access、Edge 元数据和 GB28181 projector 测试通过（`build/responsibility-entity-metadata-focused-ctest.log`）。未使用旧服务器二进制声称新映射业务集成通过，完整构建和回归仍待补齐。差异检查通过。

## 工作区协作

Edge 诊断对象归属：`EdgeProjectionRuntime` 改为构造时接收所属 Worker 的 `RuntimeDiagnostics&`，`server.cpp` 注入该 Worker 已创建的实例；租约失效直接更新此实例，不再通过 `currentWorkerDiagnostics()` 查找。诊断对象在 Worker 组件中的生命周期长于投影组件。worker-isolation 测试直接包含 Edge runtime，并在编译期约束必须注入诊断对象；目标编译、Worker 隔离及 architecture 检查通过（`build/responsibility-edge-diagnostics-build.log`、`build/responsibility-edge-diagnostics-ctest.log`）。此项消除 Edge 的全局诊断访问，不代表 `common/observability.h` 已归位；模块重放计数仍依赖公共运行对象，完整服务器和租约失效集成仍待补齐。

前端公共目录复查：`SnapshotStream` 管理订阅、取消和子订阅切换，已从 `web/utils/snapshot-stream.ts` 移至现有 SSE 接入目录 `web/lib/snapshot-stream.ts`，同步 6 个源码消费者和快照测试引用，未改实现。锁定 Bun 的类型检查、lint、生产构建及 7 个修改源码的 Biome 格式检查通过（`build/responsibility-snapshot-location-{typecheck,lint,build,format}.log`）；快照订阅及前端结构测试 10/10 通过（`build/responsibility-snapshot-location-test.log`）。本次未改变页面布局，未以该检查代替全仓实际页面验收。公共权限 Hook 使用稳定空权限引用，会话 store 仅维护和持久化状态，通用 mutation Hook 通过注入函数操作数据；所核对文件未发现页面模块反向依赖，保留现归属。

Collector 状态映射复查：原 `CollectorStateRecord` 仅有键名，现拆为实际使用的 `CollectorWorkerRecord`（Worker 编号、版本、应用状态和时间）与 `CollectorLinkRecord`（保留动态字段的链路快照）。service 通过映射生成持久化字段，保留 DEL/HSET 顺序、消息元数据过滤和后置更新时间覆盖。全仓引用检查只发现旧 `iot:runtime:connection:` 的删除方，没有写入方，已删除该失效键、`eraseConnection` 和调用，不增加兼容清理。新增 collector-state 目标独立编译通过，测试覆盖消息元数据排除、动态及二进制字段、旧更新时间覆盖、源事件不变、Worker 字段契约和键隔离；collector-state/architecture 两项通过（`build/responsibility-collector-state-build.log`、`build/responsibility-collector-state-ctest.log`）。该目标直接验证实体映射，不替代 Collector service/runtime 的完整编译及实际存储回归。

Collector 发送前的链路所有者查询归入已有 ownership service 的 `isLinkOwner`，runtime 保留本地连接查找、关闭和消息确认；保留非 String 回执判定为失去所有权的原行为。结构检查增加原生命令及 Lua 的 Hash 读写检测（含大小写命令），并用锁和 Stream 命令验证不会误归类。architecture 目标重建和检查通过（`build/responsibility-raw-hash-build.log`、`build/responsibility-raw-hash-ctest.log`）。最新完整 Release 构建仍失败于同期 SL651 未声明成员（`build/responsibility-collector-owner-release.log`），不能据此认定本批服务器编译完成。随后补齐 `LogResultRecord.protobufBytes` 并接入网关日志结果和级别回执的序列化、Redis 写入，保留原 Protobuf、期限和发布顺序；这部分同样待完整编译与集成验证。Hash 检查仅扩大静态覆盖，不证明所有 String 映射和真实调用已完成审查。

终端实体复查：将一次性票据的节点 ID 明确为 `TerminalTicketRecord.nodeId`，将终端所有者值明确为 `TerminalSessionRecord.nodeSession`，接入实际 GETDEL、SET 和所有权读取；保留键名、完整会话逐字比较及过期语义。确认、输出序号的 Lua 和队列协议不变。Redis 终端状态测试 6/6、43 个断言通过（`build/responsibility-terminal-records-test.log`）；更新依赖旧参数表达式的结构断言后，edge-service 目标重建和 architecture/edge-service 检查通过（`build/responsibility-terminal-records-build.log`、`build/responsibility-terminal-records-ctest-final.log`）。这些检查不替代尚未完成的完整服务器编译与集成。RPC monitor 的停止令牌、取消信号轮询和消费流续期属于运行生命周期，本轮保留在 runtime，未把信号声明为存储实体。

“补全SL651协议M1至M4”仍在同一工作区活动。此前缺失的定义现已补齐，本轮完整 Release 构建通过；本任务未覆盖该协议改动。旧段落中因缺失定义而未完成的编译验证，以本轮新二进制回归结果为准，后续新增改动仍需重新验证。

开始修改时 auth、user service 已存在 ORM 改动；执行期间 link、protocol service 和 system ORM 集成测试也出现其他改动。保留这些改动，验收和差异审查须明确区分来源，不回退其他任务的代码。

GB28181 控制操作迁移期间，工作区 Ruvia 固定提交由其他改动更新为 `13eba75f3b8317a9fe4b5e27ca70554a3e887cab`（扩展类型化 SQL repository 并分离 Redis 实体宏）。当前构建正按该提交重新编译依赖；既有 ORM 能力缺口记录需基于新版本复核，不能沿用旧版本结论作为最终证据。

已复核新提交的 `RedisEntityKey.h`：Redis ORM 仍强制 `ruvia:orm:` 加十六进制表前缀与非空 ID，不能直接覆盖现有固定原生键；因此本任务针对固定 Hash/String 的自定义映射理由仍成立。新增 Redis 宏本身不解决多键 Lua 原子行为及动态 Hash 字段映射问题。其他 SQL repository 能力需继续按实际调用复核。

## UUID 与进程身份专项审查（待整改）

当前实际调用清单：`build/responsibility-uuid-call-inventory.json`，按源码行列出 UUID 生成、进程身份和消息 ID 使用点；计数包含定义行，不能当作独立调用数。

- `service/common/uuid.h` 持有线程局部 `UuidV7Generator`，内部包含随机引擎、递增序列、上次时间戳及互斥锁。它确实是可变运行对象，不能因 thread_local 就视为无状态工具；当前是线程归属，尚未证明与 Worker 生命周期一一对应。
- 同文件的 `service::runtime::instanceId()` 首次调用生成进程身份并缓存为不可变字符串。该值用于 Redis 消费组、owner token、租约和重放计数，不能改成每 Worker 不同值，不能在正常操作中重复生成。
- `common/message.h` 多个键名函数的默认参数隐式读取该身份，`nextMessageId()` 还转发调用有状态生成器。仅把 uuid.h 改路径不能修复公共契约中隐式状态依赖。
- Modbus、S7、SL651 的 parsedAction 原先直接调用消息 ID 生成器；现已在所属 Worker runtime 发布前分配 messageId，SL651 用会话内发布令牌关联持久化完成，causationId 保持不变。生成器本身的 Worker 装配仍待落实。
- server 的 Worker 初始化已有公开 `workerState` 接入点。整改需先明确生成器与不可变进程身份的装配入口，再同步所有实际调用和纯契约参数；不得通过跨层共享 service、全局回调或把状态搬入 utils 绕过边界。

必要验证包括每个 Worker 内单调 UUIDv7、同毫秒与时钟回退、并发唯一性、进程重启身份变化、原 Redis 键格式、缩容/恢复、RPC 围栏与 SL651 多包重传。当前仅完成依赖审查，未修改生成行为，也未完成上述专项验证。

本轮最终全仓 diff --check 发现其他改动的 web/pages/iot/link/index.tsx 第 35 行行尾空格（本轮未修改该文件），未代为覆盖。collector-publication-integration.ts 新增文件及本轮文档未发现行尾空格。

补充审查：edge.protocol.h 的 randomUuidV7Bytes 另有线程局部随机引擎，outbound 还读取当前时间与 platformId；同文件 publicBaseUrlStorage/platformIdStorage 保存可变配置。该问题尚未被公共 UUID 调用名检查覆盖，需要将出站信封的运行输入及配置归属一起整改，不能只删除随机引擎名字。新增结构规则仅覆盖 pure definition 对 nextUuidV7/nextMessageId/UuidV7Generator 的直接调用；它是回归防线，不是任意间接可变状态依赖的完整证明。
