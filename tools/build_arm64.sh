#!/bin/bash
# 交叉编译 arm64 静态二进制（在 WSL/Debian 里跑）
#   sudo apt install gcc-aarch64-linux-gnu
set -e
cd "$(dirname "$0")/.."
aarch64-linux-gnu-gcc -Os -s -static \
  -Wno-format-truncation -Wno-unused-result \
  -o module/bin/feather src/feather.c
echo "built module/bin/feather ($(stat -c %s module/bin/feather) bytes)"

# 端到端测试（需要 binfmt/qemu-user 才能直接跑 arm64；否则用 x86 构建）
if [ -n "${RUN_TESTS:-}" ]; then
    FEATHER_TEST_BIN="$PWD/module/bin/feather" bash tests/run_tests_wsl.sh
fi
