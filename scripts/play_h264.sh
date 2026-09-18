#!/bin/sh
# 功能：在开发板 Weston/Wayland 屏幕上循环播放已保存的 H.264 文件。
# 使用 sh 调用即可；环境只作用于本脚本及 ffplay，不修改当前终端或系统配置。
set -eu
if [ "$#" -ne 1 ]; then
    printf '用法：sh scripts/play_h264.sh /绝对路径/video.h264\n' >&2
    exit 2
fi
VIDEO_PATH=$1
[ -f "$VIDEO_PATH" ] && [ -s "$VIDEO_PATH" ] || {
    printf '[ERROR] 视频不存在或为空：%s\n' "$VIDEO_PATH" >&2
    exit 1
}
command -v ffplay >/dev/null 2>&1 || {
    printf '[ERROR] 板端未找到 ffplay，请确认 SDK 镜像中的 FFmpeg 播放组件。\n' >&2
    exit 1
}
# 沿用本板已经验证的 /run/wayland-0；允许通过环境变量覆盖其他显示会话。
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-wayland}
export SDL_RENDER_DRIVER=${SDL_RENDER_DRIVER:-software}
if [ "$SDL_VIDEODRIVER" = wayland ] && [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
    printf '[ERROR] Wayland 会话不存在：%s/%s\n请确认板端 Weston 已启动及显示环境变量正确。\n' "$XDG_RUNTIME_DIR" "$WAYLAND_DISPLAY" >&2
    exit 1
fi
# H.264 的 SPS 携带图像参数；无需再指定 NV12 像素格式或分辨率。
# 0 表示无限循环；Ctrl+C（启动终端）或 q（播放窗口）退出。
exec ffplay -f h264 -i "$VIDEO_PATH" -loop 0 -fs -an
