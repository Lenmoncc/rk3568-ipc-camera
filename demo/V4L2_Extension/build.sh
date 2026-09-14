#!/bin/sh
# 在 Ubuntu 中执行：sh build.sh
# 可选：SDK_ROOT=/实际路径/rk3568_linux_sdk sh build.sh
set -eu
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
SDK_ROOT=${SDK_ROOT:-"$HOME/rk3568_linux_sdk"}
BOARD_BUILD_DIR="$SDK_ROOT/buildroot/output/rockchip_atk_dlrk3568"
CXX="$BOARD_BUILD_DIR/host/bin/aarch64-buildroot-linux-gnu-g++"
SYSROOT="$BOARD_BUILD_DIR/host/aarch64-buildroot-linux-gnu/sysroot"

if [ ! -x "$CXX" ]; then
    printf '[ERROR] 未找到可执行的交叉编译器：%s\n' "$CXX" >&2
    printf '请通过 SDK_ROOT 指定当前 SDK 根目录。\n' >&2
    exit 1
fi
for header in core.hpp core/ocl.hpp imgcodecs.hpp highgui.hpp videoio.hpp; do
    if [ ! -f "$SYSROOT/usr/include/opencv4/opencv2/$header" ]; then
        printf '[ERROR] sysroot 中缺少 OpenCV 头文件：%s\n' "$header" >&2
        exit 1
    fi
done
for library in core imgcodecs highgui videoio; do
    if [ ! -e "$SYSROOT/usr/lib/libopencv_$library.so" ]; then
        printf '[ERROR] sysroot 中缺少 libopencv_%s.so（或链接已失效）\n' "$library" >&2
        exit 1
    fi
done
if [ ! -f "$SYSROOT/usr/include/libv4lconvert.h" ] || [ ! -e "$SYSROOT/usr/lib/libv4lconvert.so" ]; then
    printf '[ERROR] sysroot 缺少 libv4lconvert.h 或 libv4lconvert.so\n' >&2
    exit 1
fi
TARGET=$("$CXX" -dumpmachine)
case "$TARGET" in
    aarch64*) ;;
    *) printf '[ERROR] 编译器目标不是 AArch64：%s\n' "$TARGET" >&2; exit 1 ;;
esac
printf '[BUILD] compiler=%s\n[BUILD] sysroot=%s\n' "$CXX" "$SYSROOT"
"$CXX" --version
mkdir -p "$SCRIPT_DIR/bin"
for name in preview_libv4l preview_opencv; do
    printf '\n[BUILD] %s.cpp\n' "$name"
    # -rpath-link 仅帮助链接器查找目标库的间接依赖，不把 Ubuntu 路径写入运行时 RPATH。
    case "$name" in
        preview_libv4l) set -- -lv4lconvert -lopencv_imgcodecs -lopencv_highgui -lopencv_core ;;
        preview_opencv) set -- -lopencv_videoio -lopencv_highgui -lopencv_core ;;
    esac
    "$CXX" --sysroot="$SYSROOT" -std=c++11 -Wall -Wextra -Wpedantic -O0 -g \
        -I"$SYSROOT/usr/include/opencv4" \
        "$SCRIPT_DIR/$name.cpp" -o "$SCRIPT_DIR/bin/$name" \
        -L"$SYSROOT/usr/lib" \
        "-Wl,-rpath-link,$SYSROOT/usr/lib" "-Wl,-rpath-link,$SYSROOT/lib" \
        "$@"
    if command -v file >/dev/null 2>&1; then file "$SCRIPT_DIR/bin/$name"; fi
done
printf '\n[BUILD] 完成；将 bin/ 和 run.sh 一起传到开发板。\n'
