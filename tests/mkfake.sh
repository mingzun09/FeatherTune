#!/bin/sh
# 构造一棵假的 Android 文件树，用来在 PC 上跑通场景状态机
# 用法: sh mkfake.sh   ->  生成 ./fake/{sys,proc,...}
set -e
ROOT=${1:-$(dirname "$0")/fake}
rm -rf "$ROOT"
mkdir -p "$ROOT"

mkf() { mkdir -p "$(dirname "$1")"; printf '%s\n' "$2" > "$1"; }

# ---------------- cpufreq: policy0 = CPU0-5(性能簇), policy6 = CPU6-7(超大核)
C="$ROOT/sys/devices/system/cpu"
mkf "$C/cpufreq/policy0/cpuinfo_max_freq" 3628800
mkf "$C/cpufreq/policy0/cpuinfo_min_freq" 300000
mkf "$C/cpufreq/policy0/scaling_max_freq" 3628800
mkf "$C/cpufreq/policy0/scaling_min_freq" 300000
mkf "$C/cpufreq/policy0/scaling_governor" walt
mkf "$C/cpufreq/policy0/affected_cpus" "0 1 2 3 4 5"
mkf "$C/cpufreq/policy0/scaling_available_frequencies" "300000 787200 1305600 1555200 1900800 2246400 2611200 2880000 3244800 3628800"
mkdir -p "$C/cpufreq/policy0/walt"
mkf "$C/cpufreq/policy0/walt/up_rate_limit_us" 500
mkf "$C/cpufreq/policy0/walt/down_rate_limit_us" 20000

mkf "$C/cpufreq/policy6/cpuinfo_max_freq" 4608000
mkf "$C/cpufreq/policy6/cpuinfo_min_freq" 300000
mkf "$C/cpufreq/policy6/scaling_max_freq" 4608000
mkf "$C/cpufreq/policy6/scaling_min_freq" 300000
mkf "$C/cpufreq/policy6/scaling_governor" walt
mkf "$C/cpufreq/policy6/affected_cpus" "6 7"
mkf "$C/cpufreq/policy6/scaling_available_frequencies" "300000 787200 1305600 1555200 1900800 2246400 2611200 2880000 3244800 3628800 4000000 4608000"
mkdir -p "$C/cpufreq/policy6/hmbird"
mkf "$C/cpufreq/policy6/hmbird/target_loads" 90
mkf "$C/cpufreq/policy6/hmbird/apply_freq_immediately" 1
mkf "$C/cpufreq/policy6/hmbird/soft_freq_max" 0

# ---------------- 屏幕模式（刷新率探测用）
mkf "$ROOT/sys/class/graphics/fb0/modes" "U:1080x2354p-165"

# ---------------- 背光 / 电池
mkf "$ROOT/sys/class/backlight/panel0-backlight/brightness" 120
mkf "$ROOT/sys/class/power_supply/battery/capacity" 78
mkf "$ROOT/sys/class/power_supply/battery/temp" 330
mkf "$ROOT/sys/class/power_supply/battery/status" Discharging

# ---------------- hmbird sysctl
H="$ROOT/proc/sys/hmbird_II"
mkf "$H/coefficient" "1024,1024"
mkf "$H/perf_high_ratio" "60,60"
mkf "$H/cpus_reserved" 240
mkf "$H/frame_per_sec" 120
mkf "$H/heavy_boost" "0 400 140"
mkf "$H/prefer_cpu" ""
mkf "$H/freq_policy" "0,0"
mkf "$H/prefer_cluster" ""
mkf "$H/prefer_preempt" ""
mkf "$H/prefer_idle" ""
mkf "$H/set_uclamp" ""

# ---------------- WALT（一加 15 本机的真实调度器/触控加速）
# kernel/sched/walt/sysctl.c：/proc/sys/walt/{input_boost_ms,sched_boost_on_input,sched_boost,...}
W="$ROOT/proc/sys/walt"
mkf "$W/input_boost_ms" 2000
mkf "$W/input_boost_freq" "0 0 0 0 0 0 0 0"
mkf "$W/sched_boost_on_input" 1
mkf "$W/sched_boost" 1
mkf "$W/sched_upmigrate" 762
mkf "$W/sched_downmigrate" 500

# OPLUS 自己的 touch_boost kobject（真机挂在 /sys/devices/platform/**/touch_boost/）
TT="$ROOT/sys/devices/platform/soc/oplus-touch-boost/touch_boost"
mkdir -p "$TT"
mkf "$TT/touch_boost_ms" 300
mkf "$TT/touch_boost_freq" 1800000

# ---------------- cgroup v2
for g in top-app foreground background system-background; do
  mkf "$ROOT/sys/fs/cgroup/$g/cpu.uclamp.max" "max 100%"
  mkf "$ROOT/sys/fs/cgroup/$g/cpu.uclamp.min" "min 0%"
  mkf "$ROOT/sys/fs/cgroup/$g/cpuset.cpus" "0-7"
done

# ---------------- 进程：4242=游戏 5555=普通应用
mkf "$ROOT/proc/4242/cmdline" "com.test.game"
mkf "$ROOT/proc/4242/task/4242/comm" "com.test.game"
mkf "$ROOT/proc/4242/task/4300/comm" "RenderThread"
mkf "$ROOT/proc/4242/task/4301/comm" "UnityMain"
mkf "$ROOT/proc/4242/task/4302/comm" "AudioTrack"
mkf "$ROOT/proc/4242/task/4305/comm" "UnityGfxDeviceW"
mkf "$ROOT/proc/4242/task/4306/comm" "GfxRenderWorker"
mkf "$ROOT/proc/4242/task/4307/comm" "Job.worker 0"
mkf "$ROOT/proc/4242/task/4303/comm" "LoadingThread"
mkf "$ROOT/proc/4242/task/4304/comm" "random_worker"

# 干扰项：top-app 里排在前面的系统进程（真机常见），模块必须能绕过它选中游戏
mkf "$ROOT/proc/1111/cmdline" "com.android.systemui"
mkf "$ROOT/proc/1111/task/1111/comm" "systemui"

mkf "$ROOT/proc/5555/cmdline" "com.test.app"
mkf "$ROOT/proc/5555/task/5555/comm" "com.test.app"

mkf "$ROOT/sys/fs/cgroup/top-app/cgroup.procs" "1111
4242"

# ---------------- 充电控制（方案 A）用到的节点
PS="$ROOT/sys/class/power_supply/battery"
mkf "$PS/constant_charge_current_max" 6000000
mkf "$PS/input_suspend" 0
# 功率采样用：模拟一加 15（双电芯）——voltage_now 单芯 µV、current_now 串联 mA（负=充电）
# voltage_max 8.9V ⇒ 自动判定 2S，功率 = 4.247V × 2 × 4.421A ≈ 37.5W
mkf "$PS/voltage_now" 4247000
mkf "$PS/current_now" -4421
mkf "$PS/voltage_max" 8900000

# ---------------- usb：适配器/协议天花板（charge why 用 voltage_max × current_max 判断）
U="$ROOT/sys/class/power_supply/usb"
mkf "$U/online" 1
mkf "$U/voltage_max" 20000000     # 20V
mkf "$U/current_max" 3250000      # 3.25A → 65W（模拟 65W PD 适配器）
mkf "$U/voltage_now" 19500000
mkf "$U/current_now" 3770000

# ---------------- 充电加速：oplus_chg_v2 的温度覆盖参数 + DT 分桶表 ----------------
# oplus_dbg_tbat = 机身温度（v2 oplus_chg_comm.c 的 module_param），gauge_dbg_tbat = 电池温度
mkf "$ROOT/sys/module/oplus_chg_v2/parameters/oplus_dbg_tbat" 0
mkf "$ROOT/sys/module/oplus_chg_v2/parameters/gauge_dbg_tbat" 0
# DT: oplus,temp_range 是大端 u32 数组，标准表 {-50,0,50,100,160,250,350}（0.1℃）
#   → 6 段：little_cold/cool/little_cool/normal_low/normal_high/warm，
#     normal_high = [160,250) → 模块应自动把伪装值算成 170（17.0℃）
DTD="$ROOT/sys/firmware/devicetree/base/oplus,ufcs_strategy"
mkdir -p "$DTD"
printf '\377\377\377\316\000\000\000\000\000\000\000\062\000\000\000\144\000\000\000\240\000\000\000\372\000\000\001\136' > "$DTD/oplus,temp_range"
# 电芯串联数：charger v2 的 voltage_max 是单芯电压（~4.5V），不能再拿它判 2S
printf '\000\000\000\002' > "$DTD/oplus,batt_num"
# 电池温度降流起点表（6 个 u32，0.1℃）：一加 15 是 400/420/430/390/410/420
printf '\000\000\001\220\000\000\001\244\000\000\001\256\000\000\001\206\000\000\001\232\000\000\001\244' > "$DTD/oplus,ufcs_strategy_batt_high_temp"

# ---------------- 热区（charge why 用它判断真实温度落在哪个分桶段）----------------
TZ="$ROOT/sys/class/thermal/thermal_zone0"
mkdir -p "$TZ"
mkf "$TZ/type" "shell_back"
mkf "$TZ/temp" 29000          # 29.0℃ → 标准表里落在 warm 段
TZ1="$ROOT/sys/class/thermal/thermal_zone1"
mkdir -p "$TZ1"
mkf "$TZ1/type" "batt"
mkf "$TZ1/temp" 33000         # 33.0℃

echo "fake tree ready: $ROOT"
