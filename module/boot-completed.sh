#!/system/bin/sh
# boot completed: 二次确认守护进程存活
MODID=feather_tune
DATA=/data/adb/feather
BIN=/data/adb/modules/$MODID/bin

sleep 8
PID=$(cat "$DATA/featherd.pid" 2>/dev/null)
if [ -z "$PID" ] || ! kill -0 "$PID" 2>/dev/null; then
  if [ -x "$BIN/feather" ]; then
    "$BIN/feather" start
  else
    [ -x "$BIN/shell/featherd.sh" ] && "$BIN/shell/featherd.sh" start
  fi
fi
exit 0
