# rk3568-ipc-camera

基于正点原子 RK3568 的嵌入式 Linux 音视频采集与推流项目及学习记录。
目标：720P NV12 → MPP H.264；ALSA PCM → AAC；RTMP 推流与 MP4 录像。

## 当前进度

- 已验证：配置、日志、原始帧有界队列、V4L2→队列→NV12 保存和开发板本地回放。
  板端 300 帧全部消费、无丢帧、实际 25fps；RKISP 逐帧 field 兼容已确认。
- 板端已验证：MPP H.264 1280×720/25fps，300 帧编码、10 个关键帧，保存文件并在板端正常播放。
- 板端已验证：空 EOS 兼容修复生效，300 帧编码排空完成，退出码 0；回放不再循环 seek。
- 已完成：ALSA 双声道 PCM 采集→原始音频队列→本地文件，提供板端回放脚本；主机验证通过，板端已验证录音/听音验收。
- 已完成：FFmpeg AAC-LC 编码、ADTS 保存与板端本地回放脚本；41 项真实 AAC/API/集成检查和 ASan/UBSan 通过，AAC 板端已验证。
- 本次实现：音视频并行录制、共同时间轴、编码包队列、MP4 封装与板端回放脚本；41 项录像检查及 ASan/UBSan 通过，板端已验证。
- 待实现：RTMP 与双输出分发、长期时钟漂移补偿。`demo/` 为独立学习实验，不参与正式编译或调用。

## Ubuntu 编译

```bash
sh scripts/build.sh
```

复用 Buildroot SDK 的 ARM64 编译器和 sysroot。正式构建默认开启 MPP、ALSA 和 FFmpeg，使用 SDK 的
MPP/ALSA/FFmpeg 头文件与 `librockchip_mpp`、`libasound`、`libavformat`、`libavcodec`、`libswresample`、`libavutil`，不链接主机库、不升级原有 FFmpeg 4.4.1。
默认 SDK：`$HOME/rk3568_linux_sdk`；其他位置用 `SDK_ROOT=/实际路径 sh scripts/build.sh`。

生成 `bin/ipc_camera`。将它上传到板端 `/root/rk3568_ipc_camera/ipc_camera`；
将 `configs/ipc.conf` 上传到该目录的 `ipc.conf`（可保留已有配置），
将 `scripts/play_h264.sh`、`scripts/play_pcm.sh`、`scripts/play_aac.sh`、`scripts/play_mp4.sh` 上传到该目录。
**本次请同步 `ipc.conf` 中 `audio.channels=2`，旧的单声道配置在当前 RK809 硬件上会失败。**

## 开发板音视频 MP4 录像

Ubuntu 使用 `sh scripts/build.sh -B` 编译并更新板端程序及 `play_mp4.sh`。
在板端 `/root/rk3568_ipc_camera/` 下逐条运行，输出文件须不存在：

```bash
./ipc_camera --config ipc.conf --record --seconds 30 --encode-fps 25 --mp4 /root/rk3568_ipc_camera/record_av_01.mp4
echo $?
ffprobe -v error -show_entries stream=codec_name,width,height,sample_rate,channels,start_time,duration -show_entries format=duration -of default=noprint_wrappers=1 /root/rk3568_ipc_camera/record_av_01.mp4
sh ./play_mp4.sh /root/rk3568_ipc_camera/record_av_01.mp4
```

预期一条 H.264 720p 视频和一条 AAC 48kHz 双声道音轨，开发板画面、声音正常。
`video_enqueued=video_encoded=video_packets`，`video_eos=1 audio_drained=1 trailer=1`，退出码 0。
录制时长从共同起点计算，保留设备启动偏差，不要求 30 秒恰好 750 帧。
`--seconds 0` 持续运行，Ctrl+C 收尾成功后退出 130；MP4 文件仍可播放。
`--mp4` 省略时使用 `output.record_path`，父目录需已经存在。

**独立调试入口继续保留**：MP4 没声音时先单独录 PCM 检查采集，再单独录 AAC 检查编码；
两者正常后排查 MP4 与播放器。详细命令、线程/所有权、同步边界和 Git 提交见
[音视频并行录像与 MP4 回放说明](docs/07_音视频并行录像与MP4回放说明.md)。

## 开发板编码与本地播放

每条命令分别执行；输出文件须不存在，重测请更换文件名：

```bash
cd /root/rk3568_ipc_camera
chmod +x ipc_camera
./ipc_camera --config ipc.conf --encode --frames 300 --encode-fps 25 --output /root/rk3568_ipc_camera/video_720p_25fps.h264
echo $?
sh /root/rk3568_ipc_camera/play_h264.sh /root/rk3568_ipc_camera/video_720p_25fps.h264
```

正常无丢帧时应有 `submitted=encoded=300`、`eos=1`、退出码 0，视频约 12 秒。
**播放仍在开发板屏幕上。** 脚本自动为本次 ffplay 设置已验证的 Wayland/SDL 环境，全屏播放一次并退出；
再次观看请重新运行脚本，终端 Ctrl+C 或播放窗口 q 可提前退出。当前只有视频，无音频。
不使用裸流循环 seek，避免板端 ffplay 反复输出 `error while seeking`。

`--encode-fps 25` 配置码控和码流声明帧率，不强制改变摄像头帧率；默认值为 `video.fps`。
`--frames 0` 连续运行，Ctrl+C 正常排空后返回 130。
裸 H.264 不保留逐帧容器时间戳，原始队列丢帧时不能保持实际采集时间间隔。

完整的接口、所有权、异常处理、SDK 排查、板端解码计数和 Git 提交步骤见：
[MPP 硬编码与板端回放说明](docs/04_MPP硬编码与板端回放说明.md)。

## 开发板音频采集与本地回放

板端已确认录音设备 `hw:0,0`，支持声道范围 2..8，单声道设置失败。
本次使用 48kHz、双声道、S16_LE，程序严格设置并读回参数，上一阶段已通过板端录音与听音验收。
以下每条命令单独执行，录音期间对麦克风说话，输出文件须不存在：

```bash
cd /root/rk3568_ipc_camera
./ipc_camera --config ipc.conf --audio-capture --seconds 10 --pcm /root/rk3568_ipc_camera/audio_48k_stereo_01.pcm
echo $?
wc -c /root/rk3568_ipc_camera/audio_48k_stereo_01.pcm
sh ./play_pcm.sh /root/rk3568_ipc_camera/audio_48k_stereo_01.pcm 48000 2
```

预期四项 `*_samples` 均为 480000（每声道），文件 1920000 字节，`xruns=suspends=queue_full=0`，
退出码 0，板端耳机/扬声器听到录制的声音。脚本默认 `plughw:0,0`，无需 Wayland 环境。
`--seconds 0` 持续录音，Ctrl+C 有序排空后退出 130。上述独立调试模式互斥，同时录音录像请用 `--record`。
录音溢出/队列满会报错停止，不静默丢样；`peak_ch0/peak_ch1` 可辅助排查全零或单侧无信号。
完整说明见 [ALSA 音频采集与板端回放](docs/05_ALSA音频采集与板端回放说明.md)。

## 开发板 AAC 编码与本地回放

先在 Ubuntu 执行 `sh scripts/build.sh -B`，更新板端程序与 `play_aac.sh`。
采集仍使用 `hw:0,0`、48000Hz、双声道、S16_LE；AAC-LC 目标码率 128000bit/s。
每条命令分别执行，录音时对麦克风说话，新文件不得已存在：

```bash
./ipc_camera --config ipc.conf --audio-encode --seconds 10 --aac /root/rk3568_ipc_camera/audio_aac_01.aac
echo $?
ffprobe -v error -show_entries stream=codec_name,profile,sample_rate,channels -of default=noprint_wrappers=1 /root/rk3568_ipc_camera/audio_aac_01.aac
sh ./play_aac.sh /root/rk3568_ipc_camera/audio_aac_01.aac
```

播放脚本在开发板解码为临时 WAV 后，用 `aplay -D plughw:0,0` 本地播放，结束清理临时文件，
无需桌面/Wayland。长录音的临时 WAV 需要足够的 `TMPDIR` 空间。
预期 `input_samples=converted_samples=480000`、`drained=1`，退出码 0，AAC-LC/48000Hz/2 声道，听音正常。
`submitted_samples` 包含尾部补零；ADTS 不保存编码延迟裁剪信息，不能要求播放长度与输入严格相等。
完整架构、统计语义、异常行为、测试方法见 [AAC 编码与板端回放说明](docs/06_AAC编码与板端回放说明.md)。

## 保留的采集模式

默认不传模式时只检查配置。`--capture` 保留原始采集验证，不运行编码：

```bash
./ipc_camera --config ipc.conf --capture --frames 300 --dump /root/rk3568_ipc_camera/capture_720p_new.nv12 --dump-frames 60
```

开发板本地播放原始 NV12：

```bash
XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland SDL_RENDER_DRIVER=software ffplay -f rawvideo -pixel_format nv12 -video_size 1280x720 -framerate 25 -i /root/rk3568_ipc_camera/capture_720p_new.nv12 -loop 0 -fs
```

`--encode` 也可同时传 `--dump/--dump-frames`，但两种运行模式不能同时指定。

## 主机测试

```bash
make host-test
make host-encoder-sanitize ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
make host-audio-sanitize ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
```

覆盖配置、日志、队列、42 个模拟采集场景、59 个模拟编码集成场景、64 个模拟音频场景及编码/音频接口边界。
模拟 MPP 使用 `tests/mpp_headers/` 下固定版本官方公开头文件；**正式构建不包含该目录**。
主机模拟编码产物不可播放，也不能证明真实硬件性能。上述 sanitizer 命令关闭 LeakSanitizer，仅检查 ASan/UBSan。

AAC 测试使用真实 FFmpeg 4.4.1 编码库，需主机开发依赖；`make host-aac-test` / `make host-aac-sanitize` 的参数和验证结果见第 06 号文档。

录像的 `host-record-test` / `host-record-sanitize` 使用真实 FFmpeg 4.4.1 和模拟设备，执行参数见第 07 号文档。

只测某部分可运行 `make host-queue-test`、`make host-capture-test`、`make host-encoder-test`、`make host-audio-test`。
`bin/host/`、`bin/encoder-mock/`、`bin/encoder-asan/` 以及 `bin/audio-mock/`、`bin/audio-asan/` 均不能部署到开发板。
板端队列测试仍可通过 `sh scripts/build.sh all queue-test` 构建 `bin/test_frame_queue`，预期 9 项通过。

## 配置和文档

- 视频初版固定 1280×720 NV12/H.264；本阶段音频为 48000Hz 双声道 S16_LE，实际参数及声音由板端验收。
- `audio.codec=aac`、`audio.bitrate=128000` 在 `--audio-encode` 模式实际用于编码；PCM 模式保持不变。
- RTMP 默认关闭，SRS 地址占位；`output.record_path` 是 `--record` 的默认路径，`--mp4` 可覆盖；独立 H.264 编码仍用 `--output`。
- [ALSA 音频采集与板端回放](docs/05_ALSA音频采集与板端回放说明.md)
- [MPP 硬编码与板端回放](docs/04_MPP硬编码与板端回放说明.md)
- [V4L2 采集与队列接入](docs/03_V4L2采集与队列接入说明.md)
- [原始数据队列](docs/02_原始数据队列实现说明.md)
- [配置与日志](docs/01_配置与日志模块实现说明.md)
- [工程设计](docs/RK3568_IPC初版工程设计与实施流程.md) / [骨架历史记录](docs/模块骨架说明.md)
- `scripts/`：正式构建/启动/回放；`shell/`：已有环境检查；`demo/`：独立实验。
