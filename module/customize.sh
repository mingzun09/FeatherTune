#!/system/bin/sh
# FeatherTune installer (KernelSU / Magisk / APatch)
SKIPUNZIP=0

MODID=feather_tune
DATA=/data/adb/feather

ui_print "- 羽量调校 FeatherTune v1.2.1（C 守护进程版）"
ui_print "- SoC: $(getprop ro.board.platform 2>/dev/null) / $(getprop ro.product.model 2>/dev/null)"
ui_print "- Android $(getprop ro.build.version.release 2>/dev/null) (SDK $(getprop ro.build.version.sdk 2>/dev/null))"

if [ "$(getprop ro.product.cpu.abi)" != "arm64-v8a" ]; then
  ui_print "! 当前 ABI 非 arm64-v8a，将回落到 shell 实现"
fi

ENV_ROOT="unknown"
[ -d /data/adb/ksu ] && ENV_ROOT="KernelSU"
[ -d /data/adb/ap ] && ENV_ROOT="APatch"
[ -d /data/adb/magisk ] && ENV_ROOT="Magisk"
META="no"
[ -d /data/adb/modules/meta-overlayfs ] && META="meta-overlayfs"
[ -f /data/adb/modules/httoolsST ] && META="httoolsST"
ui_print "- ROOT 管理器: $ENV_ROOT / 元模块: $META"

rm -rf "$MODPATH/bin" 2>/dev/null
unzip -o "$ZIPFILE" 'bin/*' 'etc/*' 'webroot/*' -d "$MODPATH" >/dev/null 2>&1

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm_recursive "$MODPATH/bin" 0 0 0755 0755
[ -d "$MODPATH/webroot" ] && set_perm_recursive "$MODPATH/webroot" 0 0 0755 0644

# --- 停掉旧实例（v1.0 shell 版 / 旧二进制）---
if [ -f "$DATA/featherd-daemon.pid" ]; then
  kill "$(cat "$DATA/featherd-daemon.pid" 2>/dev/null)" 2>/dev/null
  rm -f "$DATA/featherd-daemon.pid"
fi
rm -f "$DATA/control.sock" "$DATA/stop" 2>/dev/null

# --- 用户数据目录（升级不覆盖配置）---
mkdir -p "$DATA" 2>/dev/null
for f in config.ini games.conf rules.conf apps.conf labels.conf; do
  if [ ! -f "$DATA/$f" ] && [ -f "$MODPATH/etc/$f" ]; then
    cp -f "$MODPATH/etc/$f" "$DATA/$f"
    ui_print "- 初始化配置: $f"
  fi
done
touch "$DATA/feather.log"
chmod 0755 "$DATA" 2>/dev/null

# --- 二进制自检 ---
if [ -x "$MODPATH/bin/feather" ]; then
  V=$("$MODPATH/bin/feather" version 2>/dev/null)
  if [ -n "$V" ]; then
    ui_print "- 守护进程可执行: $V"
  else
    ui_print "! 二进制无法运行，将自动使用 shell 实现"
  fi
fi

ui_print "- 安装完成，重启后生效（也可在 WebUI 点“重启守护进程”）"
ui_print "- WebUI: 模块卡片打开；若界面连不上，用浏览器访问 http://127.0.0.1:7891/"
