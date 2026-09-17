#!/bin/sh
# RK3568 IPC 初版板端环境检查。POSIX sh；只使用板端已有程序和库。
# 默认查询环境；采集、录音、目录写入及 H.265 封装测试需显式开启。
# 输出：summary.txt（摘要）、env_check.log（完整日志），可选 audio_check.wav。
# OK 只表示检查命令执行成功，不代表整个硬件或音视频链路通过验收。

usage() {
    cat <<'EOF'
用法：sh check_rk3568_env.sh [选项]

默认：查询系统、视频设备、媒体拓扑、ALSA、原有 FFmpeg、MPP、网络和存储。
      不安装软件，不升级 FFmpeg，不修改网络，不停止其他进程。

选项：
  --video DEVICE         视频节点，默认 /dev/video0
  --record-dir DIR       正式录像目录，默认 /userdata（默认只检查）
  --output-dir DIR       报告父目录，默认 /tmp；每次创建独立子目录
  --ffmpeg PATH          指定 SDK 原有 ffmpeg 的绝对路径，默认 PATH 中的 ffmpeg
  --server HOST          额外检查 SRS 主机 Ping / TCP 端口
  --port N               RTMP 端口，默认 1935
  --test-video           使用当前格式采集 120 帧并丢弃，最长 20 秒
                        不设置分辨率、格式或帧率；先停止其他采集程序
  --test-audio DEVICE    打开指定录音设备，查询参数并录制 5 秒 WAV
  --rate N              录音采样率，默认 48000
  --channels N          录音声道数，默认 1；PCM 固定为 S16_LE
  --test-write           在录像目录创建、写入、删除一个临时文件
  --hevc-sample FILE     使用已验证的 H.265 素材测试 FLV 封装（不重新编码）
  -h, --help             显示帮助

示例：
  sh check_rk3568_env.sh
  sh check_rk3568_env.sh --server 192.168.1.100
  sh check_rk3568_env.sh --test-video --test-audio hw:0,0 --rate 48000 --channels 1

采集/录音/封装测试要求已有 timeout 工具，否则跳过，不自动安装。
录音参数需按设备支持情况调整。默认运行即可收集首轮环境排查资料。
脚本退出 0 表示报告生成完成；WARN/SKIP 要查看摘要，不能视为整体验收通过。
EOF
}

die() { printf '错误：%s\n' "$*" >&2; exit 2; }
need_value() { [ "$#" -ge 2 ] && [ -n "$2" ] || die "$1 缺少参数"; }
positive_int() {
    case "$1" in ''|*[!0-9]*) return 1 ;; esac
    [ "$1" -gt 0 ] 2>/dev/null
}

VIDEO_DEV=/dev/video0
RECORD_DIR=/userdata
OUT_BASE=/tmp
FFMPEG_CMD=ffmpeg
SERVER=
PORT=1935
TEST_VIDEO=0
AUDIO_DEV=
AUDIO_RATE=48000
AUDIO_CHANNELS=1
TEST_WRITE=0
HEVC_SAMPLE=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --video) need_value "$@"; VIDEO_DEV=$2; shift 2 ;;
        --record-dir) need_value "$@"; RECORD_DIR=$2; shift 2 ;;
        --output-dir) need_value "$@"; OUT_BASE=$2; shift 2 ;;
        --ffmpeg) need_value "$@"; FFMPEG_CMD=$2; shift 2 ;;
        --server) need_value "$@"; SERVER=$2; shift 2 ;;
        --port) need_value "$@"; PORT=$2; shift 2 ;;
        --rate) need_value "$@"; AUDIO_RATE=$2; shift 2 ;;
        --channels) need_value "$@"; AUDIO_CHANNELS=$2; shift 2 ;;
        --test-video) TEST_VIDEO=1; shift ;;
        --test-audio) need_value "$@"; AUDIO_DEV=$2; shift 2 ;;
        --test-write) TEST_WRITE=1; shift ;;
        --hevc-sample) need_value "$@"; HEVC_SAMPLE=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "未知参数：$1（使用 --help 查看）" ;;
    esac
done

positive_int "$PORT" && [ "$PORT" -le 65535 ] || die '端口必须为 1–65535'
positive_int "$AUDIO_RATE" || die '采样率必须为正整数'
positive_int "$AUDIO_CHANNELS" || die '声道数必须为正整数'
case "$SERVER" in -*) die '服务器地址不能以 - 开头' ;; esac
case "$OUT_BASE" in /*) ;; *) die '--output-dir 请使用绝对路径' ;; esac
case "$RECORD_DIR" in /*) ;; *) die '--record-dir 请使用绝对路径' ;; esac
case "$VIDEO_DEV" in /dev/*) ;; *) die '--video 应为 /dev/ 下的设备路径' ;; esac

umask 077
mkdir -p "$OUT_BASE" || die '无法创建报告父目录'
OUT_DIR=$(mktemp -d "$OUT_BASE/rk3568-env-XXXXXX") || die '无法创建报告目录'
LOG_FILE="$OUT_DIR/env_check.log"
SUMMARY_FILE="$OUT_DIR/summary.txt"
LAST_FILE="$OUT_DIR/command.tmp"
: > "$LOG_FILE" || die '无法写入日志'
: > "$SUMMARY_FILE" || die '无法写入摘要'
trap 'rm -f "$LAST_FILE"' 0
trap 'printf "\n检查中断，已有日志：%s\n" "$OUT_DIR"; exit 130' INT TERM

note() {
    printf '%s\n' "$*"
    printf '%s\n' "$*" >> "$SUMMARY_FILE"
    printf '%s\n' "$*" >> "$LOG_FILE"
}
section() { note ""; note "===== $* ====="; }

# 每个命令的原始输出进入日志，失败后继续检查其他项目。
# CHECK_RC/LAST_FILE 可用于紧接着分析本次检查，避免凭命令存在就判定能力。
run() {
    CHECK_TITLE=$1
    CHECK_LIMIT=$2
    shift 2
    : > "$LAST_FILE"
    printf '\n--- %s ---\n命令：' "$CHECK_TITLE" >> "$LOG_FILE"
    printf ' %s' "$@" >> "$LOG_FILE"
    printf '\n' >> "$LOG_FILE"
    if ! command -v "$1" >/dev/null 2>&1; then
        CHECK_RC=127
        note "[SKIP] $CHECK_TITLE：缺少命令 $1"
        return 0
    fi
    if command -v timeout >/dev/null 2>&1; then
        timeout -k 2 "$CHECK_LIMIT" "$@" > "$LAST_FILE" 2>&1
    else
        "$@" > "$LAST_FILE" 2>&1
    fi
    CHECK_RC=$?
    cat "$LAST_FILE" >> "$LOG_FILE"
    if [ "$CHECK_RC" -eq 0 ]; then
        note "[OK] $CHECK_TITLE"
    else
        note "[WARN] $CHECK_TITLE：退出码 $CHECK_RC，详见日志（124 通常为超时）"
    fi
}

active_run() {
    if command -v timeout >/dev/null 2>&1; then
        run "$@"
    else
        CHECK_RC=127
        : > "$LAST_FILE"
        note "[SKIP] $1：缺少 timeout，跳过主动测试"
    fi
}

note 'RK3568 IPC 板端环境检查'
note "检查时间：$(date '+%Y-%m-%d %H:%M:%S %z')"
note "报告目录：$OUT_DIR"
note '本脚本不检查 Ubuntu SDK 文件；不验证 FFmpeg 与 SDK 构建产物的同源关系。'
note 'OK=命令成功；WARN=需要查看输出；SKIP=未执行。硬件最终能力以实测为准。'

section '1 系统与工具'
run '系统发行版' 10 cat /etc/os-release
run '内核与架构' 10 uname -a
run '用户态位数' 10 getconf LONG_BIT
run 'glibc 版本' 10 getconf GNU_LIBC_VERSION
run '当前运行用户' 10 id
run '内存' 10 free -h
run '文件系统空间' 10 df -h
run '工具路径' 10 sh -c '
for t in v4l2-ctl media-ctl arecord aplay ffmpeg ffprobe mpi_enc_test ip nc timeout fuser; do
    printf "%s: " "$t"
    command -v "$t" || printf "未找到\n"
done'
run '内核日志' 15 dmesg
if [ "$CHECK_RC" -eq 0 ]; then
    grep -Ei 'imx415|rkisp|rkcif|csi|dphy|mpp|rkvenc|vepu|vcodec' "$LAST_FILE" > "$OUT_DIR/kernel_media.txt"
    note '[INFO] 音视频相关内核日志已提取到 kernel_media.txt；无匹配不代表无驱动。'
fi

section '2 摄像头与媒体拓扑'
run '视频与媒体设备节点' 10 sh -c '
for d in /dev/video* /dev/media* /dev/v4l-subdev*; do
    [ -e "$d" ] && ls -l "$d"
done
exit 0'
run '视频设备列表' 10 v4l2-ctl --list-devices
run '视频设备能力与配置' 10 v4l2-ctl -d "$VIDEO_DEV" --all
run '支持的视频格式' 10 v4l2-ctl -d "$VIDEO_DEV" --list-formats-ext
run '当前实际视频格式' 10 v4l2-ctl -d "$VIDEO_DEV" --get-fmt-video
run '帧率参数查询（部分 ISP 驱动不支持）' 10 v4l2-ctl -d "$VIDEO_DEV" --get-parm
for MEDIA_DEV in /dev/media*; do
    [ -c "$MEDIA_DEV" ] || continue
    run "媒体拓扑 $MEDIA_DEV" 10 media-ctl -d "$MEDIA_DEV" -p
done
note '[INFO] 多平面 API 与 num_planes 分开判断；帧率查询失败不能直接认定采集异常。'
if [ "$TEST_VIDEO" -eq 1 ]; then
    active_run '使用当前格式连续采集 120 帧' 20 v4l2-ctl -d "$VIDEO_DEV" \
        --stream-mmap=4 --stream-count=120 --stream-to=/dev/null
    note '[TODO] 根据日志核对帧率和持续取帧情况；本项没有验证画面内容。'
else
    note '[SKIP] 连续视频采集：可用 --test-video 开启，使用当前格式。'
fi

section '3 音频与 ALSA'
run '声卡信息' 10 cat /proc/asound/cards
run '硬件录音设备' 10 arecord -l
run 'ALSA PCM 名称' 10 arecord -L
run '硬件播放设备' 10 aplay -l
run '音频设备节点' 10 ls -l /dev/snd
if [ -n "$AUDIO_DEV" ]; then
    note "[INFO] 录音设备=$AUDIO_DEV，S16_LE，采样率=$AUDIO_RATE，声道=$AUDIO_CHANNELS"
    active_run '音频参数与 5 秒录音' 10 arecord -D "$AUDIO_DEV" --dump-hw-params \
        -f S16_LE -r "$AUDIO_RATE" -c "$AUDIO_CHANNELS" -d 5 "$OUT_DIR/audio_check.wav"
    if [ "$CHECK_RC" -eq 0 ]; then
        note '[TODO] 将 audio_check.wav 复制到电脑试听，确认输入、音量和速度。'
    fi
else
    note '[SKIP] 录音测试：确认设备后用 --test-audio hw:X,Y 开启。'
fi

section '4 SDK 原有 FFmpeg 与运行库'
FFMPEG_BIN=$(command -v "$FFMPEG_CMD" 2>/dev/null)
if [ -n "$FFMPEG_BIN" ]; then
    note "[INFO] 当前 FFmpeg：$FFMPEG_BIN"
    run 'FFmpeg 路径解析' 10 readlink -f "$FFMPEG_BIN"
    run 'FFmpeg 版本及组件版本' 10 "$FFMPEG_BIN" -version
    run 'FFmpeg 构建配置' 10 "$FFMPEG_BIN" -buildconf
    run 'FFmpeg 动态库依赖' 10 ldd "$FFMPEG_BIN"
    note '[INFO] 静态链接程序的 ldd 失败不代表运行库缺失。'
    run 'FFmpeg 编码器列表' 10 "$FFMPEG_BIN" -hide_banner -encoders
    run 'AAC 编码器输入要求' 10 "$FFMPEG_BIN" -hide_banner -h encoder=aac
    run 'FFmpeg 输出封装器' 10 "$FFMPEG_BIN" -hide_banner -muxers
    run 'FFmpeg 输入和输出协议' 10 "$FFMPEG_BIN" -hide_banner -protocols
else
    note '[SKIP] 未找到 ffmpeg 可执行文件；继续检查库，不据此认定缺少 FFmpeg 开发能力。'
fi
run '动态库缓存' 10 ldconfig -p
if [ "$CHECK_RC" -eq 0 ]; then
    grep -Ei 'libav(codec|format|util)|libswresample|libasound|rockchip_mpp|libmpp' "$LAST_FILE" \
        > "$OUT_DIR/media_libraries.txt"
    note '[INFO] 相关库缓存已提取到 media_libraries.txt。'
fi
run '常见目录中的音视频库（包含符号链接）' 20 sh -c '
for d in /lib /usr/lib /usr/local/lib /opt; do
    [ -d "$d" ] || continue
    find "$d/" -maxdepth 5 \( -name "libavcodec.so*" -o -name "libavformat.so*" \
        -o -name "libavutil.so*" -o -name "libswresample.so*" -o -name "libasound.so*" \
        -o -name "librockchip_mpp.so*" -o -name "libmpp.so*" \) -print
done'
note '[TODO] 在 Ubuntu 上核对 SDK 对应头文件、库、sysroot 和构建来源。'
note '[INFO] FFmpeg 未列出 rkmpp 编码器，不代表应用不能直接调用 MPP 硬编码。'
note '[TODO] AAC、FLV、MP4 和 RTMP Output 支持需按日志核对，命令成功本身不代表全部支持。'

if [ -n "$HEVC_SAMPLE" ]; then
    if [ -n "$FFMPEG_BIN" ] && [ -f "$HEVC_SAMPLE" ]; then
        active_run 'H.265 素材直接封装为 FLV' 20 "$FFMPEG_BIN" -hide_banner -nostdin -n \
            -i "$HEVC_SAMPLE" -map 0:v:0 -an -c:v copy -t 3 -f flv "$OUT_DIR/hevc_mux_check.flv"
        note '[TODO] 先核对日志中输入确为 HEVC；封装成功仍需验证服务器和播放器。'
    else
        note '[SKIP] H.265 封装测试：FFmpeg 或素材文件不存在。'
    fi
else
    note '[SKIP] H.265 FLV 封装实测：有已验证素材后使用 --hevc-sample FILE。'
fi
note '[INFO] 原有 FFmpeg 若不支持 H.265 FLV/RTMP，记录限制，再考虑初版 H.264 + AAC；不自动更换版本。'

section '5 RKMPP 驱动和编码测试入口'
run 'MPP 与内存分配设备节点' 10 sh -c '
for d in /dev/mpp_service /dev/rkvenc* /dev/vpu_service /dev/dma_heap/* /dev/ion; do
    [ -e "$d" ] && ls -l "$d"
done
exit 0'
run 'MPP 已加载模块（驱动也可能内建）' 10 sh -c '
if [ -r /proc/modules ]; then
    grep -Ei "mpp|rkvenc|vepu|vcodec" /proc/modules || :
fi'
run 'SDK 编码样例路径' 10 sh -c 'command -v mpi_enc_test'
note '[TODO] 不自动运行未知版本的 MPP 样例；根据 SDK 帮助及输入帧布局再做编码实测。'
note '[INFO] 设备节点、库或测试程序存在，只能说明环境线索，不能证明硬编码成功。'

section '6 网络与 SRS 可达性'
run '网卡地址' 10 ip addr show
run '路由' 10 ip route show
if [ -n "$SERVER" ]; then
    run 'SRS 主机 Ping' 8 ping -c 3 -W 2 "$SERVER"
    # Debian 使用 bash /dev/tcp，避免各版本 nc 的 -z 参数差异。
    # /dev/tcp 只建立 TCP 连接，不发送业务数据；没有 bash 时尝试 nc。
    if command -v bash >/dev/null 2>&1; then
        active_run 'SRS TCP 端口连接' 6 bash -c 'exec 3<>/dev/tcp/"$1"/"$2"' _ "$SERVER" "$PORT"
    else
        active_run 'SRS TCP 端口连接（nc）' 6 nc -z -w 3 "$SERVER" "$PORT"
    fi
    note '[INFO] TCP 连接成功仅代表端口可达，SRS 服务配置及实际收流需在服务器侧确认。'
else
    note '[SKIP] SRS 可达性：使用 --server 实际UbuntuIP 开启。'
fi

section '7 录像目录'
run '录像目录属性' 10 ls -ld "$RECORD_DIR"
if [ -d "$RECORD_DIR" ]; then
    run '录像目录剩余空间' 10 df -h "$RECORD_DIR"
    run '录像目录 inode' 10 df -i "$RECORD_DIR"
    if [ -w "$RECORD_DIR" ]; then
        note '[OK] 当前用户对录像目录有写入权限；实际写入另行测试。'
    else
        note '[WARN] 当前用户无录像目录写入权限。'
    fi
    if [ "$TEST_WRITE" -eq 1 ]; then
        active_run '录像目录临时文件写入测试' 10 sh -c '
p=$(mktemp "$1/.ipc-write-check-XXXXXX") || exit 1
trap '\''rm -f "$p"'\'' 0
trap '\''exit 130'\'' INT TERM
printf "RK3568 IPC write check\n" > "$p" || exit 1
cat "$p"' _ "$RECORD_DIR"
        note '[INFO] 小文件写入成功不代表录像吞吐或断电恢复通过。'
    else
        note '[SKIP] 目录写入测试：使用 --test-write 开启。'
    fi
else
    note '[WARN] 录像目录不存在；脚本没有自动创建，请用 --record-dir 指定实际目录。'
fi

section '检查完成'
note '仍需人工确认：音画内容、实测帧率、MPP 编码、音视频同步、RTMP 播放与 MP4 回放。'
note "摘要：$SUMMARY_FILE"
note "完整输出：$LOG_FILE"
note '请把 summary.txt 和 env_check.log 一起提供用于后续判断。'
exit 0
