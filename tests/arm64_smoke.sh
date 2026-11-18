#!/bin/bash
# 在 x86 上跑 arm64 静态二进制（qemu-user），验证交叉编译产物真的能执行
set -u
PROJ=/mnt/d/省电模块
FAKE=$PROJ/tests/fake
DATA=/tmp/arm64_data
BIN=$PROJ/module/bin/feather
Q=/usr/bin/qemu-aarch64-static

rm -rf "$DATA"; mkdir -p "$DATA"
sh "$PROJ/tests/mkfake.sh" "$FAKE" >/dev/null || exit 1
cp "$PROJ"/module/etc/config.ini "$PROJ"/module/etc/games.conf "$PROJ"/module/etc/rules.conf "$DATA"/
echo 50 > "$FAKE/sys/class/power_supply/battery/capacity"
echo 330 > "$FAKE/sys/class/power_supply/battery/temp"
echo Charging > "$FAKE/sys/class/power_supply/battery/status"

export FEATHER_DATA="$DATA" FEATHER_CPU_ROOT="$FAKE/sys/devices/system/cpu" \
       FEATHER_CGROUP_ROOT="$FAKE/sys/fs/cgroup" FEATHER_SYS_ROOT="$FAKE/sys" \
       FEATHER_PROC_ROOT="$FAKE/proc" FEATHER_HMBIRD_SYS="$FAKE/sys/module/hmbird" \
       FEATHER_HTTP_PORT=7897 FEATHER_WEBROOT="$PROJ/module/webroot"

echo "== arm64 二进制信息 =="
file "$BIN" 2>/dev/null | head -1
echo "== version =="
$Q "$BIN" version
echo "== charge =="
$Q "$BIN" charge | head -8
echo "== 起守护进程 3 秒，看 state 与 socket =="
$Q "$BIN" run &
DPID=$!
sleep 3
$Q "$BIN" status | grep -E '^(profile|runner|version|accel|battery|temp)=?' | head -6
echo "-- socket 命令 --"
$Q "$BIN" mode game
$Q "$BIN" status | grep -E '^profile='
echo "-- http --"
curl -s -m 3 http://127.0.0.1:7897/status | head -2
echo "-- 退出还原 --"
kill $DPID 2>/dev/null; wait $DPID 2>/dev/null
sleep 1
echo "tbat=$(cat "$FAKE/sys/module/oplus_chg_v2/parameters/oplus_dbg_tbat")"
echo DONE
