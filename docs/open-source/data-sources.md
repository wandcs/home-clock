# 当前地点资产的来源与分发归属

2026-10-30。用于当前公开快照的第三方数据说明；项目自有代码的MIT不替代这些数据与字体许可。不包含原始网页/接口响应、机器路径或实机表。

## 当前资产与范围

版本：`R5-20261020:0e04c9a0b4709bd20c081377c5b729d65b9b052c138bf338f05cec69424d4d5f`。合计5168项，其中5138项可选、30项仅为层级锚点；可选来源组为大陆3182项（`f3-r3-20261019-v2`）、港澳台418项（`f3-r5-hmt-2026-10-20`）、海外城市1538项（`f3-r5-foreign-2026-10-20`）。这描述已打包范围，不是完整全球地名库或当前官方全国行政清单。

| 当前文件 | 字节 | SHA256 |
| --- | ---: | --- |
| `assets/application/locations.lco` | 1357525 | `0e04c9a0b4709bd20c081377c5b729d65b9b052c138bf338f05cec69424d4d5f` |
| `assets/application/location_metadata.bin` | 355582 | `1d102ec715c634ebf5665a08aa1d8341fca52a5f25a79d9e4199823ef79d39b1` |
| `assets/application/location-catalog.json` | 1481853 | `90cc59a9383c0a070c9d312233f9b47f0797a5a7b30e13f30922300354414543` |

逻辑输入catalog摘要为 `ae9cd4d084f94fabec096c8c6d6ed762d84c471153e5196663ad180f76dd9b14`。当前索引、元数据与说明应成组分发，不用在线每日变化的同名下载地址冒充原冻结包。

## 上游、许可与加工

| 来源 | 固定来源/范围 | 许可与归属 |
| --- | --- | --- |
| GeoNames | CN、HK、MO、TW国家包，cities15000、admin1/admin2、countryInfo冻结包 | CC BY4.0；署名GeoNames并链接来源/许可，声明本项目筛选与加工。官方[原包说明](https://download.geonames.org/export/dump/readme.txt)确认许可。 |
| yescallop/areacodes | `data/2025.txt`，研究记录提交 `2da87e7648f0da7e10dea39e8a613107ed3fe273` | [CC0](https://github.com/yescallop/areacodes/blob/master/LICENSE)；作为行政名称/代码身份源，不称官方认证。 |
| Wikidata | 2026-10-19 R3-v2及2026-10-20 R5结构化实体/声明快照；缺少lastrevid的请求以原响应SHA版本化 | [结构化数据CC0](https://www.wikidata.org/wiki/Wikidata:Licensing)；不把说明文字或整页HTML当CC0再分发。 |
| 臺灣內政部地政司 | 2026，臺灣地區地名資料_行政區域類2026，资料集[40281](https://data.gov.tw/dataset/40281) | [政府資料開放授權條款第1版](https://data.gov.tw/license)；保留机关、年度、资料集及加工声明。 |
| IANA tzdb | 2026e，asia/europe/zone1970地域时区依据 | 公有领域，保留来源及现有LICENSE文本；来源时区与产品用时选择分开保存。 |
| pypinyin/OpenCC | 固定pypinyin0.55.0、OpenCC0.1.7，用于拼音和繁简加工 | pypinyin MIT、OpenCC Apache2.0及NOTICE；相应完整文本随公开包LICENSES保留。 |
| Unicode | 15.0.0规范化映射 | 保留 `assets/application/Unicode-LICENSE.txt`，不将Unicode数据重新标成项目MIT。 |

加工包括：身份及层级关联、候选代表点筛选、别名与中文标签选择、繁简转换、拼音/首字母检索键、时区政策和压缩索引生成。代表点用于天气查询，不是街道门牌或导航定位。GeoNames与Wikidata可能相互引用，不把一致记录称独立实测。官方行政网页、香港/澳门公开页面及Getty个别条目只用于有限事实佐证；原网页/原响应缓存不进入公开包，也不额外主张其页面图像/整库许可。

归属声明：**地点数据基于GeoNames（CC BY4.0）、yescallop/areacodes与Wikidata结构化数据（CC0）；台湾行政地名基于內政部地政司2026资料集40281（OGDL1.0）。本项目进行筛选、关联、繁简/拼音处理和索引转换，不代表来源机构背书。** 字体另遵循Noto SIL OFL1.1；完整版权与许可见项目THIRD_PARTY_NOTICES及LICENSES和资源旁许可。

## 冻结原包摘要

这些是已有研究源清单提炼的公开URL与原包摘要；不是本轮重新下载，更不是声称上游当前同名文件仍保持相同字节。

| 原包/输入 | SHA256 |
| --- | --- |
| [europe](https://data.iana.org/time-zones/tzdb-2026e/europe) | `fec11bf6c6bd0619a8777bbdaa2ecd184fde291cb4b97efa87d334c7145ba3c2` |
| [zone1970.tab](https://data.iana.org/time-zones/tzdb-2026e/zone1970.tab) | `5f23781702bf51408ff1581daa109ab3b9257fb433b1554e579d92172892c0cf` |
| [asia](https://data.iana.org/time-zones/tzdb/asia) | `02b69728085f6b0c999da70de2f1f9a2d8952b533a450d53bb41e03eb7d0e92e` |
| [CN.zip](https://download.geonames.org/export/dump/CN.zip) | `d5c8b5b5c71c5e23eee0bd21fa6856fe4a55c0e2c8dc81b70b4c1047ee50bd81` |
| [HK.zip](https://download.geonames.org/export/dump/HK.zip) | `17890946471bf6374b48a02a4e52633b50699c5b2e8097f3b3a3289721e86757` |
| [MO.zip](https://download.geonames.org/export/dump/MO.zip) | `a05c804956245583c714b8ada9fee0005a7a4a0a9a7d6a57f52ed702ca6eba9f` |
| [TW.zip](https://download.geonames.org/export/dump/TW.zip) | `665d5ab419e60f8224d642512d6f0f23fff5ec984d19b637150478db2ef6bfe9` |
| [admin1CodesASCII.txt](https://download.geonames.org/export/dump/admin1CodesASCII.txt) | `b577baa3476bbe94f0454545e517e1835401d79c5b1105c8e02cf1d4a505e8ec` |
| [admin2Codes.txt](https://download.geonames.org/export/dump/admin2Codes.txt) | `985b37390cad51f51b03897e503748db5acddfd9442438532cfab1ac9fd1cbcd` |
| [cities15000.zip](https://download.geonames.org/export/dump/cities15000.zip) | `e900af1e095974fe987247255de6de022b35f5dde5099ab0d186e17ecf06ce96` |
| [countryInfo.txt](https://download.geonames.org/export/dump/countryInfo.txt) | `c55f638cfaafb4fe45a9aa15120a0f1f78e8a34a8e1847d7b9320cf740fbc9ab` |
| [download](https://opdadm.moi.gov.tw/api/v1/no-auth/resource/api/dataset/C8DB1578-7554-4DBA-82AD-AD0105992C68/resource/167ADA8B-A97E-4261-ACD0-329968B2F1FE/download) | `a06ac609ef7513ad03b5930e408b0a2d874ef6bb7208da5c487c4dc4695b84db` |
| [2025.txt](https://raw.githubusercontent.com/yescallop/areacodes/master/data/2025.txt) | `73520f533c8e3f1aadb3bb55166656a491fb1e7703b2e2172e5694ec8845bab0` |

Wikidata的批量SPARQL及补充实体请求由R3-v1/v2、R5摘要绑定；公开说明不复制大段查询、页面正文或原始缓存。内部源清单的文件摘要如下，用于标识本说明的历史依据，公开包使用上面已打包数据即可，不把内部历史目录作为编译前置。

| 历史依据文件 | SHA256 |
| --- | --- |
| `docs/research/f3-r3-location-catalog/source-manifest.json` | `cbea8d98862fa65df21824ab34c8672b5a295050049583449f4ab64bd6fb49a7` |
| `docs/research/f3-r3-location-catalog-v2/source-manifest.json` | `5025227b4b92ebcc9e808cbfec01045960757e39b610945b6f0a432a878e466b` |
| `docs/research/f3-r5-location-coverage/source-manifest.json` | `e081878a19323473af7077fca50b8e24a572b70180d40d43f78ec24044b571ac` |
| `docs/research/f3-r5-location-coverage/baseline-source-manifests.json` | `a21034887821544487e6dee6c3a2c457faec63966fb072a3221e18aeaf18a749` |

## `release_ready=false` 的含义

当前资产将原研究行标志保留为 `research_release_ready=false`。R3/R5说明中的未完成事项原为正式输入、天气时区/日期、P4资源、字形与触摸接入；该值不是第三方许可证的拒绝标记，也不是本说明宣布数据受来源机构认证。后续接入完成不应通过改写历史记录来伪装旧阶段已发布。

本说明为当前精确资产建立来源/许可/加工归属依据；公开快照还必须保留对应署名与许可证，且不携带原始网页缓存/研究实机表。更新数据时应产生新的数据版本、哈希和加工说明；不要对所有历史行直接翻转为true。
