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
printf '[BUILD] compiler=%s\n[BUILD] sysroot=%s\n' "$CC" "$SYSROOT"
make -C "$PROJECT_DIR" SDK_ROOT="$SDK_ROOT" CC="$CC" SYSROOT="$SYSROOT" WITH_MPP="$WITH_MPP" MPP_INCLUDE="$MPP_INCLUDE" MPP_LIBS="$MPP_LIBS" "$@"
printf '[INFO] 已构建；--capture 验证原始采集，--encode --output 新文件.h264 验证硬编码。音频、RTMP、MP4 尚未接入。\n'
