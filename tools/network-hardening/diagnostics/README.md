# 轻量分配失败诊断检查

仅检查生产network_memory模块的回调注册、能力过滤、首条保留和并发快照。不会运行UI、SDK构建、串口或设备。

从仓库根，以已有桌面用户环境执行：

```powershell
python tools/network-hardening/diagnostics/check.py --out .local/s25/diagnostics/run-new
```

这些是可选的维护测试，不是正常构建的前置。前置为 `.local/esp-idf-v6.1` 下的锁定ESP-IDF6.1源码、WSL/GCC及现有ASan/UBSan/TSan；输出必须是.local/s25/diagnostics下的新目录。日志、完整命令、生成的SDK注册/失败分发原函数和输入摘要保留在该目录。没有下载/安装或全局配置改变。

测试提取锁定heap_caps.c中的注册函数与失败分发函数，并调用真实生产回调；失败的size/caps由宿主注入，不宣称已在P4耗尽内存。cap值与回调typedef来自锁定SDK头。原FreeRTOS SAFE临界区在宿主用pthread互斥量替代；线程测试验证一致快照，不能代替目标ISR/IRAM布局审阅。

两个小场景：筛选/首条记录，以及4个并发写者各2000次加一个快照读者。普通allocator/free/printf/puts在回调中被wrapper拒绝；ASan/UBSan覆盖两个场景，TSan复用同一并发场景。若宿主TSan明确因WSL地址布局不可用，原失败会保留并报告为未覆盖；实际race或断言不会按环境问题放过。
