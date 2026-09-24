#!/bin/sh
# Ubuntu 中运行。使用项目 SDK_ROOT 配置，不引入 demo 或 Ubuntu 系统库。
set -eu
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd)
SDK_ROOT=${SDK_ROOT:-"$HOME/rk3568_linux_sdk"}
BOARD_BUILD_DIR="$SDK_ROOT/buildroot/output/rockchip_atk_dlrk3568"
CC="$BOARD_BUILD_DIR/host/bin/aarch64-buildroot-linux-gnu-gcc"
SYSROOT="$BOARD_BUILD_DIR/host/aarch64-buildroot-linux-gnu/sysroot"
[ -x "$CC" ] || { printf '[ERROR] 编译器不存在：%s\n' "$CC" >&2; exit 1; }
[ -d "$SYSROOT" ] || { printf '[ERROR] sysroot 不存在：%s\n' "$SYSROOT" >&2; exit 1; }
case "$("$CC" -dumpmachine)" in
    aarch64*) ;;
    *) printf '[ERROR] 编译器目标不是 AArch64\n' >&2; exit 1 ;;
esac
# 缺少 MPP 时明确报错；不能悄悄编出不具备硬编码能力的正式二进制。
# WITH_MPP=0 仅用于显式保留基础采集能力；本机测试由 Makefile 独立设置。
WITH_MPP=${WITH_MPP:-1}
MPP_INCLUDE=${MPP_INCLUDE:-"$SYSROOT/usr/include/rockchip"}
MPP_LIBS=${MPP_LIBS:--lrockchip_mpp}
if [ "$WITH_MPP" = 1 ]; then
    if [ ! -f "$MPP_INCLUDE/rk_mpi.h" ] && [ -f "$SYSROOT/usr/include/rk_mpi.h" ]; then
        MPP_INCLUDE="$SYSROOT/usr/include"
    fi
    [ -f "$MPP_INCLUDE/rk_mpi.h" ] && [ -f "$MPP_INCLUDE/rk_venc_cfg.h" ] || {
        printf '[ERROR] 缺少 SDK MPP 开发头文件：%s\n请检查 SDK 的 MPP 安装，或显式设置 MPP_INCLUDE。\n' "$MPP_INCLUDE" >&2
        exit 1
    }
fi
# 音频启用时检查 SDK 的 ALSA 开发接口，链接器继续核对实际库和符号。
WITH_ALSA=${WITH_ALSA:-1}
ALSA_INCLUDE=${ALSA_INCLUDE:-"$SYSROOT/usr/include"}
ALSA_LIBS=${ALSA_LIBS:--lasound}
if [ "$WITH_ALSA" = 1 ] && [ ! -f "$ALSA_INCLUDE/alsa/asoundlib.h" ]; then
    printf '[ERROR] 缺少 SDK ALSA 头文件：%s/alsa/asoundlib.h\n' "$ALSA_INCLUDE" >&2
    exit 1
fi
# AAC 接口与库必须来自同一 SDK；不通过升级板端 FFmpeg 解决编译依赖。
WITH_FFMPEG=${WITH_FFMPEG:-1}
FFMPEG_INCLUDE=${FFMPEG_INCLUDE:-"$SYSROOT/usr/include"}
FFMPEG_LIBS=${FFMPEG_LIBS:--lavformat -lavcodec -lswresample -lavutil}
if [ "$WITH_FFMPEG" = 1 ]; then
    for header in libavformat/avformat.h libavcodec/avcodec.h libswresample/swresample.h libavutil/audio_fifo.h; do
        [ -f "$FFMPEG_INCLUDE/$header" ] || { printf '[ERROR] 缺少 SDK FFmpeg 头文件：%s/%s\n' "$FFMPEG_INCLUDE" "$header" >&2; exit 1; }
    done
fi
printf '[BUILD] compiler=%s\n[BUILD] sysroot=%s\n' "$CC" "$SYSROOT"
make -C "$PROJECT_DIR" SDK_ROOT="$SDK_ROOT" CC="$CC" SYSROOT="$SYSROOT" WITH_MPP="$WITH_MPP" MPP_INCLUDE="$MPP_INCLUDE" MPP_LIBS="$MPP_LIBS" WITH_ALSA="$WITH_ALSA" ALSA_INCLUDE="$ALSA_INCLUDE" ALSA_LIBS="$ALSA_LIBS" WITH_FFMPEG="$WITH_FFMPEG" FFMPEG_INCLUDE="$FFMPEG_INCLUDE" FFMPEG_LIBS="$FFMPEG_LIBS" "$@"
printf '[INFO] 已构建；--capture 验证原始采集，--encode --output 新文件.h264 验证硬编码。--audio-capture --pcm 新文件.pcm 验证音频。--audio-encode --aac 新文件.aac 验证 AAC。--record --mp4 新文件.mp4 验证音视频录像；使用 configs/ipc-rtmp.conf 时同时推流，--stream 可只推流。\n'
