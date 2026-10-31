# 硬件与接线

已验证板卡为微雪ESP32-P4-WIFI6-Touch-LCD-7B，PCB Rev1.1、ESP32-P4 v3.2、32MB Flash、1024×600触屏。Wi-Fi由板载ESP32-C6从机提供，本工程不自动更新C6固件。

[Waveshare官方资源](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-7B/Resources-And-Documents)提供本板资料。仓库使用固定版本BSP，不能直接套用其他P4板的引脚。

## 外设

- 四针SHT4X/SHT40模块，测量地址0x44。
- 五针GY-302 V322/BH1750模块，测量地址0x23；本项目ADDR未连接。
- 配套扬声器连接板上SPK，USB持续供电，不使用电池。
- 杜邦线与面包板用于原型接线。

## 共享I²C

两个传感器并联到板上I²C接口，按实际丝印识别信号，不按线色判断。

| 开发板 | SHT40 | BH1750 |
| --- | --- | --- |
| VCC，选择3V3 | VIN/VCC | VCC |
| GND | GND | GND |
| SDA，GPIO7 | SDA | SDA |
| SCL，GPIO8 | SCL | SCL |

原接线照片显示VCC选在3V3一侧，没有万用表电压或并联上拉阻值实测。复现时核对自己模块的供电范围、VCC选择和针脚顺序。

触摸与音频也使用共享I²C。BSP初始化总线，应用只增加设备句柄，不创建第二条占用相同GPIO的总线；传感器事务为100kHz。

## 上电与摆放

USB-to-UART口已用于本项目供电和调试。公共构建不自动操作串口或烧录，端口号由用户电脑决定。

温湿度模块避开屏幕与芯片热源，光照模块避免直接受屏幕自照。裸板原型验证不等同于永久安装、电气测量或外壳设计。
