# 天气字段来源与许可

本目录运行资源来自home-clock项目原创程序图形：R7晴光、R6柔云/雨雪与R8月面/月晕/星点/云月。原文入口与LF规范SHA在manifest中，生成器`tools/weather-background/generate.py`保留其公式、光束中心/宽度/衰减、云形和粒子范围；没有下载天气照片、月球图片、视频或第三方运行素材。

`fields.bin`是有界颜色/形状/极坐标字段，不是整屏动画帧或视频；`fields.c`是同字节的只读C表示，另含204个原粒子参数；`fields.h`列出布局偏移。目标链接fields.c，fields.bin只供独立重建/哈希验证，不重复嵌入二者。生成器用numpy/Pillow，固件不依赖它们；没有新增渲染框架或字体。

太阳贡献继承B2的0.82设备校正，网页原稿未改。CPU分层/网格/量化与原GPU场的差异逐项见manifest及docs/weather-background.md；不声称两者逐像素相同。月面为固定天气插画，不表示真实月相、月亮位置、可见性或天文观测。

报告中静态前景示意读取既有Noto字体和应用线图，只为固定布局的文字可读性观察；字体与相关图标仍沿项目已有OFL与Microsoft Fluent MIT文件，见`assets/application/OFL.txt`、`Fluent-LICENSE.txt`及R6 `ASSETS.md`。它们不进入本背景组件，也未修改、重制或新增产品图标/字体。
