#!/system/bin/sh
# featherd-daemon.sh —— 守护进程控制（兼容旧调用：start/stop/restart/status/run）
MODID=feather_tune
BIN=/data/adb/modules/$MODID/bin

if [ -x "$BIN/feather" ]; then
  case "$1" in
    run) exec "$BIN/feather" run ;;
    *)   exec "$BIN/feather" "${1:-status}" ;;
  esac
fi

# 兜底：老 shell 版守护进程
exec sh "$BIN/shell/featherd-daemon.sh" "$@"
