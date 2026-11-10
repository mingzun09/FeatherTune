#!/system/bin/sh
# 模块卡片的“操作”按钮：自动 / 省电 / 游戏 轮换
MODID=feather_tune
DATA=/data/adb/feather
BIN=/data/adb/modules/$MODID/bin

cur=$(sed -n 's/^mode=//p' "$DATA/state" 2>/dev/null | head -1)
case "$cur" in
  auto)    next=battery ;;
  battery) next=game ;;
  game)    next=auto ;;
  *)       next=auto ;;
esac

"$BIN/ctl.sh" mode "$next" >/dev/null 2>&1
msg="羽量调校: 模式 -> $next"
command -v ui_print >/dev/null 2>&1 && ui_print "$msg" || echo "$msg"
exit 0
