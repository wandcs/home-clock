# 天气背景字段

本目录是原创程序图形的运行资源，不是天气照片、视频或整屏动画帧。

- fields.c为正式链接的只读字段与粒子参数。
- fields.h定义布局、尺寸和偏移。
- fields.bin保留同一字段字节供重建与摘要核对，正式构建不重复嵌入它。
- manifest.json记录原始程序图形来源、转换与误差边界。
- [来源与许可](NOTICE.md)随资源保留，原创部分采用根MIT许可。

运行渲染在[weather_background.c](../../firmware/main/weather_background.c)，调度与场景适配在[background_runtime.c](../../firmware/main/background_runtime.c)。普通固件构建直接使用已提交字段，不需要Pillow、浏览器或历史网页原型。

历史字段生成器、设计原型及采样比较保留在完整开发仓库，未作为首次构建前置打包。月面是固定天气插画，不表示真实月相、位置或天文观测。
