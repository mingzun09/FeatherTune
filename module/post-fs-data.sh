#!/system/bin/sh
# post-fs-data: 只做准备动作，任何写入失败都必须放行（防止卡开机）
MODID=feather_tune
DATA=/data/adb/feather

mkdir -p "$DATA" 2>/dev/null
[ -f "$DATA/config.ini" ] || cp -f "/data/adb/modules/$MODID/etc/config.ini" "$DATA/config.ini" 2>/dev/null
[ -f "$DATA/games.conf" ] || cp -f "/data/adb/modules/$MODID/etc/games.conf" "$DATA/games.conf" 2>/dev/null
[ -f "$DATA/rules.conf" ] || cp -f "/data/adb/modules/$MODID/etc/rules.conf" "$DATA/rules.conf" 2>/dev/null
rm -f "$DATA/stop" "$DATA/featherd-daemon.pid" 2>/dev/null
# 开机时不可能有守护进程在跑：清掉上次崩溃/被杀留下的看护锁，
# 否则 service.sh 会误判「已有实例」而永远不启动（只能重装才恢复）。
rm -rf "$DATA/.sup.lock" 2>/dev/null
exit 0
