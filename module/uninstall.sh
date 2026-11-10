#!/system/bin/sh
# 卸载：停守护进程 + 把频率上限还回去
MODID=feather_tune
DATA=/data/adb/feather
BIN=/data/adb/modules/$MODID/bin

if [ -x "$BIN/feather" ]; then
  "$BIN/feather" stop >/dev/null 2>&1
elif [ -x "$BIN/shell/featherd.sh" ]; then
  "$BIN/shell/featherd.sh" stop >/dev/null 2>&1
fi

if [ -f "$DATA/featherd.pid" ]; then
  kill "$(cat "$DATA/featherd.pid" 2>/dev/null)" 2>/dev/null
fi
rm -f "$DATA/stop" "$DATA/featherd.pid" "$DATA/control.sock" 2>/dev/null

for p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$p" ] || continue
  [ -r "$p/cpuinfo_max_freq" ] && cat "$p/cpuinfo_max_freq" > "$p/scaling_max_freq" 2>/dev/null
  [ -r "$p/cpuinfo_min_freq" ] && cat "$p/cpuinfo_min_freq" > "$p/scaling_min_freq" 2>/dev/null
done

rm -f "$DATA/state" "$DATA"/*.tmp "$DATA"/.seen.* "$DATA/.rules.clean" 2>/dev/null
exit 0
