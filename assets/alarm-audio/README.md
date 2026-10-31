# 默认闹铃音源

当前音型为S16-fuller-three-tone-1：原创三音、22050Hz单声道s16le、4秒一组、176400字节。每音0.8秒，组尾静音1.4秒，参考峰0.8FS。源包络保持30秒由−24dB渐强至−6dB；产品保留独立音量调节。

- 参数与冻结WAV摘要：[candidate.json](candidate.json)。
- 数学合成器：[synthesize.py](../../tools/alarm-audio/synthesize.py)。
- 固件资源与摘要：[manifest.json](manifest.json)。
- 生成/核对：在仓库根运行 python tools/alarm-audio/generate.py，或加 --check 只核对。
- 可选WAV输出：python tools/alarm-audio/synthesize.py，写入忽略目录 .local/generated/alarm-audio，不播放声音。

reference-4s.pcm供重建核对，正式固件只编译alarm_reference.c，不重复嵌入PCM。音源为项目原创数学合成，按根MIT许可证授权；不含第三方采样。旧研究和实测证据按当时版本保留，目录归位不改变音源字节。
