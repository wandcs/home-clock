# 构建固件

公共入口为 `tools/build.ps1`，使用已安装并激活的ESP-IDF环境。已验证的SDK基线为 **v6.1.0**，提交 `fff9895c82d744c7237be8847347bdd1b07c6643`；工具记录见[toolchain-lock.json](../firmware/toolchain-lock.json)。工具随SDK安装，不由本项目安装脚本自动下载或升级。

当前公开流程以Windows PowerShell为入口。PowerShell 7为推荐环境；其他系统可参考[乐鑫ESP32-P4构建说明](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/get-started/start-project.html)，本项目尚未对其他系统完成干净构建验证。

## 从完整源码构建

在已激活的ESP-IDF v6.1终端、仓库根目录执行：

```powershell
.\tools\build.ps1 -Check
.\tools\build.ps1
```

`-Check`只读检查源码、必要许可文件、资源摘要、SDK版本和工具环境，不生成配置、不下载依赖、不运行SDK构建。缺SDK时会列出缺项；只看源码包可运行 `python tools/check-build.py --source-only`。

默认输出为 `.local/public-build`。第一次构建将tracked的 `firmware/sdkconfig`复制到该目录，只将 `CLOCK_DIAGNOSTICS`设为关闭；后续保留这个独立配置的其他选择。每次显式reconfigure后build，并核生成头中的诊断开关与115200控制台。原来的 `firmware/sdkconfig`、`firmware/dependencies.lock`及开发板均不修改。

自选输出目录应位于本仓库 `.local` 下，例如：

```powershell
.\tools\build.ps1 -BuildDirectory .local/my-build
```

构建产物、配置及生成的依赖锁都留在该目录。复用已有构建时要保持SDK与路径一致；需要从头构建时选择新的空目录即可。升级到当前Windows短编译器路径处理后，旧缓存仍记录原编译器路径，需选择新的 `-BuildDirectory`。脚本会提示，不自动删除缓存。

## 依赖与资源

[组件清单](../firmware/main/idf_component.yml)指定直接依赖和本地BSP，[约束文件](../firmware/dependency-constraints.txt)固定19个registry组件版本。脚本对子进程设置该约束文件，并拒绝调用环境遗留的额外约束。首次构建由Component Manager下载缺失组件并生成独立lock；不要将tracked lock复制到新位置，其本地BSP相对路径属于原目录。[官方版本求解说明](https://docs.espressif.com/projects/idf-component-manager/en/latest/use/explanation_version_solver.html)说明了约束、清单及生成锁的关系。

网络需能访问乐鑫组件仓库；已存在的SDK工具和组件缓存可以复用。生成锁可能更新依赖层级或manifest元信息。精确组件版本与本地BSP兼容修改见[兼容说明](../components/waveshare__esp32_p4_wifi6_touch_lcd_7b/COMPATIBILITY.md)。

正常固件构建直接编译已提交的字体、图形、音型和地点索引，不需要原开发机的`.local`内容。天气提示的小字补充包同样已生成，来源和缺字范围见[资源记录](../assets/application/weather_hint_fonts.json)，可用[生成器](../tools/generate-weather-hint-fonts.py)在固定源字体/Pillow环境重现；日常构建无需运行它。原始数据、源字体及部分生成环境属于可选的资源重生成流程，见[应用资源](../assets/application/README.md)与[天气背景资源](../assets/weather-background/README.md)。必要署名和许可随对应资源保存；doctor检查文件存在和摘要，不判断地点数据时效或替代许可审阅。

这里的可复现是从完整源码与明确版本重新构建。编译时间、安装路径等可能影响ELF和BIN摘要，不承诺不同电脑产物逐字节一致。干净源码构建的实际验证条件会单独记录，包括所复用的SDK、工具和缓存。

## 可选MET Norway联系信息

公共入口默认给构建子进程传入空的 `HOME_CLOCK_MET_CONTACT_CONFIG`，生成单个NUL字节，不读取任何已有私有联系文件。MET当前天气与未来完整日期的参考温区备源因而均未配置；Open-Meteo主源和低频空气质量查询不受影响。

需要启用上述备源时，在忽略目录中自行创建JSON，例如 `.local/my-weather-contact.json`：

```json
{"met_user_agent":"home-clock/0.4 (+https://example.org/contact)"}
```

将示例地址替换为你自己的可联系页面或邮箱，再显式构建：

```powershell
.\tools\build.ps1 -MetContactConfig .local/my-weather-contact.json
```

该值会嵌入固件，可从构建产物中读取；它不在界面显示，构建工具也不打印内容。不要将真实联系文件提交到仓库。每次需保持启用时都提供该参数；不提供参数会恢复无联系配置。`wifi.txt`不是公共构建输入，Wi-Fi密码通过设备触屏配置。

旧私有构建入口仍为 `build-application.ps1` / `build-display-touch.ps1`，保留其历史SDK路径及默认联系文件行为；公共流程不依赖它们。生成器在上述scoped环境变量不存在时沿用旧行为，显式 `--config`仍可选文件。

## 开发配置

只有需要开发串口控制时才显式选择：

```powershell
.\tools\build.ps1 -Development -BuildDirectory .local/development-build
```

开发控制台沿用460800的应用诊断协议；默认生产版为115200被动日志，二者不要混用。构建模式只影响产物，不自动连接设备。

## 手动烧录

确认自己的板卡是本项目目标后，使用自己电脑枚举到的端口。下面是对应随仓库配置的三镜像命令；将`COMx`换成实际端口。命令由你主动执行，会写入并重启P4，不更新C6。

```powershell
$port = 'COMx'
Push-Location .local/public-build
try {
    python -m esptool --chip esp32p4 --port $port --baud 460800 write-flash --flash-mode dio --flash-size 32MB --flash-freq 80m 0x2000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 home_clock.bin
} finally {
    Pop-Location
}
```

自选输出目录时替换上面的目录。具体产物信息以本次生成的 `flasher_args.json` 为准。构建入口不执行这条命令，也不要求使用项目历史备份或验收证据工具。

## 常见缺项

- `IDF_PATH`、编译器或SDK Python包缺失：重新打开对应v6.1的ESP-IDF终端后再执行检查。
- 激活脚本要求不存在的Python虚拟环境：使用该SDK安装时匹配的Python或对应IDF终端；系统默认Python版本可能与已安装的SDK环境不同。
- SDK、工程路径带空格：按[官方路径限制](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/get-started/windows-start-project.html#start-a-project)换到无空格短路径。Windows的MinGW工具链会展开多库头文件路径，长工具根可能报标准库头不存在。实际Windows构建会从当前SDK的GCC位置推导编译器根，在本仓库忽略目录创建 `.local/tc` junction，并只给构建子进程加入其 `bin` 路径。它复用同一套编译器，不修改SDK或全局PATH，不依赖原开发机的别名。已有映射仅核对；路径冲突会明确停止并保留原项。`-Check`不创建映射。SDK和工程仍应使用无空格短路径。
- 资源摘要不符或文件缺失：重新取得完整源码包；不要用空数据文件或占位字体绕过检查。
- 联系JSON无效：检查`met_user_agent`字符串；诊断不会回显其内容。

## 已验证范围

公共入口已从不含原Git历史、个人配置、managed组件目录和旧编译树的源码快照完成正式构建，四个纯C核心实际编译链接，诊断关闭、控制台115200。验证复用了已安装的ESP-IDF 6.1环境与注册表下载缓存，禁用ccache；不是在全新电脑上重装全部工具的验证。此轮没有烧录设备。
