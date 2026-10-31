# 快速开始

构建目标是微雪 ESP32-P4-WIFI6-Touch-LCD-7B，P4 v3.x、32 MB Flash。构建不需要连接开发板。完整接线、功能范围及第三方许可见[项目首页](../README.md)。

## 准备环境

1. 将完整源码放在短路径，例如 `C:\src\home-clock`。路径不要包含空格。
2. 按[乐鑫 Windows 安装说明](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/get-started/windows-setup.html)安装 **ESP-IDF v6.1** 和 ESP32-P4 工具。已安装对应版本时直接复用。
3. 打开该版本已激活的 ESP-IDF PowerShell 终端，再进入仓库根目录。环境激活方法见[官方说明](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/get-started/windows-start-project.html#activate-the-environment)。推荐 PowerShell 7，Python 至少3.10。

## 检查并构建

```powershell
Set-Location C:\src\home-clock
.\tools\build.ps1 -Check
.\tools\build.ps1
```

默认生成生产配置：开发串口控制关闭，控制台115200，目标ESP32-P4。构建成功后查看：

```text
.local/public-build/home_clock.bin
.local/public-build/home_clock.elf
.local/public-build/bootloader/bootloader.bin
.local/public-build/partition_table/partition-table.bin
.local/public-build/flasher_args.json
```

脚本不会连接或烧录设备。正常构建使用仓库中的字库、图形和地点索引输出，不必下载源字体、原始地理数据，也不必安装资源生成器的Pillow等依赖。

还没装SDK时，也可以用普通Python检查源码包是否完整：

```powershell
python tools/check-build.py --source-only
```

## 联系信息和设置

默认固件不嵌入联系邮箱，不读取当前目录中的 `wifi.txt` 或私有天气联系文件。Open-Meteo主天气源照常使用；需要启用MET Norway当前天气备源时，按[构建说明](building.md#可选met-norway联系信息)明确提供自己的联系配置。

烧录后，通过屏幕配置Wi-Fi和天气位置。默认闹铃音色、音量滑条、自动校时、调休日历等行为沿用固件；构建不会创建测试闹铃或改开发板上的设置。

[详细构建、故障排查及手动烧录](building.md)。
