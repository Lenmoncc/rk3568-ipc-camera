#!/bin/sh
# 功能：在开发板本地用 aplay 播放 S16_LE 交错 PCM；无需视频显示环境。
# 参数：文件 [采样率=48000] [声道数=2] [播放设备=plughw:0,0]。
# 裸 PCM 不含参数头，采样率/声道数必须与采集日志中的 actual 一致。
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 4 ]; then
    printf '用法：sh play_pcm.sh 文件.pcm [48000] [2] [plughw:0,0]\n' >&2
    exit 2
fi
PCM_PATH=$1
PCM_RATE=${2:-48000}
PCM_CHANNELS=${3:-2}
PCM_DEVICE=${4:-plughw:0,0}
case "$PCM_RATE" in ''|*[!0-9]*|??????*) printf '[ERROR] 无效采样率\n' >&2; exit 2;; esac
if [ "$PCM_RATE" -lt 8000 ] || [ "$PCM_RATE" -gt 96000 ]; then
    printf '[ERROR] 采样率必须为 8000..96000\n' >&2
    exit 2
fi
case "$PCM_CHANNELS" in 1|2) ;; *) printf '[ERROR] 声道数必须为 1 或 2\n' >&2; exit 2;; esac
[ -f "$PCM_PATH" ] && [ -s "$PCM_PATH" ] || {
    printf '[ERROR] 音频不存在或为空：%s\n' "$PCM_PATH" >&2
    exit 1
}
command -v aplay >/dev/null 2>&1 || { printf '[ERROR] 板端未找到 aplay\n' >&2; exit 1; }
# 录音使用 hw 严格核对参数；播放使用 plughw 允许声卡必要的格式转换。
# 不更改 mixer/音量，播完自动退出，Ctrl+C 可提前停止。
exec aplay -D "$PCM_DEVICE" -t raw -f S16_LE -r "$PCM_RATE" -c "$PCM_CHANNELS" -- "$PCM_PATH"
