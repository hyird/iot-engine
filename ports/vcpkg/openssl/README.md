# OpenSSL 构建适配来源

本目录从 [Ruvia 的 OpenSSL overlay](https://github.com/hyird/Ruvia/tree/e4058c9573d8021d0694a79aebae41883fa4760d/ruvia-web/vcpkg-overlay/openssl) 同步，固定来源提交为 `e4058c9573d8021d0694a79aebae41883fa4760d`，对应 OpenSSL `3.6.4`、port-version `2`。

新版本 Ruvia 的公开 HTTP/QUIC 实现需要支持 QUIC 的 OpenSSL。项目保持原 vcpkg 提交，使用此 overlay 提供匹配版本；构建脚本与补丁保持上游内容，未进行风格性修改。

构建适配的 Ruvia MIT 许可证保留在 `RUVIA_LICENSE.txt`；OpenSSL 源码许可证由 port 安装的 `copyright` 保留，包元数据声明 `Apache-2.0`。本说明及 `RUVIA_LICENSE.txt` 为来源记录，不参与 port 的编译行为。
