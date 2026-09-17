#!/bin/sh
# 在开发板前台运行。使用脚本自身位置寻找程序和默认配置，避免依赖当前目录。
# 覆盖配置：sh scripts/run.sh --config /absolute/path/ipc.conf
# 仅检查配置：sh scripts/run.sh --check-config
set -eu
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd)
PROGRAM="$PROJECT_DIR/bin/ipc_camera"
CONFIG_FILE="$PROJECT_DIR/configs/ipc.conf"
[ -x "$PROGRAM" ] || { printf '[ERROR] 请先编译并部署：%s\n' "$PROGRAM" >&2; exit 1; }
case "$(uname -m)" in
    aarch64|arm64) ;;
    *) printf '[ERROR] run.sh 面向 ARM64 开发板，本机检查请使用 make host-check。\n' >&2; exit 1 ;;
esac
# 用户后续传入的 -c/--config 覆盖默认路径；main 采用最后一次配置路径参数。
exec "$PROGRAM" --config "$CONFIG_FILE" "$@"
