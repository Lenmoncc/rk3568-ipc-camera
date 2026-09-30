# rk3568-ipc-camera

基于**正点原子 RK3568 开发板套件及配套 MIPI 摄像头**的嵌入式 Linux 音视频工程。
V4L2 采集 NV12、MPP 硬件编码 H.264；ALSA 采集 PCM、FFmpeg 编码 AAC；支持 RTMP 直播与本地 MP4 同时录像。

**当前交付版本：1.0.0，初版核心功能已完成。** 已验证板端采集、编码、本地回放和 SRS 推流跑通。

## 使用的开发板套件

| 项目 | 本工程实际使用情况 |
| --- | --- |
| 开发板 | 正点原子 RK3568 开发板套件；板端系统标识 `ATK-DLRK3568` |
| 主控/架构 | Rockchip RK3568，ARM64 / AArch64 |
| 摄像头 | 开发板套件配套 MIPI 摄像头，当前板端驱动识别为 **IMX415** |
| 摄像头通路 | 已有 MIPI/ISP 通路，`rkisp_v5` / `rkisp_mainpath`，设备 `/dev/video0` |
| 视频工作参数 | 1280×720、NV12，板端实测约 **25fps**；H.264、2Mbps、GOP 30 |
| 音频 | 板载 RK809 声卡 `hw:0,0`；48kHz、双声道、S16_LE；AAC-LC、128kbps |
| 系统与编译 | 配套 RK3568 Linux SDK / Buildroot 系统；Ubuntu 交叉编译 |
| 多媒体依赖 | SDK 原有 MPP、ALSA、FFmpeg 4.4.1 |
| 板端本地回放 | ffplay + Wayland，软件渲染，ALSA 音频 |
| 推流接收端 | Ubuntu 虚拟机上的 SRS；本实验地址 `192.168.137.100` |



## 不编译，直接在开发板试用

工程保留两个 **ARM64 可执行文件**：`bin/ipc_camera`、`bin/test_frame_queue`。
在原配套系统或兼容运行环境中可以直接运行；程序为动态链接，依赖说明及校验值见 [bin/README.md](bin/README.md)。
将完整工程目录上传或解压到开发板，进入工程根目录（以下示例路径为 `/root/rk3568-ipc-camera`；如实际目录不同，调整第一行）：

```bash
cd /root/rk3568-ipc-camera
chmod +x bin/ipc_camera bin/test_frame_queue
(cd bin && sha256sum -c SHA256SUMS)
./bin/ipc_camera --help
sh scripts/run.sh --check-config
mkdir -p /userdata/record
```

先验证本地音视频录像，文件名须未存在：

```bash
sh scripts/run.sh --record --seconds 30 --encode-fps 25 --mp4 /userdata/record/trial_local_01.mp4
echo $?
sh scripts/play_mp4.sh /userdata/record/trial_local_01.mp4
```

然后推流并同时保存 MP4，要求 `configs/ipc-rtmp.conf` 中的 SRS 地址从开发板可达：

```bash
sh scripts/run.sh --config configs/ipc-rtmp.conf --record --stream --seconds 30 --encode-fps 25 --mp4 /userdata/record/trial_rtmp_01.mp4
echo $?
sh scripts/play_mp4.sh /userdata/record/trial_rtmp_01.mp4
```

推流运行期间，在虚拟机另一个终端观看直播：

```bash
ffplay -i rtmp://192.168.137.100:1935/live/cam01
```

只推流、不生成本地录像：

```bash
sh scripts/run.sh --config configs/ipc-rtmp.conf --stream --seconds 30 --encode-fps 25
```

**保存的录像仍在开发板本地播放。** 网络直播观看和本地文件回放是两种用途。
重复试用请更换输出文件名，程序拒绝覆盖已有媒体文件。`--seconds 0` 持续运行，Ctrl+C 请求有序停止。
保留先前将程序、配置与播放脚本平铺到 `/root/rk3568_ipc_camera/` 的部署方式，旧命令见各阶段文档；上面的快捷流程使用工程原目录结构。

## 工作模式与配置

| 模式 | 验证内容 | 主要输出参数 |
| --- | --- | --- |
| 默认或 `--check-config` | 只加载、校验配置，不访问采集设备或网络 | 无 |
| `--capture` | NV12 视频采集及原始队列 | `--frames`、`--dump`、`--dump-frames` |
| `--encode` | MPP H.264 独立编码 | `--frames`、`--encode-fps`、`--output` |
| `--audio-capture` | PCM 独立采集 | `--seconds`、`--pcm` |
| `--audio-encode` | AAC-LC / ADTS 独立编码 | `--seconds`、`--aac` |
| `--record` | 音视频 MP4；RTMP 开关启用时同时推流 | `--seconds`、`--encode-fps`、`--mp4` |
| `--stream` | 只推流；可与 `--record` 组合 | `--seconds`、`--encode-fps` |

`configs/ipc.conf` 默认禁用 RTMP，用于本地调试；`configs/ipc-rtmp.conf` 启用 `rtmp://192.168.137.100:1935/live/cam01`。
地址为本实验局域网地址，其他使用者应修改为自己的 SRS 地址。
独立 NV12/H.264/PCM/AAC 模式始终保留，调试命令与对应播放脚本见文档索引。
配置中的视频目标帧率仍为 30；本板实测 25，所以示例显式使用 `--encode-fps 25`，该参数不强制摄像头改变采集帧率。

正常双输出结束：MP4 `trailer=1`，RTMP `completed=1 error=0`，视频与音频包数分别闭合。

| 退出码 | 含义 |
| --- | --- |
| 0 | 有限运行完成，所选输出成功 |
| 130 | 信号停止且正常收尾 |
| 3 | RTMP 失败，但本地 MP4 完整收尾 |
| 1 | 运行失败，或只推流模式下网络失败 |
| 2 | 参数或模式组合错误 |

## 从源码编译与更新可执行文件

需要修改业务源码时，在 Ubuntu 工程根目录使用原 SDK：

```bash
sh scripts/build.sh -B all queue-test
file bin/ipc_camera bin/test_frame_queue
(cd bin && sha256sum ipc_camera test_frame_queue > SHA256SUMS)
```

默认 SDK 为 `$HOME/rk3568_linux_sdk`，可通过 `SDK_ROOT=/实际路径 sh scripts/build.sh -B all queue-test` 指定。
正式编译使用 SDK 的 ARM64 编译器、头文件和动态库。修改源码后应同步更新二进制及校验值，并在板端复验。



## 工程结构

| 目录 | 职责 |
| --- | --- |
| `src/`、`include/` | 正式采集、编码、队列、时间戳、封装、网络及运行管理模块 |
| `bin/` 根目录 | 可直接部署的 ARM64 主程序与队列测试程序、校验说明 |
| `configs/` | 本地调试与 SRS 推流配置 |
| `scripts/` | SDK 编译、板端启动及本地媒体回放 |
| `tests/` | 主机模拟设备、故障注入与真实编码/封装检查 |
| `docs/` | 各阶段说明、架构、部署和交付记录 |
| `demo/` | 独立学习实验，仅供参考，不参与正式模块编译或调用 |
| `shell/` | 已有环境检查和学习脚本 |

双采集、双编码、独立 MP4/RTMP 输出通过有界队列连接；编码包按独立引用分发。
网络 AVIO 在受监督的子进程内运行，网络故障不会直接阻塞本地录像。生命周期与数据归属见架构文档。

## 已完成能力
- 正点原子 RK3568 开发板套件及配套 MIPI 摄像头的 V4L2 NV12 采集。
- MPP H.264 硬件编码；ALSA PCM 采集与 FFmpeg AAC-LC 编码。
- 独立 NV12、H.264、PCM、AAC 调试模式和开发板本地文件回放。
- 音视频共同时间轴、原始数据与编码包有界队列、MP4 双轨录像。
- FLV/RTMP 推送 SRS；编码一次、独立引用分发给网络和本地输出。
- 网络故障隔离、受监督网络子进程和有序停止。


## 验证状态与已知限制

- 已验证板端原始采集、H.264、PCM、AAC、MP4 本地播放以及 SRS 推流完成。
- 主机回归与故障注入的阶段结果见各模块文档。
- 直播延时存在；杂音存在。
- 初版未实现自动重连、录像分段、断电恢复或长期时钟漂移补偿。普通 MP4 依赖正常收尾写入索引。
- 长时间稳定性、物理音画同步误差及尚无记录的板端故障场景保留为后续验证项。

## 文档索引

- [初版交付与直接试用说明](docs/09_初版交付与直接试用说明.md)
- [版本记录](CHANGELOG.md) / [预编译程序说明](bin/README.md)
- [工程架构与实施流程](docs/RK3568_IPC初版工程设计与实施流程.md)
- [01 配置与日志](docs/01_配置与日志模块实现说明.md)
- [02 原始数据队列](docs/02_原始数据队列实现说明.md)
- [03 V4L2 采集](docs/03_V4L2采集与队列接入说明.md)
- [04 MPP H.264 与板端回放](docs/04_MPP硬编码与板端回放说明.md)
- [05 ALSA PCM 与板端回放](docs/05_ALSA音频采集与板端回放说明.md)
- [06 AAC 与板端回放](docs/06_AAC编码与板端回放说明.md)
- [07 MP4 录像与板端回放](docs/07_音视频并行录像与MP4回放说明.md)
- [08 RTMP 与双输出](docs/08_RTMP推流与双输出说明.md)
- [虚拟机 SRS 部署](docs/虚拟机SRS服务器部署完整操作流程.md)

基础主机测试为 `make host-test`；AAC、MP4 和 RTMP 测试还需要主机 FFmpeg 开发库，运行方法见对应阶段文档。
历史阶段文档保留当时的实施顺序与待办描述，当前功能范围以本 README 和第 09 号说明为准。
