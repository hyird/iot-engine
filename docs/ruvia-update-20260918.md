# 2026-09-18 Ruvia 更新

## 固定版本

`CMakeLists.txt` 从 `13eba75f3b8317a9fe4b5e27ca70554a3e887cab` 经 `e13f6ec6998aa0d3f013bfc1c6d7206bff3aaa67` 更新到本次检查时上游主线最新提交 `7a60bfbc0e4cd359ed3edb18faea0d1c7ea28483`，继续使用完整 SHA。

[上游变更](https://github.com/hyird/Ruvia/compare/13eba75f3b8317a9fe4b5e27ca70554a3e887cab...e13f6ec6998aa0d3f013bfc1c6d7206bff3aaa67) 包括实体组合写入、模型字段校验及嵌套数组修复、HTTP 协议职责调整、WebSocket 客户端 TLS 初始化和关闭握手修复。

[后续提交 `7a60bfbc`](https://github.com/hyird/Ruvia/commit/7a60bfbc0e4cd359ed3edb18faea0d1c7ea28483) 将普通 HTTP handler 统一为 `Task<>`（Web 层默认结果为 `HttpResponse`），移除路由直接返回响应模型的支持。Core 层不再提供默认结果类型；无返回值任务显式使用 `Task<void>`。

## 接入迁移

- 路由直接使用公开 `JsonBody<T>`、`QueryModel<T>`、`PathModel<T>`。通用长度、范围、枚举和邮箱约束随请求模型声明，删除旧 `RUVIA_VALIDATE_*`、`RUVIA_RULE*` 及仅转发校验的类型。
- 请求必填、UUID、自定义谓词和嵌套字段约束直接声明在所属 `types.h` 模型中。设备创建/更新及分组创建/更新使用独立模型；GB28181 各路径使用独立参数模型。删除全部 14 个模块级 `schema.h`、自有 Validator 和 Controller 中的重复字段校验；数据库 `service/config/schema.h` 保持原样。
- 调试事件注册请求模型，统一使用公开 `JsonBody<T>::validate`；串口、终端的标识符及嵌套命令规则同样随模型声明，不再使用自定义 Validator 适配。
- 链路协议与端点组合、唯一性等业务校验归 Service；需要保留动态字段、原始数值、重复键或清空语义的请求，其纯解析随所属输入类型放在 `types.h`，不新建校验层。
- 普通 HTTP handler 全部返回 `Task<>`；已有具体 DTO 的接口通过 `c.json(model)` 返回模型。内部共享查询继续返回 `Task<Model>`，SSE 在事件出口序列化；文件及原始动态 JSON 使用各自的响应 API，不添加适配别名或恢复上游已移除的返回方式。
- 删除上一轮为 handler 直接返回模型增加的 `RequestContext` 外借 arena 构造函数。模型在业务操作的临时内存中构建，由 handler 在作用域释放前显式序列化；SSE 每次事件独立释放模型内存。`RequestContext` 显式包含所用的 DB/Redis 公开头文件。
- 动态 JSON 请求使用公开请求文本读取和 `JsonValue::parse`，保留原始数值表示、重复键读取、部分更新及清空语义；媒体类型检查为无 I/O 的公共工具。错误媒体类型仍为 415，畸形 JSON 仍为 400。
- 第三方指令接口改用 `jsonIf<T>`，显式处理媒体类型不匹配，不额外施加管理接口的字段规则。
- 后台 `WebWorkerContext::resource()` 统一迁移为 `pool()`，响应模型显式使用 `ModelOptions` 绑定原 Worker 的池。
- 没有修改 Ruvia 源码、添加旧 API 别名、兼容宏或转发头；没有修改历史数据库迁移及通信协议。

## 回归覆盖

新增 `request-validation` CTest，经过 Ruvia 的实际路由及请求分发管线，覆盖创建/更新差异、缺失/null/false、字段类型、重复键、畸形 JSON、400/415、查询默认值与边界、路径参数、嵌套数组约束和错误路径。追加 `Task<>` 默认结果的编译期检查，以及模型显式响应的 201 状态码、JSON 媒体类型、转义和超过 8 KiB 的内容检查：临时模型作用域结束后，HTTP 响应内容仍完整。架构检查不再允许模块级 `schema.h`，纯类型文件继续禁止 I/O。GB28181 请求测试更名为 `gb28181-request`。

### 本次 `7a60bfbc` 验证

- Windows、Linux Release 完整构建均通过，日志为 `build/ruvia-7a60-build-windows.log`、`build/ruvia-7a60-build-linux.log`。
- Linux CTest 36/36 通过；Windows CTest 35/36 通过。两平台的 `request-validation`、临时内存及架构检查均通过。Windows 唯一失败仍为 `gb28181-sip` 的 IPv6 UDP 回环收包超时，Linux 同用例通过；未跳过或修改该用例。日志为 `build/ruvia-7a60-ctest-linux.log`、`build/ruvia-7a60-ctest-windows.log`。
- HTTP/SSE、前端架构、DTU 配置契约测试 11/11 通过，共 273 个断言，日志为 `build/ruvia-7a60-contracts.log`。
- `git diff --check` 通过；两平台依赖源码均为完整固定 SHA `7a60bfbc0e4cd359ed3edb18faea0d1c7ea28483`，无本地补丁。

### 初次升级验证记录

以下为初次 `e13f6ec6` 升级的历史验证结果。

HTTP/SSE 与前端架构、DTU 配置契约测试共 11 项通过，日志为 `build/ruvia-update-contracts-final.log`。

- Windows Release 完整构建通过，日志为 `build/ruvia-update-build-windows-pass.log`。
- Windows CTest 35/36 通过，包括新增请求校验和架构检查。`gb28181-sip` 在 IPv6 UDP 回环收包处失败，独立 .NET IPv6 UDP 回环探针同样超时；未修改网络配置、跳过测试或将失败记为通过。日志为 `build/ruvia-update-ctest-windows-final.log`。
- Linux Release 完整构建和 CTest 36/36 全部通过，包括 SIP IPv6 用例；日志为 `build/ruvia-update-build-linux-final.log`、`build/ruvia-update-ctest-linux.log`。WSL vcpkg 提交已核对为 CI 固定的 `4bca8fd8654e5ba76f92661db7bfe954768ad8ef`。
- `git diff --check` 通过；两平台 Ruvia 源码均为锁定提交，依赖源码无本地补丁。

本地 Linux 使用 WSL Clang 22.1.8，CI 配置为 Clang 19；本地验证不能替代同提交的两平台 CI 发布验收。本次不发布、不部署。
