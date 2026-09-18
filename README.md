# rk3568-ipc-camera

基于正点原子 RK3568 的嵌入式 Linux 音视频采集与推流项目及学习记录。

目标链路：720P NV12 → MPP H.264；ALSA PCM → AAC；RTMP 推流及 MP4 录像。

## 当前进度

已完成工程骨架、配置读取与校验、线程安全日志、命令行及启动脚本，以及原始数据有界队列。现已实现 V4L2 多平面采集 → 队列 → 消费检查/原始画面保存，可通过 `--capture` 运行，尚待本阶段上板验证。声卡、编码、编码包队列、推流和 MP4 录像待实现；`demo/` 保持独立，不参与正式程序的编译或调用。

沿用 Buildroot SDK 工具链及原有 FFmpeg 4.4.1。本阶段只依赖 libc 和 pthread。

## 编译与运行

Ubuntu 中：

```bash
sh scripts/build.sh
```

部署 `bin/ipc_camera`、`scripts/run.sh` 和 `configs/ipc.conf`，保持相对目录结构。开发板中：

```bash
sh /实际项目路径/scripts/run.sh --check-config
```

直接运行可执行文件时，可用 `--config /实际路径/ipc.conf` 指定配置。默认只做配置检查并退出；显式传入 `--capture` 才打开摄像头，仍不推流。

最新修复：为已协商逐行格式的 RKISP 节点兼容逐帧 `field=ANY`，并补充首帧诊断。
修复依据、模拟验证及板端待确认项见采集说明第六节。

## 视频采集与队列接入

Ubuntu 根目录执行 `sh scripts/build.sh`，将新 `bin/ipc_camera` 与 `configs/ipc.conf`
分别上传到板端 `/root/ipc_camera`、`/root/ipc.conf`，执行：

```bash
chmod +x /root/ipc_camera
/root/ipc_camera -c /root/ipc.conf --capture --frames 300 \
  --dump /userdata/capture_720p.nv12 --dump-frames 60
echo $?
```

输出文件须不存在；已有文件时更换文件名。预期读回 `1280x720 NV12`，正常排空后
`enqueued == consumed`，打印 `capture complete` 且返回 0。目标 30fps 不代表实测结果。
`--frames 0` 持续采集，Ctrl+C 请求停止、排空并返回 130。

将 NV12 下载到 Ubuntu，可用以下命令检查画面（30 是指定播放速度）：

```bash
ffplay -f rawvideo -pixel_format nv12 -video_size 1280x720 -framerate 30 capture_720p.nv12
```

更多异常处理、统计解释、重启验证见 [V4L2 采集与队列接入说明](docs/03_V4L2采集与队列接入说明.md)。

## 当前配置

- 视频固定为 1280×720、NV12、H.264。
- 音频暂用 48000Hz、单声道作为开发初值，接入采集前应填写已验证值。
- SRS 地址保留 `rtmp://SRS_SERVER_IP/live/stream`，并设置 `output.rtmp_enabled=false`。
- 以后启用推流时必须填写真实 URL，空地址和占位主机不能通过启用状态的校验。

## 本机测试

```bash
make host-test
```

测试产物在 `bin/host/`，不能部署到 RK3568。此命令包含配置、日志、队列及模拟驱动采集测试。

只运行队列测试：

```bash
make host-queue-test
```

## 队列上板验证

Ubuntu 项目根目录执行，沿用 SDK 工具链：

```bash
sh scripts/build.sh all queue-test
```

将 `bin/test_frame_queue` 上传到板端 `/root/`，在开发板执行：

```bash
chmod +x /root/test_frame_queue
/root/test_frame_queue
```

预期最后输出 `PASS: 9 frame queue scenarios (no hardware required).`，退出码为 0。
测试使用模拟数据，不需要配置文件、摄像头、声卡或 SRS。主程序仍只检查配置；
看到配置摘要不能代替运行队列测试。

## 模拟采集测试

```bash
make host-capture-test
```

本机独立 `ipc_camera_mock` 链接模拟系统调用，验证真实正式模块的控制流；
正式 `ipc_camera` 不链接模拟文件。模拟测试通过不能替代摄像头画面和帧率验证。

## 文档与目录

- [V4L2 采集与队列接入说明](docs/03_V4L2采集与队列接入说明.md)：每个模块的职责、内存与时间戳、线程退出、上板验证。
- [原始数据队列实现说明](docs/02_原始数据队列实现说明.md)：接口、所有权、同步原理、关闭流程和测试步骤。
- [配置与日志模块实现说明](docs/01_配置与日志模块实现说明.md)：代码细节、配置规则、部署和验证。
- [工程设计](docs/RK3568_IPC初版工程设计与实施流程.md)：模块、线程和数据传递。
- [模块骨架阶段记录](docs/模块骨架说明.md)：上一阶段的历史说明。
- `scripts/`：工程构建与启动；`shell/`：环境检查；`demo/`：独立学习实验。
