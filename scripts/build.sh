#!/bin/sh
# Ubuntu 中运行。复用 demo 的 SDK_ROOT 配置，不使用 Ubuntu 系统库替代目标库。
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
printf '[BUILD] compiler=%s\n[BUILD] sysroot=%s\n' "$CC" "$SYSROOT"
make -C "$PROJECT_DIR" SDK_ROOT="$SDK_ROOT" CC="$CC" SYSROOT="$SYSROOT" "$@"
printf '[INFO] 当前实现配置、日志与原始帧队列；编译通过不代表采集、编码或推流功能已完成。\n'
