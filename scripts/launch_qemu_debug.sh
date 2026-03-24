#!/bin/bash
# @file scripts/launch_qemu_debug.sh
# @brief 以调试模式构建并启动 QEMU
#
# 此脚本会：
#   1. 以 Debug 模式配置 CMake（带符号信息，无优化）
#   2. 构建整个项目
#   3. 启动 QEMU 调试模式（GDB stub on :1234）

set -e  # 遇到错误立即退出

# 项目根目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

# 导入日志工具
source "${SCRIPT_DIR}/log/logging.sh"

log_info "========================================"
log_info "Cinux OS - QEMU 调试模式启动"
log_info "========================================"
CLEAN_CMD="rm -rf \"$BUILD_DIR\""
log_info "${CLEAN_CMD}"

# 步骤 1: 配置 CMake（Debug 模式）
log_info ""
log_info "[1/3] 配置 CMake (Debug 模式)..."
CMAKE_CMD="cmake -B \"$BUILD_DIR\" -DCMAKE_BUILD_TYPE=Debug -S \"$PROJECT_ROOT\""
log_cmd "$CMAKE_CMD"
eval "$CMAKE_CMD"

# 步骤 2: 构建项目
log_info ""
log_info "[2/3] 构建项目..."
BUILD_CMD="cmake --build \"$BUILD_DIR\" -j\"$(nproc)\""
log_cmd "$BUILD_CMD"
eval "$BUILD_CMD"

# 步骤 3: 启动 QEMU 调试模式
log_info ""
log_info "[3/3] 启动 QEMU 调试模式..."
log_info "GDB 将监听在 localhost:1234"
log_info "在 VSCode 中按 F5 连接调试器"
log_info "按 Ctrl+C 停止 QEMU"
log_info ""
cd "$BUILD_DIR"
QEMU_CMD="make run-debug"
log_cmd "$QEMU_CMD"
eval "$QEMU_CMD"
