#!/bin/bash
# 造一个假 APK + 假 pm，验证 icon 抠图链路
set -e
cd /mnt/d/省电模块

export FEATHER_DATA=/root/ic/data
export FEATHER_CPU_ROOT=/root/ic/fake/sys/devices/system/cpu
export FEATHER_CGROUP_ROOT=/root/ic/fake/sys/fs/cgroup
export FEATHER_SYS_ROOT=/root/ic/fake/sys
export FEATHER_PROC_ROOT=/root/ic/fake/proc

rm -rf /root/ic; mkdir -p /root/ic/data /root/ic/bin /root/ic/apk/res/mipmap-xxxhdpi /root/ic/apk/res/mipmap-hdpi
sh tests/mkfake.sh /root/ic/fake >/dev/null

# 两个不同大小的 png（只有体积差异，用来验证“选最大的”）
python3 - <<'PY'
import os, zlib, struct
def png(path, size):
    # 生成 size x size 的纯色 PNG
    raw = b''.join(b'\x00' + bytes([30, 120, 200]) * size for _ in range(size))
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    hdr = struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', hdr) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
png('/root/ic/apk/res/mipmap-hdpi/ic_launcher.png', 24)
png('/root/ic/apk/res/mipmap-xxxhdpi/ic_launcher.png', 192)
PY

(cd /root/ic/apk && zip -qr /root/ic/base.apk .)

cat > /root/ic/bin/pm <<'EOF'
#!/bin/sh
# 假 pm：只回答 path
[ "$1" = path ] && echo "package:/root/ic/base.apk"
EOF
chmod +x /root/ic/bin/pm
export PATH=/root/ic/bin:$PATH

echo "--- 抠图标 ---"
./module/bin/feather icon com.tencent.mm
ls -l /root/ic/data/icons/ 2>/dev/null
echo "--- 再取一次（走缓存）---"
./module/bin/feather icon com.tencent.mm
echo "--- 不存在的包（应进负缓存）---"
cat > /root/ic/bin/pm <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x /root/ic/bin/pm
./module/bin/feather icon com.not.exist
cat /root/ic/data/icons/.miss 2>/dev/null
