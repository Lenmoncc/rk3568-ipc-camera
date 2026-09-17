#!/bin/sh
# 在开发板的项目目录中前台运行；不启动守护，不设置显示或系统库环境。
set -eu
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd)
PROGRAM="$PROJECT_DIR/bin/ipc_camera"
[ -x "$PROGRAM" ] || { printf '[ERROR] 请先编译并部署：%s\n' "$PROGRAM" >&2; exit 1; }
case "$(uname -m)" in
    aarch64|arm64) ;;
    *) printf '[ERROR] run.sh 面向 ARM64 开发板，本机检查请使用 make host-check。\n' >&2; exit 1 ;;
esac
exec "$PROGRAM" "$@"
