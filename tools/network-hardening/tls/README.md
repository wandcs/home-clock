# 非阻塞 TLS 连接修正

固定 ESP-IDF v6.1 提交 `fff9895c82d744c7237be8847347bdd1b07c6643`，只修改 `esp_tls.c` 的非阻塞 CONNECTING 分支：每轮 select 前重建 fd_sets；select 失败和非零 SO_ERROR 直接失败并保留系统错误。证书、主机名校验、TLS 握手、DNS 和全局配置均保持原实现。

生成构建目录内的替代源：

```powershell
python tools/network-hardening/tls/prepare.py --source .local/esp-idf-v6.1/components/esp-tls/esp_tls.c --output .local/s25/tls/esp_tls.c --manifest .local/s25/tls/manifest.json
```

生成器核对原始字节 SHA-256 和三个唯一上下文，不修改 SDK，不接受原路径覆盖。`connect-select.patch` 用于审阅；生产接入应调用生成器，避免把通用补丁模糊应用到未审阅版本。

定向验证：

```powershell
python tools/network-hardening/tls/run.py
```

依赖 Python 3、当前桌面用户注册的 WSL/其中的 cc（Linux 直接使用 sh/cc），以及已安装的锁定 SDK；不安装依赖、不构建 SDK、不使用开发板。输出在 `.local/s25/tls/result.json`。旧版程序应退出 2，修正版应退出 0，驱动器仅在三条旧失败/新成功且原 SDK 未变时退出 0。

测试使用原/修正后的实际 `esp_tls_low_level_conn`、`esp_tls_conn_new_async` 和 `ms_to_timeval`，不伪造 select 或 SO_ERROR 返回值。真实 loopback 监听队列先被占满，pending TCP 首轮超时；释放队列后 SYN 重试完成，再调用状态机。另用 SIGALRM 产生真实 EINTR、连接未监听端口产生真实 ECONNREFUSED。只在准备连接和 TLS 密码学处设置窄 seam：不声称覆盖完整证书握手或 ESP32/lwIP 行为。

[CMake 接入](../apply.cmake)在 project 后定位 `esp-tls` 的真实 COMPONENT_LIB，生成 `${CMAKE_BINARY_DIR}/clock-network-fixes/tls/esp_tls.c`，严格替换 target SOURCES 中唯一的原 `esp_tls.c`，保留同 target 的 private includes/definitions。原组件公开包含目录已包括其源目录，私有包含目录含 `esp_tls_private.h` 等；无需把原 SDK 原文整包复制到仓库。配置时将原源、生成器设为 CMAKE_CONFIGURE_DEPENDS，生成失败直接 FATAL_ERROR。切 SDK 必须重审补丁，不静默回落原实现。

上述测试只覆盖锁定连接函数的恢复与错误处理，不代表原家庭长期故障已唯一归因。生成源保持 Espressif 原 Apache-2.0 头；这是本项目局部补丁，不冒称上游官方合入。
