#!/bin/sh
# 功能：在开发板本地将 AAC 解码为临时 WAV，再用指定 ALSA 设备播放。
# 参数：文件.aac [播放设备=plughw:0,0]；不改变 mixer，不依赖图形桌面。
# 先完整解码检查状态，避免 POSIX 管道只报告 aplay 成功而掩盖解码失败。
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    printf '用法：sh play_aac.sh 文件.aac [plughw:0,0]\n' >&2
    exit 2
fi
AAC_PATH=$1
AAC_DEVICE=${2:-plughw:0,0}
[ -f "$AAC_PATH" ] && [ -s "$AAC_PATH" ] || { printf '[ERROR] 音频不存在或为空：%s\n' "$AAC_PATH" >&2; exit 1; }
# 绝对路径避免以 - 开头的文件名被命令误判为选项。
case "$AAC_PATH" in /*) ;; *) AAC_PATH="$PWD/$AAC_PATH";; esac
for command_name in ffmpeg aplay mktemp; do
    command -v "$command_name" >/dev/null 2>&1 || { printf '[ERROR] 板端未找到 %s\n' "$command_name" >&2; exit 1; }
done
AAC_TEMP=$(mktemp -d "${TMPDIR:-/tmp}/ipc-aac.XXXXXX")
# 唯一临时目录退出即清理；信号处理走 EXIT，避免残留解码文件。
trap 'rm -f "$AAC_TEMP/audio.wav"; rmdir "$AAC_TEMP"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
ffmpeg -nostdin -hide_banner -loglevel error -xerror -i "$AAC_PATH" -map 0:a:0 -c:a pcm_s16le -f wav "$AAC_TEMP/audio.wav"
aplay -D "$AAC_DEVICE" -- "$AAC_TEMP/audio.wav"
