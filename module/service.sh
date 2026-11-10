#!/system/bin/sh
# late_start service: 启动 C 守护进程（单实例 + 看护）
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

mkdir -p "$DATA" 2>/dev/null

if [ -x "$BIN/feather" ]; then
  # 看护也只允许一份：用 mkdir 的原子性当锁（toybox 不一定有 flock）
  LOCKDIR="$DATA/.sup.lock"
  if ! mkdir "$LOCKDIR" 2>/dev/null; then
    exit 0
  fi
  echo $$ > "$DATA/supervisor.pid"

  (
    while true; do
      "$BIN/feather" run >/dev/null 2>&1
      rc=$?
      if [ "$rc" = "3" ]; then
        echo "[$(date '+%m-%d %H:%M:%S')] supervisor: 已有实例在跑，退出看护" >> "$DATA/feather.log"
        break
      fi
      echo "[$(date '+%m-%d %H:%M:%S')] supervisor: 守护进程退出(rc=$rc)，2s 后重启" >> "$DATA/feather.log"
      sleep 2
    done
    rmdir "$LOCKDIR" 2>/dev/null
  ) >/dev/null 2>&1 </dev/null &
  exit 0
fi

[ -x "$BIN/shell/featherd-daemon.sh" ] && "$BIN/shell/featherd-daemon.sh" start
exit 0
