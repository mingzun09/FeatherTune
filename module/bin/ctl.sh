#!/system/bin/sh
# ============================================================================
#  ctl.sh —— WebUI / 命令行的统一入口（薄壳）
#  优先走 C 守护进程（socket 直连，~1ms）；二进制跑不起来时才回落 shell 版。
#  之所以保留 ctl.sh 这个名字：WebUI 与旧脚本都按这个路径调用。
# ============================================================================
MODID=feather_tune
BIN=/data/adb/modules/$MODID/bin

if [ -x "$BIN/feather" ]; then
  # 用 exec 直接替换进程，省掉一层 shell
  exec "$BIN/feather" "$@"
fi

if [ -x "$BIN/shell/ctl.sh" ]; then
  echo "chan=shell-legacy" >&2
  exec sh "$BIN/shell/ctl.sh" "$@"
fi

echo "err=no_binary"
exit 1
