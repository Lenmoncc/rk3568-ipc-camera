# AAC 编码、ADTS 保存与开发板本地回放

## 1. 本阶段任务与边界

前置阶段已在 RK3568 验证 H.264 视频与双声道 PCM 录音/回放。
本阶段在正式工程接入 FFmpeg AAC-LC，复用已有 ALSA 采集和原始帧队列。

参数：`hw:0,0` → 48000Hz、双声道、S16_LE → AAC-LC、128000bit/s → ADTS `.aac`。
保留 `--audio-capture --pcm` 方便对照原始声音；新增 `--audio-encode --aac`。
视频与音频仍为独立运行模式，当前不实现音画同步、编码包队列、MP4、RTMP。
`output.record_path` 仍是预留项，不能据此认为已经生成 MP4。

## 2. 模块、线程与所有权

| 模块 | 职责与接口 |
|---|---|
| `alsa_capture` | 保持既有采集实现，生成独占堆内存 PCM 块和每声道样本下标 |
| `audio_pipeline` | 采集线程入有界队列；消费者选择 PCM 保存或 AAC 编码；主线程处理信号和资源汇总 |
| `audio_encoder` | `init/push/finish/get_stats/deinit`；S16_LE→FLTP、采样 FIFO、AAC-LC、导出编码参数 |
| `audio_adts` | 根据 AudioSpecificConfig 校验格式，为每个编码包添加 7 字节 ADTS 头并写文件 |
| `play_aac.sh` | 开发板本地解码成临时 WAV，用指定 ALSA 播放设备回放，退出清理 |

采集线程 `try_push` 成功后转移 PCM 所有权，不再访问该块。消费者 `push` 编码时仅借用 PCM，
返回后即可释放或改写输入。编码器持有自己分配的 FLTP 帧、FIFO、编码上下文和 AVPacket。
FFmpeg 可能持有 AVFrame 的引用；应用释放自己引用不会破坏编码器的数据。

`IpcAudioPacketSink` 是同步回调，`IpcEncodedPacket/AVPacket` 仅在回调内有效；
后续接编码包队列时必须 `av_packet_ref` 或复制，禁止存储借用指针。
`get_params` 返回借用的 `IpcStreamParams`，生命周期到编码器释放；后续封装器应复制 codecpar。
ADTS 接收端借用 `FILE*`；主线程在消费者结束后检查 `fclose`，不会与消费者同时关闭文件。

## 3. 格式转换、时间戳与排空

- ALSA 一次读取的 PCM 块大小与 AAC 帧大小不同。通过 `AVAudioFifo` 累计采样，按编码器实际 `frame_size` 取帧，不能按一次 read 编一次帧。
- 使用 `libswresample` 转换交错 S16_LE 至 FLTP，保持采样率和声道数，不做隐藏的降采样或混音。
- PCM 时间戳连续性按 `sample_index` 校验。首块微秒 PTS 只换算一次到 `1/sample_rate` 时间基，此后累加已提交样本数，避免逐块舍入误差。
- AAC 编码延迟可能使首包 PTS 早于首输入帧，甚至为负；保留真实包时间戳，不强行改成零。
- `send_frame` 返回 EAGAIN 时先接收输出，再重送同一帧；每次发送后取完所有可用包。
- 正常结束或 Ctrl+C：采集停止→关闭并排空应用队列→排空转换器→尾部补零至完整编码帧→发送 NULL→接收至 EOF。
- 重复成功的 `finish` 不再重复输出；编码中途失败进入失败状态，调用方释放实例，不重新提交可能已部分消费的块。

ADTS 每帧有 AAC 参数与长度，无应用逐包 PTS，也不携带本项目用于精确裁剪编码延迟的容器信息。
因此裸 AAC 播放时长可能略长于输入，不能用它验收最终音画同步。后续 MP4 使用编码包时间戳和编码参数。

## 4. Ubuntu 构建与部署

在工程根目录：

```bash
sh scripts/build.sh -B
```

默认 `WITH_MPP=1 WITH_ALSA=1 WITH_FFMPEG=1`。沿用原 SDK 编译器/sysroot，链接
`librockchip_mpp`、`libasound`、`libavcodec`、`libswresample`、`libavutil`。
正式应用直接调用库，不通过 ffmpeg 命令执行编码。命令行 ffmpeg 仅用于回放解码和验收。
缺少 SDK 头文件时脚本明确报错；链接失败需检查同一 SDK 的库安装，不把主机 x86 库复制到板端。
`WITH_FFMPEG=0` 仅用于显式禁用 AAC，此时 `--audio-encode` 明确失败，不能用于 AAC 验收。

上传以下文件至开发板 `/root/rk3568_ipc_camera/`：

| Ubuntu 文件 | 板端文件 |
|---|---|
| `bin/ipc_camera` | `ipc_camera` |
| `configs/ipc.conf` | `ipc.conf`，保留已确认的其他参数 |
| `scripts/play_aac.sh` | `play_aac.sh` |

配置中核对 `audio.sample_rate=48000`、`audio.channels=2`、`audio.sample_format=S16_LE`、
`audio.codec=aac`、`audio.bitrate=128000`。硬件不支持单声道采集，保持双声道。

## 5. 开发板验收

### 5.1 正常录音与参数检查

每条命令单独执行；输出文件必须不存在，重测使用新文件名。录制时对麦克风说话。

```bash
cd /root/rk3568_ipc_camera
chmod +x ipc_camera
./ipc_camera --help
./ipc_camera --config ipc.conf --audio-encode --seconds 10 --aac /root/rk3568_ipc_camera/audio_aac_01.aac
echo $?
ls -l /root/rk3568_ipc_camera/audio_aac_01.aac
ffprobe -v error -show_entries stream=codec_name,profile,sample_rate,channels -of default=noprint_wrappers=1 /root/rk3568_ipc_camera/audio_aac_01.aac
```

`echo $?` 紧跟录音命令，避免读到 `ls` 等其他命令的状态。帮助输出应包含 `--audio-encode/--aac`，
否则先检查是否上传了新二进制。

正常 10 秒录音，预期如下：

| 统计 | 预期 |
|---|---|
| `captured/enqueued/consumed/saved_samples` | 都是 480000，每声道样本数 |
| AAC `input_samples/converted_samples` | 都是 480000 |
| AAC `frame_samples` | 使用编码器实际值；原生 AAC-LC 通常为 1024 |
| AAC `submitted_samples/padding_samples` | frame_size=1024 时为 480256/256；补零不属于实际采集 |
| AAC `initial_padding` | 编码器启动延迟，单独打印，与尾部补零不同 |
| AAC `packets/bytes` | 均大于 0；bytes 含 ADTS 头，码率为目标值，文件大小不要求固定 |
| AAC `drained` | 1，收到编码器 EOF |
| `queue_full/xruns/suspends` | 都为 0 |
| 退出码 | 0 |
| ffprobe | `codec_name=aac`、`profile=LC`、`sample_rate=48000`、`channels=2` |

AAC 模式下 `saved_samples` 表示成功交给编码链路的真实输入样本数；只有 `drained=1`、最终关闭文件成功且退出码 0，
才能确认整段输出已完成。出现错误时文件可能只含前缀，不能仅凭 `saved_samples` 宣告成功。

### 5.2 开发板本地回放

```bash
sh ./play_aac.sh /root/rk3568_ipc_camera/audio_aac_01.aac
```

脚本保持解码结果的采样率/声道数，默认通过 `aplay -D plughw:0,0` 回放；
不需要 Wayland、SDL 或 PC，不自动调整系统 mixer。应听到正常语速的录音，声音无明显截断、变速或异常噪声。
可显式指定已经验证可用的播放设备作为第二参数。

脚本先检查 ffmpeg 解码成功再播放，避免 POSIX shell 管道掩盖解码错误。默认临时 WAV 位于 `/tmp`，
10 秒双声道音频约 1.92MB；长录音需足够空间，可使用已有的 `/userdata` 作为临时目录：

```bash
TMPDIR=/userdata sh ./play_aac.sh /root/rk3568_ipc_camera/audio_aac_01.aac
```

无显示界面时也可播放，播放完成/正常信号退出后清理临时 WAV。Ctrl+C 提前停止播放，不影响已保存 AAC。

### 5.3 连续录音与 Ctrl+C

```bash
./ipc_camera --config ipc.conf --audio-encode --seconds 0 --aac /root/rk3568_ipc_camera/audio_aac_stop_01.aac
```

录制数秒后 Ctrl+C，再单独运行：

```bash
echo $?
sh ./play_aac.sh /root/rk3568_ipc_camera/audio_aac_stop_01.aac
```

预期退出码 130、`drained=1`，文件正常播放。若停止前发生设备/写盘等错误则退出 1，而非 130。

### 5.4 异常与原功能回归

重复使用已存在输出路径应失败且原文件不变。设备 XRUN/挂起、队列满、写盘失败都会明确报错停止；
不静默丢样，不假装时间连续。已入队且可处理的数据会排空；写错后的剩余块只释放，不继续写损坏文件。
磁盘系统调用本身可能阻塞，此阶段不宣称硬实时停止上限。

保留上一阶段 `--audio-capture --pcm`、`--capture --dump`、`--encode --output` 的回归命令。
各运行模式互斥；AAC 不能混用 `--pcm/--output/--frames/--encode-fps/--dump`。

## 6. 主机验证与局限

基础回归：

```bash
make host-test
```

真实 AAC 测试需要主机 FFmpeg 4.4.1 开发头文件/库及 ffmpeg/ffprobe 工具；不得将 ARM64 SDK 库用于主机运行。
假设主机 FFmpeg 安装在 `/tmp/ipc-ffmpeg44`：

```bash
make host-aac-test FFMPEG_INCLUDE=/tmp/ipc-ffmpeg44/include FFMPEG_LIBS='-L/tmp/ipc-ffmpeg44/lib -lavcodec -lswresample -lavutil -lm'
make host-aac-sanitize FFMPEG_INCLUDE=/tmp/ipc-ffmpeg44/include FFMPEG_LIBS='-L/tmp/ipc-ffmpeg44/lib -lavcodec -lswresample -lavutil -lm' ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
```

测试用官方 FFmpeg 4.4.1 源码编译真实 AAC 编码库，仅 ALSA 输入模拟。覆盖零输入、1/1023/1024/1025 样本尾部、
单/双声道、跨块重分帧、时间戳顺序、PCM 借用后改写、输出回调失败、ADTS 逐帧边界、独立解码、
左右声道 440/880Hz 音调与能量检查，以及 CLI、设备/队列/文件错误和 SIGINT/SIGTERM 排空。

ASan/UBSan 检查应用代码；本次主机第三方 FFmpeg 静态库未加 sanitizer，且禁用 LeakSanitizer，不能宣称已完整验证第三方库或无内存泄漏。
主机验证不能代替板端 SDK 交叉编译、实际录音、声音质量与实时性能验收。

参考：FFmpeg 4.4 的 [AAC 转码示例](https://ffmpeg.org/doxygen/4.4/transcode__aac_8c-example.html)、
[send/receive API](https://ffmpeg.org/doxygen/4.4/group__lavc__encdec.html)。本工程实现独立编写，不将第三方源码加入工程包。

本次已完成的主机验证：

- 基础回归通过：配置/日志/队列、42 项 V4L2、59 项 MPP、64 项 ALSA 场景及对应 API 检查。
- 41 项真实 AAC/API/集成检查通过，生成的 ADTS 文件经独立 FFmpeg 解码与 ffprobe 参数核对。
- 同组 AAC 检查通过 ASan/UBSan；编译开启 `-Wall -Wextra -Wpedantic -Werror`。
- 回放脚本验证了带空格路径、解码失败、播放失败和临时目录清理。
- `demo/`、`shell/` 与上传工程逐文件内容完全一致，正式构建不依赖二者。
- 尚未在本环境执行 RK3568 SDK 交叉编译或板端实测，板端验收按第五节执行。

