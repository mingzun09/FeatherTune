#!/system/bin/sh
# late_start service: 启动守护进程（单实例 + 看护 + 死锁自愈）
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
  LOCKDIR="$DATA/.sup.lock"
  # 看护锁：目录里的 pid 还活着才算「已有实例」。
  # 以前只用 mkdir 成功/失败判断，进程被游戏/系统杀掉后锁目录会留下来，
  # 下次开机 service.sh 直接 exit 0 —— 表现就是「软重启后再也起不来，只能重装」。
  if [ -d "$LOCKDIR" ]; then
    oldpid=$(cat "$LOCKDIR/pid" 2>/dev/null)
    if [ -n "$oldpid" ] && kill -0 "$oldpid" 2>/dev/null; then
      echo "[$(date '+%m-%d %H:%M:%S')] supervisor: 已有看护进程 $oldpid，跳过" >> "$DATA/feather.log"
      exit 0
    fi
    echo "[$(date '+%m-%d %H:%M:%S')] supervisor: 清理残留锁（pid=${oldpid:-?} 已不在）" >> "$DATA/feather.log"
    rm -rf "$LOCKDIR"
  fi
  if ! mkdir "$LOCKDIR" 2>/dev/null; then
    exit 0
  fi
  echo $$ > "$LOCKDIR/pid"
  echo $$ > "$DATA/supervisor.pid"

  # 看护脚本写到数据目录再 setsid 拉起：service.sh 退出后它仍归 init 收养，
  # 不随管理器/游戏进程组一起被杀。
  cat > "$DATA/supervisor.sh" <<EOF
#!/system/bin/sh
while true; do
  "$BIN/feather" run >/dev/null 2>&1
  rc=\$?
  if [ "\$rc" = "3" ]; then
    echo "[\$(date '+%m-%d %H:%M:%S')] supervisor: 已有实例在跑，退出看护" >> "$DATA/feather.log"
    break
  fi
  echo "[\$(date '+%m-%d %H:%M:%S')] supervisor: 守护进程退出(rc=\$rc)，2s 后重启" >> "$DATA/feather.log"
  sleep 2
done
rm -rf "$LOCKDIR"
EOF
  chmod 0755 "$DATA/supervisor.sh"
  if command -v setsid >/dev/null 2>&1; then
    setsid "$DATA/supervisor.sh" >/dev/null 2>&1 </dev/null &
  else
    "$DATA/supervisor.sh" >/dev/null 2>&1 </dev/null &
  fi
  exit 0
fi

[ -x "$BIN/shell/featherd-daemon.sh" ] && "$BIN/shell/featherd-daemon.sh" start
exit 0
