#!/bin/bash
# ============================================================================
#  编译 aarch64 静态二进制（Rust 版守护进程）
#    bash tools/build_arm64.sh
#  依赖：rustup + target aarch64-unknown-linux-gnu + gcc-aarch64-linux-gnu
# ============================================================================
set -e
cd "$(dirname "$0")/.."
export PATH="$HOME/.cargo/bin:$PATH"

OUT=${OUT:-/root/ft.new}
rustc --edition 2021 \
      -C opt-level=3 -C panic=abort -C codegen-units=1 \
      -C target-feature=+crt-static -C strip=symbols \
      --target aarch64-unknown-linux-gnu \
      -C linker=aarch64-linux-gnu-gcc \
      rust/main.rs -o "$OUT"

cp -f "$OUT" module/bin/feather
chmod 0755 module/bin/feather
sync
echo "built $(stat -c %s module/bin/feather) bytes  $(md5sum module/bin/feather | cut -d' ' -f1)"
