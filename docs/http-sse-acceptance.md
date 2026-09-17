# HTTP 与 SSE 验收报告

## 交付

- 日期：2026-09-17。
- 环境：`192.168.5.100:10050`。
- 发布：`3e99326adecf25b4a0d2c192a8135d8ad0afa0f1`。
- 前端：`a0e5b771.js`；浏览器已确认实际加载。
- 服务：`iot.service` 为 active，全部就绪检查通过。
- 结论：按用户确认的现有生产功能范围，通过本次 HTTP/SSE 改造验收。

## 通信与浏览器

| 要求 | 验证结果 |
| --- | --- |
| 普通操作使用 HTTP | 设备保存为 PUT，历史查询为 GET，登录为独立 HTTP 操作 |
| 配置页面不订阅 | 角色、部门、用户和六类协议页面正常加载，切换记录无 SSE |
| 设备共用连接 | 列表、分组、指标、调试共用 `/v1/device/events`，最大活跃数 1 |
| 链路共用连接 | 列表及报文共用 `/v1/link/events`，打开、关闭弹窗最大活跃数 1 |
| 告警共用连接 | `records`、`stats`、`user` 同一连接 |
| 边缘节点按需连接 | 空节点页面正常，`nodes`、`user` 同一连接 |
| 无业务轮询 | 网络观察无周期性业务 GET；重连只发生在断线后 |
| 有界重连 | 退避耗尽后显示错误，连续静默超过 40 秒，未重启第二轮重试 |
| 手动恢复 | 点击重新连接恢复列表，随后两次调试状态变化正常推送 |
| 变化隔离 | 指标与分组使用命名事件；隔离集成验证无关查询及重复通知不产生业务推送 |
| 页面及表单可用 | 设备筛选、分组、原值保存、历史、调试及链路暂停/恢复正常 |
| 布局 | 768×720 验证表格内部横向滚动、弹窗独立滚动及固定操作区 |
| 登录与退出 | 新版本退出后显示登录页，重新登录正常，保留返回路由 |
| favicon | 图标内置 HTML；75 秒空闲观察无 favicon 请求，切换时可见扩展改写图标 |

已逐页打开系统管理三页、六类协议、链路、设备、边缘节点、告警、开放接入、登录和视频入口。
视频入口明确提示未启用。布局检查使用相同布局源码的上一部署，通信和重试修复在新版本复验。
浏览器控制台未发现运行错误；定向断线只影响测试浏览器的链路 SSE，故障注入已结束。
调试开关已恢复关闭，未更改生产链路参数，测试设备保存保持原值。

## 本次修复

- 普通查询与写入恢复 HTTP，移除统一业务 WS 及多余管理端订阅。
- 页面组件共享 SSE，仅订阅当前视图需要的数据。
- 禁止定时业务查询与轮询降级，保留有界断线退避及注释心跳。
- 查询层关闭自动重试，避免重复启动 SSE 自身的退避循环。
- 新连接等待旧数据流取消完成。
- 链路调试保留已有列表；开关保存不再刷新枚举和公网 IP。
- 第三方 HTTP/SSE、已部署固件协议及专用双向 WS 保持兼容。

## 构建与集成

- 前端类型、lint、生产构建、修改源码格式检查通过；111 项测试通过。
- Linux Release、35 项 CTest 通过。
- Windows Release 通过；34 项适用测试中媒体端口绑定首次失败，单项复测通过。
- Windows SIP 环境例外保留，Linux SIP 测试通过；未宣称 Windows 全套一次通过。
- Windows 客户端 6 项测试及同快照安装包通过。
- 真实服务集成覆盖 HTTP CRUD、权限、撤销、共享订阅、256 条指令、设备协议和第三方兼容。
- 新发布后端与已验证集成的二进制哈希相同；本轮运行代码修改集中在前端。
- 上传前源码与冻结快照一致；最终报告属于验收后补充文档。
- 部署前已验证备份恢复、制品哈希、架构、依赖及重复迁移；切换后验证资源与服务。

## 证据

- `build/deploy-3e99326/{prepare,stage,cutover}.log`。
- `build/deploy-3e99326/browser-link-debug.json`：两次开关 PUT 无额外枚举或公网 IP GET，45 次报文事件，最大活跃 SSE 为 1。
- `build/deploy-3e99326/browser-disconnect.jsonl`：最后一次关闭后静默 40 秒，注入正常结束。
- `build/deploy-3e99326/browser-recovery.json`：手动恢复连接上 `links` 三次、`user` 一次；包括初始快照与后续两次变化。
- `build/deploy-3e99326/browser-device.json`：设备保存、历史 GET、调试范围切换；最大活跃 SSE 为 1。
- `build/deploy-2166468/browser-link-idle-favicon.json`：75 秒空闲无新请求，不代表已有 SSE 不存在。
- `build/sse-browser-release-{linux,windows,client,web-tests}.log`、`build/sse-browser-release-media-retry.log`。
- `build/sse-cancellation-tests.log`、`build/sse-final-{typecheck,lint,web-build}.log`。
- 隔离集成：`build/device-command-shared-integration-fresh.log`、`build/device-debug-shared-integration.log`、`build/access-http-only-integration-final.log`、`build/edge-shared-inventory-integration-final.log`、`build/video-sse-integration.log`、`build/sse-cleanup-vpn-integration.log`。

网络观察按实际 HTTP 200 SSE 响应统计活跃连接，仅覆盖观察窗口，未做完整 TCP 重组。
浏览器操作、源代码检查及隔离集成共同构成证据，不能将抓包计数扩展为所有硬件已验证。

## 明确未覆盖

- 用户已确认按现有生产功能验收，记录实机未覆盖项。
- 生产没有边缘节点：未测试线上真实节点、串口、终端、固件升级和节点 VPN。
- GB28181 未启用：未测试线上摄像头预览、录像和 PTZ。
- 测试设备没有可下发要素：浏览器验证了提示，未向现场设备发送写控制指令；指令执行由隔离集成覆盖。
- 未在生产执行破坏性删除、权限扩权或全量业务 CRUD；相应接口由隔离集成覆盖。
