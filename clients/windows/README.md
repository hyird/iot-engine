# iot-egine Windows 客户端

Windows 10/11 x64，C++20 / C++/WinRT + WinUI 3 原生 Fluent 控件，WiX 6.0.2 Burn EXE 内嵌 MSI。Windows App SDK 随包部署，用户无需另装运行时。

界面支持 Per-Monitor V2、高分屏缩放、紧凑窗口布局、系统主题。平台地址不显示在界面。勾选“记住账号密码”后，成功登录的凭据存入当前用户 Windows 凭据管理器；取消勾选会清除保存项。

仅注册一个后台服务 iot-egine.tunnel，显示名称 iot-egine。该 C++ 服务直接调用官方 WireGuardNT 驱动 API 管理网卡、地址、路由和隧道，同时订阅平台配置、刷新令牌并恢复连接。GUI 仅用于配置和查看状态，关闭 GUI 不影响隧道或配置同步。服务运行于 LocalSystem，程序安装到 Program Files/iot-egine。不包含独立隧道宿主或第二个隧道服务。内部状态保存在 ProgramData/IotEngineVpn，凭据键沿用已有标识以保留用户数据。

从桌面或开始菜单启动，登录后选择设备并应用。关闭窗口不停止后台连接。获取设备使用 WinHTTP 流式读取，避免 SSE 长连接等待填满缓冲区。

## 构建
在 Windows x64、Visual Studio 2022 C++ 工具链及 Windows SDK 环境下配置 CMake：

```powershell
cmake -S clients/windows -B build/windows-client-cmake -G "Visual Studio 17 2022" -A x64
cmake --build build/windows-client-cmake --config Release --target windows-client --parallel 4
cmake --build build/windows-client-cmake --config Release --target windows-installer --parallel 4
```

`windows-client` 会构建 WinUI、后台服务、WireGuard 原生 DLL，运行 native CTest、生成自包含包并执行打包 GUI 自测；`windows-installer` 在此基础上生成 `build/iot-egine-Setup-0.7.3-x64.exe`。已验证的包可以直接运行 `cmake --build build/windows-client-cmake --config Release --target windows-installer-from-package` 重新生成 WiX 安装包。

WinUI NuGet 依赖版本和 SHA256 固定在 winui/packages.lock.json；WireGuard 固定在 wireguard.lock.json。安装时要求管理员权限，日常 GUI 不需要提升权限。安装会绑定发起安装的用户 SID，可通过 MSI OWNER 属性指定用户。

WiX 安装事务保存服务配置，失败时恢复；不包含旧 Inno 或旧服务名的迁移路径。旧版本须先卸载。代码自测不等同于多显示器实机验收。

同一账号可在多台电脑同时使用：每台电脑生成独立密钥，服务端按客户端公钥创建 Peer 和分配独立 IP；同一公钥重试保留原 IP，退出一台不撤销另一台。同步时间按本机时区显示 yyyy-MM-dd HH:mm:ss。

安装包省略未使用的 Windows ML 引擎 DirectML.dll、onnxruntime.dll、onnxruntime_providers_shared.dll；保留 WinUI 离线运行时。后续若增加 AI/机器学习功能，须重新纳入这些依赖。

WiX 引导程序和 MSI 均禁用创建系统还原点；保留安装事务回滚。默认安装无需选择组件或目录。

设备列表在登录状态下每 10 秒自动刷新在线状态、名称、地址和网段；刷新保留未应用的勾选与搜索条件。请求失败保留上一份列表并提示错误，按间隔重试，退出登录后停止刷新。

退出登录前弹出确认，默认选择取消。后台自动刷新访问令牌及刷新令牌；正常在线运行不依赖 GUI 续期，长期停用超过刷新令牌有效期后需重新登录。
