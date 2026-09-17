# rk3568-ipc-camera

基于正点原子 RK3568 的嵌入式 Linux 音视频采集与推流项目及学习记录。

目标链路：720P NV12 → MPP H.264；ALSA PCM → AAC；RTMP 推流及 MP4 录像。

## 当前进度

已完成工程骨架、配置读取与校验、线程安全日志、命令行及启动脚本。采集、队列、编码、推流和录像尚未接入主工程；`demo/` 中保留独立实验。

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

直接运行可执行文件时，可用 `--config /实际路径/ipc.conf` 指定配置。默认只做配置检查并退出，不会打开硬件或推流。

## 当前配置

- 视频固定为 1280×720、NV12、H.264。
- 音频暂用 48000Hz、单声道作为开发初值，接入采集前应填写已验证值。
- SRS 地址保留 `rtmp://SRS_SERVER_IP/live/stream`，并设置 `output.rtmp_enabled=false`。
- 以后启用推流时必须填写真实 URL，空地址和占位主机不能通过启用状态的校验。

## 本机测试

```bash
make host-test
```

测试产物在 `bin/host/`，不能部署到 RK3568。

## 文档与目录

- [配置与日志模块实现说明](docs/01_配置与日志模块实现说明.md)：代码细节、配置规则、部署和验证。
- [工程设计](docs/RK3568_IPC初版工程设计与实施流程.md)：模块、线程和数据传递。
- [模块骨架阶段记录](docs/模块骨架说明.md)：上一阶段的历史说明。
- `scripts/`：工程构建与启动；`shell/`：环境检查；`demo/`：独立学习实验。
