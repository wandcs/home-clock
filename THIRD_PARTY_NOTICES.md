# 第三方来源与许可

根[MIT许可证](LICENSE)只适用于项目拥有的代码、文档与原创资源。已有第三方文件及其派生资源保留各自协议和版权声明；下载的ESP-IDF/组件也按原许可证使用。

## 代码和生成工具

| 内容 | 来源与许可 | 仓库中的归属文本 |
| --- | --- | --- |
| 本板BSP | Waveshare / Espressif，Apache-2.0；本项目保留固定版本兼容副本 | [BSP LICENSE](components/waveshare__esp32_p4_wifi6_touch_lcd_7b/LICENSE) |
| 拼音处理来源 | python-pinyin/pypinyin，MIT | [pypinyin](LICENSES/pypinyin-MIT.txt) |
| 简繁转换来源 | BYVoid/OpenCC，Apache-2.0 | [OpenCC LICENSE](LICENSES/OpenCC-Apache-2.0.txt)、[NOTICE](LICENSES/OpenCC-NOTICE.txt) |
| 节假日数据与年度fixture | NateScarlet/holiday-cn，MIT；项目有解析、缓存及格式转换 | [holiday-cn](LICENSES/holiday-cn-MIT.txt) |

ESP-IDF 6.1和19个固定注册表组件由构建环境/组件管理器取得，不作为本仓库自有MIT内容重新授权。版本约束位于firmware/dependency-constraints.txt，安装后保留组件原始许可证。

## 字体、图标与程序图形

| 内容 | 来源与处理 | 许可文本 |
| --- | --- | --- |
| 中文/拉丁字形 | Noto Sans CJK、Noto Sans，裁剪并生成固件字形 | [OFL](assets/application/OFL.txt)、[NotoSans](assets/application/NotoSans-LICENSE.txt) |
| 线图标与舒适表情 | Microsoft Fluent UI / Fluent Emoji；项目有几何、配色、尺寸和栅格化修改 | [Fluent](assets/application/Fluent-LICENSE.txt)、[comfort LICENSE](assets/application/comfort-LICENSE.txt)、[修改说明](assets/application/comfort-NOTICE.txt) |
| Unicode映射数据 | Unicode数据条款 | [Unicode](assets/application/Unicode-LICENSE.txt) |
| 天气背景 | 本项目原创程序图形/字段，不含下载天气照片或厂商动画 | [背景说明](assets/weather-background/NOTICE.md)；原创部分MIT |
| 三音铃声 | 本项目原创数学合成，不含第三方采样或厂商铃声 | [音源说明](assets/alarm-audio/README.md)；MIT |

## 地点数据

当前索引经过本地选取、合并、显示名转换、拼音/别名处理和二进制编码；不是原来源的未修改官方清单。来源版本、当前文件摘要与覆盖限制见[地点来源说明](docs/open-source/data-sources.md)。

| 来源 | 许可与说明 |
| --- | --- |
| [GeoNames](https://www.geonames.org/) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)，保留归属与修改说明 |
| areacodes | [CC0文本](LICENSES/areacodes-CC0.txt) |
| [Wikidata](https://www.wikidata.org/wiki/Wikidata:Licensing) | 结构化数据CC0 |
| 台湾内政部地政司行政地名数据 | [政府资料开放授权条款1.0](LICENSES/Taiwan-OGDL-1.0.txt)，[官方数据集40281](https://data.gov.tw/dataset/40281) |
| IANA tzdb | [原许可说明](LICENSES/IANA-tzdb.txt)，公开领域/个别保留项按原文 |

资源旁的[Location-NOTICE](assets/application/Location-NOTICE.txt)应与数据一起保留。历史研究标记release_ready=false不等于统一禁止再分发；本次归属核对不覆写旧报告，也不认证全国或全球现势完整性。

## 在线天气服务

- [Open-Meteo](https://open-meteo.com/)：数据许可、[署名](https://open-meteo.com/en/licence)及[服务条款](https://open-meteo.com/en/terms)分别适用。免费端点仅适用于其条款允许的非商业用途，商业使用需选择相应服务；项目MIT不改变这些限制。
- [MET Norway](https://api.met.no/)：按[接口使用要求](https://api.met.no/doc/TermsOfService)提供可联系的User-Agent并遵守数据归属要求。数据另遵守[MET列明的来源许可](https://api.met.no/doc/License)，包括NLOD2.0和CC BY4.0的归属要求。公共构建默认不带维护者的联系信息，由使用者显式配置后才启用。

本清单记录实际来源与分发义务，不把软件、数据和在线服务当作同一种授权。
