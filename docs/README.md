# iot-engine 设计文档

本文档集描述基于 Ruvia 0.1.3、CMake、vcpkg、Redis Streams 和
TimescaleDB 的物联网 Web Service。

当前阶段只确定架构和接口约束，文档不代表相关功能已经全部实现。

## 文档索引

- [总体架构](architecture.md)
- [项目结构](project-structure.md)
- [Redis 队列设计](redis-streams.md)
- [设备与查询响应模型](device-communication.md)
- [TimescaleDB 数据模型](database.md)
- [HTTP API 草案](api.md)
- [RBAC 与设备级权限](authorization.md)
- [iot-manager 参考基线](iot-manager-reference.md)
- [重写阶段规划](rewrite-plan.md)
- [设计模式建议](design-patterns.md)

## 核心约束

1. 项目是前后端分离的 Web Service。
2. `web/` 和 `service/` 分别保存前端与后端源码，目录组织参考 `hyird/antd-admin` 的 `ruvia` 分支。
3. `package.json`、Vite 配置、`CMakeLists.txt` 和 `vcpkg.json` 位于根目录。
4. 生产部署采用一个后端进程；Ruvia 同时提供 API 和前端静态资源。
5. 主动上报、查询命令和查询响应全部经过 Redis Streams。
6. 设备适配器不能直接写 TimescaleDB。
7. 数据库事务提交成功后才能确认 Redis 消息。
8. 南向设备接入与北向 Web/API 在逻辑上分离，跨层交互只能使用 Redis Streams。
9. 除平台 RBAC 外，每台设备使用独立 ACL，支持用户和部门共享；只有创建者可以分享和删除设备。
10. PostgreSQL 业务数据优先使用 JSONB；主外键、权限、状态和时间等约束字段保留为普通列。
11. `D:\Workspace\iot-manager` 是业务兼容和迁移参考，旧项目只读。
