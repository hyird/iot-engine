# 内部接口迁移方案

## 目标与当前状态

### 最新状态：2026-09-17

- 生产 `192.168.5.100:10050` 已部署 `3e99326`，HTTP/SSE 改造及浏览器发现的修复已上线，按用户确认的现有生产功能范围完成验收。
- 已检查系统管理、六类协议、链路、设备、边缘节点、告警、开放接入及登录页面。
- 设备列表、分组、指标使用同一连接；连续指标事件未重复推送列表与分组。
- 链路真实报文、暂停与恢复正常；测试设备保持原值保存后，编辑弹窗正常关闭。
- 768×720 下表格横向滚动、弹窗独立滚动及固定操作区正常。
- 生产无边缘节点，GB28181 未启用；用户同意按现有生产功能验收，记录节点、串口及视频实机未覆盖。
- 断线验收发现 Query 自动重试叠加 SSE 重试；已修复并发布，复验确认耗尽后静默超过 40 秒，手动恢复后继续推送变化。
- 链路调试保留已有列表，调试开关不再刷新枚举与公网 IP。
- 新连接等待旧数据流取消完成；异步取消回归测试通过。新版本打开、关闭链路调试的活跃 SSE 最大为 1。
- 新版本调试开关的两次 PUT 均未触发枚举或公网 IP 重新查询；45 次报文事件共用页面连接。
- 旧版本额外 75 秒空闲观察无业务 HTTP 请求，也无 favicon 请求；不能将扩展改写图标认定为业务轮询。
- 修复快照 `3e99326`：前端 111 项测试、Linux 35 项测试、Windows 客户端 6 项测试通过。
- Windows 媒体测试首次端口绑定失败，单项复测通过；Windows SIP 环境例外由 Linux 测试覆盖。
- `3e99326` 已通过备份恢复、哈希、依赖、重复迁移和切换后检查。
- 最终结论及未覆盖项见 `docs/http-sse-acceptance.md`；下方阶段记录不代表当前仍有相同待办。
- 证据：`build/deploy-2166468/browser-progress-20260917.md`、`build/deploy-3e99326/acceptance-status.md`。

以下保留各阶段实施记录；“待发布”“未部署”等描述为当时状态，以本节为准。

### 进行中：视频页面按需查询与全局检查

- 视频页仅保留设备及通道变化 SSE；启动状态、媒体能力、录像状态按需 HTTP 查询。
- 切换预览读取录像状态，开始或停止录像后显式刷新，不做定时查询。
- 删除健康、单设备、流列表、单流和录像五个多余 SSE 入口；HTTP 地址保留。
- 前端类型、lint、生产构建、格式检查及 110 项测试通过。
- Windows Release 与 34 项适用 CTest、Linux Release 与完整 35 项 CTest 通过。
- 真实 SIP 验证注册、改名推送、预览启动/续期/停止及原 Collector 归属。
- 持久化阻塞、迟到投影、收据恢复和租约失效回归通过。
- 证据：`build/video-sse-integration.log`、`build/video-sse-windows.log`、`build/video-sse-web-tests-final.log`。
- 删除设备 Service 中未被调用、仅返回空数组的两组旧订阅占位接口。
- 前端订阅扫描仅发现设备、链路、告警、边缘节点、视频五类页面使用业务 SSE。
- 定时器中的终端/串口保活、预览租约续期和本地绘制不是业务查询轮询。
- Windows 客户端仅订阅当前 Peer 配置；全局连接数量仍须线上网络记录验证。
- 链路、告警、边缘节点和视频补充订阅错误提示及手动重试；设备原有失败页保留。
- 节点日志订阅失败时先恢复连接再显式采集；VPN 状态错误单独显示。
- 重试界面已通过类型、lint、构建、格式及前端回归；线上断线交互仍待验证。
- 证据：`build/sse-retry-ui-tests.log`、`build/sse-retry-ui-build.log`、`build/video-sse-linux.log`。
- 已删除 VPN 网络列表、单网络、路由、Peer、会话及诊断六个管理端 SSE。
- 保留桌面客户端设备与配置订阅协议，管理端普通 HTTP 地址不变。
- 本次入口清理的双平台构建及 VPN 集成正在验证。
- 浏览器工具暂无法加载请求头策略；SSH 正常，线上浏览器验收未完成。
- 当前未部署，线上逐页网络及功能验收待完成。

### 进行中：边缘节点按需订阅

- 分组配置改为 HTTP GET，支持取消；写入后刷新，无定时查询。
- 删除分组及未使用的固件目录 SSE，保留原 HTTP 接口。
- 集成用例改为写入后读取分组；权限撤销继续用节点实时连接验证。
- 节点清单、选中详情、日志及 VPN 状态共用 `/v1/edge/events`，按命名事件更新。
- 清单不再逐页建立 SSE；详情、日志和 VPN 随当前节点与页签调整范围。
- 网络表单详情、VPN 网络选项使用 HTTP；Peer 和路由执行状态保留实时推送。
- 分组树计数改由实时节点清单计算，不依赖配置查询的旧计数。
- 删除独立节点详情、日志 SSE；保留普通 HTTP 和串口、终端双向协议。
- 类型检查、lint、前端构建、格式检查与 109 项前端测试通过。
- Windows Release 与 34 项适用 CTest、Linux Release 与完整 35 项 CTest 通过。
- 真实服务验证改名、共享日志、显式采集、权限隔离和撤销，未产生心跳采集。
- 证据：`build/edge-shared-integration.log`、`build/edge-shared-windows-tests.log`、`build/edge-shared-linux.log`。
- 补充真实服务测试通过：201 个额外节点完整返回，重复通知无业务事件及额外心跳。
- 首次批量测试数据的 JSON 参数编码已修正；失败日志保留。
- 证据：`build/edge-shared-inventory-integration-final.log`、`build/edge-shared-ci-web-tests.log`。
- 当前未部署，线上浏览器验证待完成。

### 待发布：设备指令结果并入页面 SSE

- 指令提交直接返回 HTTP 受理结果，不再等待 SSE 才结束提交操作。
- 页面单独展示执行进度、失败原因和设备回读；订阅失败可重试结果查询。
- 重试结果不会再次发送指令；结果完成或停止查看后取消结果订阅。
- 指令结果使用 `/v1/device/events` 的 `commands` 事件，与列表、分组、指标和报文共享连接。
- `commandIds` 最多 256 条，不再拆分为多个独立 SSE；删除原单条和批量结果 SSE。
- 状态读取移入设备 Service，复用指令实体映射；提交仍归指令 Service，无服务依赖环。
- 原 HTTP 状态查询地址、响应及资源权限保留；结果完成后保存展示内容并取消对应订阅。
- 指令状态通知不触发设备列表或指标查询；设备数据仍由对应事件驱动。
- 前端类型、lint、生产构建和 108 项测试通过；新增界面仍待真实浏览器验证。
- 共享五类事件及 256 条指令的前端测试通过，证据：`build/device-command-shared-ci-web-tests.log`。
- Windows Release、34 项适用 CTest、Linux Release 和完整 35 项 CTest 通过。
- 真实服务验证 256 条指令的长地址、完成状态、回读值、权限隔离和原 HTTP 查询。
- 指令通知未查询被静默改动的指标；MC 3E/4E、FINS TCP、DLT645 1997/2007 指令回归通过。
- 第三方 HTTP/SSE 的响应、设备权限、实际写入和回读回归通过。
- 迁移前的源码位置和等待应答断言已更新；首次格式临时文件导致的架构失败记录保留。
- 编译中修改头文件导致 Windows 增量对象失真；删除生成的 `server.obj` 后重新构建，集成通过。
- 证据：`build/device-command-shared-integration-fresh.log`、`build/device-command-shared-windows-fresh.log`。
- Linux 证据：`build/device-command-shared-linux-final.log`、`build/device-command-shared-linux-tests-final.log`。
- 本批未部署，不能据此认定线上验收完成。

### 待发布：设备调试复用页面 SSE

- 列表、分组、指标和选中设备报文共用 `/v1/device/events`。
- `debugDeviceId` 限定报文范围；关闭调试或卸载视图后取消对应订阅。
- 删除独立设备报文 SSE；保留调试 HTTP 查询、开关和权限校验。
- 切换范围保留已有列表，避免等待快照时卸载调试窗口。
- 修复晚加入查询触发健康连接重连；仅失败订阅需要重试。
- 重连回归先复现失败再验证修复，证据：`build/sse-late-reader-before.log`、`build/sse-late-reader-after.log`。
- 前端类型、lint、生产构建、格式检查及 108 项测试通过。
- Windows Release 与 34 项适用 CTest、Linux Release 与完整 35 项 CTest 通过。
- 真实服务验证共享四类事件、报文变化隔离、调试权限错误、非法范围及 400 设备快照。
- 证据：`build/device-debug-shared-integration.log`、`build/device-debug-shared-ci-web-tests-final.log`。
- 构建证据：`build/device-debug-shared-windows.log`、`build/device-debug-shared-linux.log`。
- 指令结果合并进度见上节；本批未部署，设备页完整场景仍待线上浏览器验收。

### 待发布：开放接入配置与历史记录恢复 HTTP

- 密钥、Webhook、调用记录使用普通 GET；删除三个管理端 SSE 路由。
- 查询支持取消且无定时刷新；页面提供手动刷新。
- 写入后按关联关系刷新配置、名称、Webhook 数量及历史记录查询。
- 第三方 `/open-api` HTTP 与 SSE 协议保持不变。
- 前端类型、lint、生产构建、修改文件格式检查及 106 项测试通过。
- Windows Release 与 34 项适用 CTest、Linux Release 与 35 项 CTest 通过。
- 真实服务验证 CRUD、密钥轮换、权限、字段校验、异步落库及第三方 HTTP/SSE。
- 首次测试变量重名、随后异步落库时序断言失败，均已修正；保留失败日志。
- 测试等待后台提交仅限隔离测试库，产品未增加 HTTP 或数据库轮询。
- 证据：`build/access-http-only-integration-final.log`、`build/access-http-only-ci-web-tests.log`。
- 构建证据：`build/access-http-only-windows.log`、`build/access-http-only-linux.log`。
- 本批未部署；线上浏览器零 SSE、手动刷新和窄屏检查仍待完成。

### 待发布：公共用户信息复用页面 SSE

- 自有前端使用 `X-SSE-User: 1` 请求共享用户事件；未请求该事件的客户端协议不变。
- 认证中间件通过 Ruvia 请求级状态提供查询，页面 SSE 按 `user` 事件推送用户信息。
- 查询在连接所属 Worker 执行；只在初始订阅、认证变化时读取用户资料，遥测不会触发用户查询。
- 用户资料初始读取使用 HTTP；公共布局只观察已有实时连接，不建立独立用户 SSE。
- 离开最后一个实时视图后关闭连接；无实时需求、页面订阅失败时均不建立兜底 SSE。
- 令牌刷新清除旧缓存，账号替换不复用用户快照；页面权限错误与用户事件隔离。
- 保留 `/api/...` 管理接口地址，修复订阅层只接受 `/v1/...` 的限制。
- 前端类型检查、lint、格式、生产构建与 105 项测试通过；Linux CTest 35/35、Windows 34 项适用测试通过。
- 真实服务验证了共享用户快照、权限变化、令牌过期、遥测查询隔离及设备/链路回归。
- 隔离探测只在临时测试库的单次事务中抑制通知触发器；生产逻辑没有此处理。
- 证据：`build/shared-user-sse-integration-final.log`、`build/shared-user-sse-ci-web-tests.log`、`build/shared-user-sse-linux.log`、`build/shared-user-sse-windows.log`。
- 后续前端修正已通过类型、lint、格式、生产构建及订阅/认证 17 项测试。
- 修正覆盖零连接页面、切换、权限隔离、账号替换，以及耗尽重试后的持续订阅恢复。
- 上述服务端证据不代表最新前端已通过线上验收；本批未部署。
- 设备调试/指令、边缘节点与视频仍待按实时需求整理；开放接入已按上述方案恢复 HTTP。

### 待发布：链路列表与调试共享 SSE

- 链路列表与选中链路的报文共用 `/v1/link/events`，事件为 `links`、`packets`。
- 打开或关闭调试时调整 `debugLinkId` 订阅参数；页面只保留一个选中的调试窗口。
- 报文通知只查询调试数据；列表变化仍更新列表，调试权限错误不阻断列表。
- 移除独立调试 SSE，保留调试 HTTP 查询与开关接口。
- 编辑表单的节点选择改为按需 HTTP 查询，读取全部分页并传递取消信号，不建立节点 SSE。
- 前端类型检查、lint、格式、生产构建与 100 项测试通过。
- Windows Release、34 项适用 CTest 及链路真实服务集成通过；已知 IPv6 用例未覆盖。
- Linux Release 与完整 CTest 35/35 通过，证据：`build/link-shared-sse-linux.log`。
- 集成验证共享连接、报文推送、列表更新、非法调试 ID，以及仅查询权限下的事件隔离。
- 证据：`build/link-shared-sse-integration.log`、`build/link-shared-sse-windows.log`、`build/link-shared-sse-ci-web-tests.log`。
- 公共用户信息已按上述机制共享；整页行为仍待线上浏览器验收，本批未部署。

### 待发布：告警记录与统计共享 SSE

- 告警页面改用 `/v1/alert/events`，`records` 与 `stats` 事件共用连接。
- 两个前端查询使用相同筛选、分页参数；记录按页返回，统计保持全局范围。
- 移除旧记录、统计和未使用的分组 SSE 入口，保留对应 HTTP 查询。
- 前端类型检查、lint、格式检查、生产构建及 CI 中 98 项前端测试通过。
- 真实 PostgreSQL、Redis、服务进程集成通过，覆盖触发、分页、单条和批量确认后的双事件更新。
- 证据：`build/alert-shared-sse-integration.log`、`build/alert-shared-sse-ci-web-tests.log`。
- Windows Release 成功；CTest 首次有两项媒体端口绑定失败，原二进制独立复测两项通过。
- 保留首次失败及复测记录：`build/alert-shared-sse-windows.log`、`build/alert-shared-sse-media-recheck.log`。
- Linux Release 与完整 CTest 35/35 通过，证据：`build/alert-shared-sse-linux.log`。
- 公共用户信息已按上述机制共享；尚未部署，整页单连接仍须浏览器验收。

### 新约束：页面单 SSE 与注释心跳

- 每个页面最多一条 SSE，包括页面内弹窗、抽屉和公共布局的订阅。
- 设备列表、分组、指标与公共用户信息已共享连接，调试和指令订阅仍须合并。
- 边缘节点详情及 VPN 等仍须合并；尚未满足整页单连接。
- 服务端保活改为 SSE 注释，仅等待超时时发送；数据未变的通知不发送心跳。
- 使用 Ruvia 公开 `Context::stream().write()`，未修改 Ruvia 源码。
- 前端 SSE 相关 11 项测试通过，证据：`build/event-driven-sse-web-tests.log`。
- Linux Release 与 CTest 35/35 通过；Windows Release 与 34 项适用测试通过，已知 IPv6 环境失败项未覆盖。
- 构建证据：`build/event-driven-sse-linux.log`、`build/event-driven-sse-windows.log`。
- 设备、边缘节点真实集成通过，验证了注释保活、空闲无业务事件、重复通知不触发心跳及权限撤销。
- 集成证据：`build/event-driven-sse-integration.log`；其中认证过期测试因原 20 秒等待上限失败，不能记为整组通过。
- 认证用例改为最多等待 40 秒，覆盖验证器 15 秒容差及下一次检查；服务端令牌逻辑未改。
- 认证独立复测通过，覆盖登录、刷新、用户变化及令牌过期错误，证据：`build/event-driven-sse-auth-integration.log`。
- 本批尚未部署，不能用既有线上浏览器结果作为本批验收。

### 待验证发布：设备页合并 SSE

- 按用户要求，设备列表、分组计数和实时指标合并到 GET `/v1/device/events`，分别以 `devices`、`groups`、`realtime` 事件发送。删除内部 `/v1/device/realtime/events` 与 `/v1/device/groups/tree-count/events` 路由；普通 GET/POST/PUT/DELETE 和第三方接口保持原有行为。
- 同一浏览器页面中的三个数据 Hook 共享连接，各自缓存最新事件。服务端按变更主题查询：遥测只重新读取实时指标，不重查列表及分组；初始连接和断线恢复发送各自快照，空闲心跳不查询业务数据。
- 每类数据分别校验权限并发送本事件的错误；分组无权限不影响设备指标，权限变化后通过原连接更新。前端最后一个观察者离开才关闭连接，账号切换清除所有事件缓存。
- 类型检查、lint 及 8 项 HTTP/SSE 前端测试通过；后端构建、真实集成和线上浏览器复验尚未完成。线上仍为 `f47eefa`。

### 待发布：Collector UUID 生命周期

- CollectorWorker 独占 UUIDv7 生成器，TCP 与 GB28181 采集组件使用所属 Worker 构造注入的引用，连接令牌、采集消息和投影 ID 不再从线程局部入口取得。算法和协议字段不变；Service Worker 与进程身份的隐式状态仍待继续处理。
- Linux Release/完整 CTest 35/35、真实 GB28181 Worker 和工业协议/设备 HTTP/SSE 集成通过。Windows Release 成功但 GB28181 IPv6 用例未通过；独立本机 IPv6 UDP 回环同样超时，IPv4 正常。详细证据见职责整改记录，不能宣称 Windows 全部通过。
- 本批及前述规则选择器按需 GET 尚未部署，线上保持 `f47eefa`。本次不重复用旧版浏览器结果作为新后端验收。

### 后续规则验收与待发布改动

- 在 `f47eefa` 真实浏览器创建关联“测试”设备的禁用规则，全程状态为禁用；空备注重新编辑成功，名称筛选命中修改后规则，再次编辑阈值为 `888888`。随后选中本次规则，通过批量删除清理，恢复空筛选后规则与模板均为空，控制台无错误或警告。没有启用规则或向设备发送控制命令。
- 75.05 秒观察 `build/alert-rule-browser-network.json` 覆盖新增、编辑和名称筛选：POST/PUT 后查询规则列表，未请求模板列表或告警 SSE；两次打开编辑窗口各产生一次设备列表 SSE。该观察在批量删除前结束，删除成功依据实际页面结果，不把抓包误记为覆盖删除。
- 本地已将规则窗口设备选择由 `useDeviceList` 改为设备 service 的 `useDeviceConfigurationList`，打开时通过 API 的 `queryDeviceList` 调用已有 GET `/v1/device`，关闭时停止自动获取；无轮询、焦点或重连刷新。后端 GET 与 SSE 本来共用 `listSnapshot`，保持设备范围和协议配置字段，包括禁用设备，不改第三方接口。设备管理页继续使用原实时 SSE。
- 本批尚未部署。前端类型检查、lint、格式、生产构建及相关 11 项测试通过（`build/alert-device-selection-*.log`）；上线后仍须验证窗口打开请求和字段回填。全仓其他整改与验收继续进行。

### 最新部署与复验（2026-09-17，f47eefa）

- 当前生产 `f47eefa6ea76d757d4f1bd3b2eac859123501228`，分支 `codex/http-sse-release-20260917-074626`，前端 `/b64f4f66.js`。556 个文件匹配发布快照。本次相对 `b958c74` 只修改告警前端、测试和文档，后端源码与二进制未变化，沿用已验证的 WSL Release 二进制。
- 按 CI 原步骤的完整前端测试列表运行 25 个文件，96/96 通过；Windows 同提交安装包 6/6 通过，类型检查、lint、格式和前端生产构建通过。告警真实服务集成及批量确认原 SSE 推送测试通过。证据：`build/alert-cache-ci-web-tests.log`、`build/alert-cache-release-native.log`、`build/alert-cache-source-verification.log` 及下述功能测试日志。
- 制品 SHA256 `7618c6bc371853fdf932a4278a36a48f94043130b72f5af96aa044129204ef93`；备份 `/opt/iot/backups/before-f47eefa-20260916T234655Z` 保留，数据库备份 9248582 字节。恢复副本和两次迁移、架构/依赖/文件哈希、切换后的实际二进制与前端资源、就绪、UTC、迁移和 Redis AOF 均通过；服务 active/running，NRestarts=0，`.env` 权限 600。详见 `build/alert-cache-production-{backup,stage,cutover}.log`。
- Chrome 真实界面新增空分类/描述的独立模板，详情读取阈值为 `999999`；编辑名称与阈值后再次打开读到 `777777`，随后删除自建模板，模板和规则列表恢复为空，控制台无错误或警告。未向设备应用模板；内置浏览器同步刷新至新版本。
- `build/alert-cache-browser-network.json` 的 75 秒观察仅记录 6 个请求：两次编辑各一次详情 GET，保存一次 PUT 后一次模板列表 GET，删除一次 DELETE 后一次模板列表 GET。无规则列表 GET、无 SSE 建连请求、无 favicon 请求，最后约 26 秒无新增请求。该证据证明本轮模板操作不再触发无关查询或重建实时订阅，不代表所有页面和长期断线均已验收。
- 全仓职责审查、公共 UUID 状态的 Worker 生命周期及其余页面/长期断线/硬件完整验收仍需继续；整体目标未完成。

### 告警查询范围与详情接入（已随 f47eefa 发布）

- 模板编辑详情已由 service 接入 TanStack Query，不再向页面暴露 API 对象；并发读取合并，后续编辑重新获取最新数据。模板保存/删除只刷新模板缓存，规则保存/删除/应用只刷新规则列表；告警确认不失效查询，由现有 SSE 接收变化。
- 实际 Hook/QueryClient 测试验证上述刷新范围、确认不重启实时查询、详情并发合并及后续编辑重新读取，3/3 通过，已加入 `.github/workflows/build.yml` 原前端测试步骤，不改变缓存或制品路径。相关前端测试合计 15/15，类型检查、lint、格式和生产构建通过。真实服务集成验证单条及批量确认后原 SSE 连接上的变化推送，批量确认同时验证统计增加。证据 `build/alert-query-cache-test.log`、`build/alert-cache-contract-tests.log`、`build/alert-cache-integration.log`。
- `b958c74` 下述抓包属于修复前记录，仍包含保存后的 SSE 重建；本批线上证据以上节 `f47eefa` 为准。

### 最新部署与复验（2026-09-17，b958c74）

- 当前生产为 `b958c746d20b307f46ef59a3a73dbf8847158d25`，发布分支 `codex/http-sse-release-20260917-073129`，前端 `/fcde9f5b.js`。包含显式消息/实体/协议身份传入、告警表单空值回填及页面重复缓存失效清理。555 个文件匹配发布快照，原 HEAD 和暂存区未改变；Ruvia 两份依赖源码无改动。
- Linux Release 和完整 CTest 35/35、Windows 安装包 6/6（含 181 秒 SSE 重试预算）、前端类型检查/lint/构建/格式检查通过。证据为 `build/alert-release-{linux,native,source-verification}.log`、`build/alert-nullable-*.log`；消息身份的真实设备、串口、终端、工业协议及 GB28181 集成证据为此前 `build/message-identity-*-integration.log`。
- 制品 SHA256 `308abbaa37917f68233a6aa3465efeb0dac6a27b8612d101feca523727bec6a2`；运行二进制 SHA256 `bfeb88de9143ea8d4d9049cabaee83b11b46b6e3fc51e43094708241ffe32b6c`。备份 `/opt/iot/backups/before-b958c74-20260916T233250Z` 保留，数据库备份 9069311 字节。恢复到独立副本并两次迁移成功；切换后检查实际二进制和静态资源、就绪、迁移、UTC、Redis AOF 均通过。服务 active/running，NRestarts=0，生产 `.env` 权限 600。详见 `build/alert-release-production-{backup,stage,cutover}.log`。
- Chrome 与内置浏览器刷新到新版本。真实界面创建分类与描述均为空的独立模板，重新编辑名称与阈值成功，刷新页面再打开仍为 `888888`；最终删除自建模板，模板和规则列表恢复为空，未向设备应用模板。控制台无错误或警告。
- `build/alert-fixed-browser-network.json` 的 75.14 秒观察中，保存只有一次 PUT，随后规则和模板各一次 GET，实时记录和统计各一次 SSE 重建；后续约 58 秒没有新请求。此前保存的两轮列表查询已消除，本次窗口没有 favicon 请求。此证据仅覆盖本次告警页面操作与观察窗口，不替代全部页面和长期断线验收。
- 整体仍未完成：公共 UUID 运行状态的 Worker 生命周期、全仓实际职责审查、其余页面完整操作矩阵、长期断线及真实 Edge/视频硬件验收仍需继续。后文旧版本及“尚未部署”为历史记录。

### 最新部署与复验（2026-09-17，4e7dba6）

- 当前生产为 `4e7dba6034d15714dfb08be584caeaa26167e357`，发布分支 `codex/http-sse-release-20260917-064840`，前端 `/67d4df3f.js`。555 个工作区文件逐一核对匹配快照；原 HEAD 和暂存区保留。下节 `1c37b59` 及“尚未重新部署”属于先前记录。
- 本次交付包含 Worker 平台身份隔离、角色必填中文提示及历史数值显示修正。Linux Release 与完整 CTest 35/35；Windows Release 相关 6 项测试及采集器测试、Edge 默认/自定义平台集成、串口、终端 3/6、固件和设备 HTTP/SSE 集成通过。前端类型检查、lint、生产构建和修改文件格式检查通过；Windows 包含 SSE 重试预算的 6 项测试及安装包生成通过。
- 归档 SHA256 为 `07aaf21c06c6935e1e9ab48b85fe86b6da943ded6613891de79f4cf5e79aaa46`，服务器二进制 SHA256 为 `2c5001f0049244b5ed34bf3c2500e3a016fa45845bd9b25aa9a055ede6cb891e`。备份 `/opt/iot/backups/before-4e7dba6-20260916T224921Z` 保留旧二进制、静态文件、配置及数据库；旧版本 `1c37b59` 保留。
- 切换前检查哈希、架构及动态依赖，恢复数据库备份到独立副本并两次执行迁移。切换后就绪状态、实际运行二进制、前端资源字节、同快照 Windows 下载清单、迁移及 Redis 检查通过。证据为 `build/platform-identity-production-{backup,stage,cutover}.log`。
- Chrome 真实浏览器刷新后确认加载 `/67d4df3f.js`，空白角色表单显示中文名称/编码必填提示；未创建角色。设备页显示 239 台设备；`MB-000013` 的历史面板加载 134 条记录，其中同一时间的旧记录由 `4.7940000000000005` 显示为 `4.794 m³/h`，`4.858`、`5.012` 等尾数亦已修正。控制台无错误或警告。另一个内置浏览器标签页也已刷新。
- 这批修正已上线，但全仓职责审查、全部页面完整功能矩阵、长时间断线交互及真实 Edge/视频硬件验收仍未完成，不标记整体目标完成。

#### 同版本后续浏览器验收

- 告警模板实际浏览器验收发现空值缺陷：不填分类可新建，重新编辑后提交被接口拒绝为 `category 必须是字符串`。数据库以 NULL 保存空分类、描述和规则备注，页面原样回填造成请求类型错误。本地已将三处表单回填规范为字符串，并将响应类型声明为可空；尚未部署，不能视为线上修复完成。补入分类后，同一测试模板可更新名称和阈值，刷新页面重新打开仍为 `888888`，最后通过界面删除本次模板，模板和规则列表恢复为空；未向设备应用模板。
- 本次 75 秒被动抓包 `build/alert-template-browser-network.json` 确认模板保存使用 HTTP PUT；成功后出现重复规则/模板 GET，代码确认页面与 service 同时失效缓存。本地已删除规则保存、模板保存及模板应用成功回调中的重复失效，缓存刷新由 service 统一负责。前端类型检查、lint、生产构建、修改文件格式检查均通过（`build/alert-nullable-*.log`）；修复后的线上行为待同版本部署复验。观察中还包含认证刷新及 SSE 重建，不将本次操作期间抓包作为长时间空闲无轮询的证明。

- Modbus 导入：从已导出配置准备一个独立、禁用、未绑定设备的测试类型，通过真实文件选择器导入；重复选择同一文件后创建带 `(导入)` 后缀的独立配置，原配置未被覆盖。错误配置先验证字节序校验，再用第一项合法、第二项协议为 S7 的文件验证整体拒绝，页面显示第二项协议类型错误且第一项没有被保存。最后通过界面删除两条自建配置，树恢复为原有 9 条，控制台无错误或警告。此项没有制造服务器保存失败，不覆盖导入中途服务器故障的部分成功分支。

- 部门页新增独立测试部门 `audit_4e7dba6`，修改名称和排序为 7，刷新页面后实际行仍显示新值，随后通过界面删除该测试部门；最终树和表格均为 0 条，未改动其他部门。浏览器控制台无错误或警告。
- Chrome 在窄屏协议页点击 Modbus 导出，页面显示“已导出 9 条配置”。实际文件位于系统重定向下载目录 `G:/Users/Downloads/Modbus_configs_20260916 (3).json`，7968 字节，解析为 9 条 JSON 记录；顶层字段为 `protocol,name,enabled,config,remark`，不含内部 ID 或创建/更新时间。浏览器工具的 download 事件等待曾超时，但实际下载文件可读取，不能据该等待超时认定产品导出失败。重复验收产生了同名编号下载文件，未删除下载目录中的文件。
- 本节只证明部门新增/编辑/刷新持久化/删除和 Modbus 实际导出；其他协议导出、导入部分失败、用户及角色完整 CRUD、长期断线和硬件操作仍待完成。

### 当前部署与浏览器验收（2026-09-17）

后续本地改动尚未重新部署：Worker 平台身份配置隔离和角色表单中文必填提示正在验收。线上仍为下述 `1c37b59`，不能将本地测试结果视为线上新版本证据。

补充线上只读验收：Chrome 筛选 `MB-000282` 后历史面板正确显示空结果；切换 `MB-000013` 后历史查询实际加载 132 条记录，翻页后显示下一批更早时间记录，控制台无错误或警告。发现历史数值直接显示浮点尾数（如 `4.7940000000000005`），展示精度仍需处理；此项不代表所有历史范围和导出场景已验收。

历史显示修正已在本地完成，待部署复验：只读检查该设备 Redis 元数据确认 `scale=0.001`、`decimals=-1`，实时字符串为 `4.794`。历史页保留缩放元数据，仅当数值距离对应精度的舍入结果不超过相对浮点误差时去除尾数；更细的小数、极小数和原始字符串保持原值，显式小数位设置仍优先。实际页面函数验证了上述边界及科学计数缩放，类型检查、lint、格式检查与生产构建通过（`build/history-precision-*.log`）。未改存储值或设备协议。

以下为最新结果，后文保留改造过程；其中早期“尚未部署”“上下文修正待构建”等记录已由本节替代，不能据此认定所有架构审计已完成。

- 已通过本机 WSL 制品部署到 `192.168.5.100:10050`，源码快照为 `1c37b595568a570670d9031e3b2bd8591ec38b3a`，分支 `codex/http-sse-release-20260917-060932`。原工作区 HEAD 和暂存区保留；发布前逐文件核对 555 个源码文件与快照一致。
- 最终 `RequestContext` 修正后的 Linux Release 与 CTest 35/35、Windows Release 与相关 CTest 3/3 通过。新二进制的系统/业务 ORM、认证 HTTP/SSE、设备 CRUD 与旧编码保存、SL651 OFFSET、串口、终端协议 3/6 集成全部通过，夹具 `build/system-orm-fixture-2c2b0a11f74e461d937e84c922b23f08`，日志 `build/http-sse-context-integration.log`。
- 前端最终类型检查、lint、生产构建通过；lint 保留一条既有模板字符串信息提示。Windows CMake 完成原生 HTTP/SSE 探针、6/6 CTest、WinUI 自测、文件校验和安装包生成，安装包元数据使用同一源码快照。两份 Ruvia 源码工作区均无改动。
- 生产备份在 `/opt/iot/backups/before-1c37b59-20260916T221011Z`。切换前校验制品哈希、x86_64 架构和动态依赖，并恢复数据库备份到独立副本、连续执行两次迁移。切换后验证运行二进制、首页与 JS 实际返回字节、Windows 下载清单、迁移 0050、数据库 UTC、Redis AOF 和 readiness。当前 `iot.service` 为 active/running，重启计数 0；旧版本与备份保留。
- Chrome 真实页面显示 239 台设备；测试设备 `MB-000282` 保持原字段提交，编辑表单正常关闭。链路调试窗口显示真实 TX/RX 报文，暂停、恢复实时跟随、关闭窗口均已操作。FINS 独立禁用测试配置完成新增、修改及删除，页面分别显示更新成功、删除成功，未留下测试配置。
- 75 秒服务端被动网络观察仅记录请求方法、路径和 SSE 事件数量，不记录认证头或业务载荷。Chrome 刷新后建立设备相关 SSE，空闲期间没有重复业务 GET、没有 favicon 请求，实时流收到 24 次快照。另一个旧标签页曾继续重试 `/v1/channel`；已确认它加载 `/4c0d1e41.js`，刷新后使用 `/98cd579a.js`。刷新后的第二次 75.12 秒观察没有任何新 HTTP 请求，设备实时流继续收到 28 次快照，旧 WS 重试已停止。原始元数据见 `build/http-sse-browser-network.json`、`build/http-sse-browser-idle-after-refresh.json`。
- 内置浏览器宽屏验证 219 个客户端地址提示框：实际可视高度 320px，内容高度 4856px，使用内部滚动，不再撑高页面。Chrome 窄屏验证设备页和链路表格的滚动与操作入口。
- 仅对本次浏览器的设备实时 SSE socket 主动断开一次，未重启生产服务、未断开采集设备。随后 75.05 秒观察中只有一次 `/v1/device/realtime/events` 重连 GET，新连接收到 21 次快照，没有普通 GET 轮询、favicon 或统一 WS 请求；浏览器页面继续显示设备数据。证据 `build/http-sse-browser-disconnect.log`、`build/http-sse-browser-reconnect.json`。此项覆盖单连接中断恢复，长时间网络不可用及重试耗尽的线上交互仍待验收。
- 已打开设备、链路、边缘节点、六种工业协议、角色、部门、用户、开放接入、告警和 GB28181 页面；角色/用户真实记录已加载，保留超级管理员禁止编辑/删除行为。内置浏览器退出后显示登录页，使用授权账号重新登录并回到原链路页面。页面打开不等于全部功能验收：部分首次快照取得早于数据请求完成，必须继续检查最终数据、表单和操作；全部 CRUD/导入导出及完整性能验收仍未完成。角色空表单能阻止提交，但未填写字段显示英文 `Invalid input`，错误提示仍需改进。
- 当前线上无边缘节点，GB28181 未启用，因此不能声称已通过真实硬件串口/终端、固件升级或视频播放验收。本地模拟协议集成结果与线上实机结果分别记录。全仓结构、实体实际使用、UUID/平台配置归属等剩余项目仍以 `docs/file-responsibility-audit.md` 为待办，不以本次部署代替整改完成。

### HTTP/SSE 改造进度（2026-09-17）

- 后端调试解析、类型校验、操作分发及连接生命周期收回边缘节点 Controller；删除 `middleware/api_channel.h`、`middleware/ws_event.h` 和无调用的 `common/event.h`。删除全局控制器工厂、线程局部注册表、自动注册宏和无调用的附加权限分支；每条调试连接显式装配 12 项鉴权/串口/终端操作，注册表与会话均由接入 Worker 执行和销毁。
- 第一轮 Windows Release 与真实服务串口/终端协议 3/6 联调通过（`build/edge-debug-dispatch-integration.log`，夹具 `build/system-orm-fixture-346a91bf52ed4436af74d7ab1077388c`）。结构检查指出 Controller 内的 I/O 上下文适配，现已复用公共 `RequestContext` 承载独立内存区域与取消信号，源码结构检查通过；此修正后的两平台构建、专用请求测试与后续联调待完成。CMake 测试改为 `edge-debug-request`，保留解析、验证和 Worker 归属检查。
- Linux 重新配置曾误用默认编译器，已中止并显式设置 `CC=/usr/sbin/clang CXX=/usr/sbin/clang++`，对应依赖恢复完成；后续日志 `build/edge-debug-context-linux.log`。尚未部署，线上真实浏览器逐页验收仍待执行。

- 系统 ORM 测试恢复真实 HTTP 登录/刷新/查询/写入，保留权限、参数化、空值、部门循环、用户角色替换和数据库故障回滚断言。业务 ORM 测试恢复 HTTP，并保留第三方 SSE、协议数字/布尔、设备共享链路编辑、Webhook Header 与 PostgreSQL 语义对照、分组递归等覆盖；两项独立服务联调通过（`build/system-orm-http-restored.log`、`build/business-orm-http-restored.log`）。删除旧 `api-channel-integration.ts`、`auth-event-integration.ts`、`ws-event-fixture.ts` 和通知型 `live-query-integration.ts`；现行 HTTP/SSE 测试承担对应业务与权限验证，专用调试连接测试承担连接生命周期验证。
- CI 前端清单加入 HTTP 客户端和 SSE 订阅测试，修正边缘分组界面断言中的旧 WS 操作名；完整清单 93/93 通过（`build/frontend-ci-audit.log`）。默认系统联调入口已改用现行 HTTP/SSE 测试，五项组合验证全部通过（`build/http-sse-default-integration.log`，夹具 `build/system-orm-fixture-e090476493424944911199a43b65cdde`）。后端调试框架归属、最终完整审计、部署与线上真实浏览器验收仍未完成。

- 前端专用调试连接与订阅实现已收回 `edge_node.api.ts`，公开类型在 `edge_node.types.ts`，认证恢复由所属 Service 在应用入口接入；删除公共层 `api-channel.ts`、`snapshot-subscriptions.ts`，公共快照请求层仅保留 SSE。测试改为串口/终端操作，并改名为 `edge-debug-connection`、`edge-debug-subscriptions`、`edge-debug-refresh`，同步 CI 路径。
- 调试连接自动重试最多 9 次，短暂成功不会重置预算，稳定连接 30 秒后可重置；到达上限释放订阅并报告状态，手动重开可恢复。20 项连接/订阅/结构/契约测试通过，包括模拟计时器验证重试上限与手动恢复；类型检查/lint/生产构建通过。真实服务串口与终端协议 3/6 联调再次通过（`build/edge-debug-module-integration.log`，夹具 `build/system-orm-fixture-26ce570b77fd45f498e6646a7ce202d3`）。后端旧通用调试框架归属与其他旧 WS 测试、完整验收和部署仍未完成。

- 删除专用调试连接残留的 `ConnectionUploads`、上传计时器/权限集合，以及临时文件对象的旧 WS Base64 分块方法、目标/请求体/用户等无用状态；HTTP 上传继续采用原始字节、大小/摘要校验与文件锁恢复。删除 `web/lib/event-request.ts`，仅供串口/终端使用的请求错误处理归入边缘节点 API。
- 更新旧“禁止 GET/SSE”契约测试以符合 HTTP/SSE 分工：专用 WS 只允许调试事件，前端禁止定时 refetch 和通用 WS 地址。11 项结构/契约测试、3 项前端上传/终端测试、类型检查/lint/构建、Windows Release 与 3 项 CTest、跨进程上传锁/异常退出恢复均通过。真实服务复验启动恢复、HTTP 上传/权限撤销/旧固件下载、串口以及终端协议 3/6，通过日志 `build/debug-upload-cleanup-integration.log`，夹具 `build/system-orm-fixture-49b5940249f34bdc981280d265d22393`。Linux Release 与全量 35/35 CTest 已通过（`build/debug-upload-cleanup-linux.log`）；尚未部署。

- 串口与终端切换到 `/v1/edge/debug` 专用连接，只注册调试鉴权和串口/终端事件；删除 `/v1/channel`、WS 登录/刷新/退出/当前用户处理及其认证 DTO。登录模块不再持有调试认证入口，前端专用连接鉴权由边缘节点模块接入。
- Windows Release、前端类型检查/lint/生产构建与 5 项客户端测试通过。真实服务联调确认旧入口 404、专用连接拒绝普通业务事件、串口收发/监听、终端协议 3/6、连接归属及显式/断线/HTTP 退出后客户端关闭的资源回收；HTTP 认证和 SSE 初始快照/变化/过期验证通过。日志 `build/edge-debug-integration.log`，夹具 `build/system-orm-fixture-90a61e0a12604694abc933fa337ffe81`。该轮 Linux Release 与 35/35 CTest 通过（`build/edge-debug-linux.log`）。旧通用框架、前端模块归属及旧测试清理未完成，尚未部署或进行本轮线上浏览器验收。

- HTTP 上传切换后，跨进程文件锁/强制终止回收测试与实际 Service Worker 启动恢复再次通过；保留活跃上传、最终 `.bin` 与非本服务命名文件，重复恢复幂等。日志 `build/http-upload-recovery.log`、`build/http-upload-startup-recovery.log`，启动夹具 `build/system-orm-fixture-e4102f432d2d4ac9b2da295cb8cc87f5`。
- 删除前端旧事件请求层重复的认证刷新注入，所有刷新使用 HTTP 入口；15 项会话/HTTP/SSE 测试、类型检查、lint、生产构建和格式检查通过（`build/http-refresh-unified-tests.log`）。串口/终端仍暂用旧连接，专用调试入口和统一 WS 基础设施清理未完成，未部署。

- 固件管理上传已切换为 `POST /v1/edge/:id/firmware` 单次 HTTP 原始字节流，查询参数校验文件名、大小与保留配置选项；接入 Worker 完整读取请求，以受限内存块写入加锁临时文件，校验大小与 SHA-256，结束前重新鉴权。删除固件上传的 WS start/chunk/finish/abort 路由与包装 DTO，前端保留上传进度且不自动重放失败写操作。设备端固件分块/旧版本令牌下载协议保持兼容。
- Windows Release、api-upload/edge-service/architecture 三项 CTest、前端检查与 8 项上传/HTTP 客户端测试通过。真实服务联调覆盖 2 MiB 以上上传、存储字节/摘要对照、刷写命令、声明大小边界、未登录、断线清理、上传中权限撤销阻止登记，以及 0.3.44 令牌下载与错误令牌拒绝。日志 `build/http-firmware-integration.log`，夹具 `build/system-orm-fixture-7a5bc6ed6aed4ebc853754511283bf95`。Linux Release 与 35/35 CTest 已通过（`build/http-firmware-linux.log`）；上传恢复已验证，专用调试 WS、旧统一通道清理与线上浏览器验收仍在后续推进，尚未部署。

- 为固件 HTTP 流式上传补充上传字节进度、取消和连接错误处理，共享现有 HTTP 鉴权与响应处理；15 项客户端测试、69 个断言通过（`build/http-sse-upload-client-tests.log`），前端检查通过。业务上传入口暂未切换；后续接入单次 HTTP 请求，保持接入 Worker 归属、临时文件锁、大小/摘要校验与失败清理，设备端分块协议保持兼容。

- 边缘节点普通管理接口恢复 `/v1/edge` GET/POST/PUT/DELETE，列表、详情、分组、固件目录和日志使用显式 `/events` SSE；删除这些普通操作的 WS 注册、包装 DTO 和专用事件校验。前端对应 API 同步切换，串口/终端与上传入口仍待后续迁移，设备固件连接及令牌下载协议保持原行为。
- Windows Release、前端类型检查/lint/生产构建/格式检查与 edge/architecture 7 项 CTest 通过。真实服务与模拟边缘设备联调通过分组 CRUD/循环约束、重命名推送、分组筛选、注册删除保护、分页和类型校验、配置指令/审批/同步的操作者归属、日志 protobuf 回执与筛选、心跳不采集日志、权限撤销中断已建立 SSE。日志 `build/http-sse-edge-integration.log`，夹具 `build/system-orm-fixture-03b2cc0a6eb44a76b1f87b185a842bf0`。Linux Release 已通过；重新编译更新的 edge-service-test 后，35/35 CTest 通过（`build/http-sse-edge-final-linux.log`）。未部署，线上浏览器验收未完成。

- 原生客户端 SSE 断线重试限制为 9 次；短暂收到快照不会重置失败次数，稳定连接 30 秒后才恢复重试预算。达到上限停止隧道并等待用户手动连接/同步，不进行后台查询或重连。新增实际退避时长验收：9 次首帧后断线、停止后再观察 31 秒、手动同步恢复订阅；Windows Release 重新编译与 6/6 CTest 通过，日志 `build/http-sse-native-retry-tests.log`（约 181 秒）。

- 原生 Windows 平台适配已改为 WinHTTP HTTP/JSON，配置订阅使用 `/config/events` SSE；取消信号合并调用方与会话生命周期，关闭会话后允许新请求建立。删除无生产调用方的 WS 传输实现及其旧协议测试，探针和 CMake 验证入口更名为 HTTP/SSE。
- Windows 客户端 CMake 完整构建、5/5 CTest、HTTP/SSE 传输验证及 WinUI 界面/包验证通过。原生探针与真实后端联调验证登录/刷新、查询、创建/修改、变化推送、取消、撤销和重新请求，日志 `build/http-sse-native-backend.log`；构建和测试见 `build/http-sse-native-final-build.log`、`build/http-sse-native-final-tests.log`。尚未部署；客户端重连边界、边缘节点余下迁移、统一 WS 清理及线上全页面浏览器验收继续进行。

- VPN 管理与桌面接入恢复 `/v1/vpn` 的 GET/POST/PATCH/DELETE；网络、Peer、路由、桌面配置和会话通过显式 `/events` SSE 推送。令牌注册仍独立使用免登录的 `/v1/vpn/client/enroll`，桌面权限组合和 Service 内地址回收逻辑保持原行为。移除 VPN WS 入口、更新包装 DTO 与仅为 WS 存在的校验包装；边缘节点页面的 VPN 调用同步切换 HTTP/SSE。
- VPN Windows Release、architecture/vpn-cidr CTest、前端类型检查/lint/生产构建/修改文件格式检查通过。真实 PostgreSQL/Redis/HTTP/SSE 联调通过 GET 与 SSE 初始快照一致性、查询/路径校验、权限拒绝、在线状态推送、历史地址回收、并发分配与复用、迟到撤销隔离、令牌幂等注册、跨用户隔离、路由变化与权限撤销。日志 `build/http-sse-vpn-integration.log`，夹具 `build/vpn-desktop-fixture-664fa0e7364143789024002bd4ea06cd`。Linux Release 与 35/35 CTest 通过（`build/http-sse-vpn-linux.log`）；尚未部署，线上浏览器验收未完成。

- 视频管理恢复 `/v1/gb28181` HTTP 路由：配置普通 GET，健康/设备/流/录像状态提供显式 `/events` SSE，控制操作使用原有 POST/PUT/DELETE 路径。路由参数通过 Ruvia 类型化校验，JSON 字段由纯 schema 解析；移除视频统一 WS 事件入口，保留原 Service 与 Collector Redis RPC 边界。
- 视频前端已切换 HTTP/SSE；退出时停止预览使用带认证的 HTTP `keepalive`，预览定时心跳仅续租、不查询业务数据。HTTP/SSE/会话测试 12 项、59 个断言通过；前端类型检查、lint、生产构建及修改文件格式检查通过。Windows Release 与 schema/architecture/media/projector 等 5 项 CTest 通过（`build/http-sse-gb-ctest-windows.log`）。
- 真实双 Collector 联调通过 HTTP 云台指令、设备名称提交后响应及 SSE 更新、参数边界、非空目录/录像投影、原连接 Worker 归属、31 秒数据库阻塞顺序、迟到重试、收据恢复及租约丢失隔离；新增预览创建/续租/停止验证，确认 SIP INVITE 到达原 socket 并关闭对应 RTP 服务。日志 `build/http-sse-gb-integration.log`，夹具 `build/rpc-integration-fixture-9f4c8012bef5460cb59e9818063afc6c`。此测试未发送真实摄像头媒体，不替代线上播放验收。
- 视频 Linux Release 与 35/35 CTest 通过（`build/http-sse-gb-linux.log`）。剩余边缘节点/VPN/原生客户端、认证与统一 WS 基础设施清理、布局性能及线上浏览器全页面验收尚未完成，未部署。

- 开放接口管理恢复 `/api/device/options`、`/api/open-access-key`、`/api/open-webhook`、`/api/open-access-log` 的 HTTP 查询与写入；调用配置、Webhook、访问日志通过各自 `/events` SSE 推送，设备选项使用普通 GET。删除管理端统一 WS DTO/校验包装，分页溢出与参数结构校验保留。
- 第三方 `AccessController` 从类声明到文件末尾按 SHA256 核对与本轮改动前一致；仅管理 Controller 迁移。`tests/access-http-sse-integration.ts` 替代旧事件测试，实际验证密钥/Webhook CRUD、密钥轮换立即失效、部分更新/空值清除、请求头与 PostgreSQL JSONB 对照、权限/设备 ACL、使用时间与日志变化 SSE；第三方 HTTP/SSE 查询、命令实际写入回读及 MC/FINS/DLT645 回归通过。日志 `build/http-sse-access-integration.log`，夹具 `build/system-orm-fixture-a3372d58530848638f9e7f32a3b55a5a`。
- 本轮 Windows Release 与 architecture/access/access-webhook/access-service 四项 CTest、前端类型检查/lint/生产构建/修改文件格式检查通过；Linux Release 与 35/35 CTest 通过（`build/http-sse-access-linux.log`）。尚未部署，边缘节点/视频/VPN/原生客户端、统一 WS 基础设施清理与线上逐页浏览器验收仍待完成。

- 死信管理恢复 `GET /v1/system/outbox/dead-letters`、`POST /v1/system/outbox/dead-letters/:id/replay`，变化订阅使用 `/dead-letters/events` SSE；已删除这部分统一 WS 事件。保留当前 Worker 计数及数据库事务，没有引入后台查询轮询。
- 死信 HTTP/SSE 与系统管理真实联调通过：未登录/无权限拒绝、路径校验、死信新增及重放推送、写入失败不确认、计数持久化失败回滚、Redis 暂停不影响已提交重放、并发重复重放恰好提交一次。日志 `build/http-sse-outbox-integration.log`，夹具 `build/system-orm-fixture-4f9f02d5b5e74ccd93cf523f780ee680`。Windows Release/architecture 检查通过，Linux Release/CTest 35/35 通过（`build/http-sse-outbox-linux.log`）。
- 告警前后端已切换为规则/模板普通 HTTP 查询与写入，告警记录、分组统计及总览使用独立 SSE；移除 WS 更新包装及无用途 DTO。HTTP 客户端补充批量 DELETE JSON 请求体，12 项客户端测试、57 个断言通过。前端类型检查、lint、生产构建和格式检查通过。告警 Windows Release 与 architecture/alert-runtime/link-service/protocol-service 四项 CTest 通过。实际服务联调覆盖规则/模板 CRUD、阈值/变化率/位条件、触发及确认 SSE、统计/分组 SSE、恢复、重复消息和变化存储，并验证 12 个无权限 HTTP/SSE 操作拒绝；日志 `build/http-sse-alert-integration.log`，夹具 `build/system-orm-fixture-103082d0f9a946d6a63958d93c9452a1`。Linux 最终构建与 35/35 CTest 通过，日志 `build/http-sse-alert-linux.log`。

- 协议配置恢复 `/v1/protocol/configs` 的 GET/POST/PUT/DELETE；前端普通 TanStack Query 查询，不建立实时订阅或定时刷新。Controller 通过 Ruvia 公开 `jsonValue()` 读取请求体，再交给 schema 纯函数校验，保持大整数、指数表示、部分更新与 null 清除语义，未修改 Ruvia 源码。
- 链路管理恢复 HTTP CRUD、选项、枚举和调试开关；列表变化与调试报文分别通过 `/v1/link/events`、`/v1/link/:id/debug/packets/events` 推送。链路与协议 API/Service 已去掉统一 WS 调用。
- `protocol-http-integration.ts`、`link-http-sse-integration.ts` 取代对应 WS 测试；设备、工业协议及 SL651 偏移测试的准备操作也改为真实 HTTP。五组隔离服务联调全部通过，覆盖权限、所有权、精确 JSON 数值、失败写入隔离、链路/报文变化推送、400 台设备快照、指令写入回读及第三方接口兼容。证据：`build/http-sse-protocol-link-integration.log`，夹具 `build/system-orm-fixture-bff44238097646d2b6c28c64f3330b1d`。
- schema 职责修正后，Windows Release 复建及协议 HTTP/SL651 偏移联调再次通过：`build/http-sse-protocol-final-integration.log`，夹具 `build/system-orm-fixture-12153b1964e1426f8aa6e02f295d0198`。Linux 最终构建与 35/35 CTest 已通过，日志 `build/http-sse-protocol-link-final-linux.log`。两平台 Ruvia 源码工作区检查为空。
- 本轮 Windows Release、前端类型检查/lint/生产构建/修改文件格式检查通过；HTTP/SSE/刷新会话客户端测试 12 项通过。Linux Release 已构建通过，随后按结构检查将请求体读取移回 Controller，最终复建和 35 项 CTest 均通过。Windows CTest 初次 33/35，schema 职责修正后 architecture 单独复测通过；GB28181 IPv6 UDP 仍失败，独立 Python `::1` UDP 回环也超时，记录为本机网络环境限制，未跳过测试。Linux 初次 34/35，仅同一 schema 结构检查失败，GB28181 SIP 通过。尚未部署；剩余统一 WS 模块、原生客户端、结构审计与线上全部页面真实浏览器验收未完成。

- 设备、分组、分享、历史查询和指令已迁移为 HTTP；列表 `/v1/device/events`、实时值 `/v1/device/realtime/events`、调试报文及指令状态使用显式 SSE 入口。设备前端及 Device/Command Controller 已移除统一 WS 事件调用；指令保持幂等键，GET 批量状态订阅按 64 个 ID 分组限制 URL 长度。
- 认证身份与短生命周期内存统一由 `middleware/request_context.h` 的 `RequestContext` 持有，每次 SSE 查询重新创建，继续在接入 Worker 执行。原 `SnapshotContext` 已移动并更名，无兼容别名。
- 遥测变化主题改为 `device.realtime`，配置订阅不再被每次遥测唤醒；第三方 `access` 订阅仍接收该主题。实际 SSE 隔离测试通过：实时流推送上报时间变化，配置流保持心跳（`build/http-sse-device-isolation-integration.log`；夹具 `build/system-orm-fixture-af49f09cf5f04e379ee672a9419046e9`）。页面合并实时值后保留未变化设备对象，减少卡片无关重绘，实际浏览器性能尚待验证。
- `tests/device-http-sse-integration.ts` 取代旧设备 WS 测试，覆盖 HTTP CRUD、原有连字符编码保存、历史、调试 SSE、用户/分组分享与撤销、400 台设备大快照。协议模拟联调通过 MC 3E/4E、FINS TCP、DLT645 1997/2007 的 HTTP 下发/SSE 回执、写入和回读；第三方 HTTP/SSE 接口回归通过。日志 `build/http-sse-device-command-integration.log`，夹具 `build/system-orm-fixture-9c3ee6f4efe348c8bf07d6fd13b5b956`。该次测试的协议准备当时仍使用 WS；已在上方最新一轮迁移为 HTTP 并重新通过。
- 本轮前端类型检查、lint、生产构建、修改文件格式检查及 12 项客户端测试通过。Windows 与 Linux Release 构建通过（`build/http-sse-device-command-build-windows.log`、`build/http-sse-device-command-build-linux.log`）；现有 Linux CTest 35/35 通过（`build/http-sse-device-command-ctest-linux.log`）。尚未部署，统一 WS 其他模块与原生客户端迁移、完整线上浏览器验收仍未完成。

- 认证页面已使用 HTTP 登录、刷新和退出；`GET /v1/auth/me` 返回普通 JSON，`GET /v1/auth/me/events` 提供当前用户 SSE。其他尚未迁移模块和原生客户端仍引用统一 WS 会话入口，该入口及 WS 订阅基础设施尚待清理，不能宣称已完成统一 WS 移除。
- SSE 每次查询使用独立 `SnapshotContext` 内存作用域，连接不按固定时间断开。初始订阅及变更通知触发查询；心跳只检查令牌并保活。Redis 同一批变更按主题合并通知后再确认，避免逐条确认期间重复唤醒同主题查询。
- 前端 SSE 复用同地址订阅，最后一个观察者退出时取消；换账号取消旧请求并拒绝迟到响应，真实断线才有界退避重连。HTTP/SSE/刷新会话 12 项客户端测试、55 个断言通过（`build/http-sse-auth-client-tests.log`）。
- Windows 与 Linux Release 构建通过（`build/http-sse-auth-build-windows.log`、`build/http-sse-auth-build-linux.log`）。隔离真实服务通过系统管理 HTTP 回归、认证 HTTP/SSE 推送及令牌过期、HTTP 登录限流测试（`build/http-sse-auth-integration.log`；夹具 `build/system-orm-fixture-5432810db6ad42789e5c0f3875bdb553`）。前端类型检查、lint、生产构建通过，现有终端字符串拼接 lint 提示仍在。以上尚未部署或进行本轮线上浏览器验收。

- 角色、部门、用户管理的 Controller 已恢复标准 GET/POST/PUT/DELETE，参数分别由查询、路径与 JSON 校验接入；保留当前 Worker 内 Service 与数据库实现。
- 对应前端查询改用 TanStack Query 普通查询，关闭定时刷新；用户表单角色选项经角色模块公开 Service 获取。HTTP 客户端保留空查询参数，避免 `parent_id=` 的根部门过滤语义丢失。
- 前端类型检查、lint、生产构建及 HTTP 客户端 3 项测试通过；lint 存在原有终端字符串拼接提示。`tests/system-http-integration.ts` 已通过真实本地服务的权限、CRUD、根部门过滤和循环父级保护验证，日志 `build/http-sse-system-integration.log`，隔离夹具 `build/system-orm-fixture-3d9349a750ab4df58f1a8cf9d3cbf03b`。
- Windows 与 Linux Release 服务构建通过（`build/http-sse-system-build-windows.log`、`build/http-sse-system-build-linux.log`）；现有 Linux CTest 35/35 通过（`build/http-sse-system-ctest-linux.log`）。认证、设备与其他模块、实时 SSE、串口/终端及 Windows 客户端仍须迁移；当前中间状态尚未部署，不能作为完整验收结果。


**最新方向（2026-09-17）**：用户已取消统一业务 WS，`AGENTS.md` 第 4 节改为标准 HTTP 方法与按需 SSE 推送，禁止前后端业务轮询。以下 WS 记录仅用于追踪已做工作与回归证据，不再作为目标架构。线上当前为 `0614933`，设备大列表已通过真实浏览器验证（239 台），角色创建/删除成功，临时角色已清理；HTTP/SSE 改造与完整浏览器验收尚未完成。继续处理设备保存（现有 `MB-...` 编码被前后端字母数字校验拒绝）、卡顿/实时延迟、favicon `no-cache` 重复验证。当前保存与图标修正仅在工作区，尚未发布。

2026-09-17 线上验收：`f32871d` 制品已完成备份恢复、两次迁移预演和服务/静态资源校验，但真实浏览器发现设备列表与实时数据返回 `10004`，已立即回滚至 `225719b`。原因是自有 `ServerConfig.maxWebSocketMessageBytes=64 KiB` 同时限制出站回复。调整框架传输上限至 16 MiB，保留自有请求解析器 64 KiB 入站校验，不修改 Ruvia。新增 400 台设备的大列表/实时快照回归：原版本复现失败，修改后通过；另验证超长入站请求仍关闭连接。设备页面补充明确错误与重新加载入口。修正版尚待重新发布及完整线上浏览器验收。

部署验收必须通过真实浏览器逐页执行，覆盖查询、写操作、推送更新、断线重连、串口、终端、上传以及控制台和网络请求；不能以构建或接口测试替代。需要实际设备才能完成的项目单独记录结果和限制。

上传暂存恢复已补充：`UploadedFile` 使用内核文件锁持有暂存身份，目录锁覆盖创建与恢复竞争；每个 Service Worker 以相同机制在启动时恢复，新上传开始时也尝试回收。仅处理普通文件且文件名为 UUID 的 `.upload`，跳过仍被其他 Worker/进程持有的文件，不删除 `.bin` 或无关文件。Windows 与 Linux 原生跨进程测试均验证活跃保护、强制终止后的回收、重复回收和已登记镜像保留（`build/ws-upload-recovery-process-windows.log`、`build/ws-upload-recovery-process-linux.log`）。真实服务启动及上传/取消回归通过，日志 `build/ws-upload-recovery-integration.log`，夹具 `build/system-orm-fixture-702c8af5e5494e6e8e8c97be39750803`；Windows Release、`api-upload` 与 `ws-event` 通过。Linux 服务重建完成，更新后的 `ws-event-test` 已重建，全部 35 项 CTest 通过（`build/ws-upload-recovery-ctest-linux.log`）。数据库结果不确定的最终 `.bin` 仍按保护已提交数据的策略保留，尚不能宣称最终文件对账已验证。

前端当前 CI 列表 22 文件共 81 项测试通过（`build/ws-current-web-contracts.log`）。Windows 安装包的 `windows-native-tests` 现依赖原生 WS 集成门禁；CI Windows 客户端任务准备固定 Bun 并将路径传给 CMake，平台、缓存及制品路径未变，本地完整门禁通过（`build/windows-ws-package-gate.log`）。

Windows 客户端业务已接入 `PlatformEventConnection`：`WinHttpPlatformVpnApi` 在单一 WS 上执行登录、刷新、认证恢复、查询、写入与配置订阅。每次请求生成新 UUID，接收线程只分发响应，业务回调仍由原调用线程执行；订阅取消独立发送 `subscription.cancel`，认证更新结束旧订阅以触发重新订阅，注销/服务停止关闭认证连接。旧 HTTP/SSE 请求、SSE 解析器及其旧解析测试已删除，IPC、状态文件和 WireGuard 身份未改变。原生客户端对本地 WS 测试服务通过登录/刷新/查询/增改删、并发推送、取消后继续请求、连接关闭后认证恢复及刷新结束旧订阅验证；传输测试覆盖 120 KiB 文本、取消挂起接收、重复关闭、拒绝二进制/超长/关闭响应。统一 CMake 入口 `windows-event-transport-test` 同时运行传输与业务探针，日志 `build/windows-platform-events-cmake-test.log`；客户端 5 项 CTest 通过（`build/windows-platform-events-ctest.log`），包含注销清理认证连接断言。此段为模拟对端结果；真实后端与完整安装包进展见下文，TLS、异常网络恢复及部署后逐页浏览器验收仍待完成。WinHTTP 收发并行及关闭约束依据 [Microsoft 并发说明](https://learn.microsoft.com/en-us/windows/win32/winhttp/concurrency-in-winhttp)。

Windows 原生客户端已与真实 C++ 服务联调通过：独立 PostgreSQL/Redis 中完成登录、刷新、设备列表、客户端创建、配置查询、设备选择更新及推送、取消订阅、撤销错误、关闭后认证恢复，并核对客户端记录已持久化为 revoked。联调前对照类型校验发现更新事件必须使用 `{id,configuration}`，已修正原生适配并增加模拟端断言。日志 `build/windows-native-backend-integration.log`，夹具 `build/system-orm-fixture-b5623c55a4ca40d78aaa1503fa2d7bbd`。完整 CMake 安装包构建通过，包含 5 项客户端 CTest、WinUI 自动化和文件校验；产物 `build/iot-egine-Setup-0.7.4-x64.exe`，SHA256 `668fc7f884b2c186e65703fb8f5d459bc07d1f705d226887f27985605ea81ae6`，日志 `build/windows-ws-installer-build.log`。尚未进行安装/卸载生命周期或真实 TLS/异常网络验证。

部署预检：192.168.5.100 免密 SSH 可用，iot.service 为 active，现有入口仍指向 `/opt/iot/releases/225719b/iot-engine`；`.env` 权限 600，未读取内容。WSL 制品要求 GLIBC 2.38、GLIBCXX 3.4.35，服务器提供 GLIBC 2.43、GLIBCXX 3.4.35 与 libatomic；仍须上传后用目标机实际动态加载核验。此前 Linux 全量增量构建与全部 35 项 CTest 已通过（`build/ws-complete-build-linux.log`、`build/ws-complete-ctest-linux.log`）；随后增加的上传恢复修改按上文单独重建复测。尚未切换生产或进行部署后逐页浏览器验收。

终端已注册 `edge.terminal.open/events.subscribe/write/resize/keepalive/output.ack/close`，通过 `edge-terminal` RPC 绑定用户、业务连接与节点完整会话身份；旧 `/edge/v1/terminal`、浏览器票据和定时读取实现已删除。输入按 4 KiB 分块，收到设备确认通知才继续；输出单一订阅按 `more` 排空已知积压，连续流不按内容去重。输入入队原子校验就绪、关闭标记、序号与上一包确认，关闭队列满保留可重试身份。输出、输入确认和失败关闭与 Redis 通知原子提交；输出队列限制 64 帧，每次 RPC 最多读取 32 帧。内部队列实际使用 Protobuf 编码，旧固件无序号数据和打开票据字段保持兼容。

最新终端验证：Windows Release 构建及 `edge-service`、`ws-event`、`edge-serial-debug`、`api-upload` 四项 CTest 通过。真实 C++/PostgreSQL/Redis/模拟设备联调覆盖协议 6 与旧协议 3、输入分块及确认背压、输出浏览器确认、同内容帧、40 帧积压分批排空、重复订阅拒绝、跨连接拒绝、窗口调整、显式/断线/注销关闭；并通过串口和统一通道取消边界回归。日志 `build/ws-terminal-events-regression.log`，夹具 `build/system-orm-fixture-b66d9166cf804391ae356d526ee53448`。窗口调整顺序发送并合并待发送尺寸；前端类型检查、lint、生产构建通过，日志前缀 `build/ws-terminal-resize-`。Linux Release 构建和全部 35 项 CTest 通过（`build/ws-terminal-events-ctest-linux.log`）；前端 14 项相关测试通过（`build/ws-terminal-events-web-tests.log`）。两平台 Ruvia 均保持固定提交且工作区干净。尚未部署或完成逐页浏览器验收，原生 Windows 客户端迁移、真实硬件、异常退出恢复及慢消费者仍待验证。

终端 Redis 状态 6 项测试通过，新增关闭通知计数断言（`build/ws-terminal-state-tests.log`）。原专用二进制终端连接测试已改为真实 JSON WS 连接测试，验证控制/输出/确认共用一条连接、UUID 不复用及二进制往返，1 项通过（`build/ws-terminal-connection-tests.log`），已加入 CI 前端测试列表。这里的 WS 对端是测试服务，不代替生产 C++ 终端与设备联调。

最新后端门禁：Linux Release 与全部 35 项 CTest 通过（`build/ws-serial-lifetime-build-linux.log`、`build/ws-serial-lifetime-ctest-linux.log`），包含修复后的 `ws-event`；Windows Release 与串口、上传、事件三项 CTest 通过。新版真实服务回归通过串口显式/重复/断线/退出登录关闭、关闭后写入拒绝、取消边界、统一连接和固件上传；日志 `build/ws-serial-lifetime-integration.log`，夹具 `build/system-orm-fixture-b83f58fb902c4a7fb6c7359c5a2f22e7`。这些结果尚不覆盖真实硬件、慢消费者和异常退出恢复。

终端前端已移除专用 WS 与票据，改用 `edge.terminal.open/write/resize/output.ack/keepalive/close/events.subscribe`；所有事件走现有业务连接。输入按最多 16 KiB 顺序发送，输出交给 xterm 完成后确认，队列超限明确关闭而非静默丢弃，正常关闭先排空已接收输出。类型检查、lint、生产构建、修改文件 Biome 检查通过；终端通道、保留的协议向量和抽屉共 13 项测试通过（`build/ws-terminal-web-tests.log`）。新增通道测试加入原 CI 前端列表，未改变平台、缓存、制品路径。后端终端事件已接入并通过上述集成验证；真实页面尚未验收。

取消边界修复已通过 Windows Release 与真实服务回归：待执行写请求不能被 `subscription.cancel` 中断，重复取消订阅保持幂等，固件上传的连接/账号/权限清理回归通过。日志 `build/ws-cancel-regression-after.log`，夹具 `build/system-orm-fixture-989994f02bb842d5aa59d14b1ff2d8d6`。

串口新增 `edge.serial.open/command/events.subscribe/close` 注册；连接持有资源、订阅持有事件游标，关闭连接、账号切换和撤权通过原 Worker 的 Redis RPC 清理。首轮 Windows Release 与真实 WS/Redis/模拟节点 Protobuf 联调通过，覆盖打开、手动配置、HEX 收发、恢复监听、40 个连续数据报、游标不重复、连接隔离、重复写拒绝、断线关闭及旧固件能力门禁；日志 `build/ws-serial-session-integration.log`，夹具 `build/system-orm-fixture-9fd21ff37bc34254976ef2c4d4863ce6`。随后补充关闭标记与命令入队的原子检查，关闭队列满时保留可重试身份，避免订阅停止令牌随每次推送嵌套增长。新增显式关闭、重复关闭、关闭后写入拒绝和退出登录清理断言尚待运行。最新两平台构建运行中（`build/ws-serial-lifetime-build-windows.log`、`build/ws-serial-lifetime-build-linux.log`）；并发清理、慢消费者、异常退出恢复和真实硬件仍须验证。

原版 Ruvia 的 Linux 全量构建通过，但本次 CTest 为 34/35：`ws-event` 段错误（`build/ws-upstream-ctest-linux.log`）。GDB 栈定位到 `ValidationError` 析构：事件临时 arena 已销毁，而框架仍持有其中分配的校验异常；见 `build/ws-upstream-ws-event-crash.log`。自有事件封装已通过公开 `ValidationErrorOptions.resource` 指定独立生命周期的异常存储，不修改 Ruvia；正在重建和复测，结果以 `build/ws-serial-lifetime-ctest-linux.log` 为准。

新增取消边界回归发现 `subscription.cancel` 会接受正在执行的写请求。`api-channel-integration.ts` 使用事务锁保持写请求活动，断言取消必须返回 `10001`，在修改前服务上稳定失败；日志 `build/ws-cancel-regression-before.log`，夹具 `build/system-orm-fixture-ae32e0f1945c4b7a9d5ce808ebf6f3c6`。现已限制取消对象为订阅，使用当前 Worker 的完成通知等待订阅退出并释放通知接收器后才确认。Windows 服务正在重建（`build/ws-cancel-build-windows.log`），须用新二进制重新运行该回归；此前通过结果不覆盖此新增断言。Linux 当前全量构建的服务入口已在此次修改前完成编译，须在该构建结束后增量重建，不能将其旧二进制计为修复已验证。本轮前端类型检查与 lint 通过（`build/ws-current-typecheck.log`、`build/ws-current-lint.log`，lint 保留一条既有信息级提示）。

`business-orm-integration.ts` 已整体改为原生事件，使用同一连接完成认证、快照、写入及保留 JSON 原始数值文本的请求。协议精度边界、链路/设备共享关系、Webhook Header 与 PostgreSQL 对照、开放接口权限及分组循环断言均通过，日志 `build/ws-business-event-integration.log`，夹具 `build/system-orm-fixture-5ff37214702741c1b6b136da5faf27d2`。第三方接口仍通过真实 HTTP/SSE 验证；无人引用的旧模拟 HTTP 客户端 `tests/api-channel-fixture.ts` 已删除。这不代替尚未完成的串口、终端和原生客户端验收。

恢复原版 Ruvia 后，Windows Release 及 `api-upload`、`ws-event` 两项 CTest 已通过（`build/ws-upstream-build-windows.log`、`build/ws-upstream-ctest-windows.log`）。固件上传、统一 WS、开放接入三项真实服务回归通过，包含旧固件令牌下载、账号/权限变化清理、变化推送以及第三方 HTTP/SSE 保留；日志为 `build/ws-upstream-integration.log`，夹具为 `build/system-orm-fixture-6d8ea8197a194409bb8bd806ed2f3841`。Windows 与 WSL 的 Ruvia checkout 均为原固定提交且工作区干净。Linux 全量重建仍在运行，尚未部署，逐页浏览器验收尚未执行。

Ruvia 依赖已恢复为仓库原先固定的 `13eba75f3b8317a9fe4b5e27ca70554a3e887cab`。删除了无调用方的 `requireApiChannel`、旧 HTTP 认证/分发中间件和 `live::snapshot`，业务代码不再调用 `isSubrequest` 或模拟 HTTP 分发；第三方接口仍使用自身 HTTP/SSE 实现。恢复固定版本后的两平台重新配置、构建与回归正在执行。Linux 重新配置必须同时设置 `CC=/usr/sbin/clang CXX=/usr/sbin/clang++`，避免 vcpkg 按默认 GCC 选择不同依赖缓存；旧 Linux 配置的 34 项 CTest 未包含后来新增的 `ws-event`，须重新生成后补全测试范围。

固件上传已接入 `edge.firmware.upload.start/chunk/finish/abort` 四个事件。暂存文件由连接独占，每连接最多两个、每文件最多 128 MiB、每块最多 16 KiB；空闲五分钟到期。完成登记期间继续占用名额，重复完成不重复登记或下发；同用户刷新令牌保留暂存，断线、退出、切换账号及撤权清理暂存。权限变化通过通知检查，定时器只处理资源到期和令牌失效，不轮询业务查询。首次真实上传联调通过（`build/system-orm-fixture-61b9f8e8225a45dfa5142a5cca3eaea4`），包含浏览器 API 的分块、摘要、设备报文、连接隔离、取消、账号/权限变化及旧固件令牌下载。完成阶段名额保留与空闲清理已由 Windows `ws-event` 实际 Worker 测试验证；最终固件与统一 WS 联调通过，日志为 `build/ws-upload-event-integration.log`，夹具为 `build/system-orm-fixture-6ddea330866347d59c3349fdae0674a9`。Windows Release、`api-upload` 与 `ws-event` CTest 通过；Linux 正在补全配置后复测。锁表实验中额外请求先在数据库阶段返回 `10004`，不能据此验证资源计数；改由 Worker 内生命周期测试验证完成阶段占用及空闲清理。进程异常退出后的磁盘暂存回收仍须在恢复流程中核验，不能把连接析构清理当作重启恢复已完成。

Edge 新增 16 个原生基础事件，涵盖节点/分组查询与写入、固件列表、网络配置、配置同步及日志。管理 HTTP 路由已删除，旧固件令牌下载仍保留；`live-api-contract-test.ts` 的两项检查现已通过。日志等级设置从 50 毫秒查询回执改为先订阅 Redis 通知再发送指令，Collector 持久化回执后发布通知。`tests/edge-event-integration.ts` 在真实 WS、PostgreSQL、Redis 与模拟节点协议连接上通过：分组循环/占用限制、创建者、节点改名推送、筛选与分组分配、注册删除限制、参数与分页溢出、权限、网络报文、批准注册、配置同步、日志首查/显式采集/过滤推送、日志等级回执以及旧 HTTP 的 404。日志 `build/ws-edge-event-integration.log`，夹具 `build/system-orm-fixture-d9fd5932293e4c7584defe8fd11a895e`。Windows 和 Linux Release 构建通过。串口、终端的连接资源事件仍未接完，上传进展见上文；模拟节点验证不能替代真实硬件及部署后的浏览器验收。

最新验证：本轮 Edge 改造的 Linux Release 构建及全部 34 项 CTest 通过（`build/ws-edge-event-build.log`、`build/ws-edge-event-ctest.log`），Windows Release 与 `edge-service`、`ws-event` 两项 CTest 通过。旧注册删除路由断言已改为 `edge.enrollment.delete` 事件注册断言。前端 78 项测试通过，内部 HTTP 路由检查继续作为发布门禁；连接资源与原生客户端尚未完成，不能据此宣称迁移完成。此前 ZLMediaKit 的全局初始化顺序问题已修复；删除 Axios 直接依赖后，锁定依赖安装及类型检查通过。

以根目录 `AGENTS.md` 的「WS 接口设计」为准。浏览器和自有 Windows 客户端各自只持有一条业务 WS，登录、查询、订阅、写入、上传、串口和终端共用。

当前工作区的 `api_channel.h` 已切换到事件注册表直接分发，认证、角色、部门、用户、链路、死信、协议配置、设备、指令、告警、开放接口管理、GB28181、VPN 及 Edge 基础 Controller 已迁移。事件默认要求登录，业务事件声明原有权限码，WS 层在执行及订阅刷新前验证权限；鉴权等待期间身份改变时取消原操作。串口、终端、自有客户端及恢复清理仍在迁移，不能作为完成版本发布。此前基于 `method/path` 的业务联调不代表新事件协议已验收。

开放接口管理新增 11 个原生事件：`open_access.device_options.subscribe`、`open_access.key.list.subscribe/create/update/rotate/delete`、`open_access.webhook.list.subscribe/create/update/delete`、`open_access.log.list.subscribe`。更新事件使用 `{id,configuration}`；Service 接收类型化字段，保留省略与显式空值的区别。管理 HTTP 路由已删除，第三方接口未迁移为 WS。`tests/access-event-integration.ts` 在真实隔离数据库及 Redis 上通过，覆盖轮换/撤销、字段保留/清除、Header 与 PostgreSQL JSONB 语义对照、权限、变化推送、第三方 HTTP/SSE 首帧及旧管理 HTTP 的 404。日志为 `build/ws-access-event-integration.log`，夹具为 `build/system-orm-fixture-30a0c9aa138c483db688f8f3d23f76b6`。Windows Release 与 `access-service` CTest 通过；本轮 Linux Release 构建及全部 34 项 CTest 通过（`build/ws-access-event-ctest.log`）。

前端完整测试当前为 78 通过、0 失败（20 个文件，`build/ws-edge-frontend-tests.log`），包含内部 HTTP 路由检查；连接资源事件尚未实现的部分仍需独立验收。根 `bun run lint` 已通过（1 条信息级提示）；Biome 明确排除 `build/`，避免扫描构建备份中的嵌套配置。

GB28181 管理的 21 个接口已改为直接注册 WS 事件，涵盖状态、SIP 配置、设备/流列表与详情、名称、映射、目录、预览、云台、录像查询、回放及录制。请求 schema 接收类型化 JSON，Service 继续使用接入 Worker 的 Redis RPC，未改变 Collector 的连接归属和第三方视频传输。Windows Release 与 GB28181 schema CTest 通过；本轮 Linux Release 及全部 34 项 CTest 已通过（`build/ws-gb-event-ctest.log`）。`tests/gb28181-worker-integration.ts` 已通过实际 WS + SIP + PostgreSQL + Redis 联调：订阅变化、PTZ 报文送达、重命名提交后响应、其他 Collector 拒绝连接令牌、数据库阻塞时顺序、重复/迟到投影与回执恢复、租约丢失时隔离旧连接。日志 `build/ws-gb-worker-integration.log`，夹具 `build/rpc-integration-fixture-78e894ebff9a451a9584c57d2a5bb343`。这不代表真实摄像头视频预览及录制已验收，仍须部署后的浏览器与设备验证。

第三方 `/open-api/...` HTTP/SSE、第三方视频传输及设备固件协议属于保留范围。EdgeNode `0.3.44` 所需 Protobuf 消息、旧固件带令牌下载不得随内部 HTTP 入口一起删除。

### 部署后浏览器验收范围（尚未执行）

VPN 本轮已注册 23 个原生事件，包含网络、路由、Peer、桌面设备/配置、Enrollment、会话及诊断；旧 VPN HTTP/SSE 入口已移除。多权限声明由 WS 层统一校验，桌面接口仍同时要求 `iot:vpn:query`、`iot:vpn:enroll`、`iot:edge:query`。`tests/vpn-desktop-integration.ts` 改用生产 `ApiChannel` 后全部通过：实际登录/刷新、并发分配、历史撤销地址复用、迟到断开隔离、同密钥重试、节点选择、跨用户拒绝、权限撤销、配置推送、停用与幂等撤销。日志为 `build/ws-vpn-desktop-integration.log`，夹具为 `build/vpn-desktop-fixture-ae8144bde84f4054851520a3b7efe200`。该夹具关闭真实 WireGuard Hub，只证明 API、权限与数据库流程；原生 Windows 客户端与实际隧道仍待验证。Windows Release、WS/VPN CTest 已通过，本轮 Linux Release 及全部 34 项 CTest 已通过（`build/ws-vpn-event-ctest.log`）。该轮内部 GET 门禁仍有 Edge 的 6 条旧管理路由；后续 Edge 基础事件改造已移除。

依据 `web/routes/index.tsx`，逐个进入以下实际路由；每页记录操作、结果和控制台/网络证据，不能以访问首页代表全部页面通过。

| 路由 | 必查操作 |
| --- | --- |
| `/login` | 登录、失败提示、退出、会话恢复 |
| `/system/role`、`/system/dept`、`/system/user` | 列表、筛选、表单校验、增改删、权限变化 |
| `/iot/link` | 列表、配置编辑、调试日志、大量连接地址时滚动范围 |
| `/iot/modbus`、`/iot/sl651`、`/iot/s7`、`/iot/dlt645`、`/iot/fins`、`/iot/mc` | 各协议配置列表、详情、校验和保存 |
| `/device` | 导航树、设备列表/卡片、详情、实时值、历史、指令、共享 |
| `/iot/open-access` | 调用配置、密钥轮换、Webhook、访问日志 |
| `/iot/edge` | 分组、详情、网络、日志、VPN、固件上传、串口监听/收发/暂停、终端 |
| `/iot/alert` | 规则、模板、记录、确认、统计 |
| `/iot/gb28181` | 设备/通道、目录、预览、云台、录像；保留第三方视频传输 |

跨页检查单条业务 WS、离页取消订阅、变化推送、断线重连、身份切换、写操作不自动重放，以及窄屏下表格和弹窗内部滚动。测试数据须可识别、可清理；涉及真实设备和生产配置的操作依据现有授权执行，并记录未能覆盖的实际设备能力。

## 协议及会话

### 已迁移事件

| 事件 | `data` | 权限 |
| --- | --- | --- |
| `auth.login` | `username`、`password` | 匿名 |
| `auth.refresh` | `refresh_token` | 匿名，验证刷新令牌 |
| `auth.session` | `token` | 匿名，验证访问令牌 |
| `auth.logout` | `{}` | 清除当前连接身份 |
| `auth.me.subscribe` | `{}` | 已登录 |
| `role.list.subscribe` | `page`、`pageSize`、可选 `keyword`、`status` | `system:role:query` |
| `role.detail.subscribe` | `id` | `system:role:query` |
| `role.options.subscribe` | `{}` | `system:user:query` |
| `role.create` | 原创建字段 | `system:role:add` |
| `role.update` | `id` 与原更新字段，位于同一对象 | `system:role:edit` |
| `role.delete` | `id` | `system:role:delete` |
| `dept.list.subscribe` | 分页、可选 `keyword`、`status`、`parent_id` | `system:dept:query` |
| `dept.detail.subscribe` | `id` | `system:dept:query` |
| `dept.options.subscribe` | `{}` | `system:dept:query` |
| `dept.create` / `dept.update` / `dept.delete` | 原业务字段；更新与删除使用 `id` | 对应 `system:dept:add` / `edit` / `delete` |
| `user.list.subscribe` | 分页、可选 `keyword`、`status` | `system:user:query` |
| `user.detail.subscribe` | `id` | `system:user:query` |
| `user.options.subscribe` | 可选 `keyword` | `system:user:query` |
| `user.create` / `user.update` / `user.delete` | 原业务字段；更新与删除使用 `id` | 对应 `system:user:add` / `edit` / `delete` |
| `link.list.subscribe` | 分页及原筛选字段 | `iot:link:query` |
| `link.detail.subscribe` | `id` | `iot:link:query` |
| `link.options.subscribe` / `link.enums.subscribe` / `link.public_ip.subscribe` | `{}` | `iot:link:query` |
| `link.create` | 原链路配置字段 | `iot:link:add` |
| `link.update` | `id`、`configuration`（原链路配置对象） | `iot:link:edit`，另检查创建者 |
| `link.delete` | `id` | `iot:link:delete`，另检查创建者 |
| `link.debug.subscribe` | `id` | `iot:link:edit` |
| `link.debug.set` | `id`、`enabled` | `iot:link:edit` |
| `outbox.dead_letters.subscribe` | `{}` | `system:outbox:manage` |
| `outbox.replay` | `id` | `system:outbox:manage` |
| `protocol.list.subscribe` / `protocol.options.subscribe` | 分页、`protocol`（options 必填） | `iot:protocol:query` |
| `protocol.detail.subscribe` | `id` | `iot:protocol:query` |
| `protocol.create` | `name`、`protocol`、`config`，可选 `enabled`、`remark` | `iot:protocol:add` |
| `protocol.update` | `id` 与可选更新字段，位于同一对象 | `iot:protocol:edit`，另检查创建者 |
| `protocol.delete` | `id` | `iot:protocol:delete`，另检查创建者 |
| `device.list.subscribe` / `device.realtime.subscribe` / `device.options.subscribe` | `{}`；保持原有完整授权范围查询 | `iot:device:query` |
| `device.detail.subscribe` | `id` | `iot:device:query`，另检查设备授权 |
| `device.history.subscribe` | `id`、`startTime`、`endTime`，可选 `page`、`pageSize` | `iot:device:query`，另检查设备授权 |
| `device.create` | 原设备创建字段 | `iot:device:add` |
| `device.update` | `id`、`configuration`（设备更新字段） | `iot:device:edit`，另检查所有者 |
| `device.delete` | `id` | `iot:device:delete`，另检查所有者 |
| `device.debug.subscribe` / `device.debug.set` | `id`；set 另需 `enabled` | `iot:device:edit`，另检查所有者 |
| `device.shares.subscribe` / `device.share_targets.subscribe` | `id` | `iot:device:share`，另检查所有者 |
| `device.shares.replace` | `id`、`shares` | `iot:device:share`，另检查所有者 |
| `device.group.tree.subscribe` / `device.group.tree_count.subscribe` | `{}` | `iot:device-group:query` |
| `device.group.detail.subscribe` | `id` | `iot:device-group:query` |
| `device.group.create` | 原分组创建字段 | `iot:device-group:add` |
| `device.group.update` | `id`、`configuration`（分组更新字段） | `iot:device-group:edit` |
| `device.group.delete` | `id` | `iot:device-group:delete` |
| `device.group.shares.subscribe` / `device.group.share_targets.subscribe` | `id` | `iot:device-group:share`，另检查所有者 |
| `device.group.shares.replace` | `id`、`shares` | `iot:device-group:share`，另检查所有者 |
| `device.command.create` | `deviceId`、`command`（`idempotency_key`、`elements`） | `iot:device:command`，另检查设备操作授权及远控开关 |
| `device.command.status.subscribe` / `device.command.statuses.subscribe` | 单条 `id`；批量 `ids` 数组，1–256 条 | `iot:device:command`，逐条检查设备操作授权 |

上述管理事件写入成功返回 `data:null`；查询返回业务对象或数组，不携带旧 HTTP `code/message/data` 外层。角色、部门、用户及授权变更沿用数据库变化通知，订阅刷新时重新检查权限，撤权后以 `11007` 错误结束订阅。用户删除的操作者身份取自已验证的 WS 上下文，客户端不能指定操作者绕过自删除限制。

链路创建者取自 WS 身份；边缘配置下发显式传递已验证的用户 ID，经原 Worker 的 Redis RPC 执行。`0050_dead_letter_query_changes` 为死信列表增加事务内变化通知：普通 Outbox 行不触发；可见死信的新增、变更、重放和删除产生 `system` 主题事件。未改写已执行迁移。死信重放与重放计数仍在同一事务中提交，计数归接入连接的 Worker。

### 连接行为

协议配置事件使用类型化字段，`config` 保留已验证原始 JSON 数值。请求形状校验归 schema，依赖已存协议类型的配置规则归 Service；Service 不导入 schema。更新保持 PostgreSQL `jsonb` 顶层合并语义，省略备注不修改，`null` 或空字符串清空。项目内数组遍历及字符串解码只使用公开 API，已移除本模块对 Ruvia 私有 JSON 解析接口的调用。

- 请求仅为 `{id,event,data}`，每次请求生成新的 UUID；事件名归前端所属模块的 `api` 文件。
- 正常回复及后续推送为 `{id,data}`；失败为 `{id,error:{code,message}}`。沿用已有业务错误码，不向客户端暴露 HTTP 状态作为业务协议。
- 登录、认证恢复与令牌更新使用明确事件；认证状态由接入 Worker 的连接会话持有。认证失败不能改变已有会话身份；成功切换账号前取消原账号订阅并释放其连接资源。
- 订阅事件采用 `device.list.subscribe` 等业务名称。先登记变化通知，再执行首查，避免首查与登记之间漏掉变化；变化通知触发重新查询，未变化的数据不重复发送。
- `subscription.cancel` 自身使用新 UUID，`data.subscriptionId` 指向目标订阅。取消之后不得继续发送该订阅结果；取消必须传播到等待中的操作并等待其退出。
- 断线重连只恢复查询订阅。已发送写操作不自动重放，向调用方说明结果未确认；串口、终端的输入也不得重放。
- 串口和终端拥有独立顺序号、有限队列及背压。终端数据不得静默裁剪；设备确认与实际交付边界一致。连接关闭必须释放资源，租约负责异常退出后的回收。

## 框架与业务边界

当前固定 Ruvia 提交尚无 `RUVIA_WS_EVENT`。按用户最新要求，不修改 Ruvia 源码，在项目内封装事件注册与直接分发，底层只使用 Ruvia 公开 API，不借 `detail` 绕过边界，也不通过 HTTP 路由转发。

- Controller 自动注册语义事件，保留每个 Worker 独立的 Controller 与中间件实例。注册表在启动期固定，重复事件及 Worker 注册差异须报错。
- 事件上下文拥有独立的参数及操作内存、取消状态，借用原 Worker 的数据库、Redis 和运行资源；不创建 HTTP 子请求，不经过 HTTP 路由表。
- 校验器解析类型化 `data`，Controller 接收类型化参数、调用所属 Service 并返回业务结果。HTTP 参数、查询字符串及响应头不能充当事件参数或订阅元数据。
- 认证、权限、回包、取消和断线清理统一接入 WS 生命周期；业务查询和数据库操作仍归所属 Service。
- `modules/` 与 `features/` 继续通过 Redis 或数据库交互。设备连接由原 Collector Worker 持有，WS 业务执行及结果回包由原 Service Worker 完成。
- 公共契约只包含无 I/O 的格式与编码；持久化映射归所属 `entity`，连接会话及任务状态不能放入 `common/`。

## 切换顺序

1. 完成项目内事件注册、类型化参数、Worker 归属、异常与取消的功能测试。
2. 逐模块迁移 Controller、schema、Service 参数和前端 API。完成后删除内部 HTTP 路由、`Context::dispatch()` 适配、旧 SSE 分支及相应测试假设。
3. 将上传、串口、终端资源接入同一事件会话，验证分块、顺序、背压、断线释放和旧固件行为。
4. 迁移 Windows 平台适配与配置订阅，保持 IPC、WireGuard、安装和凭据身份稳定，使用 CMake 完成构建与测试。
5. 使用同一源码版本交付 Linux 可执行文件与 `web/`。按用户指定在本机 WSL 编译并部署到 `192.168.5.100`，不等待 CI；记录 Windows 验证结果和未覆盖项。

## 验收

最新 Windows Release 服务已通过实际 PostgreSQL、Redis 和 WS 联调：`auth-rate-limit-integration.ts` 验证原生认证错误及 Redis 限流窗口；`api-channel-integration.ts` 使用真实前端 `ApiChannel`/`SnapshotSubscriptions` 验证单连接认证、写入、参数校验、数据库外部变更推送、取消和主动刷新，并验证第三方 HTTP/SSE 仍保留；`alert-orm-integration.ts` 验证告警计算、恢复、重复消息、模板应用，以及新增的确认、更新推送和删除断言。三项均通过，日志为 `build/ws-alert-event-integration.log`，独立测试目录为 `build/system-orm-fixture-24319ccb0e6f4af1897c42dca28b73f3`。这些测试覆盖已迁移模块，未覆盖未注册的开放接入管理、GB28181 管理和边缘资源事件，也不替代部署后的逐页浏览器验收。

完整 Linux Release 构建已通过，但首次全量 CTest 为 30/34；4 项 GB28181 测试在 `main` 前段错误（`build/ws-alert-event-ctest.log`）。GDB 显示 ZLMediaKit `IceTransport.cpp` 的全局端口管理 token 在 `config.cpp` 的 `Broadcast::kBroadcastReloadConfig` 字符串构造前访问它，崩溃位于 NoticeCenter 插入 map 节点时（`build/ws-linux-gb-crash.log`、`build/ws-linux-gb-registers.log`）。已在现有固定版本依赖补丁 `ports/cmake/patch-zlmediakit.cmake` 中把监听注册改为首次获取端口管理器时执行，移除该全局 token；未修改 Ruvia。补丁重复执行成功，正在重新构建并执行完整 CTest，结果见 `build/ws-zlm-startup-build.log`、`build/ws-zlm-startup-ctest.log`；验证结束前该问题仍未关闭。

旧 `web/lib/http.ts` 与 `api-channel-adapter.ts` 已在确认无源码导入后删除。共享通道与固件集成测试已移除 Axios 适配，旧订阅测试改为原生事件并保留刷新合并、过期回复忽略及取消行为；新增行为测试已加入 CI 原有前端测试列表，未改平台、缓存或制品路径。完整前端类型检查现已通过；本轮通道、快照、认证刷新与上传共 17 项测试通过，web lint 通过。仍有旧协议测试夹具及依赖清理待完成，这些检查不代表未实现的后端事件、终端资源或生产功能已经通过。

边缘节点前端管理请求已改用 `edge.*` 事件，VPN 管理使用 `vpn.network/peer/route.*` 事件。节点和分组更新明确携带 `id`，配置更新使用 `configuration`；串口操作使用 `{id,sessionId}`，发送内容放在 `command`。固件上传采用独立的 `edge.firmware.upload.start/chunk/finish/abort` 事件，开始参数为节点 ID、文件名、大小及保留设置，后续以 `uploadId` 标识。对应资源事件的后端注册与断线清理仍待完成。终端暂存的票据请求和独立二进制 WS 尚未移除，须整体迁移，不得作为最终协议交付。生产业务页面已无 `lib/http` 导入，旧层仍被部分集成测试引用，后续须迁移并删除。

`firmware-upload-test.ts` 已使用原生事件测试，2 项通过：分块边界、顺序确认、提交及错误确认后中止。`session-refresh-test.ts` 已移除 Axios 模拟，4 项通过：并发刷新合并、无令牌不请求、业务失效清理、网络故障保留凭据及迟到结果不覆盖新账号。web lint 与修改文件 Biome 通过；完整类型检查仍因旧 adapter 失败，后端资源行为及实际页面尚未验收。

GB28181 管理前端已声明原生事件：`gb28181.health.subscribe`、`sip_config.subscribe`、`device.list.subscribe`、`stream.list.subscribe`、`device.rename`、`channel.rename`、`catalog.query`、`preview.start/stop/renew`、`ptz.move`、`ptz.position.set`、`recording.subscribe/start/stop`（均以 `gb28181.` 为前缀）。参数直接使用已有 DTO 字段，预览停止复用当前业务连接身份，页面不再保存用于该请求的 token 引用。第三方视频播放实现未改动。后端对应注册、原 Worker 会话归属及断线回收仍待迁移和联调；退出页面时发送停止请求只能尽力执行，必须由服务端清理与租约保障资源回收，不能仅依赖浏览器发送成功。修改文件 Biome 与 web lint 通过，完整类型检查仍有旧 Axios adapter 的两处错误。当前整体 Linux 构建继续运行，生产构建及实际视频页面验证须在后续完成。

开放接入管理前端已改用内部事件：`open_access.device_options.subscribe`、`open_access.key.list.subscribe/create/update/rotate/delete`、`open_access.webhook.list.subscribe/create/update/delete`、`open_access.log.list.subscribe`（斜杠表示同一前缀下的独立事件）。更新参数为 `{id,configuration}`；查询使用原有结构化过滤字段。Zod 响应及输入校验保留。对应后端事件尚待注册，当前仅为前端迁移进度，不能交付；第三方 `/open-api/device/...` Controller 未修改。该文件 Biome 和 web lint 通过，完整类型检查仍因旧 Axios adapter 失败。生产构建待正在运行的完整 Linux 构建完成后核验，避免同时写入 `build/web`。

告警校验器已移除 Ruvia 私有 JSON 数组解析，协议配置与告警共同使用 `service::utils::visitJsonArray`，只通过公开 `JsonValue` 校验元素并保留原始数值文本。新增条件测试覆盖嵌套数组/对象、引号与反斜杠、空数组、尾逗号、尾随 JSON 和非数组输入；WSL `alert-runtime`、`protocol-service` 两项均通过（`build/ws-json-array-ctest.log`）。协议源码断言同步更新为共享函数的实际调用。告警页面条件行 UUID 已使用支持 HTTP 来源的公共生成函数，前端生产构建及 web lint 通过，完整类型检查仍受旧 adapter 两处错误影响。已启动完整 Linux Release 与 CTest，日志分别为 `build/ws-alert-event-release.log` 和 `build/ws-alert-event-ctest.log`；结束前不能认定该验证通过。

WSL 前端构建入口已恢复：使用锁定 Bun 1.3.14 执行 `bun install --frozen-lockfile --os any --cpu x64`，未改动 `bun.lock` 或 `package.json`，随后 `bun run build:web` 在 WSL 成功（`build/ws-wsl-frontend-install.log`、`build/ws-wsl-web-build.log`）。此前 `vite: command not found` 已解决。告警集成测试又补充了确认人持久化、重复确认拒绝、参数校验、更新推送、模板修改删除与旧 HTTP 404 检查，但尚未执行这些新增断言。Linux 服务源码编译仍在运行，私有 JSON 数组遍历替换须待本次编译结束后进行，避免构建期间修改输入。

告警后端现已注册与前端对应的 17 个原生事件，删除本模块旧 HTTP/SSE 注册及响应外层；列表过滤、分页和分组天数通过 JSON 类型化参数传入，Service 不再读取 HTTP 请求或认证中间件，操作者使用已验证的 WS 用户身份。分页增加偏移溢出检查，分组天数限制为 1–365。原告警 ORM 集成测试已改用独立原生 WS 客户端，等待新服务构建后运行。上一阶段的 Windows Release 及 Windows/WSL `alert-runtime` 测试均通过；本次事件变更正在编译（`build/ws-alert-event-linux-build.log`），不能沿用上一阶段的通过结果。告警校验器仍有旧 Ruvia 私有 JSON 数组遍历调用，须替换为公开 API 并验证条件数组，完成后才能验收该模块。

告警前端已声明待实现的事件契约：`alert.rule.list.subscribe`、`detail.subscribe`、`create`、`update`、`delete`、`delete_batch`、`apply_template`；`alert.template.list.subscribe`、`detail.subscribe`、`create`、`update`、`delete`；`alert.record.list.subscribe`、`grouped.subscribe`、`acknowledge`、`acknowledge_batch`，以及 `alert.stats.subscribe`。其中前缀分别为 `alert.rule`、`alert.template`、`alert.record`。更新参数为 `{id,configuration}`，批量操作为 `{ids}`，分组查询为 `{days}`，其他业务字段保留。设备选项直接使用已实现的 `device.options.subscribe`。前端生产构建和该文件 Biome 检查通过，但上述告警后端事件尚未注册，必须完成后端迁移及实际联调后才能交付，当前页面不具备完整运行条件。

告警模块正在迁移：写入的 JSON 参数解析已从 Service 移至请求接入层，Service 接收 `RuleInput`、`TemplateInput`、`ApplyTemplateInput` 或已校验的 UUID 集合，不再包含 schema；实例改为 `thread_local`。原有查询参数、认证读取和 HTTP 注册尚未改为原生事件，因此不能将该模块计为迁移完成。已启动 Windows Release 服务构建和 WSL 告警测试构建，日志分别为 `build/ws-alert-types-windows-build.log`、`build/ws-alert-types-build.log`，测试日志为 `build/ws-alert-types-ctest.log`；结果须以进程结束后的日志为准。

设备、分组、指令、链路和协议的前端 API 已逐项对齐现有 Controller 事件。设备与链路更新使用 `configuration`，指令创建使用 `deviceId` 与 `command`，批量状态订阅使用最多 256 个 UUID 的数组；不再拼接 URL 或逗号查询字符串。请求 ID 与设备指令幂等键复用无状态 UUID v4 生成函数，支持 HTTP 局域网环境。相关 9 项通道/订阅测试和修改文件 Biome 检查通过；类型检查仍有旧 Axios adapter 的两处错误。实际页面及服务联调尚未完成，不能将通道测试视为这些页面已验收。

登录与系统管理前端已接入原生事件：`login.api.ts` 使用认证事件，角色、部门、用户 API 使用各自 Controller 的语义事件及结构化参数。新增 `event-request.ts` 负责错误展示与原生请求，不重试写入；认证恢复和刷新由登录 Service 提供，在应用入口注入。迟到刷新结果不得覆盖新会话，网络失败保留登录凭据。前端生产构建通过（`build/ws-native-channel-web-build.log`），通道与订阅 9 项测试通过；这些结果不代替页面运行验收。根目录 lint 被 `build/frontend-deploy-47f388d/biome.json` 的嵌套根配置阻止，另行运行 `biome lint web`，日志为 `build/ws-native-channel-web-lint.log`。完整类型检查仍因待删除的旧 Axios adapter 失败；其余页面 API、资源流及真实服务联调仍未完成。

快照管理器已使用事件名与结构化参数，共享键由规范化 JSON 生成，并捕获参数避免调用方后续修改影响订阅。主动刷新取消原订阅并使用新 UUID，账号切换清除缓存，单个消费者异常不会中断其他消费者。`tests/edge-debug-subscriptions-test.ts` 的 3 项测试与通道 6 项测试合计 9 项通过；两个前端源文件 Biome 检查通过。最新类型检查仅报告旧 Axios adapter 的两处类型错误，但页面 API 仍须从 URL 迁移为事件，不能据此认定前端可运行。部署后必须使用浏览器逐页调试全部页面，检查操作、推送、控制台、重连和网络请求，并明确记录真实设备依赖及未覆盖项。

本机 WSL 的 Release 首次构建已完成依赖准备，但在业务源码编译阶段失败（`build/ws-wsl-build.log`）。Clang 要求泛型 Context 派生的 ORM 查询及链式模型调用显式使用 `template` 消歧；已修正认证、部门、角色、用户、链路和死信 Service 中对应调用，未修改 Ruvia 源码。增量复编已完成 `service/server.cpp.o`，但前端构建步骤因找不到 `vite` 退出；日志为 `build/ws-wsl-build-retry.log`。完整 Linux 构建与 CTest 尚未通过，后续需修复 WSL 前端依赖入口。

前端 `web/lib/api-channel.ts` 已改为原生语义事件，不再发送模拟 HTTP 字段。连接恢复先等待注入的认证恢复方法，再发送受保护操作；重新订阅使用新 UUID，取消请求使用独立 UUID，已发送写操作断线或取消等待后报告结果不确定，不自动重发。UUID 使用 `crypto.getRandomValues`，支持部署环境的 HTTP 局域网来源。`tests/edge-debug-connection-test.ts` 的 6 项行为测试通过，覆盖乱序关联、单连接、认证恢复、重连、取消、旧响应拒绝和消费者异常隔离；该源文件 Biome 检查通过。调用层迁移尚未完成：旧 Axios adapter 与快照管理器仍依赖已删除的 HTTP 形式，完整类型检查失败，见 `build/ws-native-channel-typecheck.log`。此阶段不能发布前端，后续须迁移调用方、删除 adapter，并执行全套前端检查与实际服务联调。

设备与指令迁移通过 Windows Release 的 `iot-engine`、`device-service-test` 构建，以及 `device-service`、`architecture`、`worker-isolation`、`ws-event` CTest；构建日志为 `build/ws-device-event-build.log`。`tests/device-event-integration.ts` 在两个实际 WS 会话中验证设备/分组 CRUD、实时与历史首查、调试设置、用户与分组授权、撤权推送、所有者限制及旧 HTTP 入口移除。`tests/industrial-protocol-integration.ts` 已改为语义事件，模拟 MC 3E/4E、FINS TCP、DL/T645 1997/2007，验证 Collector 采集入库、指令写入回读，以及 WS 指令状态持续推送到成功。另验证第三方 HTTP 指令原响应外层、设备 ACL 拒绝及 SSE 首帧，确认开放接口未被内部迁移替代。全部通过，日志为 `build/ws-device-event-integration.log`。测试中的 TCP 周期采集属于设备协议行为，内部客户端状态查询不使用轮询。

本轮完整 CTest 日志为 `build/ws-device-ctest.log`：除已有 IPv6 UDP 环境失败外，`command-service` 的源码断言未识别模板调用语法，已更新并重新构建复测通过。工业 Collector 现有实现会比较回读字节，但未填充可选 `actual_values`；本次保留这一行为并比较数据库与事件响应，不能把协议回读成功表述为客户端已收到实际数值明细。EdgeNode 的实际值返回契约保持不变，仍需在后续边缘联调中验证。

协议配置迁移通过 Windows Release 构建及 `ws-event`、`protocol-service` CTest，构建日志为 `build/ws-protocol-event-build.log`。`tests/protocol-event-integration.ts` 使用真实服务验证 CRUD、变化推送、权限与创建者限制、旧入口移除、JSON 合并、备注省略和清空、超过 JavaScript 安全整数范围的数值保存及精确小数边界，覆盖 Modbus、S7、MC、FINS、DLT645 的数组和转义字符串。`tests/protocol-offset-integration.ts` 已改用语义事件，验证 SL651 OFFSET 首次写入与更新的边界、科学计数法、错误类型及失败写入隔离。两项集成测试通过，日志为 `build/ws-protocol-event-integration.log`；这些结果不包含真实设备协议链路与前端联调。

本轮完整 CTest 首次结果为 33/35（`build/ws-protocol-ctest.log`）。架构测试仍匹配旧死信 HTTP 路由与 Service 权限声明，已改为原生事件与 Controller 权限声明，重新构建复测通过。剩余 `gb28181-sip` 的 IPv6 UDP 收包失败尚未解决：独立 Python UDP 回环检查同样出现 IPv4 正常、`::1` 接收超时，说明本机 IPv6 UDP 环境也存在问题；未跳过测试或修改系统网络策略。后续仍需在可用环境完成该项验证，不能宣称完整 CTest 通过。

当前已通过 Windows Release 的 `iot-engine`、`ws-event-test` 构建及 `ws-event` CTest。`tests/auth-event-integration.ts` 在独立 PostgreSQL、Redis 和实际服务进程中验证了单连接认证、角色增删改查、变化推送、输入校验、权限撤销、订阅取消及旧认证/角色 HTTP 入口移除。日志保存在 `build/ws-role-event-build.log`、`build/ws-role-event-integration.log`。这些结果仅覆盖已迁移部分，完整 CTest、Linux 构建、前端和客户端联调仍须完成。

部门与用户迁移后再次通过 Windows Release 构建和 `ws-event` CTest。`tests/system-orm-integration.ts` 已直接使用语义事件，在真实 WS 上验证分页、部门递归循环保护、空值与 UTC 字段、角色替换、数据库故障后的事务回滚、管理员保护、身份切换、订阅推送及旧 HTTP 入口移除。对应日志为 `build/ws-system-event-build.log` 和 `build/ws-system-orm-integration.log`；同版本认证与角色回归结果见 `build/ws-system-event-integration.log`。

链路与死信已通过 Windows Release 构建，日志为 `build/ws-dead-letter-migration-build.log`。`tests/link-event-integration.ts` 验证链路增删改查、类型校验、持久化 Outbox、调试推送、创建者限制和旧入口移除；`tests/outbox-replay-metrics-integration.ts` 验证死信实时推送、并发重放只提交与计数一次、数据库故障回滚及 Redis 暂停写入。`tests/dead-letter-query-migration-integration.ts` 经真实服务启停验证新库初始化、旧库升级、重复执行、校验和漂移拒绝、DDL 失败回滚与恢复，同时比较历史迁移记录和业务数据。全部通过，日志为 `build/ws-outbox-migration-integration.log`。

- 同一实际 WS 完成登录、首查、持续推送、写入、取消和重新认证；并发请求按 UUID 正确关联，无业务 GET 轮询。
- 不同账号和不同连接无法接管订阅、上传、串口及终端资源；权限变更、令牌过期与账号切换及时生效。
- 慢消费者、分片、断线、重复消息、超时、服务重启均有明确行为；单个订阅的消费异常不破坏其他操作。
- 用真实 Redis、数据库和模拟设备验证边界；HTTP/SSE 第三方正向授权及权限拒绝仍通过，第三方视频正常。
- 前端类型检查、lint、生产构建及格式检查；后端 Release 与 CTest；Windows 客户端 CMake 验证；实际页面与部署后功能验证。不能以目录、字符串或编译检查替代运行验收。
- 发布前保存二进制、静态资源和代理配置备份，校验架构、动态依赖和哈希；关键检查失败回滚。凭据不进入文档、日志或制品。

串口集成测试客户端已改用原生事件与统一 `ApiChannel`，不再使用 `method/path` 适配。对应资源事件和断线关闭仍待接入，尚未运行该测试，不能将固件上传验证结果扩展为串口已通过。
