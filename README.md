# iot-engine

当前支持 Linux、macOS 和 Windows。Windows 使用系统已安装的默认 MSVC，Ruvia 跟随上游
`main` 分支的最新提交；vcpkg 目标 triplet 为 `x64-windows-static`。

## 编译目录约束

项目只允许使用仓库根目录下的 `build/` 作为 CMake 配置和编译目录。不要创建或使用
`build-default/`、`build-debug/`、`cmake-build-*` 等其他编译目录。

统一使用以下命令：

```bash
cmake -S . -B build
cmake --build build
```

Windows（MSVC）使用：

```powershell
$env:VCPKG_DEFAULT_TRIPLET = "x64-windows-static"
$env:VCPKG_DEFAULT_HOST_TRIPLET = "x64-windows-static"
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build --output-on-failure --build-config Debug
```

默认构建会同时生成后端可执行文件、将前端打包到 `build/web/`，并把根目录 `.env`
复制到 `build/.env`。如果 `.env` 不存在，CMake 会给出警告且不会生成该文件。

运行后端时也应使用 `build/` 中的产物：

```bash
./build/iot-engine
```

Visual Studio 多配置生成器的 Windows 可执行文件位于 `build/Debug/iot-engine.exe`。
