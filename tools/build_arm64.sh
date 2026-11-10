#!/bin/bash
set -e
cd /mnt/d/省电模块
aarch64-linux-gnu-gcc -Os -static -Wno-format-truncation -Wno-unused-result -o /root/ft.new src/feather.c
aarch64-linux-gnu-strip /root/ft.new
cp -f /root/ft.new /mnt/d/省电模块/module/bin/feather
sync
echo "built $(stat -c %s /mnt/d/省电模块/module/bin/feather) bytes"
