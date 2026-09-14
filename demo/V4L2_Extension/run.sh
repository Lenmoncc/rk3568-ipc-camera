#!/bin/sh
# 开发板：sh run.sh convert / libv4l / opencv / bmp

# 查看 BMP：默认打开脚本所在目录的转换结果。
if [ "${1:-}" = "bmp" ]; then
    bmp_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
    bmp_file="${2:-$bmp_dir/frame_yu12_to_bgr.bmp}"

    if [ ! -f "$bmp_file" ]; then
        echo "[ERROR] 图片不存在：$bmp_file" >&2
        exit 1
    fi

    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run}"
    export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
    export QT_QPA_PLATFORM=wayland

    exec python3 - "$bmp_file" <<'PY'
import sys
import cv2

img = cv2.imread(sys.argv[1])
if img is None:
    raise SystemExit("[ERROR] 图片读取失败：" + sys.argv[1])

try:
    cv2.namedWindow("BMP Viewer", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("BMP Viewer", 960, 540)
    cv2.imshow("BMP Viewer", img)
    print("在图片窗口按 q / ESC，或在终端按 Ctrl+C 退出", flush=True)
    while True:
        if cv2.waitKey(100) & 0xff in (ord("q"), ord("Q"), 27):
            break
        if cv2.getWindowProperty("BMP Viewer", cv2.WND_PROP_VISIBLE) < 1:
            break
except KeyboardInterrupt:
    pass
finally:
    cv2.destroyAllWindows()
PY
fi

set -eu
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
if [ "$#" -lt 1 ]; then
    printf '用法：sh run.sh convert|libv4l|opencv [--nv12]\n' >&2
    exit 2
fi
case "$1" in
    convert|libv4l) BINARY="$SCRIPT_DIR/bin/preview_libv4l" ;;
    opencv) BINARY="$SCRIPT_DIR/bin/preview_opencv" ;;
    *) printf '[ERROR] 请选择 convert、libv4l 或 opencv\n' >&2; exit 2 ;;
esac
MODE=$1
shift
if [ "$MODE" = opencv ] && [ "$#" -ne 0 ]; then
    printf '[ERROR] opencv 模式不接受附加参数\n' >&2
    exit 2
fi
for ARG in "$@"; do
    if [ "$ARG" != --nv12 ]; then
        printf '[ERROR] 唯一可选参数为 --nv12\n' >&2
        exit 2
    fi
done
if [ ! -x "$BINARY" ]; then
    printf '[ERROR] 文件不存在或没有执行权限：%s\n' "$BINARY" >&2
    printf '先在 Ubuntu 编译并传入 bin/，再为两个程序添加执行权限。\n' >&2
    exit 1
fi
if [ ! -c /dev/video0 ]; then
    printf '[ERROR] 没有 /dev/video0，请在 RK3568 开发板上运行。\n' >&2
    exit 1
fi
# 转换保存模式不创建窗口，不要求 Weston socket 或显示变量。
if [ "$MODE" = convert ]; then
    exec "$BINARY" --convert-only "$@"
fi
# 设置仅作用于本脚本及其子进程，不修改 profile，也不启动/停止 Weston。
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export QT_QPA_PLATFORM=wayland
export QT_QPA_PLATFORM_PLUGIN_PATH="${QT_QPA_PLATFORM_PLUGIN_PATH:-/usr/lib/qt/plugins/platforms}"
export OPENCV_VIDEOIO_DEBUG="${OPENCV_VIDEOIO_DEBUG:-1}"
case "$WAYLAND_DISPLAY" in
    /*) SOCKET="$WAYLAND_DISPLAY" ;;
    *) SOCKET="$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ;;
esac
if [ ! -S "$SOCKET" ]; then
    printf '[ERROR] Wayland socket 不存在：%s\n' "$SOCKET" >&2
    exit 1
fi
printf '[RUN] %s\n[RUN] Wayland socket=%s\n[RUN] Qt platform=%s\n' \
    "$BINARY" "$SOCKET" "$QT_QPA_PLATFORM" >&2
exec "$BINARY" "$@"
