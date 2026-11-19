#!/system/bin/sh
# boot completed: 二次确认守护进程存活（含残留锁自愈）
MODID=feather_tune
DATA=/data/adb/feather
BIN=/data/adb/modules/$MODID/bin

sleep 8

# 看护锁里的 pid 已经不在 → 上次是被强杀/崩溃，清掉锁再拉起
LOCKDIR="$DATA/.sup.lock"
if [ -d "$LOCKDIR" ]; then
  oldpid=$(cat "$LOCKDIR/pid" 2>/dev/null)
  if [ -z "$oldpid" ] || ! kill -0 "$oldpid" 2>/dev/null; then
    rm -rf "$LOCKDIR"
  fi
fi

PID=$(cat "$DATA/featherd-daemon.pid" 2>/dev/null)
if [ -z "$PID" ] || ! kill -0 "$PID" 2>/dev/null; then
  if [ -x "$BIN/feather" ]; then
    "$BIN/feather" start
  else
    [ -x "$BIN/shell/featherd-daemon.sh" ] && "$BIN/shell/featherd-daemon.sh" start
  fi
fi
exit 0
