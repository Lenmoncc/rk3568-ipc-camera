# V4L2 格式转换与实时预览

本实验基于正点原子 RK3568 开发板，完成以下拓展任务：

* **任务 1：** 手写 V4L2 采集 → libv4lconvert 格式转换 → 保存 BMP。
* **任务 2：** 手写 V4L2 采集 → libv4lconvert 格式转换 → OpenCV 实时显示。

`libv4lconvert` 是 libv4l 系列中负责像素格式转换的库；本程序的设备操作使用原生 V4L2 接口。

### 1. 文件说明

| 文件                   | 作用                                             |
| -------------------- | ---------------------------------------------- |
| `preview_libv4l.cpp` | 手写多平面 V4L2 采集，完成转换保存和实时预览                      |
| `preview_opencv.cpp` | 使用 OpenCV `VideoCapture(CAP_V4L2)` 直接采集，作为对照实验 |
| `build.sh`           | 在 Ubuntu 中交叉编译，生成 `bin/` 下的可执行文件               |
| `run.sh`             | 选择运行模式、配置显示环境及查看 BMP                           |

### 2. 编译与运行

在 Ubuntu 的工程目录中执行：

```bash
sh build.sh
```

使用当前板级 SDK 对应的交叉编译环境：

```text
编译器：
/home/zgt/rk3568_linux_sdk/buildroot/output/rockchip_atk_dlrk3568/host/bin/aarch64-buildroot-linux-gnu-g++

sysroot：
/home/zgt/rk3568_linux_sdk/buildroot/output/rockchip_atk_dlrk3568/host/aarch64-buildroot-linux-gnu/sysroot/
```

SDK 路径变化时，应检查并调整 `build.sh` 中对应路径。

将编译结果和启动脚本上传到开发板后执行：

```bash
cd /root/V4L2_Extension
chmod +x run.sh bin/preview_libv4l bin/preview_opencv
```

| 命令                            | 功能                                               |
| ----------------------------- | ------------------------------------------------ |
| `./run.sh convert`            | 采集并转换 60 帧，保存最后一帧为 `frame_yu12_to_bgr.bmp`，不显示窗口 |
| `./run.sh libv4l`             | 手写采集、转换并实时显示                                     |
| `./run.sh opencv`             | OpenCV 直接采集对照实验，当前环境下失败                          |
| `./run.sh bmp`                | 显示默认转换结果                                         |
| `./run.sh bmp /root/test.bmp` | 显示指定图片                                           |

### 3. 运行配置

| 配置项               | 当前设置及含义                           |
| ----------------- | --------------------------------- |
| 摄像头节点             | `/dev/video0`，对应 `rkisp_mainpath` |
| 请求分辨率             | `1280×720`，实际结果以日志为准              |
| 默认采集格式            | YU12，转换输出为 BGR24                  |
| `--nv12`          | 转换版可追加此选项，验证 NV12 输入              |
| `XDG_RUNTIME_DIR` | `/var/run`，Weston socket 所在目录     |
| `WAYLAND_DISPLAY` | `wayland-0`，显示服务连接入口              |
| `QT_QPA_PLATFORM` | `wayland`，Qt 使用 Wayland 显示        |

显示环境由脚本配置，无需每次手动执行 `export`。BMP 查看功能依赖开发板的 Python3 和 `cv2` 模块。

```bash
./run.sh libv4l --nv12     # 验证 NV12 格式的实时预览
```

### 4. OpenCV 直接采集失败的原因

当前 `/dev/video0` 提供 **V4L2 多平面采集接口**，能力标志为 `V4L2_CAP_VIDEO_CAPTURE_MPLANE`。板端 OpenCV 4.5.4 的当前 V4L2 后端在初始化时要求单平面采集能力，因此报错：

```text
missing V4L2_CAP_VIDEO_CAPTURE
```

日志显示设备文件已打开，`VIDIOC_QUERYCAP` 查询成功，但随后能力检查失败，尚未进入取帧和显示阶段。

**这是当前 OpenCV 后端与设备节点的接口兼容性问题，不是摄像头故障、显示环境问题或硬件性能瓶颈。** 更换 YU12、NV12 像素格式不能解决该接口问题。

手写程序正确使用多平面 API，可以正常采集，再交由 libv4lconvert 转换、OpenCV 显示。因此，**`preview_libv4l.cpp` 已覆盖任务 1 和任务 2，单独的 OpenCV 直接采集程序仅用于对照验证。**

