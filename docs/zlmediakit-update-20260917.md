# 2026-09-17 ZLMediaKit 更新

## 依赖版本

ZLMediaKit 没有独立的上游 Release，采用本次检查时的最新主线提交，并固定完整 SHA 以便重现构建：

| 依赖 | 更新前 | 更新后 |
| --- | --- | --- |
| ZLMediaKit | `79d795a767da85bbba821b871265fd87d853d808` | `4b07053aa5f6d35505e8c548c2a08a978143fcf1` |
| ZLToolKit | `00f56528d28b5f4aaa94c175fe90ea9bdab4b17f` | `d7d601200f65f3111dbf9d20ed7f14d2654bbd6e` |

[上游提交](https://github.com/ZLMediaKit/ZLMediaKit/commit/4b07053aa5f6d35505e8c548c2a08a978143fcf1) 比原版本多 23 次提交，包括 RTSP Content-Base、无效 RTP 丢弃、VP9 边界检查、WebRTC 连接关闭和 RTP 字段等修复。该区间未修改 C SDK 的 `api/include`、`api/source`。

同步更新 `CMakeLists.txt` 和 `ports/cmake/patch-zlmediakit.cmake` 的版本校验。保留已有精确源码补丁、静态链接及功能开关，不跳过版本或补丁匹配检查。ICE 初始化补丁仍仅将监听注册推迟到函数局部实例初始化，未覆盖上游其他变更。`media-server` 子模块版本未变化。

## 验证与发布状态

- Windows Release 完整构建成功，包含更新依赖、应用精确补丁、ZLMediaKit、C SDK、测试及应用链接。
- Windows CTest 34/35；`gb28181-media`、`gb28181-media-proxy`、`gb28181-projector` 通过，`gb28181-sip` 在 IPv6 UDP 回环收包失败。
- 独立 .NET UDP 回环探针同样为 IPv4 收到 3 字节、IPv6 超时，与仓库此前记录一致；未修改主机网络或跳过该测试。
- 包含新 ZLM 的最终 Windows 二进制通过固件完整接口集成，日志为 `build/firmware-zlm-integration-final.log`。
- 本地 Windows 使用现有 `F:/dev/vcpkg` 安装，无法读取该目录的 Git 提交；WSL 使用固定 vcpkg 提交 `4bca8fd8654e5ba76f92661db7bfe954768ad8ef` 和 Clang 22.1.8。CI 配置为 Clang 19，故本地检查不能替代同提交的 CI 发布验收。
- Linux Release 完整构建成功，CTest 35/35 全部通过，包括 SIP IPv6 用例；不能将 Windows 的未通过项计为通过。
- 用户在获知 Windows IPv6 用例未通过后明确要求部署到 `43.142.33.210`；本次按 Linux 部署例外执行，未发布 Windows 制品，也未将本地检查记为两平台 CI 全通过。
- 已部署到 `43.142.33.210`，源码快照为 `01b4658265614c30fc53349788234da6eabd7c92`，基于 `50ddc4c` 加当时完整工作区改动生成；不移动原分支、不修改原暂存区。快照包含同时存在的 UUID、GB28181 和前端类型整理，不能将制品标记为干净的 `50ddc4c`。
- Linux 发布前再次构建与 CTest 35/35 通过。程序 SHA-256 为 `3f7a3d5070ed85cdf6f66f6511d51b93643538434a65606696c6bb293fb05579`，运行中 `/proc/<PID>/exe` 已核对一致。

## 生产部署核验

- 制品与文件哈希清单位于 `/opt/iot/releases/01b4658/`；原程序、前端、Nginx、生产配置及 `iot_engine` 数据库备份位于 `/opt/iot/deployment-backups/01b4658/`，数据库备份已检查归档目录并记录哈希。凭据仅在服务器权限受限的备份中。
- 首次切换使用程序软链接，导致 `.env` 按实际程序目录查找而未加载，健康检查失败后自动回滚。再次切换保持 `/opt/iot/server` 为实体文件，前端指向同一制品的 `web/`，约 8 秒恢复就绪；最终运行期间无启动失败和自动重启。
- 数据库仅新增 `0050_dead_letter_query_changes`，原 52 项迁移校验和全部不变，总计 53 项。回滚方案可事务撤销该新增触发器与函数，无需覆盖上线后的业务数据。
- TimescaleDB、Redis 容器健康，Redis AOF 已开启且写入正常；12 个边缘节点会话 TTL 为 85–90 秒，数据继续入库。
- HTTPS 首页外部访问 200、证书验证通过；4 个前端文件经 Nginx TLS 逐字节哈希核对。服务器经自身公网下载较大资源曾超时，随后外部客户端下载 JS、WASM 均成功且哈希一致。公网 `/internal/health/ready` 保持 404。
- 已发布的客户端与固件下载文件保留，独立哈希清单位于 `retained-downloads.json`；这些下载制品保留各自原有版本，不标记为此次构建。
- 未执行真实设备刷写、登录后全业务操作或实际视频播放验收。

构建和测试日志位于 `build/firmware-zlm-windows-build-final.log`、`build/firmware-zlm-windows-ctest-final.log`、`build/firmware-zlm-linux-build.log`、`build/firmware-zlm-linux-ctest.log`。
