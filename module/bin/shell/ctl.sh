#!/system/bin/sh
# ============================================================================
#  FeatherTune CLI — WebUI 与命令行共用的唯一入口
#  约定：所有输出都是 key=value（便于 JS 解析），错误信息以 err= 开头
# ============================================================================

BIN_DIR=${0%/*}
[ "$BIN_DIR" = "$0" ] && BIN_DIR=/data/adb/modules/feather_tune/bin
. "$BIN_DIR/lib.sh"

need_root() {
    [ "${FEATHER_SKIP_ROOT:-0}" = 1 ] && return 0
    [ "$(id -u 2>/dev/null)" = 0 ] || { echo "err=no_root"; exit 1; }
}

cmd_status() {
    detect_topology; hmbird_detect
    if [ -f "$STATE" ]; then
        cat "$STATE"
    else
        echo "profile=unknown"
    fi
    echo "runner=$([ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null && echo running || echo stopped)"
    echo "model=$(getprop ro.product.model 2>/dev/null)"
    echo "android=$(getprop ro.build.version.release 2>/dev/null)"
    echo "sdk=$(getprop ro.build.version.sdk 2>/dev/null)"
    echo "soc=$(getprop ro.board.platform 2>/dev/null)"
    echo "kernel=$(uname -r 2>/dev/null)"
    echo "policies=$(echo $POLICIES | wc -w | tr -d ' ')"
    echo "perf_policy=${PERF_POLICY##*/}:$PERF_MAX"
    echo "prime_policy=${PRIME_POLICY##*/}:$PRIME_MAX"
    echo "hmbird_present=$HMBIRD_PRESENT"
    for p in $POLICIES; do
        echo "gov_${p##*/}=$(gov_of "$p")"
        echo "max_${p##*/}=$(rd "$p/scaling_max_freq")"
    done
    echo "cfg_enable=$(cfg_get enable 1)"
    echo "cfg_mode=$(cfg_get mode auto)"
    echo "cfg_intensity=$(cfg_get intensity 50)"
    echo "cfg_auto_game=$(cfg_get auto_game 1)"
    echo "cfg_thread_opt=$(cfg_get thread_opt 1)"
    echo "cfg_screen_off=$(cfg_get screen_off_saver 1)"
    echo "cfg_kill_boost=$(cfg_get kill_boost 1)"
    echo "cfg_io=$(cfg_get io_tune 1)"
    echo "cfg_gpu=$(cfg_get gpu_tune 1)"
    echo "cfg_launch_boost=$(cfg_get launch_boost 1)"
    echo "cfg_log_level=$(cfg_get log_level 1)"
    echo "games=$(grep -cv '^[[:space:]]*\(#\|$\)' "$GAMES" 2>/dev/null)"
    echo "webui=webroot/index.html"
}

cmd_set() {
    [ -n "$2" ] || { echo "err=missing_value"; exit 1; }
    case "$1" in
        mode)  cfg_set mode "$2"; cmd_apply; return 0 ;;
        intensity)
            case "$2" in ''|*[!0-9]*) echo "err=bad_intensity"; exit 1 ;; esac
            iv=$2
            [ "$iv" -gt 100 ] && iv=100
            cfg_set intensity "$iv"
            ;;
        enable|auto_game|thread_opt|screen_off_saver|kill_boost|io_tune|gpu_tune|launch_boost)
            cfg_set "$1" "$2" ;;
        log_level|boost_ms)
            cfg_set "$1" "$2" ;;
        *) echo "err=unknown_key"; exit 1 ;;
    esac
    cmd_apply
    echo "ok=$1=$2"
}

cmd_apply() {
    [ "$(id -u 2>/dev/null)" = 0 ] || [ "${FEATHER_SKIP_ROOT:-0}" = 1 ] || return 0
    detect_topology; capture_sys_caps; hmbird_detect
    p=$(cfg_get mode auto)
    [ "$p" = auto ] && p=daily
    screen_on || p=sleep
    apply_profile "$p"
    echo "applied=$p perf=${CAP_PERF:-na} prime=${CAP_PRIME:-na}"
}

cmd_mode() {
    case "$2" in
        auto|daily|battery|game|perf|sleep) cfg_set mode "$2"; cmd_apply ;;
        *) echo "err=bad_mode"; exit 1 ;;
    esac
}

cmd_doctor() {
    echo "== FeatherTune doctor =="
    echo "model=$(getprop ro.product.model 2>/dev/null)  soC=$(getprop ro.board.platform 2>/dev/null)"
    echo "android=$(getprop ro.build.version.release 2>/dev/null) sdk=$(getprop ro.build.version.sdk 2>/dev/null) kernel=$(uname -r)"
    echo "root=$( [ -d /data/adb/ksu ] && echo KSU )$( [ -d /data/adb/ap ] && echo APatch )$( [ -d /data/adb/magisk ] && echo Magisk )"
    echo "meta=$( [ -d /data/adb/modules/meta-overlayfs ] && echo overlayfs || echo none )"
    detect_topology; hmbird_detect
    echo "-- cpufreq --"
    for p in $POLICIES; do
        echo "${p##*/}: cpus=$(rd "$p/affected_cpus") gov=$(gov_of "$p") max=$(rd "$p/scaling_max_freq")/$(rd "$p/cpuinfo_max_freq") min=$(rd "$p/scaling_min_freq")"
        echo "   freqs=$(tr ' ' ',' < "$p/scaling_available_frequencies" 2>/dev/null | cut -c1-160)"
        for t in hmbird schedutil; do
            [ -d "$p/$t" ] && echo "   $t/: $(ls "$p/$t" 2>/dev/null | tr '\n' ' ')"
        done
    done
    echo "-- hmbird --"
    echo "sysctl_dir=$([ -d "$HMBIRD_SYS" ] && echo yes || echo no) live=$(hmbird_live && echo yes || echo no)"
    for k in coefficient perf_high_ratio cpus_reserved cpus_exclusive freq_policy frame_per_sec heavy_boost; do
        [ -r "$HMBIRD_SYS/$k" ] && echo "  $k=$(cat "$HMBIRD_SYS/$k" 2>/dev/null)"
    done
    echo "-- cgroup --"
    for g in top-app foreground background system-background; do
        [ -d "$CGRP/$g" ] && echo "$g: uclamp_max=$(rd "$CGRP/$g/cpu.uclamp.max") cpus=$(rd "$CGRP/$g/cpuset.cpus" 2>/dev/null)"
    done
    echo "-- 设备节点 --"
    echo "brightness=$(rd "$SYS/class/backlight/panel0-backlight/brightness")"
    echo "battery=$(rd "$SYS/class/power_supply/battery/capacity")% $(rd "$SYS/class/power_supply/battery/status")"
    echo "kgsl=$( [ -d /sys/class/kgsl/kgsl-3d0 ] && echo yes || echo no )"
    echo "taskset=$(command -v taskset || echo none)"
    echo "-- boost 节点 --"
    for n in /sys/module/msm_performance/parameters/touch_boost /sys/devices/system/cpu/cpu_boost/input_boost_enabled /proc/sys/kernel/sched_boost; do
        [ -e "$n" ] && echo "$n=$(cat "$n" 2>/dev/null)"
    done
    echo "-- 守护进程 --"
    echo "pid=$(rd "$PIDFILE") alive=$(alive 2>/dev/null && echo yes || echo no)"
    echo "log_size=$(wc -c < "$LOG" 2>/dev/null)"
}

alive() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; }

cmd_pkg() {
    case "$2" in
        add)
            [ -n "$3" ] || { echo "err=missing_pkg"; exit 1; }
            grep -qxF -- "$3" "$GAMES" 2>/dev/null || echo "$3" >> "$GAMES"
            echo "ok=added:$3"
            ;;
        del)
            [ -n "$3" ] || { echo "err=missing_pkg"; exit 1; }
            grep -vxF -- "$3" "$GAMES" > "$DATA/.g.tmp" 2>/dev/null
            mv -f "$DATA/.g.tmp" "$GAMES"
            echo "ok=removed:$3"
            ;;
        list)
            grep -v '^[[:space:]]*\(#\|$\)' "$GAMES" 2>/dev/null
            ;;
        *) echo "err=usage" ;;
    esac
}

cmd_apps() {  # 给 WebUI 的已安装第三方应用列表
    pm list packages -3 2>/dev/null | sed 's/^package://'
}

cmd_ctl() {
    case "$1" in
        start)   "$BIN_DIR/featherd-daemon.sh" start ;;
        stop)    "$BIN_DIR/featherd-daemon.sh" stop ;;
        restart) "$BIN_DIR/featherd-daemon.sh" stop; sleep 0.5; "$BIN_DIR/featherd-daemon.sh" start ;;
        *) echo "err=usage" ;;
    esac
}

cmd_log() {
    n=${2:-40}
    tail -n "$n" "$LOG" 2>/dev/null | sed 's/^/log=/'
}

cmd_reset() {
    for f in config.ini games.conf rules.conf; do
        cp -f "/data/adb/modules/$MODID/etc/$f" "$DATA/$f" 2>/dev/null
    done
    echo "ok=reset"
}

need_root
case "$1" in
    status)  cmd_status ;;
    set)     cmd_set "$2" "$3" ;;
    apply)   cmd_apply ;;
    mode)    cmd_mode "$2" ;;
    doctor)  cmd_doctor ;;
    pkg)     cmd_pkg "$@" ;;
    apps)    cmd_apps ;;
    start|stop|restart) cmd_ctl "$1" ;;
    log)     cmd_log "$2" ;;
    reset)   cmd_reset ;;
    restore) detect_topology; capture_sys_caps; restore_all; echo "ok=restored" ;;
    *)       echo "err=unknown_cmd"; echo "usage: ctl.sh status|set k v|mode m|apply|doctor|pkg add|del|list|apps|start|stop|restart|log n|reset|restore" ;;
esac
