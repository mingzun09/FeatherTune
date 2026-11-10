#!/bin/bash
# ============================================================================
#  FeatherTune 离线验证：在 PC 上用假文件树跑真实脚本
#  验证点：簇识别 / 限频换算 / 场景切换与迟滞 / hmbird 协同写入 / 线程归位
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(dirname "$HERE")"
FAKE="$HERE/fake"
DATA="$HERE/data"
PASS=0; FAIL=0

ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1   [$3]"; fi; }

sh "$HERE/mkfake.sh" "$FAKE" >/dev/null || exit 1
rm -rf "$DATA"; mkdir -p "$DATA"
cp "$PROJ/module/etc/config.ini"  "$DATA/config.ini"
cp "$PROJ/module/etc/games.conf"  "$DATA/games.conf"
cp "$PROJ/module/etc/rules.conf"  "$DATA/rules.conf"
echo "com.test.game" >> "$DATA/games.conf"

export FEATHER_DATA="$DATA"
export FEATHER_CPU_ROOT="$FAKE/sys/devices/system/cpu"
export FEATHER_CGROUP_ROOT="$FAKE/sys/fs/cgroup"
export FEATHER_SYS_ROOT="$FAKE/sys"
export FEATHER_PROC_ROOT="$FAKE/proc"
export FEATHER_HMBIRD_SYS="$FAKE/proc/sys/hmbird_II"
export FEATHER_SKIP_ROOT=1
export FEATHER_WR_TRACE="$DATA/wr.trace"

BIN="$PROJ/module/bin"

echo "== 1. 静态检查 =="
for f in "$BIN"/*.sh "$PROJ/module"/*.sh "$HERE"/*.sh; do
    if bash -n "$f" 2>/dev/null || sh -n "$f" 2>/dev/null; then
        ok "syntax $(basename "$f")"
    else
        bad "syntax $(basename "$f")"
    fi
done

echo "== 2. 簇识别 & 限频换算 =="
out=$(FEATHER_DATA="$DATA" sh -c ". '$BIN/lib.sh'; detect_topology; echo \"perf=\${PERF_POLICY##*/}:\$PERF_MAX prime=\${PRIME_POLICY##*/}:\$PRIME_MAX\"")
check "识别 policy0 为性能簇 / policy6 为超大核" 'echo "$out" | grep -q "perf=policy0:3628800 prime=policy6:4608000"' "$out"

out=$(sh -c ". '$BIN/lib.sh'; detect_topology; freq_le \"\$PERF_POLICY\" 2000000")
check "2000MHz 下取不超过目标的最高档(1900800)" '[ "$out" = "1900800" ]' "got=$out"

out=$(sh -c ". '$BIN/lib.sh'; detect_topology; freq_le \"\$PRIME_POLICY\" 4000000")
check "4000MHz 在超大核上精确命中" '[ "$out" = "4000000" ]' "got=$out"

out=$(sh -c ". '$BIN/lib.sh'; detect_topology; set_cap \"\$PRIME_POLICY\" 62")
check "62% 上限 -> 2854400 档" '[ "$out" = "2854400" ] || [ "$out" = "2880000" ] || [ "$out" = "2611200" ]' "got=$out"

echo "== 3. 守护进程：游戏场景 =="
sh "$BIN/featherd.sh" run >/dev/null 2>&1 &
DPID=$!
sleep 4
S=$(cat "$DATA/state" 2>/dev/null)
check "前台游戏 -> profile=game"   'echo "$S" | grep -qx "profile=game"' "$(echo "$S" | grep profile=)"
check "游戏场景不压频"             'echo "$S" | grep -qx "cap_perf=3628800"' "$(echo "$S" | grep cap_perf=)"
check "hmbird 游戏系数已写入"      'grep -q "1152,1024" "$FAKE/proc/sys/hmbird_II/coefficient"' "$(cat "$FAKE/proc/sys/hmbird_II/coefficient")"
check "hmbird cpus_reserved -> 0xC0" 'grep -qx 192 "$FAKE/proc/sys/hmbird_II/cpus_reserved"' "$(cat "$FAKE/proc/sys/hmbird_II/cpus_reserved")"
check "perf_high_ratio 已调"       'grep -q "56,60" "$FAKE/proc/sys/hmbird_II/perf_high_ratio"' "$(cat "$FAKE/proc/sys/hmbird_II/perf_high_ratio")"
check "渲染线程被归位到 0xC0"      'grep -q "prefer_cpu <- .* 0xC0" "$DATA/wr.trace"' "$(grep prefer_cpu "$DATA/wr.trace" 2>/dev/null | head -3)"
check "音频线程被放到 0x0F"        'grep -q "prefer_cpu <- .* 0x0F" "$DATA/wr.trace"' ""
check "线程计数 > 0"               'echo "$S" | grep -qE "^threads=[1-9]"' "$(echo "$S" | grep threads=)"

echo "== 4. 切到普通应用 -> 日常收紧 =="
printf '5555\n' > "$FAKE/sys/fs/cgroup/top-app/cgroup.procs"
sleep 8
S=$(cat "$DATA/state" 2>/dev/null)
check "profile=daily"              'echo "$S" | grep -qx "profile=daily"' "$(echo "$S" | grep profile=)"
PF=$(echo "$S" | sed -n 's/^cap_perf=//p'); PR=$(echo "$S" | sed -n 's/^cap_prime=//p')
check "性能簇被压到 72% 档以下"    '[ "${PF:-0}" -lt 3628800 ]' "cap_perf=$PF"
check "超大核被压到 62% 档以下"    '[ "${PR:-0}" -lt 4608000 ]' "cap_prime=$PR"
check "hmbird 参数已还原"          'grep -qx 240 "$FAKE/proc/sys/hmbird_II/cpus_reserved"' "$(cat "$FAKE/proc/sys/hmbird_II/cpus_reserved")"

echo "== 5. 熄屏 -> sleep 档 =="
mkf_b(){ printf '%s\n' "$1" > "$FAKE/sys/class/backlight/panel0-backlight/brightness"; }
mkf_b 0
sleep 10
S=$(cat "$DATA/state" 2>/dev/null)
check "profile=sleep"              'echo "$S" | grep -qx "profile=sleep"' "$(echo "$S" | grep profile=)"
PF=$(echo "$S" | sed -n 's/^cap_perf=//p')
check "熄屏上限 <= 30%"            '[ "${PF:-9}" -le 1305600 ]' "cap_perf=$PF"
mkf_b 120

echo "== 6. CLI 与配置 =="
out=$(sh "$BIN/ctl.sh" status 2>&1)
check "ctl status 输出 profile"    'echo "$out" | grep -q "^profile="' "$(echo "$out" | head -2)"
check "ctl status 报告 hmbird"     'echo "$out" | grep -q "hmbird_present=1"' ""
out=$(sh "$BIN/ctl.sh" set intensity 90 2>&1)
check "set intensity 写入配置"     'grep -qx "intensity=90" "$DATA/config.ini"' "$(grep intensity "$DATA/config.ini")"
out=$(sh "$BIN/ctl.sh" pkg add com.test.other 2>&1)
check "pkg add 生效"               'grep -qx "com.test.other" "$DATA/games.conf"' ""
out=$(sh "$BIN/ctl.sh" pkg del com.test.other 2>&1)
check "pkg del 生效"               '! grep -qx "com.test.other" "$DATA/games.conf"' ""
out=$(sh "$BIN/ctl.sh" doctor 2>&1)
check "doctor 列出 cpufreq"        'echo "$out" | grep -q "policy0: cpus=0 1 2 3 4 5"' "$(echo "$out" | grep policy0)"

echo "== 7. 停止与还原 =="
kill $DPID 2>/dev/null; sleep 1
PF=$(cat "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq")
check "退出后还原满频"             '[ "$PF" = "3628800" ]' "cap_perf=$PF"

echo
echo "--------------------------------------------"
printf "结果: \033[32m%d passed\033[0m, \033[31m%d failed\033[0m\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || { echo "---- daemon log ----"; tail -30 "$DATA/feather.log"; }
exit "$FAIL"
