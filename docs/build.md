# 源码构建

依赖统一由 CMake FetchContent 获取；不需要安装或初始化 vcpkg。
Ruvia 固定完整提交，其他依赖固定提交或发布归档哈希。
依赖源码、原生构建和生成文件全部保存在构建目录中。

## 工具链

- CMake 3.28 或更新版本、Git、Bun 1.3.14。
- Linux：Clang 19、GCC 14 标准库、Ninja、Perl、Meson、Bison、Flex、pkg-config、Make、NASM。
- Windows：MSVC C++ 工具链和 Windows SDK、Ninja、Strawberry Perl、Meson、winflexbison3、JOM、NASM，以及提供 Bash、Make、pkgconf、patch、diffutils 的 MSYS2。

Windows 请在 MSVC 开发者终端运行，并确保上述工具在 PATH 中。
MSVC 编译器和链接器应排在 MSYS2 工具之前。
具体工具安装步骤与 CI 保持一致，见 `.github/workflows/build.yml`。

## 配置、构建与测试

先安装前端依赖：

```text
bun install --frozen-lockfile
```

Linux：

```sh
CC=clang-19 CXX=clang++-19 cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

Windows：

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

首次配置会下载依赖，并编译 OpenSSL 以供媒体库的配置检测使用，耗时会较长。
从旧的 vcpkg 构建目录迁移时，在配置命令中加入 `--fresh -DCMAKE_TOOLCHAIN_FILE=`，清除旧缓存。

第三方许可证随制品打包：

```text
cmake -DDEPENDENCY_BUILD_DIR=build -DDESTINATION=build/artifact -P ports/cmake/stage-third-party-notices.cmake
```
