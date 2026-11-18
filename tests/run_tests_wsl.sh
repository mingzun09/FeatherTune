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
# 等 state 里出现某一行（qemu 下启动慢，固定 sleep 会假失败）
wait_state(){
    local i=0
    while [ "$i" -lt "${2:-15}" ]; do
        grep -qx "$1" "$DATA/state" 2>/dev/null && return 0
        sleep 1; i=$((i + 1))
    done
    return 1
}

echo "== 0. 二进制 =="
# 默认编译 Rust 版（这才是 module/bin/feather 里跑的东西）。
# C 版（src/feather.c）是 v1.3.4 的对照实现，只在没有 rustc 时才用来跑非图标用例。
RUST=0
if [ -n "${FEATHER_TEST_BIN:-}" ]; then
    RUST=1
    ok "外部二进制 $(basename "$BIN") ($(stat -c %s "$BIN") bytes)"
elif RUSTC=$(command -v rustc 2>/dev/null || echo "$HOME/.cargo/bin/rustc"); [ -x "$RUSTC" ]; then
    "$RUSTC" --edition 2021 -O "$PROJ/rust/main.rs" -o "$BIN" || { echo "rustc build failed"; exit 1; }
    RUST=1
    ok "rustc 编译通过 ($(stat -c %s "$BIN") bytes)"
else
    gcc -O2 -Wno-format-truncation -Wno-unused-result -o "$BIN" "$PROJ/src/feather.c" || { echo "build failed"; exit 1; }
    ok "gcc 编译通过（C 对照版，跳过图标用例）($(stat -c %s "$BIN") bytes)"
fi

sh "$HERE/mkfake.sh" "$FAKE" >/dev/null || exit 1
rm -rf "$DATA"; mkdir -p "$DATA"
cp "$PROJ/module/etc/config.ini" "$PROJ/module/etc/games.conf" "$PROJ/module/etc/rules.conf" "$DATA/"
sed -i 's/^log_level=.*/log_level=2/' "$DATA/config.ini"
# 测试主体跑 aggressive（覆盖地板/绑核/uclamp 这些机制），official 另起一节验证
grep -q '^game_mode=' "$DATA/config.ini" && sed -i 's/^game_mode=.*/game_mode=aggressive/' "$DATA/config.ini" || echo 'game_mode=aggressive' >> "$DATA/config.ini"
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
# 假 pm：图标链路要用（path）与未来的应用列表测试（list）都覆盖
cat > "$FAKEBIN/pm" <<EOS
#!/bin/sh
case "\$1" in
  list) echo "package:com.fake.webp"; echo "package:com.fake.fallback" ;;
  path)
    case "\$2" in
      com.fake.webp)     echo "package:$DATA/apps/webp.apk" ;;
      com.fake.fallback) echo "package:$DATA/apps/fallback.apk" ;;
      com.fake.obf)      echo "package:$DATA/apps/obf.apk" ;;
      com.fake.adaptive) echo "package:$DATA/apps/adaptive.apk" ;;
    esac ;;
esac
exit 0
EOS
chmod +x "$FAKEBIN/pm"
export FAKE_WIN_FG="$DATA/window_fg"
export PATH="$FAKEBIN:$PATH"
export FEATHER_HTTP_PORT=17891          # 与线上 7891 隔离，避免抢端口

echo "== 1. 拓扑 / 限频纯函数 =="
out=$("$BIN" status 2>&1)
check "识别 policy0=性能簇 policy6=超大核" 'echo "$out" | grep -q "prime_policy=policy6:4608000"' "$(echo "$out" | grep prime_policy)"
check "守护进程未启动时也能出状态" 'echo "$out" | grep -q "^runner=stopped"' "$(echo "$out" | grep runner)"

echo "== 2. 守护进程：游戏场景 =="
"$BIN" run & DPID=$!
wait_state "profile=game" 20
S=$(cat "$DATA/state" 2>/dev/null)
check "前台游戏 -> profile=game"  'echo "$S" | grep -qx "profile=game"' "$(echo "$S" | grep profile=)"
check "前台包名绕过 systemui"      'echo "$S" | grep -qx "fg=com.test.game"' "$(echo "$S" | grep '^fg=')"
check "游戏不压频"                'echo "$S" | grep -qx "cap_perf=3628800"' "$(echo "$S" | grep cap_perf=)"
check "frame_per_sec 对齐刷新率"  'grep -qx 165 "$FAKE/proc/sys/hmbird_II/frame_per_sec"' "$(cat "$FAKE/proc/sys/hmbird_II/frame_per_sec")"
check "主线程钉 CPU7(prefer_cpu 0x80)"   'grep -q "prefer_cpu <- .* 0x80" "$DATA/wr.trace"' "$(grep prefer_cpu "$DATA/wr.trace" 2>/dev/null | head -2)"
check "图形设备钉 CPU6(prefer_cpu 0x40)" 'grep -q "prefer_cpu <- .* 0x40" "$DATA/wr.trace"' "$(grep prefer_cpu "$DATA/wr.trace" 2>/dev/null | tail -2)"
check "串行链标记 preempt"        'grep -q "prefer_preempt <- [0-9]* 1" "$DATA/wr.trace"' "$(grep prefer_preempt "$DATA/wr.trace" 2>/dev/null | head -2)"
check "并行 Job 压 uclamp"        'grep -q "set_uclamp <- [0-9]* 640" "$DATA/wr.trace"' "$(grep set_uclamp "$DATA/wr.trace" 2>/dev/null | head -2)"
check "音频走 prefer_cluster(性能簇)"     'grep -q "prefer_cluster <- .* 0x1" "$DATA/wr.trace"' ""
check "perf 簇抬底频(>=1.5G)"     '[ "$(cat "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq")" -ge 1305600 ]' "$(cat "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq")"
check "后台 cpuset 收窄 0-3"      'grep -qx "0-3" "$FAKE/sys/fs/cgroup/background/cpuset.cpus"' "$(cat "$FAKE/sys/fs/cgroup/background/cpuset.cpus")"
check "默认不碰 hmbird 系数"      'grep -qx "1024,1024" "$FAKE/proc/sys/hmbird_II/coefficient"' "$(cat "$FAKE/proc/sys/hmbird_II/coefficient")"
check "线程计数 > 0"              'echo "$S" | grep -qE "^threads=[1-9]"' "$(echo "$S" | grep threads=)"
# WALT 才是本机真正的触控加速入口（/proc/sys/walt/*），通用 cpu_boost 节点在真机上大多不存在
check "WALT input_boost_ms 已关"   'grep -qx 0 "$FAKE/proc/sys/walt/input_boost_ms"' "now=$(cat "$FAKE/proc/sys/walt/input_boost_ms" 2>/dev/null)"
check "WALT sched_boost_on_input 已关" 'grep -qx 0 "$FAKE/proc/sys/walt/sched_boost_on_input"' ""
check "守护进程已把自己钉到性能簇"  'grep -q "自身亲和性钉到 0x3f" "$DATA/feather.log"' "$(grep '自身亲和性' "$DATA/feather.log" 2>/dev/null | tail -1)"
out=$("$BIN" doctor 2>&1)
check "doctor 报 WALT 与 governor"  'echo "$out" | grep -q "^walt=yes" && echo "$out" | grep -q "^governor: policy0="' "$(echo "$out" | grep -E '^(walt|governor)')"
check "doctor 报 daemon_pin/perf_mask" 'echo "$out" | grep -q "^daemon_pin=1 perf_mask=0x3f"' "$(echo "$out" | grep '^daemon_pin')"

echo "== 2.5 规则加载 =="
out=$("$BIN" doctor 2>&1)
check "规则条数 >= 9"              'echo "$out" | grep -qE "^rules=([9]|[1-9][0-9])"' "$(echo "$out" | grep '^rules=')"
check "Job.worker 规则已加载"      'echo "$out" | grep -q "Job\\\\.worker"' "$(echo "$out" | grep -c 'rule')"

echo "== 2.7 焦点窗口优先（top-app 里游戏与别的 App 并存，窗口是别的 App）=="
printf '4242\n5555\n' > "$FAKE/sys/fs/cgroup/top-app/cgroup.procs"
echo "com.test.app" > "$FAKE_WIN_FG"
wait_state "profile=daily" 20
out=$("$BIN" status 2>&1)
check "窗口来源生效"  'echo "$out" | grep -q "^fg=com.test.app"' "$(echo "$out" | grep '^fg=')"
check "因此不判为游戏" 'echo "$out" | grep -qx "profile=daily"'  "$(echo "$out" | grep '^profile=')"
printf '4242\n' > "$FAKE/sys/fs/cgroup/top-app/cgroup.procs"
echo "com.test.game" > "$FAKE_WIN_FG"
wait_state "profile=game" 20
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

echo "== 5.5 充电控制（方案 A：0~67% 满功率 + 到墙降档）=="
PS="$FAKE/sys/class/power_supply/battery"

# 1) 拐点以下：完全不干预，必须跑满
echo 50 > "$PS/capacity"; echo 360 > "$PS/temp"; echo Charging > "$PS/status"
sleep 3
check "SOC 50% 36℃ → 满电流不干预" 'grep -qx 6000000 "$PS/constant_charge_current_max"' "cur=$(cat "$PS/constant_charge_current_max")"

# 2) 到 50℃ 温度墙：降一档（75% ≈ 90W），不是暂停
echo 500 > "$PS/temp"
sleep 3
CUR=$(cat "$PS/constant_charge_current_max")
check "50℃ 到墙 → 降一档 75%"  '[ "$CUR" = "4500000" ]' "cur=$CUR"
check "此时未暂停充电"          'grep -qx 0 "$PS/input_suspend"' "suspend=$(cat "$PS/input_suspend")"

# 3) 52℃：再降一档（50% ≈ 60W）
echo 520 > "$PS/temp"
sleep 3
CUR=$(cat "$PS/constant_charge_current_max")
check "52℃ → 再降一档 50%"     '[ "$CUR" = "3000000" ]' "cur=$CUR"

# 4) 54℃：安全线，暂停
echo 540 > "$PS/temp"
sleep 3
check "54℃ 安全线 → 暂停"      'grep -qx 1 "$PS/input_suspend"' "suspend=$(cat "$PS/input_suspend")"

# 5) 回落 45℃：恢复满档
echo 450 > "$PS/temp"
sleep 3
check "回到 45℃ → 恢复充电"    'grep -qx 0 "$PS/input_suspend"' "suspend=$(cat "$PS/input_suspend")"
check "回到 45℃ → 满电流"      'grep -qx 6000000 "$PS/constant_charge_current_max"' "cur=$(cat "$PS/constant_charge_current_max")"

# 6) 拐点以上：按温度曲线降（39℃ → 88%）
echo 70 > "$PS/capacity"; echo 390 > "$PS/temp"
sleep 3
CUR=$(cat "$PS/constant_charge_current_max")
check "SOC 70% 39℃ → 曲线 88%"  '[ "$CUR" -ge 5200000 ] && [ "$CUR" -le 5350000 ]' "cur=$CUR"

# 7) 停止充电：还原
echo Discharging > "$PS/status"
sleep 2
check "停止充电 → 还原原始电流" 'grep -qx 6000000 "$PS/constant_charge_current_max"' "cur=$(cat "$PS/constant_charge_current_max")"

echo "== 5.55 充电加速（写 oplus_dbg_tbat / DT 分桶表自动定值，v1.3.1）=="
AT="$FAKE/sys/module/oplus_chg_v2/parameters/oplus_dbg_tbat"
GT="$FAKE/sys/module/oplus_chg_v2/parameters/gauge_dbg_tbat"
echo 50 > "$PS/capacity"; echo 360 > "$PS/temp"; echo Charging > "$PS/status"
sleep 3
# DT 标准表 normal_high=[160,250)，normal_high 起点+1℃ = 170；360(36℃) 会落进 warm 段
check "自动按 DT 定值 → 伪装 170（17.0℃）" 'grep -qx 170 "$AT"' "now=$(cat "$AT")"
check "没有动 gauge_dbg_tbat（保护不被骗）"  'grep -qx 0 "$GT"' "now=$(cat "$GT")"

echo 430 > "$PS/temp"      # 43℃：还不到 45℃ 线，继续伪装
sleep 3
check "43℃ 仍伪装（未到 45℃ 线）" 'grep -qx 170 "$AT"' "now=$(cat "$AT")"

echo 450 > "$PS/temp"      # 45℃：到线，立刻还原原值
sleep 3
check "45℃ 到线 → 还原原值 0"      'grep -qx 0 "$AT"' "now=$(cat "$AT")"
out=$("$BIN" status 2>&1)
check "状态标记 accel_blocked=1"   'echo "$out" | grep -qx "accel_blocked=1"' "$(echo "$out" | grep accel_blocked)"

echo 420 > "$PS/temp"      # 回落 42℃：重新接管
sleep 3
check "回落 42℃ → 重新伪装"        'grep -qx 170 "$AT"' "now=$(cat "$AT")"

# 手动档：300（30℃）在标准表里已属 warm，模块不该拦（用户明确指定就照写）
"$BIN" set accel_tbat 300 >/dev/null 2>&1
sleep 3
check "手动 300 → 照写（钳位只到 340）" 'grep -qx 300 "$AT"' "now=$(cat "$AT")"
# 越界值必须被拦：999 越界 → cfg_load 退回自动档 → 170
"$BIN" set accel_tbat 999 >/dev/null 2>&1
sleep 3
check "越界 999 → 退回自动 170"    'grep -qx 170 "$AT"' "now=$(cat "$AT")"
"$BIN" set accel_tbat 0 >/dev/null 2>&1
sleep 3
check "回自动档 → 170"             'grep -qx 170 "$AT"' "now=$(cat "$AT")"
# v1.3.0 遗留配置：accel_tbat=360 会被 cfg_load 判定为越界，自动退回 auto
"$BIN" set accel_tbat 360 >/dev/null 2>&1
sleep 3
check "老配置 360 → 自动退回 170"   'grep -qx 170 "$AT"' "now=$(cat "$AT")"
# 功率采样（双电芯）：4.247V × 2S × 4.421A ≈ 37.5W → chg_w/chg_peak 应为 375（0.1W）
sleep 3
ST="$(cat "$DATA/state" 2>/dev/null)"
check "state 记录实测功率 37.5W"    'echo "$ST" | grep -qx "chg_w=375"' "got=$(echo "$ST" | grep '^chg_w=')"
check "充电峰值已记录"              'echo "$ST" | grep -qxE "chg_peak=37[0-9]"' "got=$(echo "$ST" | grep '^chg_peak=')"
# power 命令：能列出电芯数与 power_supply 设备属性
POUT="$("$BIN" power 2>&1)"
check "power 判定 cells=2"          'echo "$POUT" | grep -q "cells=2"' "got=$(echo "$POUT" | head -1)"
check "power 算出 37.5W"            'echo "$POUT" | grep -q "37.5W"' "got=$(echo "$POUT" | sed -n 2p)"

echo "== 5.57 电芯判定 / charge why（v1.4.2）=="
D2="$DATA/standalone"; rm -rf "$D2"; mkdir -p "$D2"
cp "$DATA/config.ini" "$D2/config.ini"
echo 4500000 > "$PS/voltage_max"        # 真机 charger v2 的 VOLTAGE_MAX 就是单芯 ~4.5V
echo 330 > "$PS/temp"                   # 33.0℃，未到 40℃ 降流线
SOUT=$(FEATHER_DATA="$D2" "$BIN" power 2>&1)
check "voltage_max=4.5V 仍判 2S（靠 DT oplus,batt_num）" 'echo "$SOUT" | grep -q "cells=2"' "$(echo "$SOUT" | head -1)"
WOUT=$(FEATHER_DATA="$D2" "$BIN" charge why 2>&1)
check "charge why: 功率按 2S 算"    'echo "$WOUT" | grep -q "power=37.5W cells=2"' "$(echo "$WOUT" | grep '^power=')"
check "charge why: 读到真实机身温度" 'echo "$WOUT" | grep -q "^real: shell shell_back=29.0"' "$(echo "$WOUT" | grep '^real:')"
check "charge why: 报适配器上限 65W" 'echo "$WOUT" | grep -q "^adapter: 65.0W"' "$(echo "$WOUT" | grep '^adapter:')"
check "charge why: 低于 90W 给出提示" 'echo "$WOUT" | grep -q "^note: 适配器/协议上限低于 90W"' "$(echo "$WOUT" | grep '^note:')"
check "charge why: 报电池温度余量" 'echo "$WOUT" | grep -q "^headroom: batt 33℃ → 降流线 40.0℃（还有 7.0℃）"' "$(echo "$WOUT" | grep '^headroom:')"
check "charge why: 亮屏/息屏峰值分开" 'echo "$WOUT" | grep -q "^peak: on=" && echo "$WOUT" | grep -q " off="' "$(echo "$WOUT" | grep '^peak:')"
check "charge why: 给出段位结论"    'echo "$WOUT" | grep -q "^verdict:"' "$(echo "$WOUT" | grep '^verdict:')"
echo 'battery_cells=1' >> "$D2/config.ini"
SOUT=$(FEATHER_DATA="$D2" "$BIN" power 2>&1)
check "battery_cells=1 覆盖 DT"     'echo "$SOUT" | grep -q "cells=1"' "$(echo "$SOUT" | head -1)"
echo 8900000 > "$PS/voltage_max"

echo Discharging > "$PS/status"
sleep 2
check "停止充电 → 还原原值 0"      'grep -qx 0 "$AT"' "now=$(cat "$AT")"

echo Charging > "$PS/status"
sleep 3
check "重新插电 → 再次伪装"        'grep -qx 170 "$AT"' "now=$(cat "$AT")"
ST2="$(cat "$DATA/state" 2>/dev/null)"
check "state 回填 cfg_charge_ctrl（WebUI 开关才不会一直是关的）" 'echo "$ST2" | grep -qx "cfg_charge_ctrl=1"' "$(echo "$ST2" | grep '^cfg_charge_ctrl=')"
check "state 标记伪装换段生效 accel_eff=1" 'echo "$ST2" | grep -qx "accel_eff=1"' "$(echo "$ST2" | grep '^accel_eff=')"
SOUT=$("$BIN" charge sweep 60 2>&1)
check "charge sweep 逐档实测"     'echo "$SOUT" | grep -q "^try="' "$(echo "$SOUT" | grep '^try=' | head -2 | tr '\n' ' ')"
check "charge sweep 给出最优值并还原" 'echo "$SOUT" | grep -q "^best=" && echo "$SOUT" | grep -q "^restore="' "$(echo "$SOUT" | grep -E '^(best|restore)=' | tr '\n' ' ')"
"$BIN" set accel_ctrl 0 >/dev/null 2>&1
sleep 3
check "accel_ctrl=0 → 还原原值 0"  'grep -qx 0 "$AT"' "now=$(cat "$AT")"
"$BIN" set accel_ctrl 1 >/dev/null 2>&1
sleep 3
check "accel_ctrl=1 → 恢复伪装"    'grep -qx 170 "$AT"' "now=$(cat "$AT")"
out=$("$BIN" charge 2>&1)
check "charge 命令报告 accel/DT"   'echo "$out" | grep -q "auto_dt=170"' "$(echo "$out" | grep accel)"

echo "== 5.6 official 模式：还权给系统 =="
"$BIN" set game_mode official >/dev/null 2>&1
sleep 3
check "official: 频率地板已撤销"  'grep -qx 300000 "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq"' "min=$(cat "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq")"
check "official: 后台 cpuset 已还原" 'grep -qx "0-7" "$FAKE/sys/fs/cgroup/background/cpuset.cpus"' "cpus=$(cat "$FAKE/sys/fs/cgroup/background/cpuset.cpus")"
check "official: hmbird 参数已按原值还原" 'grep -q "hmbird: 已还原" "$DATA/feather.log"' "$(grep -c "hmbird" "$DATA/feather.log")"
"$BIN" set game_mode aggressive >/dev/null 2>&1

if [ "$RUST" = "1" ]; then
echo "== 5.7 应用图标（APK 抠图：webp / 跳过透明前景层 / 负缓存）=="
IA="$DATA/apps"
mkdir -p "$IA/w/res/mipmap-xxxhdpi-v4" "$IA/w/res/mipmap-xxhdpi-v4" "$IA/w/res/mipmap-anydpi-v26" \
         "$IA/f/res/mipmap-xxhdpi-v4" "$IA/f/res/drawable"
# 只关心魔数和体积：抠图不做解码，浏览器才解码
mkpng() { printf '\211PNG\r\n\032\n' > "$1"; head -c $(($2 - 8)) /dev/zero >> "$1"; }
mkwebp() { printf 'RIFF\310\000\000\000WEBP' > "$1"; head -c $(($2 - 12)) /dev/zero >> "$1"; }
mkpng  "$IA/w/res/mipmap-xxxhdpi-v4/ic_launcher.png" 100
mkwebp "$IA/w/res/mipmap-xxxhdpi-v4/ic_launcher.webp" 200
mkpng  "$IA/w/res/mipmap-xxhdpi-v4/ic_launcher_foreground.png" 500   # 透明前景层，必须被跳过
printf '<adaptive-icon/>\n' > "$IA/w/res/mipmap-anydpi-v26/ic_launcher.xml"
mkpng  "$IA/f/res/mipmap-xxhdpi-v4/ic_launcher_foreground.png" 500    # 只有透明前景层
mkpng  "$IA/f/res/drawable/icon.png" 80                               # → 应退回到这张
(cd "$IA/w" && zip -qr ../webp.apk .)
(cd "$IA/f" && zip -qr ../fallback.apk .)

# 资源名被混淆（res/ab.png）+ 一张更大的非方形图（启动图）→ 只能靠尺寸挑
mkdir -p "$IA/o/res" "$IA/a/res/mipmap-xxhdpi-v4" "$IA/a/res/mipmap-anydpi-v26"
# 写入合法 PNG 头里的宽高：192x192 / 1080x2400（解析器只看这些字节）
printf '\211PNG\r\n\032\n\000\000\000\015IHDR\000\000\000\300\000\000\000\300\010\002\000\000\000\000\000\000\000' > "$IA/o/res/ab.png"
head -c 80 /dev/zero >> "$IA/o/res/ab.png"
printf '\211PNG\r\n\032\n\000\000\000\015IHDR\000\000\004\070\000\000\011\140\010\002\000\000\000\000\000\000\000' > "$IA/o/res/zz.png"
head -c 280 /dev/zero >> "$IA/o/res/zz.png"
(cd "$IA/o" && zip -qr ../obf.apk .)
# 纯自适应图标：只有透明前景层 + XML，没有位图本体 → 应退回首字母，而不是拿前景层冒充
mkpng "$IA/a/res/mipmap-xxhdpi-v4/ic_launcher_foreground.png" 500
printf '<adaptive-icon/>\n' > "$IA/a/res/mipmap-anydpi-v26/ic_launcher.xml"
(cd "$IA/a" && zip -qr ../adaptive.apk .)

out=$("$BIN" icon com.fake.webp 2>&1)
check "抠图成功并落缓存" 'echo "$out" | grep -q "^ok=" && [ -s "$DATA/icons/com.fake.webp" ]' "$out"
W1="$(head -c 4 "$DATA/icons/com.fake.webp" 2>/dev/null)"; W2="$(head -c 12 "$DATA/icons/com.fake.webp" 2>/dev/null | tail -c 4)"
check "选了 ic_launcher.webp（不是更大的前景层/更小的 png）" '[ "$W1" = "RIFF" ] && [ "$W2" = "WEBP" ]' "magic=$W1..$W2"
out=$("$BIN" icon com.fake.fallback 2>&1)
F1="$(head -c 4 "$DATA/icons/com.fake.fallback" 2>/dev/null | tail -c 3)"
check "只有前景层时退回 drawable/icon.png" 'echo "$out" | grep -q "^ok=" && [ "$F1" = "PNG" ]' "$out $F1"
out=$("$BIN" icon com.fake.missing 2>&1)
check "不存在的包 → err=no_icon" '[ "$out" = "err=no_icon" ]' "$out"
check "负缓存带时间戳（30 分钟后自动重试）" 'awk -F"\t" "\$1==\"com.fake.missing\" && \$2+0>0 {f=1} END{exit !f}" "$DATA/icons/.miss"' "$(cat "$DATA/icons/.miss" 2>/dev/null)"
out=$("$BIN" icon com.fake.webp force 2>&1)
check "force 会重抠" 'echo "$out" | grep -q "^ok="' "$out"
out=$("$BIN" icon com.fake.obf 2>&1)
check "混淆资源名：按尺寸选方形图（不是更大的启动图）" 'echo "$out" | grep -q "^ok=" && [ "$(stat -c %s "$DATA/icons/com.fake.obf")" = "113" ]' "$out/$(stat -c %s "$DATA/icons/com.fake.obf" 2>/dev/null)"
out=$("$BIN" icon com.fake.adaptive 2>&1)
check "纯自适应图标 → 退回首字母（不拿透明前景层冒充）" '[ "$out" = "err=no_icon" ]' "$out"
DOCT=$("$BIN" doctor 2>&1)
check "doctor 报告图标工具与缓存数" 'echo "$DOCT" | grep -qE "^icon tool=.+ cached=[1-9]"' "$(echo "$DOCT" | grep '^icon ')"
# HTTP 侧：同一个缓存文件必须按魔数给出 Content-Type（webp 不能写成 image/png）
code=$(curl -s -D /tmp/ih -o /tmp/ib -w '%{http_code}' "http://127.0.0.1:17891/icon?p=com.fake.webp")
check "GET /icon 200" ' [ "$code" = "200" ]' "code=$code"
check "/icon 的 Content-Type 跟随魔数" 'grep -qi "content-type: image/webp" /tmp/ih' "$(grep -i content-type /tmp/ih)"
check "/icon 返回的是 webp 本体" '[ "$(head -c 4 /tmp/ib)" = "RIFF" ]' ""
fi

echo "== 5.8 WebUI 图标/名称通道（管理器原生接口优先）=="
check "图标首选 ksu://icon"        'grep -q "ksu://icon/" "$PROJ/module/webroot/app.js"' ""
check "名字走 getPackagesInfo"     'grep -q "getPackagesInfo" "$PROJ/module/webroot/app.js"' ""
check "失败回落 daemon 抠图"       'grep -q "data-fb=" "$PROJ/module/webroot/app.js"' ""

echo "== 6. 熄屏 =="
printf '0\n' > "$FAKE/sys/class/backlight/panel0-backlight/brightness"
sleep 5
S=$(cat "$DATA/state" 2>/dev/null)
check "profile=sleep"             'echo "$S" | grep -qx "profile=sleep"' "$(echo "$S" | grep profile=)"
PF=$(echo "$S" | sed -n 's/^cap_perf=//p')
check "熄屏上限 <= 30%"           '[ "${PF:-9}" -le 1305600 ]' "cap_perf=$PF"
printf '120\n' > "$FAKE/sys/class/backlight/panel0-backlight/brightness"

echo "== 7. 停止与还原 =="
echo "  [debug] runner=$("$BIN" status 2>/dev/null | sed -n 's/^runner=//p') pidfile=$(cat "$DATA/featherd-daemon.pid" 2>/dev/null)"
"$BIN" stop >/dev/null 2>&1
sleep 1
kill $DPID 2>/dev/null
PF=$(cat "$FAKE/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq")
check "退出后恢复满频"            '[ "$PF" = "3628800" ]' "cap_perf=$PF"
check "退出后伪装的温度已还原"    '[ "$(cat "$FAKE/sys/module/oplus_chg_v2/parameters/oplus_dbg_tbat")" = "0" ]' "tbat=$(cat "$FAKE/sys/module/oplus_chg_v2/parameters/oplus_dbg_tbat")"
check "退出后 WALT 触控加速已还原" '[ "$(cat "$FAKE/proc/sys/walt/input_boost_ms")" = "2000" ]' "now=$(cat "$FAKE/proc/sys/walt/input_boost_ms" 2>/dev/null)"

echo
echo "--------------------------------------------"
printf "结果: \033[32m%d passed\033[0m, \033[31m%d failed\033[0m\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || { echo "---- apply 轨迹 ----"; grep -E "apply:|extras|restore|daemon" "$DATA/feather.log" | tail -20; echo "---- log ----"; tail -12 "$DATA/feather.log"; }
[ "$FAIL" -eq 0 ]
