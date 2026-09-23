# ALSA 音频采集、原始队列与板端回放

## 一、背景与板端结论

视频链路已完成：V4L2→原始帧队列→MPP H.264→本地文件→开发板屏幕回放。
用户最新视频日志中，300 帧采集和编码计数一致、实际 25fps、EOS 正常、资源释放完成、退出码 0。
本阶段增加独立音频采集模式；AAC、音视频同时运行、编码包分发、MP4、RTMP 留待后续。

用户在开发板执行 `arecord -l`、`aplay -l` 和 `--dump-hw-params` 后确认：

| 项目 | 实际结果 |
|---|---|
| 录音设备 | 声卡 0 `rockchiprk809`，设备 0，对应 `hw:0,0` |
| 本地模拟音频播放设备 | 声卡 0、设备 0；回放脚本默认 `plughw:0,0` |
| HDMI | 声卡 1，在当前输出中只有播放设备，不作为录音输入 |
| 访问方式 | MMAP_INTERLEAVED、RW_INTERLEAVED |
| 样本格式 | S16_LE、S24_LE、S32_LE |
| 声道能力范围 | 2..8；设置单声道失败：`Channels count non available` |
| 采样率能力范围 | 8000..96000Hz；不能仅凭范围认定任意参数组合已经验证 |

因此，配置改为 `hw:0,0 / 48000 / 2 / S16_LE`。程序严格设置并读回实际参数，
不会自动退到另一个采样率或伪装成单声道。48kHz 双声道组合、麦克风通路及实际声音仍需本次板端运行确认。
双声道数据接口不代表板上一定有两个独立麦克风；可用声道需按实际录音判断。
未来若 AAC 目标仍要求单声道，应在音频处理/编码阶段明确选声道或混音，并与双声道采集配置区分。

## 二、任务与正式工程改动

| 文件 | 职责 |
|---|---|
| `include/alsa_capture.h`、`src/alsa_capture.c` | 正式 ALSA 设备模块，参数设置/读回、启动、有限等待、PCM 读取与统计 |
| `include/audio_pipeline.h`、`src/audio_pipeline.c` | 采集线程与保存线程、原始队列、文件创建、信号和错误收尾 |
| `include/media_types.h` | 音频块补充每声道累计 `sample_index` |
| `src/timestamp.c`、`include/timestamp.h` | 音频累计采样数到微秒的安全换算 |
| `src/main.c` | 新增独立音频模式，严格检查选项组合 |
| `Makefile`、`scripts/build.sh` | SDK ALSA 依赖及独立主机模拟测试目标 |
| `configs/ipc.conf` | 默认采集改为双声道 |
| `scripts/play_pcm.sh` | 在开发板耳机/扬声器回放已保存 PCM |
| `tests/mock_alsa.c`、`tests/test_audio.py`、`tests/test_audio_api.c` | 参数、短读、错误回滚、时间戳和所有权验证 |

每个新增/修改函数具有功能注释，关键资源归属、单位、失败策略有解释。
`demo/` 与 `shell/` 内容保持不变，不参与正式工程调用。`audio_encoder.c` 仍为后续接口骨架。

## 三、方案

### 3.1 参数与读取单位

使用 `SND_PCM_NONBLOCK` 打开设备，通过 `snd_pcm_readi` 读取交错 S16_LE。
采样率、声道数、样本格式必须匹配配置，按周期建议 10ms、缓冲建议四个周期进行协商，
然后读回实际 period/buffer；日志里的实际值可能与建议值不同。

ALSA 的一个 **frame** 包含每个声道同一时刻各一个样本：

- 48kHz 双声道 S16_LE：一个 ALSA frame 为 `2 × 2 = 4` 字节。
- 每声道样本数 480,000 对应 10 秒，交错数据为 **1,920,000 字节**。
- 日志的 `captured_samples` 等字段都按每声道样本数统计，不乘声道数。
- `blocks` 是应用读取块数；短读或实际周期变化会改变块数，不作为固定验收值。

短读不丢弃，直接交付实际读到的样本；末块按剩余样本数截断，保证正常有限录音不超长。
`--seconds` 表示样本时长，不是启动到退出的墙钟耗时，初始化和文件收尾会增加总耗时。

### 3.2 所有权与线程

主线程创建队列、配置设备和独占创建文件，先启动消费线程，再启动采集线程。
采集线程独占 ALSA 的启动/读取/停止；每次读取使用独立堆内存，入队成功才转移所有权。
保存线程检查格式、样本下标及 PTS，写出有效数据后释放音频块。

工作线程通过原子变量交换停止/结束状态；普通统计在 join 后由主线程读取。
文件只由消费者写入，消费者结束后主线程检查 `fclose` 结果。
队列满时不无限阻塞采集，不继续丢弃音频后假装连续运行：停止生产，报错并排空已有队列。

### 3.3 时间戳与异常

`PTS = 软件启动时刻相对应用起点的偏移 + floor(累计每声道样本数 × 1000000 / 实际采样率)`。
换算用商和余数防止中间乘法溢出，每次从累计样本数计算，避免逐块取整误差累积。
软件启动时刻只是首样本时间估计，尚未校准 ADC/硬件缓冲延迟，也未验证音视频同步。

| 情况 | 行为 |
|---|---|
| 短读 | 交付有效部分，累计实际样本数 |
| EAGAIN / 暂时零读 | 等待最多 100ms，再检查停止标志；不当作采样丢失 |
| 连续 3 秒无样本 | 超时报错停止 |
| XRUN / overrun | 统计 `xruns`，说明采样已丢失，报错停止，不静默恢复时间线 |
| 挂起 | 统计 `suspends`，报错停止，不假设中断期间样本连续 |
| 队列满 | 统计 `queue_full`，报错停止并保存队列里此前有效数据 |
| 写入/关闭错误 | 请求停止、释放剩余块，保留失败文件供排查，退出 1 |
| SIGINT / SIGTERM | 停采集、排空应用队列、关闭文件，收尾成功退出 130 |
| 达到样本数上限 | 正常排空和关闭，退出 0 |

停止时 `snd_pcm_drop` 丢弃**驱动缓冲区尚未读取**的数据；应用已接收并入队的数据仍保存。
不会从异步信号处理器里调用 ALSA、日志或互斥锁。ALSA 等待有上限，但不对内核 ioctl、
设备关闭或本地文件系统卡死承诺硬实时截止时间。

保存端记录 `peak_ch0/peak_ch1`（S16 样本绝对峰值，0..32768）。全零会给出警告，
但峰值非零只能证明存在数值变化，不能替代听音确认，也不能证明录音输入选对。

## 四、编译、部署与板端验收

### 4.1 Ubuntu 交叉编译

在工程根目录执行：

```bash
sh scripts/build.sh -B
```

沿用现有 SDK 工具链和 sysroot，正式构建默认 `WITH_MPP=1 WITH_ALSA=1`，链接
`-lrockchip_mpp -lasound -pthread`。不升级 SDK、系统 FFmpeg 或板端 ALSA。
缺少 ALSA 开发头文件会明确报告 `alsa/asoundlib.h` 路径；库/符号是否可用由实际链接确认。
需要排查时查看 SDK 的 `usr/include/alsa/asoundlib.h` 及 `usr/lib/libasound.so*`。
`WITH_ALSA=0 sh scripts/build.sh` 只用于显式禁用音频，不能用于本阶段验收。

本地基础测试、音频模拟、视频模拟和目标构建使用独立对象目录。
不得上传 `bin/host/`、`bin/audio-mock/`、`bin/audio-asan/` 或模拟驱动产物。

### 4.2 部署

| Ubuntu 文件 | 开发板路径 |
|---|---|
| `bin/ipc_camera` | `/root/rk3568_ipc_camera/ipc_camera` |
| `configs/ipc.conf` | `/root/rk3568_ipc_camera/ipc.conf` |
| `scripts/play_pcm.sh` | `/root/rk3568_ipc_camera/play_pcm.sh` |

**旧板端配置仍为 `audio.channels=1`，必须同步新的双声道配置。** 如果已有其他自定义值，
保留它们，并将下面四个音频项设置为本阶段值；不要仅更新可执行文件：

```ini
audio.device=hw:0,0
audio.sample_rate=48000
audio.channels=2
audio.sample_format=S16_LE
```

先查看帮助确认新程序包含 `--audio-capture`、`--pcm`、`--seconds`：

```bash
cd /root/rk3568_ipc_camera
chmod +x ipc_camera
./ipc_camera --help
```

### 4.3 录音 10 秒并本地回放

以下每条命令单独执行，不使用续行反斜杠。输出路径须是不存在的新文件，重复测试改名，
程序不会覆盖历史录音。录音期间对着实际麦克风说话：

```bash
./ipc_camera --config ipc.conf --audio-capture --seconds 10 --pcm /root/rk3568_ipc_camera/audio_48k_stereo_01.pcm
echo $?
wc -c /root/rk3568_ipc_camera/audio_48k_stereo_01.pcm
sh ./play_pcm.sh /root/rk3568_ipc_camera/audio_48k_stereo_01.pcm 48000 2
```

验收：

- `[alsa] actual` 为 `rate=48000 channels=2 format=S16_LE access=RW_INTERLEAVED`。
- `captured_samples=enqueued_samples=consumed_samples=saved_samples=480000`。
- `bytes=1920000`，`wc -c` 同样为 1,920,000；`xruns=suspends=queue_full=0`。
- 打印 `audio complete; queue drained and resources released`，立即查看退出码为 0。
- 板端耳机/扬声器能听到刚才的声音，语速正常、无明显断续。

`short_reads/eagain/wait_timeouts` 可以非零，应结合最终样本数及错误状态判断，不能机械要求全部为 0。
PCM 没有文件头；回放传入的采样率和声道数必须与 `actual` 一致。
脚本使用 `aplay -D plughw:0,0 -t raw -f S16_LE -r 48000 -c 2`，播完退出，Ctrl+C 可提前停止，
不依赖 Weston/Wayland。第四参数可覆盖播放设备，例如 `plughw:1,0` 仅在需要 HDMI 输出时使用。

如果程序成功但听不到声音：

```bash
amixer -c 0 scontents
```

结合 `peak_ch0/peak_ch1` 和实际麦克风连接检查输入选择、录音开关、增益与播放通路。


### 4.4 中断、重复录音与视频回归

```bash
./ipc_camera --config ipc.conf --audio-capture --seconds 0 --pcm /root/rk3568_ipc_camera/audio_interrupt_01.pcm
```

按 Ctrl+C，然后立刻执行 `echo $?`，预期 130；正常中断后四项样本计数应一致，已保存部分可播放。
更换文件名再录音，确认设备可重复打开。

最后回归既有视频模式，输出文件也必须未存在：

```bash
./ipc_camera --config ipc.conf --encode --frames 300 --encode-fps 25 --output /root/rk3568_ipc_camera/video_after_audio_01.h264
echo $?
sh ./play_h264.sh /root/rk3568_ipc_camera/video_after_audio_01.h264
```

本阶段音频和视频运行模式互斥，`--audio-capture` 不能与 `--capture/--encode` 混用。
`--pcm/--seconds` 属于音频；`--output/--frames/--encode-fps/--dump` 属于视频。
`--consumer-delay-ms` 是两种链路共有的故障验证选项，正常录制保持默认 0。

## 五、验证结果与范围

本次主机验证：

- 54 项配置/CLI、400 条并发日志及过滤、失败配置保留、9 项原始队列测试通过。
- 原有 42 项采集模拟、59 项 MPP 编码模拟及编码 API 边界检查通过。
- 新增 64 项音频集成场景通过，逐样本核对左右声道数据连续性和交错排列。
- 音频 API 检查覆盖时间戳精度/溢出、状态契约、跨读取独立内存及重复释放安全。
- 相同音频测试通过 ASan/UBSan（本环境显式关闭 LeakSanitizer）。
- 使用真实主机 `libasound.so.2` 的 `null` 插件完成初始化、读回参数、读样本及关闭检查；
  该检查只验证接口/链接，不验证麦克风、真实采样时序或 RK3568 驱动。
- 回放脚本已检查 shell 语法、参数传递与文件名含空格的处理。

本机没有用户 SDK 工具链、RK3568 声卡或板端扬声器。**目标交叉编译、双声道录音与听音、
真实硬件稳定性仍需按第四节验收。** 当前不宣称 AAC、音视频同步或长时间无丢样已完成。

## 参考

- [ALSA 官方 PCM 接口与错误状态说明](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html)
- [ALSA 官方硬件参数接口](https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m___h_w___params.html)
- [ALSA v1.2.4 公开 PCM 头文件](https://github.com/alsa-project/alsa-lib/blob/49bd4b198e867eedb76ce8d1a504309934343532/include/pcm.h)
