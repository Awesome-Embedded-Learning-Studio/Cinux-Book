#!/bin/bash
# @file scripts/debug_qemu_debug.sh
# @brief 启动 GDB 连接到运行中的 QEMU

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

echo "========================================"
echo "Cinux OS - GDB 调试会话"
echo "========================================"
echo ""
echo "确保 QEMU 调试模式已运行:"
echo "  cd build && make run-debug"
echo ""

# 使用 gdbinit 文件启动 GDB
gdb -ix "$PROJECT_ROOT/.vscode/gdbinit"
