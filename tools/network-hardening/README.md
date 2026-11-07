# 锁定网络源的局部修正

本目录只修正当前锁定 ESP-IDF 6.1、esp-hosted 3.0.9 中已审阅的连接/SDIO 路径。生成器以原源 SHA-256 和唯一上下文为前提，把单文件副本写入本次构建目录，不修改共享 SDK 或 managed component。

| 修正 | 原源与真实编译 target | 生成器 |
| --- | --- | --- |
| 非阻塞连接等待及错误判断 | esp_tls.c；esp-tls 的 COMPONENT_LIB | [tls/prepare.py](tls/prepare.py) |
| SDIO 接收暂存及失败处理 | eh_host_bus_sdio.c；eh_host_mcu_transport | [sdio/prepare.py](sdio/prepare.py) |

正常使用 [公开构建入口](../../docs/building.md)。firmware 的 project 完成后接入 [apply.cmake](apply.cmake)，定位真实 target 的 SOURCES，先确认对应源唯一，再生成 `clock-network-fixes/<项目>/` 下的替代源；原 target 的依赖、定义和私有包含路径继续使用。esp-hosted 的顶层 IDF 组件不直接编译 SDIO 文件，不能替换错 target。

每次正常重配都会重新核对原输入并确定性生成；重复 include 有全局保护。原源 hash 不符、上下文改变、生成器缺失或目标源不唯一时配置失败，不自动退回未修正源。升级 SDK/组件时先重审源码和定向回归，再更新锁定输入，不能只改版本号或删除 hash 门槛。

`python tools/check-build.py --source-only` 只读检查项目所需源、头文件和生成器是否存在；它不调用 SDK，也不证明外部依赖 hash 匹配。外部 hash 由配置阶段的生成器核对，实际编译与设备行为分别记录。TLS 的三条宿主红绿入口见 [TLS 说明](tls/README.md)；网络内存诊断源在 firmware/main/network_memory.c/.h，设备侧内存/恢复证据由统筹集成后确认。

生成器不下载、升级依赖或调整全局 SDK 配置。派生 SDK/Hosted 源保留上游版权与 Apache-2.0；项目 MIT 不重新授权第三方内容。构建目录内的原文副本及测试二进制不提交仓库。
