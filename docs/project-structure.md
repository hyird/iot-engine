# 项目目录组织

目录组织参考 [`hyird/antd-admin` 的 `ruvia` 分支](https://github.com/hyird/antd-admin/tree/ruvia)，
保持 `web/`、`service/` 两个源码目录和根目录统一工程配置。

本项目首版不实现菜单管理，因此不创建前后端 `menu/` 模块。

## 目录结构

```text
iot-engine/
├── web/                              # React + Vite 前端
│   ├── components/                   # 通用组件
│   ├── config/                       # 前端应用配置
│   ├── hooks/                        # React hooks
│   ├── layouts/                      # 页面布局
│   ├── pages/                        # 页面模块
│   │   ├── home/
│   │   ├── login/
│   │   │   ├── index.tsx
│   │   │   ├── login.api.ts
│   │   │   ├── login.schema.ts
│   │   │   ├── login.service.ts
│   │   │   └── login.types.ts
│   │   ├── system/
│   │   │   ├── user/
│   │   │   │   ├── index.tsx
│   │   │   │   ├── user.api.ts
│   │   │   │   ├── user.schema.ts
│   │   │   │   ├── user.service.ts
│   │   │   │   └── user.types.ts
│   │   │   ├── role/
│   │   │   └── dept/
│   │   └── iot/                      # 后续 IoT 页面
│   │       ├── device/
│   │       ├── device-group/
│   │       ├── link/
│   │       ├── protocol/
│   │       ├── edge-node/
│   │       ├── alert/
│   │       ├── open-access/
│   │       └── gb28181/
│   ├── providers/                    # 全局 Provider
│   ├── public/                       # 静态资源
│   ├── routes/                       # 静态路由与权限守卫
│   ├── store/                        # Zustand store
│   ├── styles/
│   ├── utils/
│   ├── index.html
│   └── main.tsx
│
├── service/                          # C++23 + Ruvia，header-only 业务代码
│   ├── common/                       # HTTP 响应壳、AppError、分页、公共类型
│   │   ├── http.h
│   │   └── types.h
│   ├── config/
│   │   ├── app.h                     # 环境变量和应用配置
│   │   └── schema.h                  # 编译进二进制的数据库迁移
│   ├── middleware/
│   │   ├── auth.h
│   │   ├── logger.h
│   │   └── permission.h
│   ├── modules/
│   │   ├── system/
│   │   │   ├── auth/
│   │   │   │   ├── auth.controller.h
│   │   │   │   ├── auth.error.h
│   │   │   │   ├── auth.schema.h
│   │   │   │   ├── auth.service.h
│   │   │   │   └── auth.types.h
│   │   │   ├── user/
│   │   │   ├── role/
│   │   │   └── dept/
│   │   └── iot/                      # 后续 IoT 业务模块
│   │       ├── device/
│   │       ├── device-group/
│   │       ├── link/
│   │       ├── protocol/
│   │       ├── edge-node/
│   │       ├── telemetry/
│   │       ├── command/
│   │       ├── alert/
│   │       └── open-access/
│   ├── queue/                        # Redis Streams 基础设施
│   │   ├── contracts/
│   │   ├── producer.h
│   │   ├── consumer.h
│   │   ├── pending.h
│   │   └── dead-letter.h
│   ├── protocols/                    # Modbus/S7/SL651/GB28181 运行时
│   ├── utils/
│   │   ├── jwt.h
│   │   └── password.h
│   └── server.cpp                    # 唯一翻译单元与 Ruvia 启动入口
│
├── docs/
├── .clang-format
├── .env.example
├── .gitattributes
├── .gitignore
├── AGENTS.md
├── biome.json
├── CMakeLists.txt
├── package.json
├── bun.lock
├── tsconfig.json
├── vcpkg.json
├── vite-env.d.ts
└── vite.config.ts
```

## 后端模块规范

系统管理和后续 IoT 业务模块使用统一文件结构：

| 文件 | 职责 |
|---|---|
| `*.controller.h` | Ruvia Controller、路由和参数读取 |
| `*.service.h` | 业务逻辑和 SQL |
| `*.schema.h` | Ruvia 请求模型与校验规则 |
| `*.types.h` | 请求、响应 DTO |
| `*.error.h` | 模块错误定义 |

这些头文件最终由 `service/server.cpp` 包含，构成单翻译单元。当前项目规模下不再为
每个模块增加 `.cpp`，以保持与参考项目一致的直接结构。

## 系统管理首版

```text
service/modules/system/
├── auth/
├── user/
├── role/
└── dept/

web/pages/system/
├── user/
├── role/
└── dept/
```

明确不包含：

```text
service/modules/system/menu/
web/pages/system/menu/
```

角色直接保存权限代码，前端使用静态路由和权限守卫，不依赖数据库菜单生成路由。

## 南北向逻辑边界

物理目录不使用 `north/`、`south/` 名称。逻辑边界通过调用规则体现：

```text
Controller/业务服务 -> Redis iot:commands -> 协议运行时
协议运行时 -> Redis iot:telemetry / iot:responses -> 数据消费者
```

- Controller 不直接调用协议适配器；
- 协议适配器不直接写 TimescaleDB；
- 跨边界消息结构放在 `service/queue/contracts/`；
- 所有代码最终仍运行在一个 `server`/`iot-engine` 进程内。

## 根目录配置

- `CMakeLists.txt`：Ruvia FetchContent、vcpkg、单 TU 后端和前端 build target；
- `vcpkg.json`：C++ 依赖；
- `package.json`：唯一前端 manifest；
- `vite.config.ts`：`web/` 为 Vite root，`/api` 代理到 Ruvia；
- `tsconfig.json`、`biome.json`：前端类型检查和格式化；
- `.env`：前后端开发配置。

构建输出与参考项目保持接近：

```text
build/web/                         # Vite 产物
build/server[.exe]                 # 后端单进程
```
