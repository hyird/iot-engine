# iot-engine

## 编译目录约束

项目只允许使用仓库根目录下的 `build/` 作为 CMake 配置和编译目录。不要创建或使用
`build-default/`、`build-debug/`、`cmake-build-*` 等其他编译目录。

统一使用以下命令：

```bash
cmake -S . -B build
cmake --build build
```

运行后端时也应使用 `build/` 中的产物：

```bash
./build/iot-engine
```
