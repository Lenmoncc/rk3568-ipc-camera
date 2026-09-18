# rk3568-ipc-camera

基于正点原子 RK3568 的嵌入式 Linux 音视频采集与推流项目及学习记录。
目标：720P NV12 → MPP H.264；ALSA PCM → AAC；RTMP 推流与 MP4 录像。

## 当前进度

- 已验证：配置、日志、原始帧有界队列、V4L2→队列→NV12 保存和开发板本地回放。
  板端 300 帧全部消费、无丢帧、实际 25fps；RKISP 逐帧 field 兼容已确认。
- 板端已验证：MPP H.264 1280×720/25fps，300 帧编码、10 个关键帧，保存文件并在板端正常播放。
- 本次修复：空 EOS 不带输入帧元数据时误报失败、裸 H.264 循环播放 seek 失败；主机回归通过，收尾修复待板端重测。
- 待实现：音频、编码包队列与分发、RTMP、MP4。`demo/` 为独立学习实验，不参与正式编译或调用。

## Ubuntu 编译

```bash
sh scripts/build.sh
```

复用 Buildroot SDK 的 ARM64 编译器和 sysroot。正式构建默认开启 MPP，使用 SDK 的
`rk_mpi.h`、`rk_venc_cfg.h`、`librockchip_mpp`，不链接主机库、不升级原有 FFmpeg 4.4.1。
默认 SDK：`$HOME/rk3568_linux_sdk`；其他位置用 `SDK_ROOT=/实际路径 sh scripts/build.sh`。

生成 `bin/ipc_camera`。将它上传到板端 `/root/rk3568_ipc_camera/ipc_camera`；
将 `configs/ipc.conf` 上传到该目录的 `ipc.conf`（可保留已有配置），
将 `scripts/play_h264.sh` 上传到该目录的 `play_h264.sh`。

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
```

覆盖配置、日志、队列、42 个模拟采集场景、59 个模拟编码集成场景及编码接口边界。
模拟 MPP 使用 `tests/mpp_headers/` 下固定版本官方公开头文件；**正式构建不包含该目录**。
主机模拟编码产物不可播放，也不能证明真实硬件性能。第二条关闭 LeakSanitizer，仅检查 ASan/UBSan。

只测某部分可运行 `make host-queue-test`、`make host-capture-test`、`make host-encoder-test`。
`bin/host/`、`bin/encoder-mock/`、`bin/encoder-asan/` 均不能部署到开发板。
板端队列测试仍可通过 `sh scripts/build.sh all queue-test` 构建 `bin/test_frame_queue`，预期 9 项通过。

## 配置和文档

- 视频初版固定 1280×720 NV12/H.264；音频 48000Hz 单声道仍为待板端确认的初值。
- RTMP 默认关闭，SRS 地址占位；`output.record_path` 留给后续 MP4，当前编码用 `--output`。
- [MPP 硬编码与板端回放](docs/04_MPP硬编码与板端回放说明.md)
- [V4L2 采集与队列接入](docs/03_V4L2采集与队列接入说明.md)
- [原始数据队列](docs/02_原始数据队列实现说明.md)
- [配置与日志](docs/01_配置与日志模块实现说明.md)
- [工程设计](docs/RK3568_IPC初版工程设计与实施流程.md) / [骨架历史记录](docs/模块骨架说明.md)
- `scripts/`：正式构建/启动/回放；`shell/`：已有环境检查；`demo/`：独立实验。
