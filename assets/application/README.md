# 应用资源

本目录保存正常固件构建直接使用的字体、图标和地点索引，文件大小与摘要见manifest.json。构建无需下载原始字体、启动浏览器或重生成地点库。

| 内容 | 形式与来源 |
| --- | --- |
| 28/44/240px字形及天气小字形 | Noto Sans CJK / Noto Sans的子集与固件编码，保留OFL条款 |
| 天气及通用图标 | 同风格SVG与运行时ARGB/A8资源；Fluent派生部分保留原MIT署名 |
| 舒适图标 | Fluent Emoji派生几何、克制配色和栅格化，见comfort-NOTICE.txt |
| 地点索引与元数据 | 当前R5选取范围，名称/坐标/别名和来源归属见地点来源说明 |

[第三方许可](../../THIRD_PARTY_NOTICES.md)与[地点数据来源](../../docs/open-source/data-sources.md)应随本目录保留。Location-NOTICE.txt、Location-OGDL.txt、Fluent/comfort/Unicode许可证以及OFL.txt、NotoSans-LICENSE.txt不是项目MIT的替代品。

当前使用的240px字形为静态A8，七日天气大图标为80px A8；其余按各自manifest记录使用。原始字体、源数据抓取和历史制作流程不作为首次构建前置。需要修改或再生成时应记录对应上游版本、参数和新输出摘要，不用不相关机器环境或空资源绕过核对。

字节资源与C数组可能同时用于重建和审阅，正式CMake只链接其声明的那一份，不能同时重复嵌入同一个数据。目录整理不改变已经使用的字形、图标或索引字节。
