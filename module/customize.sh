#!/system/bin/sh
# FeatherTune installer (KernelSU / SukiSU / Magisk / APatch)
#
# 安装期三条铁律（v1.4.1 修 SukiSU 刷不进去的教训）：
#   1. 本文件必须是 LF 换行。CRLF 会让 Android 的 sh 在 `then\r` 处直接语法错误，
#      管理器只显示「安装失败」，没有任何别的信息。build.py 现在会拒绝打包 CRLF。
#   2. 管理器的解压结果才是基准。这里只做「补写权限 + 配置迁移」，
#      不再先 rm -rf bin 再重新解压 —— 安装环境没有 unzip 时那等于把模块删空。
#   3. 任何一步失败都不能让安装整体失败（配置迁移失败 = 用默认值，不是刷机失败）。
SKIPUNZIP=0

MODID=feather_tune
DATA=/data/adb/feather

# 管理器没提供这些函数时给个可用的替身（不同管理器/版本差异很大）
command -v ui_print >/dev/null 2>&1 || ui_print() { echo "$1"; }
command -v abort >/dev/null 2>&1 || abort() { ui_print "! $1"; exit 1; }

ui_print "- 羽量调校 FeatherTune v1.4.7"
ui_print "- SoC: $(getprop ro.board.platform 2>/dev/null) / $(getprop ro.product.model 2>/dev/null)"
ui_print "- Android $(getprop ro.build.version.release 2>/dev/null) (SDK $(getprop ro.build.version.sdk 2>/dev/null))"

if [ "$(getprop ro.product.cpu.abi)" != "arm64-v8a" ]; then
  ui_print "! 当前 ABI 非 arm64-v8a，将回落到 shell 实现"
fi

ENV_ROOT="unknown"
[ -d /data/adb/ksu ] && ENV_ROOT="KernelSU/SukiSU"
[ -d /data/adb/ap ] && ENV_ROOT="APatch"
[ -d /data/adb/magisk ] && ENV_ROOT="Magisk"
META="no"
[ -d /data/adb/modules/meta-overlayfs ] && META="meta-overlayfs"
[ -f /data/adb/modules/httoolsST ] && META="httoolsST"
ui_print "- ROOT 管理器: $ENV_ROOT / 元模块: $META"

[ -n "$MODPATH" ] || abort "MODPATH 为空，安装环境异常"

# 管理器已经按 SKIPUNZIP=0 解压过一遍。只在环境里确实有 unzip 时补一次，
# 而且绝不先删目录：没有 unzip 的安装环境（部分 SukiSU 版本）删了就补不回来。
if [ -n "${ZIPFILE:-}" ] && [ -f "$ZIPFILE" ] && command -v unzip >/dev/null 2>&1; then
  unzip -o "$ZIPFILE" 'bin/*' 'etc/*' 'webroot/*' -d "$MODPATH" >/dev/null 2>&1
fi

# 权限：set_perm_recursive 在个别管理器里不存在，逐层兜底
if command -v set_perm_recursive >/dev/null 2>&1; then
  set_perm_recursive "$MODPATH" 0 0 0755 0644
  [ -d "$MODPATH/bin" ] && set_perm_recursive "$MODPATH/bin" 0 0 0755 0755
  [ -d "$MODPATH/webroot" ] && set_perm_recursive "$MODPATH/webroot" 0 0 0755 0644
else
  find "$MODPATH" -type d -exec chmod 0755 {} + 2>/dev/null
  find "$MODPATH" -type f -exec chmod 0644 {} + 2>/dev/null
  find "$MODPATH" -name '*.sh' -exec chmod 0755 {} + 2>/dev/null
  [ -d "$MODPATH/bin" ] && chmod 0755 "$MODPATH/bin"/* 2>/dev/null
fi
[ -f "$MODPATH/bin/feather" ] && chmod 0755 "$MODPATH/bin/feather" 2>/dev/null
chmod 0755 "$MODPATH"/*.sh 2>/dev/null

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
    cp -f "$MODPATH/etc/$f" "$DATA/$f" 2>/dev/null && ui_print "- 初始化配置: $f"
  fi
done
[ -f "$DATA/feather.log" ] || touch "$DATA/feather.log" 2>/dev/null
chmod 0755 "$DATA" 2>/dev/null

# --- 配置迁移（每一步都允许失败，失败就用默认值）---
if [ -f "$DATA/config.ini" ]; then
  # v1.3.0：老配置没有充电加速这几项，补上（缺省即开启）
  if ! grep -q '^accel_ctrl=' "$DATA/config.ini" 2>/dev/null; then
    {
      echo ""
      echo "# --- 充电加速（v1.3.0 自动补入）---"
      echo "accel_ctrl=1"
      echo "accel_tbat=0"
      echo "accel_stop_temp=45"
      echo "accel_resume_temp=42"
      echo "accel_gentle=0"
    } >> "$DATA/config.ini" 2>/dev/null && ui_print "- 配置已补入充电加速项"
  fi
  # v1.3.2：老配置里的 accel_tbat=360 落在 warm/表外段（写它反而更慢）
  AT=$(sed -n 's/^accel_tbat=//p' "$DATA/config.ini" 2>/dev/null | tail -1)
  if [ -n "$AT" ] && [ "$AT" -gt 340 ] 2>/dev/null; then
    sed -i 's/^accel_tbat=.*/accel_tbat=0/' "$DATA/config.ini" 2>/dev/null
    ui_print "- accel_tbat=$AT 已改回自动档（>340 落在 warm/表外段）"
  fi
  # v1.4.1：v1.2.6 的默认曲线末尾多了一个 ",43:0"，按「取最后一个 <= 温度的点」
  # 计算时它会把 43℃ 以上全部压成 0%（等于停充）。删掉，并把配置里的旧值纠正。
  case "$(sed -n 's/^charge_curve=//p' "$DATA/config.ini" 2>/dev/null | tail -1)" in
    *,43:0) sed -i 's/^charge_curve=\(.*\),43:0[[:space:]]*$/charge_curve=\1/' "$DATA/config.ini" 2>/dev/null
            ui_print "- charge_curve 末尾的 43:0 已移除（会把 43℃ 以上压成 0%）" ;;
  esac
fi

# --- 二进制自检 ---
if [ -x "$MODPATH/bin/feather" ]; then
  V=$("$MODPATH/bin/feather" version 2>/dev/null)
  if [ -n "$V" ]; then
    ui_print "- 守护进程可执行: $V"
  else
    ui_print "! 二进制无法运行，将自动使用 shell 实现"
  fi
else
  ui_print "! 没有可执行的 bin/feather（管理器解压异常），将使用 shell 实现"
fi

ui_print "- 安装完成，重启后生效（也可在 WebUI 点“重启守护进程”）"
ui_print "- WebUI: 模块卡片打开；若界面连不上，用浏览器访问 http://127.0.0.1:7891/"
exit 0
