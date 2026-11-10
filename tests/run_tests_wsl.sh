#!/bin/bash
# ============================================================================
#  FeatherTune v1.1 验证：在 Linux(WSL) 里跑真二进制 + 假文件树
#    sh tests/run_tests_wsl.sh
#  覆盖：拓扑识别 / 限频 / 场景切换迟滞 / hmbird 协同 / 线程归位 /
#        UNIX socket 命令 / 127.0.0.1 HTTP 桥 / 停止还原
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(dirname "$HERE")"
FAKE="$HERE/fake"
DATA="$HERE/data"
BIN="${FEATHER_TEST_BIN:-$HERE/feather_x86}"
PASS=0; FAIL=0

ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s   [%s]\n' "$1" "${2:-}"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }

echo "== 0. 二进制 =="
if [ -n "${FEATHER_TEST_BIN:-}" ]; then
    ok "外部二进制 $(basename "$BIN") ($(stat -c %s "$BIN") bytes)"
else
    gcc -O2 -Wno-format-truncation -Wno-unused-result -o "$BIN" "$PROJ/src/feather.c" || { echo "build failed"; exit 1; }
    ok "gcc 编译通过 ($(stat -c %s "$BIN") bytes)"
fi

sh "$HERE/mkfake.sh" "$FAKE" >/dev/null || exit 1
rm -rf "$DATA"; mkdir -p "$DATA"
cp "$PROJ/module/etc/config.ini" "$PROJ/module/etc/games.conf" "$PROJ/module/etc/rules.conf" "$DATA/"
sed -i 's/^log_level=.*/log_level=2/' "$DATA/config.ini"
echo "com.test.game" >> "$DATA/games.conf"

export FEATHER_DATA="$DATA"
export FEATHER_CPU_ROOT="$FAKE/sys/devices/system/cpu"
export FEATHER_CGROUP_ROOT="$FAKE/sys/fs/cgroup"
export FEATHER_SYS_ROOT="$FAKE/sys"
export FEATHER_PROC_ROOT="$FAKE/proc"
export FEATHER_HMBIRD_SYS="$FAKE/proc/sys/hmbird_II"
export FEATHER_WR_TRACE="$DATA/wr.trace"

# 假 dumpsys：验证“焦点窗口优先”这条路径（真机上 dumpsys 是唯一准确的来源）
FAKEBIN="$HERE/fakebin"; rm -rf "$FAKEBIN"; mkdir -p "$FAKEBIN"
cat > "$FAKEBIN/dumpsys" <<'EOS'
#!/bin/sh
case "$*" in
  *window*)   [ -s "$FAKE_WIN_FG" ] && echo "  mCurrentFocus=Window{1a2b3c u0 $(cat "$FAKE_WIN_FG")/com.x.MainActivity}" ;;
  *activity*) [ -s "$FAKE_WIN_FG" ] && echo "    topResumedActivity=ActivityRecord{9f8e u0 $(cat "$FAKE_WIN_FG")/.MainActivity t42}" ;;
esac
exit 0
EOS
chmod +x "$FAKEBIN/dumpsys"
export FAKE_WIN_FG="$DATA/window_fg"
export PATH="$FAKEBIN:$PATH"
export FEATHER_HTTP_PORT=17891          # 与线上 7891 隔离，避免抢端口

echo "== 1. 拓扑 / 限频纯函数 =="
out=$("$BIN" status 2>&1)
check "识别 policy0=性能簇 policy6=超大核" 'echo "$out" | grep -q "prime_policy=policy6:4608000"' "$(echo "$out" | grep prime_policy)"
check "守护进程未启动时也能出状态" 'echo "$out" | grep -q "^runner=stopped"' "$(echo "$out" | grep runner)"

echo "== 2. 守护进程：游戏场景 =="
"$BIN" run & DPID=$!
sleep 3
S=$(cat "$DATA/state" 2>/dev/null)
check "前台游戏 -> profile=game"  'echo "$S" | grep -qx "profile=game"' "$(echo "$S" | grep profile=)"
check "前台包名绕过 systemui"      'echo "$S" | grep -qx "fg=com.test.game"' "$(echo "$S" | grep '^fg=')"
check "游戏不压频"                'echo "$S" | grep -qx "cap_perf=3628800"' "$(echo "$S" | grep cap_perf=)"
check "frame_per_sec 对齐刷新率"  'grep -qx 165 "$FAKE/proc/sys/hmbird_II/frame_per_sec"' "$(cat "$FAKE/proc/sys/hmbird_II/frame_per_sec")"
check "主线程钉 CPU7(prefer_cpu 0x80)"   'grep -q "prefer_cpu <- .* 0x80" "$DATA/wr.trace"' "$(grep prefer_cpu "$DATA/wr.trace" 2>/dev/null | head -2)"
check "图形设备钉 CPU6(prefer_cpu 0x40)" 'grep -q "prefer_cpu <- .* 0x40" "$DATA/wr.trace"' "$(grep prefer_cpu "$DATA/wr.trace" 2>/dev/null | tail -2)"
check "音频走 prefer_cluster(性能簇)"     'grep -q "prefer_cluster <- .* 0x1" "$DATA/wr.trace"' ""
check "perf 簇抬底频(>=1.5G)"     '[ "$(cat "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq")" -ge 1305600 ]' "$(cat "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq")"
check "后台 cpuset 收窄 0-3"      'grep -qx "0-3" "$FAKE/sys/fs/cgroup/background/cpuset.cpus"' "$(cat "$FAKE/sys/fs/cgroup/background/cpuset.cpus")"
check "默认不碰 hmbird 系数"      'grep -qx "1024,1024" "$FAKE/proc/sys/hmbird_II/coefficient"' "$(cat "$FAKE/proc/sys/hmbird_II/coefficient")"
check "线程计数 > 0"              'echo "$S" | grep -qE "^threads=[1-9]"' "$(echo "$S" | grep threads=)"

echo "== 2.5 规则加载 =="
out=$("$BIN" doctor 2>&1)
check "规则条数 >= 9"              'echo "$out" | grep -qE "^rules=([9]|[1-9][0-9])"' "$(echo "$out" | grep '^rules=')"
check "Job.worker 规则已加载"      'echo "$out" | grep -q "Job\\\\.worker"' "$(echo "$out" | grep -c 'rule')"

echo "== 2.7 焦点窗口优先（top-app 里游戏与别的 App 并存，窗口是别的 App）=="
printf '4242\n5555\n' > "$FAKE/sys/fs/cgroup/top-app/cgroup.procs"
echo "com.test.app" > "$FAKE_WIN_FG"
sleep 5
out=$("$BIN" status 2>&1)
check "窗口来源生效"  'echo "$out" | grep -q "^fg=com.test.app"' "$(echo "$out" | grep '^fg=')"
check "因此不判为游戏" 'echo "$out" | grep -qx "profile=daily"'  "$(echo "$out" | grep '^profile=')"
printf '4242\n' > "$FAKE/sys/fs/cgroup/top-app/cgroup.procs"
echo "com.test.game" > "$FAKE_WIN_FG"
sleep 4
out=$("$BIN" status 2>&1)
check "窗口切回游戏即恢复" 'echo "$out" | grep -qx "profile=game"' "$(echo "$out" | grep '^profile=')"

echo "== 3. UNIX socket 命令 =="
out=$("$BIN" status 2>&1)
check "socket status 有 profile"  'echo "$out" | grep -q "^profile=game"' "$(echo "$out" | head -1)"
out=$("$BIN" mode battery 2>&1)
check "socket mode 立即生效"      'echo "$out" | grep -q "applied=battery"' "$out"
sleep 1.2
S=$(cat "$DATA/state" 2>/dev/null)
echo "DBGLOG:"; tail -8 "$DATA/feather.log" 2>/dev/null; echo "DBGSTATE:"; cat "$DATA/state" | head -4
check "状态文件同步为 battery"    'echo "$S" | grep -qx "profile=battery"' "$(echo "$S" | grep profile=)"
check "battery 上限已压"          'echo "$S" | grep -qE "^cap_perf=1[0-9]{6}$"' "$(echo "$S" | grep cap_perf=)"
out=$("$BIN" set intensity 80 2>&1)
check "set intensity 写盘"        'grep -qx "intensity=80" "$DATA/config.ini"' "$(grep intensity "$DATA/config.ini")"
"$BIN" mode auto >/dev/null 2>&1

echo "== 4. HTTP 桥（WebUI 兜底通道）=="
code=$(curl -s -o /tmp/h1 -w '%{http_code}' http://127.0.0.1:17891/status)
check "GET /status 200"           '[ "$code" = "200" ]' "code=$code"
check "返回 key=value"            'grep -q "^profile=" /tmp/h1' "$(head -2 /tmp/h1)"
check "CORS 头存在"               'curl -s -D- -o /dev/null http://127.0.0.1:17891/status | grep -qi "access-control-allow-origin: \*"' ""
code=$(curl -s -o /tmp/h2 -w '%{http_code}' "http://127.0.0.1:17891/mode?m=game")
check "GET /mode?m=game 生效"     '[ "$code" = "200" ] && grep -q "applied=game" /tmp/h2' "$(cat /tmp/h2 2>/dev/null)"
code=$(curl -s -o /tmp/h3 -w '%{http_code}' "http://127.0.0.1:17891/games/add?p=com.http.test")
check "games/add 落盘"            'grep -qx "com.http.test" "$DATA/games.conf"' "$(grep -c http.test "$DATA/games.conf")"
code=$(curl -s -o /tmp/h4 -w '%{http_code}' "http://127.0.0.1:17891/games/del?p=com.http.test")
check "games/del 落盘"            '! grep -qx "com.http.test" "$DATA/games.conf"' ""
"$BIN" mode auto >/dev/null 2>&1

echo "== 4.5 可选：手动开 hmbird_tune 才改系数 =="
"$BIN" set hmbird_tune 1 >/dev/null 2>&1
"$BIN" mode game >/dev/null 2>&1
sleep 1.2
check "tune=1 时写入系数"         'grep -q "1152,1024" "$FAKE/proc/sys/hmbird_II/coefficient"' "$(cat "$FAKE/proc/sys/hmbird_II/coefficient")"
check "tune=1 时写 cpus_reserved" 'grep -qx 192 "$FAKE/proc/sys/hmbird_II/cpus_reserved"' "$(cat "$FAKE/proc/sys/hmbird_II/cpus_reserved")"
"$BIN" set hmbird_tune 0 >/dev/null 2>&1
"$BIN" mode auto >/dev/null 2>&1

echo "== 5. 切普通应用 -> 日常收紧 =="
echo "com.test.app" > "$FAKE_WIN_FG"
printf '5555\n' > "$FAKE/sys/fs/cgroup/top-app/cgroup.procs"
sleep 6
S=$(cat "$DATA/state" 2>/dev/null)
check "profile=daily"             'echo "$S" | grep -qx "profile=daily"' "$(echo "$S" | grep profile=)"
PF=$(echo "$S" | sed -n 's/^cap_perf=//p'); PR=$(echo "$S" | sed -n 's/^cap_prime=//p')
check "性能簇已压 < max"          '[ "${PF:-0}" -lt 3628800 ] && [ "${PF:-0}" -gt 0 ]' "cap_perf=$PF"
check "超大核已压 < max"          '[ "${PR:-0}" -lt 4608000 ] && [ "${PR:-0}" -gt 0 ]' "cap_prime=$PR"
check "退出游戏后 min 频还原"    'grep -qx 300000 "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq"' "$(cat "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq")"
check "后台 cpuset 还原"          'grep -qx "0-7" "$FAKE/sys/fs/cgroup/background/cpuset.cpus"' "$(cat "$FAKE/sys/fs/cgroup/background/cpuset.cpus")"

echo "== 6. 熄屏 =="
printf '0\n' > "$FAKE/sys/class/backlight/panel0-backlight/brightness"
sleep 5
S=$(cat "$DATA/state" 2>/dev/null)
check "profile=sleep"             'echo "$S" | grep -qx "profile=sleep"' "$(echo "$S" | grep profile=)"
PF=$(echo "$S" | sed -n 's/^cap_perf=//p')
check "熄屏上限 <= 30%"           '[ "${PF:-9}" -le 1305600 ]' "cap_perf=$PF"
printf '120\n' > "$FAKE/sys/class/backlight/panel0-backlight/brightness"

echo "== 7. 停止与还原 =="
echo "  [debug] runner=$("$BIN" status 2>/dev/null | sed -n 's/^runner=//p') pidfile=$(cat "$DATA/featherd.pid" 2>/dev/null)"
"$BIN" stop >/dev/null 2>&1
sleep 1
kill $DPID 2>/dev/null
PF=$(cat "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq")
check "退出后恢复满频"            '[ "$PF" = "3628800" ]' "cap_perf=$PF"

echo
echo "--------------------------------------------"
printf "结果: \033[32m%d passed\033[0m, \033[31m%d failed\033[0m\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || { echo "---- apply 轨迹 ----"; grep -E "apply:|extras|restore|daemon" "$DATA/feather.log" | tail -20; echo "---- log ----"; tail -12 "$DATA/feather.log"; }
[ "$FAIL" -eq 0 ]
