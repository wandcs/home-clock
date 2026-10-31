# 架构与模块边界

生产代码在firmware中；main负责设备和页面，features负责功能，core保存不依赖ESP-IDF或LVGL的纯C计算。

| 目录 | 职责 |
| --- | --- |
| firmware/main | 启动、共享状态、传感器、网络校时、天气/地点与三页UI |
| firmware/features/alarm-management | 编辑、音量、持久化与运行时所有权 |
| firmware/features/alarm-trigger | 到点、计划重排及防重复 |
| firmware/features/alarm-store | ALM1编解码与NVS端口 |
| firmware/features/alarm-audio | 音源、播放器及I²S/codec/功放适配 |
| firmware/features/calendar-* | 年度取得、解析、缓存与批准快照 |
| firmware/features/environment | 传感器快照与环境策略适配 |
| firmware/features/met-weather | MET当前天气解析 |
| firmware/core | 亮度、舒适、日照、日期与闹铃计算 |
| components / assets | 固定BSP与可追溯资源 |
| tools | 构建、生成和宿主验证 |

~~~mermaid
flowchart LR
  Sensors[SHT40 / BH1750] --> State[共享快照]
  Network[Wi-Fi / 时间 / 天气 / 年历] --> State
  State --> UI[LVGL界面]
  Core[纯C策略] --> UI
  Core --> Alarm[闹铃所有者]
  UI --> Alarm
  Alarm --> Store[NVS]
  Alarm --> Audio[音频worker]
  Audio --> Speaker[功放与扬声器]
~~~

## 所有权

BSP统一建立显示、触摸和I²C。传感器发布快照，UI取快照刷新，状态锁内不做网络、I²C或日志等阻塞工作。

闹铃所有者协调编辑、运行计划和保存。重新开启或改变已开启计划时重排，同值保存与普通校时不生成新计划；一次性结束关闭，循环保留未来安排。

音频worker独占I²S和codec。界面发出非阻塞请求，停止先关功放，后续由worker清理。试听复用同一播放器，真实闹铃优先。

天气/年历共用受控HTTPS预算；天气失败不拖住本地时钟、传感器和闹铃。仅主源失败才试已配置备源，不增加正常更新频率。

## 配置与资源

各服务负责自己的NVS数据。公共构建不读取开发者Wi-Fi文件；MET联系信息需要显式配置。

字体、地点索引、天气字段和音源已跟踪生成资源，普通构建不需下载原始字体或重生成地点库。再生成需遵守对应参数与来源，不修改旧报告伪装新证据。

公共构建默认正式配置，关闭串口开发控制。宿主、模拟和实物证据区分记录；编译不等于实际声音或触摸通过。目录整理不改变算法、页面、音源字节或存储格式，也不自动更新已正常使用的开发板。
