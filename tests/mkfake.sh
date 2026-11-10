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
mkf "$C/cpufreq/policy0/scaling_governor" schedutil
mkf "$C/cpufreq/policy0/affected_cpus" "0 1 2 3 4 5"
mkf "$C/cpufreq/policy0/scaling_available_frequencies" "300000 787200 1305600 1555200 1900800 2246400 2611200 2880000 3244800 3628800"
mkdir -p "$C/cpufreq/policy0/schedutil"
mkf "$C/cpufreq/policy0/schedutil/up_rate_limit_us" 500
mkf "$C/cpufreq/policy0/schedutil/down_rate_limit_us" 20000

mkf "$C/cpufreq/policy6/cpuinfo_max_freq" 4608000
mkf "$C/cpufreq/policy6/cpuinfo_min_freq" 300000
mkf "$C/cpufreq/policy6/scaling_max_freq" 4608000
mkf "$C/cpufreq/policy6/scaling_min_freq" 300000
mkf "$C/cpufreq/policy6/scaling_governor" schedutil
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

echo "fake tree ready: $ROOT"
