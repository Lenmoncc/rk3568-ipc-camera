# 开发板 RTMP 推流 + SRS 流媒体服务器部署完整操作流程

文档用途：主要说明虚拟机端如何部署SRS服务器以及本地自测验证。其他开发板端的配置以实际的为准。
## 一、整体架构

```
开发板（音视频采集 + RTMP 推流）
        ↓ RTMP 协议
虚拟机（Linux + SRS 流媒体服务 + 拉流播放）
```

- **推流端**：RK3568 开发板，采集摄像头音视频，编码后通过 RTMP 协议推送
- **服务端**：Ubuntu 虚拟机，运行 SRS 流媒体服务器，接收推流并转协议分发
- **播放端**：虚拟机本地 VLC / ffplay，拉取 SRS 分发的直播流播放

---

## 二、网络条件（前置必备）

部署前需确保以下网络条件已满足：

1. **虚拟机双网卡架构**
   - 网卡1（NAT 模式）：负责虚拟机访问外网，用于下载软件、拉取镜像
   - 网卡2（桥接模式）：负责与开发板局域网通信，用于接收 RTMP 推流

2. **网段一致性**
   - 虚拟机桥接网卡、开发板、宿主机有线网卡必须处于**同一局域网网段**
   - 本方案使用网段：`192.168.137.x`（Windows ICS 共享默认网段）
   - 虚拟机桥接 IP 示例：`192.168.137.100`

3. **连通性验证**
   - 开发板能 ping 通虚拟机桥接 IP
   - 虚拟机能 ping 通开发板 IP
   - 虚拟机可正常访问外网（NAT 网卡）

4. **端口放行**
   - 虚拟机防火墙放行 TCP `1935`（RTMP 推流）和 TCP `8080`（HTTP 播放）
   ```bash
   sudo ufw allow 1935/tcp
   sudo ufw allow 8080/tcp
   sudo ufw reload
   ```

---

## 三、Docker 环境安装

SRS 推荐使用 Docker 部署，避免编译依赖问题。

### 3.1 安装 Docker（国内镜像源）

由于官方源在国内访问不稳定，使用阿里云镜像一键安装：

```bash
curl -fsSL https://get.docker.com | sudo bash -s -- --mirror Aliyun
```

> 备选：若阿里云镜像仍有问题，可换 Azure 中国镜像：
> ```bash
> curl -fsSL https://get.docker.com | sudo bash -s -- --mirror AzureChinaCloud
> ```

### 3.2 配置免 sudo 执行

将当前用户加入 docker 用户组，后续执行 docker 命令无需加 sudo：

```bash
sudo usermod -aG docker $USER
newgrp docker
```

### 3.3 验证安装

```bash
docker --version
```

正常输出示例：
```
Docker version 29.8.1, build 4a63305
```

---

## 四、SRS 流媒体服务器部署

### 4.1 一键启动 SRS 容器

使用阿里云同步镜像（国内可直接拉取），以 host 网络模式启动：

```bash
docker run -d \
  --name srs \
  --restart=always \
  --net=host \
  registry.cn-hangzhou.aliyuncs.com/ossrs/srs:5
```

参数说明：
- `-d`：后台运行
- `--name srs`：容器命名为 srs
- `--restart=always`：开机自启，异常自动重启
- `--net=host`：使用宿主机网络，直接复用虚拟机端口，无需额外映射
- `registry.cn-hangzhou.aliyuncs.com/ossrs/srs:5`：SRS 5.x 阿里云镜像

### 4.2 验证容器运行状态

```bash
docker ps
```

正常输出中应包含 `srs` 容器，状态为 `Up`。

---

## 五、SRS 服务状态验证

### 5.1 验证核心端口监听

```bash
netstat -tlnp | grep -E '1935|8080'
```

正常输出：
```
tcp        0      0 0.0.0.0:8080            0.0.0.0:*               LISTEN
tcp        0      0 0.0.0.0:1935            0.0.0.0:*               LISTEN
```

- `1935`：RTMP 推流入口
- `8080`：HTTP-FLV / HLS 播放入口

### 5.2 验证 HTTP 服务响应

```bash
curl -I http://127.0.0.1:8080
```

正常输出：
```
HTTP/1.1 200 OK
Connection: Close
Content-Length: 2982
Content-Type: text/html
Server: SRS/5.0.213(Bee)
```

返回 `200 OK` 且 Server 标识为 SRS，说明 HTTP 服务正常。

### 5.3 查看 SRS 运行日志

```bash
docker logs -f srs
```

按 `Ctrl+C` 退出实时日志查看。

---

## 六、本地自环测试（必做）

在对接开发板之前，先在虚拟机本地完成「推流 → SRS 分发 → 播放」全链路验证，排除服务端问题。

### 6.1 安装测试工具

```bash
sudo apt update
sudo apt install -y ffmpeg vlc
```

### 6.2 本地模拟推流

打开一个终端，执行 FFmpeg 推流命令，生成测试画面 + 正弦波音频：

```bash
ffmpeg -re \
  -f lavfi -i testsrc=size=1280x720:rate=25 \
  -f lavfi -i sine=frequency=1000 \
  -vcodec h264 -acodec aac \
  -f flv rtmp://127.0.0.1:1935/live/test
```

参数说明：
- `-re`：按原始帧率读取，模拟实时推流
- `testsrc`：生成彩色测试条纹画面
- `sine`：生成 1000Hz 正弦波音频
- `-vcodec h264`：视频编码 H.264
- `-acodec aac`：音频编码 AAC
- `-f flv`：FLV 封装格式（RTMP 标准封装）
- `rtmp://127.0.0.1:1935/live/test`：推流地址，`live` 为应用名，`test` 为流名

### 6.3 本地播放验证

**另开一个终端**，使用 ffplay 播放：

```bash
ffplay http://127.0.0.1:8080/live/test.flv
```

或使用 VLC 图形界面：
1. 打开 VLC
2. 菜单「媒体」→「打开网络串流」（快捷键 `Ctrl+N`）
3. 输入地址：`http://127.0.0.1:8080/live/test.flv`
4. 点击「播放」

能看到彩色测试条纹画面、听到持续蜂鸣声，说明 SRS 推流-分发-播放全链路正常。

---

## 七、开发板推流联调

本地验证通过后，即可使用开发板进行真实推流。

### 7.1 推流地址

开发板上的推流目标地址，填写虚拟机桥接网卡 IP：

```
rtmp://192.168.137.100:1935/live/cam01
```

- `192.168.137.100`：虚拟机桥接网卡 IP（根据实际情况修改）
- `1935`：SRS RTMP 服务端口
- `live`：SRS 默认应用名
- `cam01`：自定义流名，可自行修改

### 7.2 编码强制要求

| 项目 | 要求 | 说明 |
|------|------|------|
| 视频编码 | **H.264** | SRS 默认不支持 H.265 直接转 RTMP 分发 |
| 音频编码 | **AAC / MP3** | 优先 AAC，是 RTMP/FLV 标准音频编码 |
| 封装格式 | **FLV** | RTMP 推流标准封装 |
| 关键帧 | 带 SPS/PPS | 硬编码输出必须带序列参数，否则首帧解码失败 |

### 7.3 FFmpeg 推流参考命令（开发板端）

如果开发板使用 FFmpeg 采集摄像头 + 麦克风推流，参考命令：

```bash
ffmpeg \
  -f v4l2 -i /dev/video0 \
  -f alsa -i hw:0 \
  -vcodec h264 -vb 1500k -s 1280x720 -r 25 -g 50 \
  -acodec aac -ab 128k -ar 44100 -ac 2 \
  -f flv rtmp://192.168.137.100:1935/live/cam01
```

参数说明：
- `-f v4l2 -i /dev/video0`：从 V4L2 摄像头采集视频
- `-f alsa -i hw:0`：从 ALSA 声卡采集音频
- `-vb 1500k`：视频码率 1500kbps
- `-s 1280x720`：分辨率 720P
- `-r 25`：帧率 25fps
- `-g 50`：GOP 关键帧间隔 50（2秒一个关键帧）
- `-ab 128k`：音频码率 128kbps
- `-ar 44100`：音频采样率 44100Hz
- `-ac 2`：双声道

### 7.4 RK3568 硬件编码推流（推荐）

RK3568 支持 MPP 硬件编码，可大幅降低 CPU 占用，使用 GStreamer 推流参考：

```bash
gst-launch-1.0 \
  v4l2src device=/dev/video0 ! \
  video/x-raw,width=1280,height=720,framerate=25/1 ! \
  mpph264enc ! h264parse ! flvmux name=mux \
  alsasrc device=hw:0 ! \
  audio/x-raw,rate=44100,channels=2 ! \
  audioconvert ! faac ! mux. \
  mux. ! rtmpsink location=rtmp://192.168.137.100:1935/live/cam01
```

---

## 八、播放端配置

### 8.1 虚拟机本地播放

开发板推流成功后，在虚拟机中播放对应流：

| 协议 | 播放地址 | 延迟 | 适用场景 |
|------|----------|------|----------|
| HTTP-FLV（推荐） | `http://127.0.0.1:8080/live/cam01.flv` | 1~3秒 | 兼容性好，延迟低 |
| RTMP | `rtmp://127.0.0.1:1935/live/cam01` | 1~3秒 | 原生低延迟 |
| HLS | `http://127.0.0.1:8080/live/cam01.m3u8` | 10~30秒 | 高兼容，非实时 |

**ffplay 命令行播放：**
```bash
ffplay http://127.0.0.1:8080/live/cam01.flv
```

**VLC 图形界面播放：**
1. 打开 VLC → 媒体 → 打开网络串流
2. 输入 `http://127.0.0.1:8080/live/cam01.flv`
3. 点击播放

### 8.2 VLC 低延迟优化

VLC 默认开启网络缓存，会增加播放延迟，可手动调小：

1. 菜单「工具」→「偏好设置」
2. 选择「输入/编解码器」
3. 将「网络缓存」调整为 `100ms`（默认 1000ms）
4. 点击保存，重启 VLC 生效

### 8.3 宿主机 Windows 播放

如需在宿主机 Windows 上播放，将地址中的 `127.0.0.1` 替换为虚拟机桥接 IP：

```
http://192.168.137.100:8080/live/cam01.flv
```

使用 Windows 版 VLC / PotPlayer 打开即可。

---

## 九、SRS Web 控制台

SRS 内置 Web 管理控制台，可查看流状态、客户端连接数等信息。

访问地址：
```
http://127.0.0.1:8080/console/
```

或在虚拟机浏览器中打开，可查看：
- 当前活跃流列表
- 推流端/播放端连接数
- 音视频编码参数、码率
- 服务器 CPU/内存占用

---

## 十、常见问题排查

### 10.1 推流连接失败

**排查顺序：**
1. 验证端口连通性：开发板执行 `telnet 192.168.137.100 1935`，不通则检查网络和防火墙
2. 查看 SRS 日志：`docker logs -f srs`，确认是否有推流连接进入
3. 核对推流地址：IP、端口、应用名、流名是否正确
4. 检查编码格式：确认视频 H.264、音频 AAC

### 10.2 有声音无画面

- 确认视频编码为 H.264，不是 H.265/MJPEG
- 确认硬编码输出带 SPS/PPS 序列参数
- 降低推流分辨率和码率测试

### 10.3 有画面无声音

- 确认音频编码为 AAC，不是 PCM/G.711
- 确认音频采样率为 44100Hz
- 检查开发板麦克风设备是否正常采集

### 10.4 播放卡顿 / 延迟高

- 调小 VLC 网络缓存至 100ms
- 降低推流码率（如 1500k → 1000k）
- 检查开发板 CPU 占用，硬编码是否生效
- 检查网络带宽是否充足

### 10.5 SRS 容器异常退出

- 查看日志定位原因：`docker logs srs`
- 端口被占用：检查 1935/8080 是否被其他进程占用
- 重启容器：`docker restart srs`

---

## 十一、常用命令速查

| 操作 | 命令 |
|------|------|
| 启动 SRS | `docker start srs` |
| 停止 SRS | `docker stop srs` |
| 重启 SRS | `docker restart srs` |
| 查看运行状态 | `docker ps` |
| 查看实时日志 | `docker logs -f srs` |
| 查看端口监听 | `netstat -tlnp \| grep -E '1935\|8080'` |
| 验证 HTTP 服务 | `curl -I http://127.0.0.1:8080` |
| 进入容器 | `docker exec -it srs bash` |

---

## 十二、推流与播放地址汇总

以虚拟机桥接 IP `192.168.137.100`、流名 `cam01` 为例：

| 用途 | 地址 |
|------|------|
| 开发板推流 | `rtmp://192.168.137.100:1935/live/cam01` |
| RTMP 播放 | `rtmp://192.168.137.100:1935/live/cam01` |
| HTTP-FLV 播放 | `http://192.168.137.100:8080/live/cam01.flv` |
| HLS 播放 | `http://192.168.137.100:8080/live/cam01.m3u8` |
| Web 控制台 | `http://192.168.137.100:8080/console/` |

> 实际使用时，请将 `192.168.137.100` 替换为你的虚拟机桥接网卡实际 IP。
