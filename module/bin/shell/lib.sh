#!/system/bin/sh
# ============================================================================
#  FeatherTune · 羽量调校  —  core library
#  Target : OnePlus 15 / SM8850 (2x Oryon Prime 4.6G + 6x Oryon Performance 3.6G)
#  Host   : KernelSU / Magisk / APatch, Android 15~17
#
#  设计约束（这部分决定了模块自己会不会变成耗电源）：
#   1. 热路径（每秒）里不 fork：读文件用 read 内建，配置一次性缓存到 C_* 变量
#   2. 所有写入都先探测节点再写，失败即跳过，绝不阻塞开机
#   3. 线程归位用“增量扫描 + 单次 awk 匹配”，几十个线程也只有 1 个 awk 进程
# ============================================================================

MODID=feather_tune
MODDIR=/data/adb/modules/$MODID
DATA=${FEATHER_DATA:-/data/adb/feather}
CONF=$DATA/config.ini
GAMES=$DATA/games.conf
RULES=$DATA/rules.conf
STATE=$DATA/state
LOG=$DATA/feather.log
PIDFILE=$DATA/featherd.pid

CPU=${FEATHER_CPU_ROOT:-/sys/devices/system/cpu}
CGRP=${FEATHER_CGROUP_ROOT:-/sys/fs/cgroup}
SYS=${FEATHER_SYS_ROOT:-/sys}
PROC=${FEATHER_PROC_ROOT:-/proc}
HMBIRD_SYS=${FEATHER_HMBIRD_SYS:-/proc/sys/hmbird_II}

HMBIRD_PRESENT=0
HMBIRD_TUNED=0
THREADS_PINNED=0
BOOST_KILLED=0
PROF=""; CAP_PERF=""; CAP_PRIME=""
RULES_CLEAN=""

# ---------------------------------------------------------------- logging ---
log() {
    _sz=$(wc -c < "$LOG" 2>/dev/null || echo 0)
    [ "${_sz:-0}" -gt 262144 ] 2>/dev/null && mv -f "$LOG" "$LOG.1" 2>/dev/null
    printf '%s %s\n' "$(date '+%m-%d %H:%M:%S')" "$*" >> "$LOG" 2>/dev/null
}

logv() {
    case "${C_log_level:-1}" in
        2|3) log "$@" ;;
    esac
    return 0
}

# ------------------------------------------------------------------- io -----
rd() { [ -r "$1" ] && cat "$1" 2>/dev/null; }

rdf() {  # 无 fork 读首行
    _rdv=""
    IFS= read -r _rdv < "$1" 2>/dev/null || true
    printf '%s' "${_rdv:-}"
}

wr() {
    [ -e "$1" ] || return 1
    printf '%s\n' "$2" > "$1" 2>/dev/null || return 1
    [ -n "${FEATHER_WR_TRACE:-}" ] && printf '%s <- %s\n' "$1" "$2" >> "$FEATHER_WR_TRACE" 2>/dev/null
    return 0
}

wrv() {  # 写 + 回读校验
    wr "$1" "$2" || return 1
    _cur=$(rd "$1")
    case "$_cur" in
        *"$2"*) return 0 ;;
        *) logv "verify-miss $1 want=$2 got=$_cur"; return 1 ;;
    esac
}

# ---------------------------------------------------------------- config ----
# 固定 12 个键，纯 shell 解析（零 fork），缓存到 C_* 变量
cfg_defaults() {
    C_enable=1
    C_mode=auto
    C_intensity=50
    C_auto_game=1
    C_screen_off_saver=1
    C_thread_opt=1
    C_kill_boost=1
    C_io_tune=1
    C_gpu_tune=1
    C_launch_boost=1
    C_boost_ms=4000
    C_log_level=1
    C_hmbird_tune=1
    C_hmbird_coeff=1152,1024
    C_hmbird_high_ratio=56,60
    C_hmbird_reserved=192
    C_hmbird_heavy=0,360,128
    CFG_VER=""
}

cfg_load() {
    _prev_ver=${CFG_VER:-}
    cfg_defaults
    CFG_EXISTS=0
    if [ -f "$CONF" ]; then
        CFG_EXISTS=1
        while IFS= read -r _line || [ -n "$_line" ]; do
            _line=${_line%$'\r'}
            case "$_line" in ''|\#*|';'*) continue ;; esac
            _k=${_line%%=*}
            [ "$_k" = "$_line" ] && continue
            _v=${_line#*=}
            _v=${_v%%#*}
            set -- $_v            # 去空白
            _v=${1:-}
            case "$_k" in
                enable)           C_enable=$_v ;;
                mode)             C_mode=$_v ;;
                intensity)        C_intensity=$_v ;;
                auto_game)        C_auto_game=$_v ;;
                screen_off_saver) C_screen_off_saver=$_v ;;
                thread_opt)       C_thread_opt=$_v ;;
                kill_boost)       C_kill_boost=$_v ;;
                io_tune)          C_io_tune=$_v ;;
                gpu_tune)         C_gpu_tune=$_v ;;
                launch_boost)     C_launch_boost=$_v ;;
                boost_ms)         C_boost_ms=$_v ;;
                log_level)        C_log_level=$_v ;;
                hmbird_tune)      C_hmbird_tune=$_v ;;
                hmbird_coeff)     C_hmbird_coeff=$_v ;;
                hmbird_high_ratio) C_hmbird_high_ratio=$_v ;;
                hmbird_reserved)  C_hmbird_reserved=$_v ;;
                hmbird_heavy)     C_hmbird_heavy=$_v ;;
            esac
            CFG_VER="$CFG_VER$_k=$_v;"
        done < "$CONF"
    fi
    CFG_LOADED=1
    [ "$_prev_ver" = "$CFG_VER" ] && CFG_CHANGED=0 || CFG_CHANGED=1
    return 0
}

cfg_get() {  # $1=key $2=default
    [ "${CFG_LOADED:-0}" = 1 ] || cfg_load
    case "$1" in
        enable)           printf '%s' "${C_enable:-$2}" ;;
        mode)             printf '%s' "${C_mode:-$2}" ;;
        intensity)        printf '%s' "${C_intensity:-$2}" ;;
        auto_game)        printf '%s' "${C_auto_game:-$2}" ;;
        screen_off_saver) printf '%s' "${C_screen_off_saver:-$2}" ;;
        thread_opt)       printf '%s' "${C_thread_opt:-$2}" ;;
        kill_boost)       printf '%s' "${C_kill_boost:-$2}" ;;
        io_tune)          printf '%s' "${C_io_tune:-$2}" ;;
        gpu_tune)         printf '%s' "${C_gpu_tune:-$2}" ;;
        launch_boost)     printf '%s' "${C_launch_boost:-$2}" ;;
        boost_ms)         printf '%s' "${C_boost_ms:-$2}" ;;
        log_level)        printf '%s' "${C_log_level:-$2}" ;;
        hmbird_tune)      printf '%s' "${C_hmbird_tune:-$2}" ;;
        hmbird_coeff)     printf '%s' "${C_hmbird_coeff:-$2}" ;;
        hmbird_high_ratio) printf '%s' "${C_hmbird_high_ratio:-$2}" ;;
        hmbird_reserved)  printf '%s' "${C_hmbird_reserved:-$2}" ;;
        hmbird_heavy)     printf '%s' "${C_hmbird_heavy:-$2}" ;;
        *)                printf '%s' "$2" ;;
    esac
}

cfg_set() {
    _k=$1; _val=$2
    [ -f "$CONF" ] || : > "$CONF"
    _t=$DATA/.cfg.tmp
    awk -F= -v k="$_k" -v v="$_val" '
        BEGIN { done=0 }
        /^[ \t]*[#;]/ { print; next }
        {
            p=index($0,"=")
            if (p>0) {
                key=substr($0,1,p-1); gsub(/[ \t]/,"",key)
                if (key==k) { print k"="v; done=1; next }
            }
            print
        }
        END { if (!done) print k"="v }
    ' "$CONF" > "$_t" 2>/dev/null && mv -f "$_t" "$CONF"
    CFG_LOADED=0
    cfg_load >/dev/null 2>&1
}

# ------------------------------------------------------------- topology -----
detect_topology() {
    POLICIES=""; PRIME_POLICY=""; PRIME_MAX=0; PERF_POLICY=""; PERF_MAX=0
    for p in "$CPU"/cpufreq/policy*; do
        [ -d "$p" ] || continue
        m=$(rdf "$p/cpuinfo_max_freq")
        [ -z "$m" ] && continue
        case "$m" in *[!0-9]*) continue ;; esac
        POLICIES="$POLICIES $p"
        if [ "$m" -gt "$PRIME_MAX" ] 2>/dev/null; then
            PERF_POLICY=$PRIME_POLICY; PERF_MAX=$PRIME_MAX
            PRIME_POLICY=$p; PRIME_MAX=$m
        elif [ "$m" -gt "$PERF_MAX" ] 2>/dev/null; then
            PERF_POLICY=$p; PERF_MAX=$m
        fi
    done
    [ -z "$PERF_POLICY" ] && PERF_POLICY=$PRIME_POLICY
    [ -n "$POLICIES" ] && log_topology_once
}

log_topology_once() {
    [ "${TOPO_LOGGED:-0}" = 1 ] && return 0
    TOPO_LOGGED=1
    log "topo: perf=${PERF_POLICY##*/}($PERF_MAX) prime=${PRIME_POLICY##*/}($PRIME_MAX)"
}

# 取不超过目标值的最高档
freq_le() {
    _p=$1; _t=$2; _lo=$(rdf "$_p/cpuinfo_min_freq")
    _f=$(awk -v t="$_t" 'BEGIN{m=0} {for(i=1;i<=NF;i++){v=$i+0; if(v<=t+0 && v>m) m=v}} END{if(m>0)print m}' \
         "$_p/scaling_available_frequencies" 2>/dev/null)
    [ -z "$_f" ] && _f=$_t
    [ -n "$_lo" ] && [ "$_f" -lt "$_lo" ] 2>/dev/null && _f=$_lo
    printf '%s' "$_f"
}

set_cap() {  # $1=policy $2=percent -> 打印生效频率
    _p=$1; _pct=$2
    _max=$(rdf "$_p/cpuinfo_max_freq"); [ -z "$_max" ] && return 1
    [ "$_pct" -gt 100 ] 2>/dev/null && _pct=100
    [ "$_pct" -lt 5 ] 2>/dev/null && _pct=5
    _t=$(( _max * _pct / 100 ))
    # 绝不抬高系统/其它工具设定的上限：记下的基线就是天花板
    _lim=""
    [ "$_p" = "$PERF_POLICY" ]  && _lim=$SYS_CAP_PERF
    [ "$_p" = "$PRIME_POLICY" ] && _lim=$SYS_CAP_PRIME
    case "$_lim" in ''|*[!0-9]*) _lim="" ;; esac
    [ -n "$_lim" ] && [ "$_t" -gt "$_lim" ] && _t=$_lim
    _f=$(freq_le "$_p" "$_t")
    if wr "$_p/scaling_max_freq" "$_f"; then
        printf '%s' "$_f"
        return 0
    fi
    return 1
}

# 记录“我们动手之前”的上限，作为物理天花板，卸载/退出时按它还原
capture_sys_caps() {
    SYS_CAP_PERF=$(rdf "$PERF_POLICY/scaling_max_freq")
    SYS_CAP_PRIME=$(rdf "$PRIME_POLICY/scaling_max_freq")
}

uncap() {
    for p in $POLICIES; do
        m=$(rdf "$p/cpuinfo_max_freq")
        [ -n "$m" ] && wr "$p/scaling_max_freq" "$m"
    done
}

gov_of() { rdf "$1/scaling_governor"; }

# --------------------------------------------------------------- uclamp -----
set_uclamp_cgrp() {  # $1=group $2=max% [$3=min%]
    _g="$CGRP/$1"
    [ -d "$_g" ] || return 1
    _ok=1
    if [ -n "$2" ] && [ -e "$_g/cpu.uclamp.max" ]; then
        printf 'max %s%%\n' "$2" > "$_g/cpu.uclamp.max" 2>/dev/null || _ok=0
    fi
    if [ -n "$3" ] && [ -e "$_g/cpu.uclamp.min" ]; then
        printf 'min %s%%\n' "$3" > "$_g/cpu.uclamp.min" 2>/dev/null || _ok=0
    fi
    return $((1 - _ok))
}

# ------------------------------------------------------- vendor boost -------
boost_off() {
    _n=0
    for node in \
        /sys/module/msm_performance/parameters/touch_boost \
        /sys/devices/system/cpu/cpu_boost/input_boost_enabled \
        /sys/module/cpu_boost/parameters/input_boost_enabled \
        /sys/module/cpu_boost/parameters/boost_ms \
        /sys/kernel/msm_performance/boost \
        /proc/sys/kernel/sched_boost \
        /sys/module/msm_perf_monitor/parameters/enable \
        /sys/kernel/oplus_boost/enable ; do
        [ -e "$node" ] || continue
        wr "$node" 0 && _n=$((_n + 1))
    done
    BOOST_KILLED=$_n
    log "boost: 关闭 $_n 个厂商加速节点"
}

boost_restore() {
    for node in \
        /sys/module/msm_performance/parameters/touch_boost \
        /sys/devices/system/cpu/cpu_boost/input_boost_enabled \
        /sys/module/cpu_boost/parameters/input_boost_enabled \
        /sys/kernel/msm_performance/boost \
        /sys/module/msm_perf_monitor/parameters/enable ; do
        [ -e "$node" ] && wr "$node" 1
    done
    [ -e /proc/sys/kernel/sched_boost ] && wr /proc/sys/kernel/sched_boost 0
    return 0
}

# ------------------------------------------------------------------ i/o -----
io_tune() {
    for q in /sys/block/sd*/queue /sys/block/mmcblk0/queue /sys/block/dm-0/queue; do
        [ -d "$q" ] || continue
        wr "$q/read_ahead_kb" 256
        wr "$q/iostats" 0
    done
}

# ------------------------------------------------------------------ gpu -----
gpu_tune() {
    for g in /sys/class/kgsl/kgsl-3d0 /sys/class/devfreq/*kgsl*; do
        [ -d "$g" ] || continue
        wr "$g/force_clk_on" 0
        wr "$g/bus_split" 1
    done
}

# ------------------------------------------------------------ detection -----
BRIGHT=""
for _b in "$SYS/class/backlight/panel0-backlight/brightness" \
          "$SYS/class/backlight/panel1-backlight/brightness" \
          "$SYS/class/leds/lcd-backlight/brightness"; do
    [ -r "$_b" ] && { BRIGHT=$_b; break; }
done

screen_on() {
    if [ -n "$BRIGHT" ]; then
        _bl=$(rdf "$BRIGHT")
        case "$_bl" in ''|*[!0-9]*) return 0 ;; esac
        [ "$_bl" -gt 0 ] && return 0
        return 1
    fi
    [ -s "$CGRP/top-app/cgroup.procs" ] && return 0
    return 0
}

# 前台包名：top-app cgroup 首个“像包名”的进程
foreground_pkg() {
    _procs=""
    for f in "$CGRP/top-app/cgroup.procs" /dev/cpuset/top-app/cgroup.procs; do
        [ -r "$f" ] || continue
        _procs=$(head -8 "$f" 2>/dev/null)
        [ -n "$_procs" ] && break
    done
    [ -z "$_procs" ] && return 1
    for pid in $_procs; do
        [ -r "$PROC/$pid/cmdline" ] || continue
        cl=$(tr '\0' '\n' < "$PROC/$pid/cmdline" 2>/dev/null | head -1)
        case "$cl" in
            ''|*:*|*/*) continue ;;
            *.*) printf '%s' "$cl"; return 0 ;;
        esac
    done
    return 1
}

is_game() {
    [ -n "$1" ] || return 1
    [ -f "$GAMES" ] || return 1
    grep -qxF -- "$1" "$GAMES" 2>/dev/null
}

battery_cap()    { rdf "$SYS/class/power_supply/battery/capacity"; }
battery_status() { rdf "$SYS/class/power_supply/battery/status"; }
battery_temp() {
    _t=$(rdf "$SYS/class/power_supply/battery/temp")
    case "$_t" in ''|*[!0-9]*) printf ''; return ;; esac
    printf '%s' $((_t / 10))
}

# --------------------------------------------------------------- hmbird -----
hmbird_detect() {
    HMBIRD_PRESENT=0
    [ -d "$HMBIRD_SYS" ] && HMBIRD_PRESENT=1
    if [ "$HMBIRD_PRESENT" = 0 ] && [ -n "$PRIME_POLICY" ]; then
        case "$(gov_of "$PRIME_POLICY")" in *hmbird*|*scx*) HMBIRD_PRESENT=1 ;; esac
    fi
    [ "$HMBIRD_PRESENT" = 1 ]
}

hmbird_live() {
    for p in $POLICIES; do
        case "$(gov_of "$p")" in *hmbird*|*scx*) return 0 ;; esac
    done
    return 1
}

# 源码依据 vendor/oplus/kernel/cpu/sched_ext/hmbird_II/
#   coefficient     8~4096，默认 1024：perf 值 -> freq 的换算系数
#   perf_high_ratio 默认 60：进入 high-perf 档的利用率门限
#   cpus_reserved   默认 0xf0：留给关键任务的核位图（CPU4-7）
#   heavy_boost     "type bottom_perf weight"，默认 "0 400 140"
hmbird_apply_game() {
    [ "$HMBIRD_PRESENT" = 1 ] || return 1
    [ "${C_hmbird_tune:-1}" = 1 ] || return 0
    _heavy=$(printf '%s' "${C_hmbird_heavy:-0,360,128}" | tr ',' ' ')
    wr "$HMBIRD_SYS/coefficient" "${C_hmbird_coeff:-1152,1024}"
    wr "$HMBIRD_SYS/perf_high_ratio" "${C_hmbird_high_ratio:-56,60}"
    wr "$HMBIRD_SYS/cpus_reserved" "${C_hmbird_reserved:-192}"   # 0xC0 -> 只用 CPU6-7 作保留核
    wr "$HMBIRD_SYS/heavy_boost" "$_heavy"
    for p in $POLICIES; do
        wr "$p/hmbird/target_loads" 88
        wr "$p/hmbird/apply_freq_immediately" 0
    done
    HMBIRD_TUNED=1
    logv "hmbird: game knobs applied"
    return 0
}

hmbird_restore() {
    [ "$HMBIRD_PRESENT" = 1 ] || return 0
    wr "$HMBIRD_SYS/coefficient" "1024,1024"
    wr "$HMBIRD_SYS/perf_high_ratio" "60,60"
    wr "$HMBIRD_SYS/cpus_reserved" 240          # 0xF0 默认
    wr "$HMBIRD_SYS/heavy_boost" "0 400 140"
    for p in $POLICIES; do
        wr "$p/hmbird/target_loads" 90
        wr "$p/hmbird/apply_freq_immediately" 1
    done
    HMBIRD_TUNED=0
    return 0
}

# --------------------------------------------------------------- profile ----
profile_pct() {  # -> "perf% prime%"
    case "$1" in
        sleep)   _pp=30; _ppr=25 ;;
        battery) _pp=50; _ppr=45 ;;
        daily)   _pp=72; _ppr=62 ;;
        game)    _pp=100; _ppr=100 ;;
        perf)    _pp=100; _ppr=100 ;;
        *)       _pp=72; _ppr=62 ;;
    esac
    case "$1" in
        game|perf) : ;;
        *)
            _int=${C_intensity:-50}
            case "$_int" in ''|*[!0-9]*) _int=50 ;; esac
            _delta=$(( (50 - _int) * 2 / 5 ))
            _pp=$(( _pp + _delta )); _ppr=$(( _ppr + _delta ))
            ;;
    esac
    [ "$_pp" -gt 100 ] && _pp=100;  [ "$_pp" -lt 15 ] && _pp=15
    [ "$_ppr" -gt 100 ] && _ppr=100; [ "$_ppr" -lt 15 ] && _ppr=15
    printf '%s %s' "$_pp" "$_ppr"
}

profile_uclamp() {  # -> "bg% fg% top%"
    case "$1" in
        sleep)   printf '20 25 30' ;;
        battery) printf '25 40 60' ;;
        daily)   printf '30 55 100' ;;
        game)    printf '25 45 100' ;;
        perf)    printf '40 70 100' ;;
        *)       printf '30 55 100' ;;
    esac
}

apply_profile() {  # $1=profile
    PROF=$1
    detect_topology
    hmbird_detect
    set -- $(profile_pct "$1")
    _pp=$1; _ppr=$2
    CAP_PERF=""; CAP_PRIME=""
    # 100% 表示“放手”，不去写 scaling_max_freq——避免把别人设的 cap 抬起来
    if [ "$_pp" -lt 100 ] && [ -n "$PERF_POLICY" ]; then
        CAP_PERF=$(set_cap "$PERF_POLICY" "$_pp")
    elif [ -n "$PERF_POLICY" ]; then
        CAP_PERF=$(rdf "$PERF_POLICY/scaling_max_freq")
    fi
    if [ "$_ppr" -lt 100 ] && [ -n "$PRIME_POLICY" ]; then
        CAP_PRIME=$(set_cap "$PRIME_POLICY" "$_ppr")
    elif [ -n "$PRIME_POLICY" ]; then
        CAP_PRIME=$(rdf "$PRIME_POLICY/scaling_max_freq")
    fi

    set -- $(profile_uclamp "$PROF")
    set_uclamp_cgrp background "$1"
    set_uclamp_cgrp foreground "$2"
    set_uclamp_cgrp top-app "$3"
    set_uclamp_cgrp system-background "$(( $1 + 10 ))"

    case "$PROF" in
        game) hmbird_apply_game; gpu_tune ;;
        perf) hmbird_restore;    gpu_tune ;;
        *)    hmbird_restore;    gpu_tune ;;
    esac
    logv "apply: $PROF perf=${_pp}%(${CAP_PERF:-na}) prime=${_ppr}%(${CAP_PRIME:-na})"
}

# ----------------------------------------------------------- thread rules ---
rules_prepare() {
    [ -f "$RULES" ] || : > "$RULES"
    sed -e 's/#.*$//' "$RULES" 2>/dev/null | awk 'NF>=3 {print $1, $2, $3}' > "$DATA/.rules.clean"
    RULES_CLEAN=$DATA/.rules.clean
}

mask_of() {
    case "$1" in
        prime)  printf '0xC0' ;;
        prime1) printf '0x80' ;;
        perf)   printf '0x3F' ;;
        low)    printf '0x0F' ;;
        bg)     printf '0x03' ;;
        *)      printf '' ;;
    esac
}

pin_tid() {  # $1=tid $2=target
    _m=$(mask_of "$2"); [ -z "$_m" ] && return 1
    if [ "$HMBIRD_TUNED" = 1 ]; then
        # hmbird 会把多核掩码扩到全核，必须走它自己的接口
        wrv "$HMBIRD_SYS/prefer_cpu" "$1 $_m" 2>/dev/null && return 0
    fi
    if command -v taskset >/dev/null 2>&1; then
        taskset -p "$_m" "$1" >/dev/null 2>&1 && return 0
    fi
    if [ -x /data/adb/ksu/bin/busybox ]; then
        /data/adb/ksu/bin/busybox taskset -p "$_m" "$1" >/dev/null 2>&1 && return 0
    fi
    if [ -x /data/adb/ap/bin/busybox ]; then
        /data/adb/ap/bin/busybox taskset -p "$_m" "$1" >/dev/null 2>&1 && return 0
    fi
    wr "$HMBIRD_SYS/prefer_cpu" "$1 $_m"
}

# 增量：只处理新出现的线程；匹配用一次 awk 完成（rname 为 ERE）
apply_threads() {  # $1=pid $2=pkg
    _pid=$1; _pkg=$2
    _tdir=$PROC/$_pid/task
    [ -d "$_tdir" ] || return 1
    [ -f "$RULES_CLEAN" ] || rules_prepare
    _seen=$DATA/.seen.$_pid
    if [ "$(rdf "$DATA/.tgpid")" != "$_pid" ]; then
        : > "$_seen"
        printf '%s' "$_pid" > "$DATA/.tgpid"
        THREADS_PINNED=0
    fi

    _list=$DATA/.tlist
    : > "$_list"
    _seenstr=" "
    while IFS= read -r _s; do _seenstr="$_seenstr$_s "; done < "$_seen"
    for d in "$_tdir"/*; do
        [ -d "$d" ] || continue
        _tid=${d##*/}
        case "$_seenstr" in *" $_tid "*) continue ;; esac
        [ -r "$d/comm" ] || continue
        IFS= read -r _comm < "$d/comm" 2>/dev/null || _comm=""
        [ -n "$_comm" ] && printf '%s %s\n' "$_tid" "$_comm" >> "$_list"
        printf '%s\n' "$_tid" >> "$_seen"
    done
    [ -s "$_list" ] || return 0

    awk -v pkg="$_pkg" '
        NR==FNR { rp[++n]=$1; rn[n]=$2; rt[n]=$3; next }
        {
            tid=$1; $1=""; comm=substr($0,2); best=""
            for (i=1;i<=n;i++) {
                if (rp[i]==pkg || rp[i]=="*") {
                    if (comm ~ rn[i]) {
                        if (rp[i]!="*") { best=rt[i]; break }
                        if (best=="") best=rt[i]
                    }
                }
            }
            if (best!="") print tid, best
        }
    ' "$RULES_CLEAN" "$_list" > "$DATA/.tapply" 2>/dev/null

    _new=0
    while read -r _t _tg; do
        [ -n "$_t" ] || continue
        pin_tid "$_t" "$_tg" && _new=$((_new + 1))
    done < "$DATA/.tapply"
    if [ "$_new" -gt 0 ]; then
        THREADS_PINNED=$((THREADS_PINNED + _new))
        logv "threads: +$_new pinned ($_pkg, total $THREADS_PINNED)"
    fi
    return 0
}

# ----------------------------------------------------------------- state ----
state_begin() { : > "$DATA/.state.tmp"; }
state_put()   { printf '%s=%s\n' "$1" "$2" >> "$DATA/.state.tmp"; }
state_flush() {
    printf 'updated=%s\n' "$(date +%s)" >> "$DATA/.state.tmp"
    mv -f "$DATA/.state.tmp" "$STATE" 2>/dev/null
}

# ---------------------------------------------------------------- restore ---
restore_all() {
    detect_topology >/dev/null 2>&1
    for p in "$CPU"/cpufreq/policy*; do
        [ -d "$p" ] || continue
        _v=""
        [ "$p" = "$PERF_POLICY" ]  && _v=$SYS_CAP_PERF
        [ "$p" = "$PRIME_POLICY" ] && _v=$SYS_CAP_PRIME
        case "$_v" in ''|*[!0-9]*) _v=$(rdf "$p/cpuinfo_max_freq") ;; esac
        [ -n "$_v" ] && wr "$p/scaling_max_freq" "$_v"
    done
    set_uclamp_cgrp background 100
    set_uclamp_cgrp foreground 100
    set_uclamp_cgrp top-app 100
    set_uclamp_cgrp system-background 100
    hmbird_restore
    boost_restore
    HMBIRD_TUNED=0
    log "restore: 已还原频率上限与调度参数"
}
