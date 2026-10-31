# 本地兼容范围

本目录来自官方 `waveshare/esp32_p4_wifi6_touch_lcd_7b 3.0.1` 发布包，保留许可证；精确来源、发布包及逐文件哈希见 `UPSTREAM.json`。

本地 `.gitattributes` 保留发布包原始字节，避免 Windows checkout 的换行转换破坏这些来源哈希；原版 Kconfig 的四行尾空白也保留，并仅对该文件说明空白例外。来源说明与此换行规则不属于上游发布包。

B3 仅将 manifest 的 LVGL `~9.5.0`、adapter `~0.6`、codec `~1.5` 改为本次验证的 `9.6.0`、`0.7.3`、`1.6.2`。C 源码和其余文件未改。它是本项目的 3.0.1 兼容副本，不是厂商发布的新版本，也不表示音频、USB 或网络已验收。新证据若需要源码补丁，必须同步本说明与来源对比。

采用 Component Manager 的 `override_path`，没有在生成的 managed_components 内保留隐式修改。更新 BSP 时重新比较上游，能移除的补丁及时移除。详见 `docs/version-review.md`。
