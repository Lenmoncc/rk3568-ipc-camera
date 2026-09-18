# MPP H.264 硬编码与板端本地回放

## 一、背景

上一阶段已经在 ATK-DLRK3568 开发板上完成 V4L2→原始帧队列→NV12 保存及屏幕回放。
用户提供的实际输出为：1280×720 NV12、`rkisp_v5`、300 帧全部采集/入队/消费、
保存 60 帧、`invalid=0`、无队列丢帧、`capture_fps=25.000`、退出码 0。
RKISP 的 G_FMT 返回 `FIELD_NONE(1)`，DQBUF 返回 `FIELD_ANY(0)`，兼容分支已经由板端日志确认。

本阶段把原始帧送入 RK3568 的 MPP 硬编码器，保存 Annex B H.264 文件，并继续在开发板本地屏幕播放。
正式工程独立于 `demo/`；本阶段不接入音频、FFmpeg 封装、MP4、RTMP 或编码包分发队列。

## 二、任务

1. 建立正式 MPP H.264 模块，实现配置、输入布局转换、编码输出、EOS 和失败回收。
2. 复用现有原始帧队列，保留采集时间戳及丢帧统计。
3. 支持本地 `.h264` 保存，拒绝覆盖已有文件，检查写入及 fclose 错误。
4. 保留 `--capture` 验证模式，新增 `--encode`、`--output`、`--encode-fps`。
5. 补齐函数功能注释、关键所有权说明、可重现主机测试与板端验证步骤。

## 三、方案

### 3.1 模块与数据流

| 模块 | 职责 |
|---|---|
| `v4l2_capture.c` | 从驱动复制独立 NV12 帧，QBUF 后交付应用帧 |
| `frame_queue.c` | 有界 FIFO，关闭后可排空，满时采集端丢弃当前新帧 |
| `video_encoder.c` | MPP 初始化、对齐 DMA 缓冲、送帧/取包、SPS/PPS、EOS、统计 |
| `video_pipeline.c` | 采集线程、编码消费线程、文件输出、信号及生命周期管理 |
| `main.c` | 配置检查、模式和参数组合校验 |
| `scripts/play_h264.sh` | 在板端 Weston/Wayland 上调用 ffplay 回放保存文件 |

主线程先打开/配置摄像头，创建输出文件与编码器、输出 SPS/PPS，再开始采集。
采集线程将应用自有帧交给队列；消费线程逐帧取出、校验、编码并写文件，最后释放原始帧。
`--dump` 仍可同时保存少量原始帧，供对比排查；`--dump-frames` 不限制 H.264 编码帧数。

### 3.2 缓冲区与所有权

- V4L2 MMAP、应用队列帧、MPP DMA 输入缓冲是三个不同的内存对象。
- MPP 输入使用 `MPP_BUFFER_TYPE_DRM` 内部分配，不申请 CACHABLE 标志；不能把普通 malloc 指针直接当作硬件输入。
- 按原始帧的 Y/UV stride 和 UV offset 逐行复制到 16 字节对齐的 MPP stride/ver_stride，填充区清零；不进行 RGB 转换。
- 先用除法检查源布局可读范围，避免 stride/offset 的乘加溢出和越界读取。
- 同时最多一帧在途；收到完整帧末输出才改写 DMA 内存。即使输出意外分片，也收齐 EOI。
- 使用 MPP 非阻塞输入模式：提交成功后帧描述由 MPP 持有，通过输出包元数据 `KEY_INPUT_FRAME` 归还，调用者取回后销毁。尚未提交的帧由应用清理；尚在 MPP 内部的帧由 `mpp_destroy` 回收。
- 空 EOS 不挂图像缓冲，允许没有归还元数据；只在所有图像已完成编码时接受。未归还的描述由库管理，应用不能凭借旧指针重复释放；普通图像帧依然要求显式归还后才允许复用 DMA 缓冲。
- `IpcH264Sink` 只在回调内借用编码数据；文件同步写入后立即释放 MPP 包。未来异步输出必须复制或建立安全引用，不能保留本回调的裸指针。
- 发生超时或硬件错误，不再复用输入缓冲。先销毁 MPP，再释放应用持有的 DMA 引用。

这是明确所有权的初版实现，包含两次图像复制，**尚未实现 DMA-BUF 零拷贝或性能优化**。

### 3.3 编码参数与时间戳

| 参数 | 本阶段行为 |
|---|---|
| 编码/输入 | H.264 Baseline、Level 4.0；NV12 1280×720 |
| 参考结构 | 默认简单 I/P，禁用 CABAC/8×8 变换；不启用 B 帧重排 |
| 码控 | CBR，目标采用 `video.bitrate`，默认 2,000,000 bit/s |
| GOP | 采用 `video.gop`，默认每 30 个编码帧一个关键帧周期 |
| 帧率 | `--encode-fps` 优先，否则 `video.fps`；输入输出同值且不做帧率转换 |
| 编码器丢帧 | 禁止 MPP 码控丢帧；原始队列满的丢帧仍明确统计 |
| 头信息 | 初始化取得 SPS/PPS，设置每个 IDR 携带参数集 |
| 分片 | 配置不分片，接收端仍正确处理带 partition/EOI 的输出 |
| PTS | 使用采集的微秒时间戳；检查编码输出 PTS 与当前输入一致 |

`--encode-fps 25` 只配置码控和码流帧率声明，不会把摄像头从 25 改为 30 fps。
当前日志里的采集 `target_fps=30 (not forced)` 与编码 `fps=25` 可以同时出现。
若希望配置摘要也一致，可自行将 `video.fps=25`，以后省略 `--encode-fps`。
GOP=30 在 25fps 时约 1.2 秒；不等于固定一秒。

**裸 H.264 文件不保存逐帧容器 PTS。** 回调里保留了真实 PTS，文件回放通常按 SPS 声明帧率进行。
若原始队列丢帧，保存文件不能表达原始时间间隔，回放时长可能缩短；程序对此输出警告。
完整时间轴封装留待 MP4/RTMP 阶段。不能用本阶段文件证明音视频同步或断电录像恢复。

### 3.4 结束和异常处理

正常达到 `--frames` 或收到 SIGINT/SIGTERM：停止采集、关闭队列、消费剩余帧、
发送不含图像的 EOS、收到编码输出 EOS，再关闭文件并释放资源。EOS 不增加编码图像帧数，避免重复编码末帧。

提交和取包使用非阻塞调用及单调时钟，应用层各最多重试 3 秒；超时则报错。
这些期限不构成对内核 ioctl、MPP 初始化/销毁或文件系统卡死的硬实时保证。

编码/文件错误时请求采集停止，继续释放队列中剩余帧，不再向失败编码器送数据。
失败产物保留供排查，不输出成功结论；再次运行须使用新文件名。

| 退出码 | 含义 |
|---|---|
| 0 | 达到帧数上限，编码/文件收尾及资源清理成功 |
| 130 | Ctrl+C 或 SIGTERM 请求停止，已正常排空；属于受控中断 |
| 1 | 配置、设备、编码、输出或清理错误 |
| 2 | 命令行参数或组合错误 |

## 四、验证

### 4.1 Ubuntu 交叉编译

在项目根目录执行：

```bash
sh scripts/build.sh
```

默认复用 `$HOME/rk3568_linux_sdk/buildroot/output/rockchip_atk_dlrk3568` 下的 ARM64 编译器/sysroot。
正式构建默认 `WITH_MPP=1`，增加 SDK 的 `usr/include/rockchip`（或 `usr/include`）与 `-lrockchip_mpp`。
`rk_mpi.h`、`rk_venc_cfg.h` 或库缺失时必须先处理 SDK 安装，不能拿主机库代替，也不升级 FFmpeg。

需要定位开发依赖时，在 Ubuntu 执行：

```bash
find "$HOME/rk3568_linux_sdk/buildroot/output/rockchip_atk_dlrk3568/host" -name rk_mpi.h -o -name rk_venc_cfg.h -o -name 'librockchip_mpp.so*'
```

若头文件安装位置不同：

```bash
MPP_INCLUDE=/实际SDK路径/usr/include/rockchip sh scripts/build.sh
```

`WITH_MPP=0 sh scripts/build.sh` 仅显式构建基础采集版本；它会拒绝 `--encode`，不能用于本阶段验收。
主机和板端、启用和禁用 MPP 的对象目录相互隔离。不要上传 `bin/host`、`bin/encoder-mock` 或 sanitizer 产物。

### 4.2 部署与采集编码（开发板执行）

沿用最近已验证的板端目录 `/root/rk3568_ipc_camera/`：

| Ubuntu 文件 | 上传到开发板 |
|---|---|
| `bin/ipc_camera` | `/root/rk3568_ipc_camera/ipc_camera` |
| `configs/ipc.conf` | `/root/rk3568_ipc_camera/ipc.conf`（已有配置可保留） |
| `scripts/play_h264.sh` | `/root/rk3568_ipc_camera/play_h264.sh` |

先看板端运行库和设备（`ls` 个别路径不存在不代表整体失败，按实际存在项判断）：

```bash
ls -l /dev/mpp_service /dev/dri/renderD* /usr/lib/librockchip_mpp.so* /lib/librockchip_mpp.so*
```

若程序提示找不到共享库，检查所用 SDK 与已烧录镜像是否配套。不要随意用其他版本库覆盖板端系统库。
以下每条命令单独复制执行，不在同一行加入续行反斜杠：

```bash
cd /root/rk3568_ipc_camera
chmod +x ipc_camera
./ipc_camera --config ipc.conf --encode --frames 300 --encode-fps 25 --output /root/rk3568_ipc_camera/video_720p_25fps.h264
echo $?
```

输出路径必须是**尚不存在的新文件**。重测可用 `video_720p_25fps_02.h264`，程序不会覆盖旧录像。
无丢帧时 300 帧约 12 秒；不同画面和码控会改变文件大小，不能套用 NV12 的固定每帧字节数。

预期检查：

- 摄像头仍为 1280×720 NV12，`capture_fps` 约 25。
- `[encoder] MPP H.264 Baseline ... fps=25 bitrate=2000000 ...`。
- `captured=300`，`dropped_full=0`；`enqueued=consumed=submitted=encoded=300`。
- `eos=1`，`bytes>0`，`first_pts_us >= 0`，`last_pts_us > first_pts_us`。
- 最后完成清理且退出码 0。`packets` 是输出分片数量，不应强制等于图像帧数。

### 4.3 开发板本地屏幕播放

仍在开发板终端执行，不需要把视频下载到电脑：

```bash
sh /root/rk3568_ipc_camera/play_h264.sh /root/rk3568_ipc_camera/video_720p_25fps.h264
```

脚本为此次播放设置 `/run/wayland-0`、Wayland 显示后端与 SDL 软件渲染，全屏播放一次并自动退出，无需每次手动 export。
图像显示在开发板屏幕，终端只显示日志。按播放窗口 `q` 或启动终端 `Ctrl+C` 可提前退出；再次观看请重新运行脚本。

不使用脚本时，等效单行命令为：

```bash
XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland SDL_RENDER_DRIVER=software ffplay -f h264 -i /root/rk3568_ipc_camera/video_720p_25fps.h264 -autoexit -fs -an
```

H.264 自带图像参数，不再指定 `-pixel_format nv12` 或 `-video_size`。
确认画面无花屏、错位、明显偏色或开头解码失败；当前没有音频。
本脚本是保存后回放，不是采集中的实时预览。回放时 CPU 占用不能用来衡量单独硬编码的 CPU 开销。

### 4.4 解码检查、帧数和中断

停止 ffplay 后，若板端有 ffprobe，可读取实际码流属性并计数：

```bash
ffprobe -v error -f h264 -select_streams v:0 -count_frames -show_entries stream=codec_name,profile,width,height,r_frame_rate,nb_read_frames -of default=noprint_wrappers=1 /root/rk3568_ipc_camera/video_720p_25fps.h264
```

期望 H.264、1280×720、25/1、300 帧（前提是没有队列丢帧）。裸流 duration 不一定可用。
若板端没有 ffprobe，用已有 ffmpeg 做完整解码检查：

```bash
ffmpeg -v error -f h264 -i /root/rk3568_ipc_camera/video_720p_25fps.h264 -f null -
```

连续运行后 Ctrl+C：

```bash
./ipc_camera --config ipc.conf --encode --frames 0 --encode-fps 25 --output /root/rk3568_ipc_camera/video_interrupt.h264
```

按 Ctrl+C 后立即 `echo $?`，预期 130、EOS 正常及队列排空，已完成部分可回放。
换一个新文件名再次编码，验证设备和资源已释放。

### 4.5 主机回归与边界测试

```bash
make host-test
make host-encoder-sanitize ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
```

- 配置、日志、队列和原有 42 个采集模拟场景回归。
- 59 个编码集成场景：正常/暂忙/无包重试、分片、RKISP field 兼容、原始帧同时保存、
  初始化各阶段失败、提交失败与超时、输出失败与超时、EOS 超时、PTS 错误、空输出、
  输入帧归还检查、空 EOS 无元数据/无归还帧、异常 EOS 载荷/提前 EOS 拒绝、
  写入/关闭错误、线程启动失败、路径保护、慢消费丢帧、命令行校验、SIGINT/SIGTERM。
- 编码 API 边界：独立 Y/UV 步长、末行无 padding、尺寸截断、stride/offset 溢出、格式/类型错误、
  无效/重复 PTS、EOS 幂等、零图像结束、EOS 后拒绝送帧，以及重复释放安全。
- 模拟器在取包时才读取 DMA 数据，并逐字节比较像素，检查提交后数据未被改写。
- 每个模拟子进程退出时检查设备和 MPP 资源归零；ASan/UBSan 检查地址访问及未定义行为。
  上述命令关闭 LeakSanitizer，不据此宣称已完成真实库或硬件泄漏检测。

模拟器输出是假编码包，不可播放；主机验证无法替代 SDK 交叉编译和板端真实编码/回放。

## 五、结果与提交

实现已接入正式工程，`demo/`、`shell/` 不变，新增/修改函数具备功能注释。
本次主机验证已通过：54 个配置/CLI 场景、400 条并发日志及过滤、失败配置保留、
9 个队列场景、42 个采集集成场景、59 个编码集成场景及编码 API 边界检查。
编码集成和 API 边界检查在 ASan/UBSan 下也已通过（LeakSanitizer 关闭）。
回放脚本通过 shell 语法、参数/环境传递、缺失文件/显示会话的检查；本机不具备实际显示会话。
当前环境没有用户 SDK 工具链和 RK3568 硬件，因此未在此处执行目标交叉编译或真实硬编码。
上一阶段原始采集及 NV12 回放已由用户确认通过。本阶段用户日志已确认 300 帧硬编码及 H.264 板端正常播放；
**原日志仍有 EOS 收尾错误，本次修复尚待板端重测，不能将可播放等同于整条流水线正常退出。** CPU 占用和长期稳定性尚未测量。

在 Ubuntu 工程目录暂存本阶段文件：

```bash
git add Makefile README.md configs/ipc.conf include/video_encoder.h include/video_pipeline.h src/main.c src/video_encoder.c src/video_pipeline.c scripts/build.sh scripts/play_h264.sh tests/mock_v4l2.c tests/mock_mpp.c tests/test_encoder.py tests/test_encoder_api.c tests/mpp_headers docs/03_V4L2采集与队列接入说明.md docs/04_MPP硬编码与板端回放说明.md
git diff --cached --stat
git commit -m "feat(video): 接入MPP H.264硬编码与板端本地回放，补充注释和测试"
```

仅给出提交操作，不代替用户在本地仓库执行。不要将生成的 `.h264`、`.nv12`、`bin/` 或 `build/` 提交。

## 参考资料

- [Rockchip MPP 官方仓库与公开 API](https://github.com/rockchip-linux/mpp)
- [官方多线程编码示例：非阻塞输入及 KEY_INPUT_FRAME 回收](https://github.com/rockchip-linux/mpp/blob/ee0773be065ba43f5f8fee218f8c0ae7343dfa57/test/mpi_enc_mt_test.c)
- [官方编码实现：异步输入队列、EOS 与帧归还](https://github.com/rockchip-linux/mpp/blob/ee0773be065ba43f5f8fee218f8c0ae7343dfa57/mpp/codec/mpp_enc_impl.c)
- [FFplay 参数说明](https://ffmpeg.org/ffplay.html)
- [SDL 渲染驱动选择](https://wiki.libsdl.org/SDL2/SDL_HINT_RENDER_DRIVER)


## 六、板端 MPP 帧率配置兼容修复

### 现象与原因

板端日志在初始化阶段报 `rc:fps_in_denom failed: MPP ret=-1`，随后显示
`dequeued=0 captured=0`。程序已识别编码命令，但配置未完成，摄像头尚未开始出帧，
输出路径已创建，因此会留下空 `.h264` 文件，回放脚本拒绝播放是正确行为。

MPP 的历史公开配置键是 `rc:fps_in_denorm`、`rc:fps_out_denorm`；参考版本
`ee0773be065ba43f5f8fee218f8c0ae7343dfa57` 的 `mpp/base/mpp_enc_cfg.c` 同时保留
历史键及 `denom` 别名。原实现依赖别名，板端库在设置该别名时失败。
本次将输入和输出分母均改为历史键，值仍为 1，不修改帧率值或跳过配置错误。

### 修复与验证

- 正式编码模块改为 `rc:fps_in_denorm=1`、`rc:fps_out_denorm=1`，补充历史拼写注释。
- 模拟 MPP 改为校验实际支持键集合，新增只接受历史分母键的旧库场景，并检查两个分母都为 1。
- 修复前的新场景复现同样的 `rc:fps_in_denom failed: MPP ret=-1`；修复后通过。
- 修复后 53 个编码集成场景和编码 API 边界检查通过；相同测试在 ASan/UBSan 下也通过（LeakSanitizer 关闭）。主机模拟不能替代本次板端重测。

### 重新验证

Ubuntu 项目根目录重新编译，上传新 `bin/ipc_camera` 覆盖实际执行路径：

```bash
sh scripts/build.sh -B
```

开发板使用新文件名，避开前一次留下的空文件；`echo $?` 必须紧跟程序，中间不要执行 `ls`：

```bash
cd /root/rk3568_ipc_camera
chmod +x ipc_camera
./ipc_camera --config ipc.conf --encode --frames 300 --encode-fps 25 --output /root/rk3568_ipc_camera/video_720p_25fps_fixed.h264
echo $?
```

确认 `submitted=encoded=300`、`eos=1`、退出码 0 后，再在开发板本地播放：

```bash
sh ./play_h264.sh /root/rk3568_ipc_camera/video_720p_25fps_fixed.h264
```

若上一版已经提交，本次修复可单独提交：

```bash
git add src/video_encoder.c tests/mock_mpp.c tests/test_encoder.py README.md docs/04_MPP硬编码与板端回放说明.md
git commit -m "fix(video): 兼容旧版MPP帧率分母配置键并补充回归测试"
```

## 七、300 帧板端回放结果与 EOS 收尾修复

### 已确认结果

用户再次编译运行的日志显示 `captured=enqueued=consumed=submitted=encoded=300`，
`invalid=0`、无队列丢帧、`capture_fps=25.000`，输出 2,777,479 字节、10 个关键帧。
ffplay 识别为 H.264 Constrained Baseline、1280×720、25fps，用户确认开发板屏幕正常播放。
`saved=0` 只表示未额外保存 NV12 原始帧，与 `.h264` 已写入的字节数不冲突。

但同一日志有 `EOS drain failed: Bad message` 和 `capture pipeline failed: Bad message`。
这证明图像编码已完成，结束处理仍未通过；原日志的 `eos=1` 在所有检查完成前赋值，不能单独作为成功依据。

### 本次修复

1. 根据代码路径与日志定位到空 EOS 缺少 `KEY_INPUT_FRAME` 的归还检查；主机模拟已复现同样报错。
   现在仅当收到零载荷 EOS、全部图像已输出时接受该结束包。图像帧缺少归还元数据、提前 EOS、
   EOS 携带意外数据及 EOS 超时仍然失败，`stats.eos` 在排空检查通过后才设为真。
2. 输入帧只在元数据明确归还时释放；空 EOS 没有归还指针时由库管理，应用不额外释放别名指针。
   模拟覆盖库内立即回收和上下文销毁时回收；真实 BSP 内部资源行为仍需板端重复运行观察。
3. 播放日志中大量 `error while seeking` 与旧脚本的裸流循环定位相符。
   脚本删除 `-loop 0`，使用 `-autoexit` 单次全屏播放，播完退出，继续沿用板端 Wayland/SDL 环境。
   `Duration: N/A` 是裸流常见表现；播放器的软件色彩转换提示不能据此判定 MPP 编码失败。
4. 修改的函数保留功能注释，补充 EOS 所有权解释；`demo/`、`shell/` 内容不变。

### 验证与板端重测

主机全套回归通过，其中编码集成增加至 59 项；编码集成及 API 边界也通过 ASan/UBSan
（关闭 LeakSanitizer）。包含零图像结束、重复 finish、缺少 EOS 元数据时的 SIGINT/SIGTERM 排空。
播放脚本已检查语法、带空格文件名、参数/环境传递及输入错误，未在本机执行实际屏幕播放。
本次收尾修复尚待板端复测，不能把上述模拟结果写成硬件验收通过。

Ubuntu 工程目录执行 `sh scripts/build.sh -B`，重新上传 `bin/ipc_camera` 和 `scripts/play_h264.sh`。
开发板每条命令单独执行，`echo $?` 紧跟采集命令；新路径须不存在：

```bash
cd /root/rk3568_ipc_camera
chmod +x ipc_camera
./ipc_camera --config ipc.conf --encode --frames 300 --encode-fps 25 --output /root/rk3568_ipc_camera/video_720p_eos_fixed.h264
echo $?
sh ./play_h264.sh /root/rk3568_ipc_camera/video_720p_eos_fixed.h264
```

预期 `submitted=encoded=300`、`eos=1`、退出码 0，打印
`capture complete; queue drained and resources released`，无 `EOS drain failed`。
若出现一次 `empty EOS without KEY_INPUT_FRAME; all image frames drained`，表示兼容分支生效。
播放应正常结束、不再因自动循环刷 seek 错误。另按 4.4 节验证 Ctrl+C 返回 130 和再次录制。

板端确认后，如果前面编码阶段已提交，本次修复可单独提交：

```bash
git add src/video_encoder.c scripts/play_h264.sh tests/mock_mpp.c tests/test_encoder.py tests/test_encoder_api.c README.md docs/04_MPP硬编码与板端回放说明.md
git commit -m "fix(video): 兼容MPP空EOS回包并修复板端裸流回放收尾"
```
