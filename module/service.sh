#!/system/bin/sh
# late_start service: 启动 C 守护进程
MODID=feather_tune
DATA=/data/adb/feather
BIN=/data/adb/modules/$MODID/bin

# 等 cpufreq 就绪，最多 30s
i=0
while [ $i -lt 30 ]; do
  [ -d /sys/devices/system/cpu/cpufreq/policy0 ] && break
  sleep 1
  i=$((i + 1))
done

# 兜底：老版本残留的 shell 守护进程先清掉
if [ -f "$DATA/featherd-daemon.pid" ] && [ ! -x "$BIN/feather" ]; then
  : # 交给 shell 版自己判断
fi

if [ -x "$BIN/feather" ]; then
  "$BIN/feather" start
  exit 0
fi

[ -x "$BIN/shell/featherd-daemon.sh" ] && "$BIN/shell/featherd-daemon.sh" start
exit 0
