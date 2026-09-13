# RK3568 V4L2 图像采集 Demo

用户空间 C 语言采集实践，包含两个可独立编译运行的单文件程序,以及各自的可执行文件和输出日志：

| 源文件 | 功能 |
|---|---|
| `capture_one.c` | 采集 1 帧 NV12 图像并保存 |
| `capture_sixty.c` | 连续采集 60 帧 NV12 图像并保存 |

两个程序均采用单线程、MMAP 方式，包含 V4L2 基本采集流程、debug 输出、错误处理和资源释放，不依赖 OpenCV 或 FFmpeg 开发库。

## 一、采集基础信息

### 硬件与设备节点

以下设备信息来自开发板的 `v4l2-ctl` 查询输出。

| 项目 | 信息 |
|---|---|
| 开发板 | 正点原子 ATK-DLRK3568，RK3568 平台 |
| 系统环境 | Linux，查询输出中的 Media version 为 5.10.160 |
| 采集节点 | `/dev/video0` |
| 驱动名称 | `rkisp_v5`，驱动版本 2.3.0 |
| 节点名称 | `rkisp_mainpath`，ISP 主通路输出 |
| 设备能力 | `Video Capture Multiplanar`、`Streaming` |
| 程序使用的 API 类型 | `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` |
| 内存管理方式 | `V4L2_MEMORY_MMAP` |

查询时，节点当前配置是 `3840×2160、NV12、1 个内存平面`。两个程序会通过 `VIDIOC_S_FMT` 请求修改为 `1280×720、NV12`，并检查实际返回结果；程序退出时不恢复原来的分辨率。

该节点枚举支持 NV12、NV21、NV16、NV61、UYVY 等 YUV 格式。NV12 的尺寸范围为 `32×32～3840×2160`，宽、高均按 8 像素步进，包含本次使用的 `1280×720`。

### 图像数据与缓冲区

- **NV12 格式**：8 bit YUV 4:2:0，先存 Y 亮度数据，再存 UV 交错色度数据，平均每像素占 1.5 字节。
- **一个内存平面**：Y 和 UV 两个数据区域连续存放。接口仍使用多平面 API，并传入一个 `v4l2_plane`。
- **四个采集缓冲区**：程序请求 4 个，实际分配数量以驱动返回值为准；缓冲区循环复用，与采集 1 帧或 60 帧无直接对应关系。
- **帧率**：程序未调用 `VIDIOC_S_PARM` 设置帧率，沿用摄像头链路当前配置。提供的格式列表没有枚举帧率，实际帧率需要上板确认。
- **文件内容**：只有未编码的 NV12 图像数据，没有音频、文件头、帧率或时间戳信息；不是 JPEG、H.264 或 MP4。

## 二、单帧采集：capture_one.c

启动采集后，取回第一帧有效数据，写入文件，然后停止视频流并释放资源。

| 配置项 | 配置值 |
|---|---|
| 设备节点 `DEVICE` | `/dev/video0` |
| 宽、高 `WIDTH` / `HEIGHT` | `1280` / `720` |
| 像素格式 | `V4L2_PIX_FMT_NV12` |
| 采集帧数 `FRAME_COUNT` | `1` |
| 请求缓冲区数 `BUFFER_COUNT` | `4` |
| 单次等待超时 `TIMEOUT_MS` | `3000` 毫秒 |
| 输出文件 `OUTPUT` | `frame_1280x720_nv12.yuv` |
| 正常完成后的文件大小 | `1,382,400` 字节 |

单帧大小：`1280 × 720 × 1.5 = 1,382,400` 字节。

本程序没有额外跳过启动阶段的帧，也没有等待自动曝光稳定的逻辑。

## 三、连续采集：capture_sixty.c

启动采集后，循环执行“等待、取帧、保存、归还缓冲区”，将 60 帧按取帧顺序追加到同一个文件，达到目标数量后退出。

| 配置项 | 配置值 |
|---|---|
| 设备节点 `DEVICE` | `/dev/video0` |
| 宽、高 `WIDTH` / `HEIGHT` | `1280` / `720` |
| 像素格式 | `V4L2_PIX_FMT_NV12` |
| 采集帧数 `FRAME_COUNT` | `60` |
| 请求缓冲区数 `BUFFER_COUNT` | `4` |
| 单次等待超时 `TIMEOUT_MS` | `3000` 毫秒 |
| 输出文件 `OUTPUT` | `clip_1280x720_nv12.yuv` |
| 正常完成后的文件大小 | `82,944,000` 字节 |

总大小：`1,382,400 × 60 = 82,944,000` 字节。

如果实际采集帧率为 30 fps 且没有丢帧，60 帧约对应 2 秒画面。程序按帧数结束，不按录制时长结束；启动和写文件也会消耗时间。单线程写盘或日志输出过慢可能影响采集，保存 60 帧不等于保证传感器期间没有丢帧。

## 四、共用的采集流程与调试信息

| 阶段 | 接口或操作 | 作用 |
|---|---|---|
| 打开设备 | `open` | 以读写、非阻塞方式打开采集节点 |
| 查询能力 | `VIDIOC_QUERYCAP` | 检查多平面采集和流式传输能力 |
| 设置格式 | `VIDIOC_S_FMT` | 请求 NV12、1280×720，检查返回的实际配置 |
| 申请缓冲区 | `VIDIOC_REQBUFS` | 请求 MMAP 缓冲区并获取实际数量 |
| 查询与映射 | `VIDIOC_QUERYBUF`、`mmap` | 获取内存信息并映射到用户空间 |
| 入队与启动 | `VIDIOC_QBUF`、`VIDIOC_STREAMON` | 提交缓冲区，再启动视频流 |
| 等待与取帧 | `poll`、`VIDIOC_DQBUF` | 等待帧就绪，取回完成的缓冲区 |
| 保存与归还 | `fwrite`、`VIDIOC_QBUF` | 写完当前帧后，将缓冲区归还驱动 |
| 停止与清理 | `VIDIOC_STREAMOFF`、`munmap`、`VIDIOC_REQBUFS(count=0)`、`fclose`、`close` | 停流，解除映射并释放资源 |

两份程序都使用 `[DEBUG]` 和 `[ERROR]` 前缀，日志写入标准错误输出 `stderr`。

| 日志内容 | 用途 |
|---|---|
| ioctl 名称与 OK / errno | 确认执行到哪一步，定位失败接口 |
| actual、planes、stride、sizeimage | 检查实际格式、内存平面数、行跨度及图像容量 |
| buffers、mmap index / length / addr | 查看实际缓冲区数量和映射结果 |
| frame、index、sequence | 查看采集进度、缓冲区编号和驱动帧序号；序号跳变可作为排查丢帧的线索 |
| timestamp | 驱动返回的帧时间戳；并非程序已计算的实际帧率 |
| bytesused、data_offset、flags | 检查帧数据长度、起始偏移和状态 |
| saved、total_bytes | 查看已写入帧数和累计图像字节数 |
| cleanup、DONE / FAILED/STOPPED | 检查清理流程及最终状态 |

## 五、编译、运行与验证

### 编译

在 Ubuntu 中使用与板端系统匹配的 ARM64 交叉编译工具链。

```bash
# 此处使用的SDK为正点原子的，编译的工具链也包含在SDK中
# 指定开发板对应的 Buildroot 编译器，仅在当前终端生效
export CC="$HOME/rk3568_linux_sdk/buildroot/output/rockchip_atk_dlrk3568/host/bin/aarch64-buildroot-linux-gnu-gcc"

"$CC" --version      # 检查编译器能否运行、查看版本
"$CC" -dumpmachine   # 查看编译目标，应包含 aarch64

# 编译单帧采集程序
"$CC" -std=c11 -Wall -Wextra -O0 -g capture_one.c -o capture_one

# 编译60帧采集程序
"$CC" -std=c11 -Wall -Wextra -O0 -g capture_sixty.c -o capture_sixty

# 检查生成文件的架构
file capture_one capture_60
```

### 开发板运行

将可执行文件传到开发板的实验目录，进入该目录后执行。

```bash
chmod +x capture_one capture_60  # 添加执行权限

./capture_one 2>capture_one.log  # 采集1帧，调试输出保存到日志
./capture_sixty 2>capture_sixty.log     # 采集60帧，调试输出保存到日志

cat capture_one.log             # 查看单帧采集日志
cat capture_sixty.log              # 查看连续采集日志

wc -c frame_1280x720_nv12.yuv clip_1280x720_nv12.yuv
```

输出文件位于运行命令时的当前工作目录，重复运行会覆盖同名文件。配置固定在源码中，不接受命令行参数；修改配置后需要重新编译。

### 文件与画面验证

```bash
wc -c frame_1280x720_nv12.yuv      # 正常完成：1382400字节
wc -c clip_1280x720_nv12.yuv       # 正常完成：82944000字节
```

将 YUV 文件传到带图形桌面的 Ubuntu，使用 FFplay 查看：

```bash
# 查看单帧图像，按 q 退出
SDL_RENDER_DRIVER=software ffplay -fs \
  -f rawvideo -pixel_format nv12 -video_size 1280x720 \
  -framerate 1 frame_1280x720_nv12.yuv

# 播放60帧，播放结束后自动退出
SDL_RENDER_DRIVER=software ffplay -fs -autoexit \
  -f rawvideo -pixel_format nv12 -video_size 1280x720 \
  -framerate 30 clip_1280x720_nv12.yuv
```

播放参数必须与实际保存格式一致。`-framerate 30` 只指定播放速度。

