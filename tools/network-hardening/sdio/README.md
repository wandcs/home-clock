# SDIO RX 内存不足恢复

此目录为锁定esp-hosted 3.0.9生成一个构建内SDIO源副本，不修改组件、SDK或C6固件。原源SHA不匹配或替换上下文不唯一时拒绝生成。

## 构建接口

从仓库根目录执行：

    python tools/network-hardening/sdio/prepare.py --source firmware/managed_components/espressif__esp_hosted/host/mcu/eh_host_mcu_transport/src/eh_host_bus_sdio.c --output <build-dir>/clock-network-fixes/eh_host_bus_sdio.c

生成器使用同目录的 streaming-rx.inc，产物内联全部补丁，无额外编译头或库。输出已相同则不重写，避免重配导致无谓重编。集成由项目构建规则替换真实component target中的唯一原SDIO源；不要把生成副本和原源同时编译。

- 原SHA：860b49c0f1bb177cf2cbdf99c4e6b992717efffde99c0324f340a99d2f5b93ae。
- 启动在其他SDIO资源/线程创建前预留2×1536B单帧DMA；不是猜测完整C6 burst容量。
- 瞬时增长分配失败保留原缓冲、在同一pending窗口内解锁等待20ms后重试一次。
- 持续OOM或超出port可无损传入的65024B线性容量时，用未发布head槽分块真实排空该批数据；每次请求地址对应本小块，成功才计数，尾块padding不消费后来入站。丢弃这批数据，保留后续接收机会。
- 不启用大packet pool、不改slot数量/PSRAM/cacheline/TLS策略。每次bus初始化仅首个DMA不足事件WARN，后续DEBUG；真正CMD53错误保持ERROR和TRANSPORT_FAILURE。

## 离线红绿

需要Python 3.9+、已解析的仓库esp-hosted组件源码和Linux GCC/ASan/UBSan。Windows通过当前桌面用户的默认WSL运行GCC；不执行SDK构建、设备、网络或依赖下载。

    python tools/network-hardening/sdio/run.py --output .local/s25/sdio/check-01

输出须为新的仓库.local子目录。入口从实际锁定源码提取reader/drain/缓冲生命周期及IDF port的block/byte拆分函数，配可控FIFO、DMA与RTOS替身：

1. 原版一次DMA失败后清IRQ却留下5003B，随后等待新IRQ。
2. 修正版7组：启动失败回滚、单帧持续OOM可收、一次增长失败重试、持续OOM排空与后来新IRQ/尾块补齐/计数回绕、线性容量超界、head/tail生命周期和重复teardown、部分底层读失败如实计数与事件。持续OOM后解除压力的下一大请求仍可完整收到，WARN不洪流。
3. 原源摘要拒绝/确定性输出检查，ASan/UBSan。

完整命令、输入与生成源SHA及结果写入输出checks.json。FIFO是基于官方协议的模型；未证明原家庭故障唯一根因、真实C6分块时序或硬件总线错误自动恢复。TCP数据丢失可由TCP重传；RPC等非TCP数据可能使当前调用超时，由调用方后续再试，不冒称所有消息无损。

接收寄存器与 block/byte 拆分沿用锁定 esp-hosted 源码；上述宿主模型不能代替硬件总线验证。生成产物保留Espressif源文件的Apache-2.0 SPDX；仓库自有生成/测试代码遵循根LICENSE，不覆盖第三方许可。
