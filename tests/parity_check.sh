#!/bin/bash
# 新旧实现输出对照：同一棵假文件树上跑 C 版与 Rust 版，逐行 diff
#
# 注意（v1.4.1 起）：C 版冻结在 v1.3.4，只作为「行为基线」保留；发布的是 Rust 版。
# 下面这条 diff 会故意出现差异，属预期：
#   - charge  : Rust 多了 cur: 行（v2 节点类型/基准/已写入值）
#   - doctor  : Rust 多了 icon / charge 两段
#   - 图标    : C 还是旧实现（只 PNG、按体积排序）
# 想跑「当前发布的实现」的回归，用 tests/run_tests_wsl.sh（默认就编译 Rust 版）。
set -u
PROJ=/mnt/d/省电模块
FAKE=$PROJ/tests/fake
DATA=/tmp/parity_data
gcc -O1 -w -o /tmp/feather_c "$PROJ/src/feather.c" || exit 1
RS=/tmp/feather_rs

sh "$PROJ/tests/mkfake.sh" "$FAKE" >/dev/null || exit 1
rm -rf "$DATA"; mkdir -p "$DATA"
cp "$PROJ"/module/etc/config.ini "$PROJ"/module/etc/games.conf "$PROJ"/module/etc/rules.conf "$DATA"/
echo 50 > "$FAKE/sys/class/power_supply/battery/capacity"
echo 330 > "$FAKE/sys/class/power_supply/battery/temp"
echo Charging > "$FAKE/sys/class/power_supply/battery/status"

export FEATHER_DATA="$DATA" FEATHER_CPU_ROOT="$FAKE/sys/devices/system/cpu" \
       FEATHER_CGROUP_ROOT="$FAKE/sys/fs/cgroup" FEATHER_SYS_ROOT="$FAKE/sys" \
       FEATHER_PROC_ROOT="$FAKE/proc" FEATHER_HMBIRD_SYS="$FAKE/sys/module/hmbird" \
       FEATHER_HTTP_PORT=7896 FEATHER_WEBROOT="$PROJ/module/webroot"

for cmd in "charge" "charge probe" "power" "probe" "status" "version" "labels" "apprule" "pkg list"; do
    /tmp/feather_c $cmd > /tmp/c.out 2>&1
    $RS $cmd > /tmp/r.out 2>&1
    if diff -q /tmp/c.out /tmp/r.out >/dev/null; then
        echo "SAME  $cmd"
    else
        echo "DIFF  $cmd"
        diff /tmp/c.out /tmp/r.out | head -12
    fi
done

# doctor 里有随机/时变字段，只比结构
/tmp/feather_c doctor > /tmp/c.out 2>&1
$RS doctor > /tmp/r.out 2>&1
echo "doctor 行数 C=$(wc -l < /tmp/c.out) R=$(wc -l < /tmp/r.out)"
diff <(sed 's/[0-9]\+/N/g' /tmp/c.out) <(sed 's/[0-9]\+/N/g' /tmp/r.out) | head -20
