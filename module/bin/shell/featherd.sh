#!/system/bin/sh
# ============================================================================
#  FeatherTune daemon — 1Hz 状态机
#  循环里只做：读伪造点文件 + 比字符串；配置只在变更时重新应用
# ============================================================================

BIN_DIR=${0%/*}
[ "$BIN_DIR" = "$0" ] && BIN_DIR=/data/adb/modules/feather_tune/bin
. "$BIN_DIR/lib.sh"

alive() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; }

do_start() {
    mkdir -p "$DATA" 2>/dev/null
    rm -f "$DATA/stop"
    if alive; then echo "already running ($(cat "$PIDFILE"))"; return 0; fi
    if command -v nohup >/dev/null 2>&1; then
        nohup "$0" run </dev/null >/dev/null 2>&1 &
    else
        ( "$0" run </dev/null >/dev/null 2>&1 & )
    fi
    echo $! > "$PIDFILE" 2>/dev/null
    sleep 0.4
    if alive; then echo "started ($(cat "$PIDFILE"))"; else echo "start failed"; fi
}

do_stop() {
    : > "$DATA/stop"
    if [ -f "$PIDFILE" ]; then
        _p=$(cat "$PIDFILE" 2>/dev/null)
        kill "$_p" 2>/dev/null
        sleep 0.5
        kill -9 "$_p" 2>/dev/null
        rm -f "$PIDFILE"
    fi
    restore_all
    echo "stopped"
}

# ------------------------------------------------------------------ loop ----
run_loop() {
    trap 'restore_all; rm -f "$PIDFILE" "$DATA/stop"; exit 0' TERM INT

    # 别被 LMKD / 厂商进程回收器顺手收掉
    printf '%s' -1000 > /proc/self/oom_score_adj 2>/dev/null

    cfg_load
    detect_topology
    capture_sys_caps
    hmbird_detect
    rules_prepare
    log "daemon start: $(getprop ro.product.model 2>/dev/null) android=$(getprop ro.build.version.release 2>/dev/null) pid=$$"

    [ "$C_kill_boost" = 1 ] && boost_off
    [ "$C_io_tune" = 1 ] && io_tune

    tick=0
    cand=""; cand_n=0; cur=""
    last_fg=""; burst_until=0
    last_batt=$(battery_cap); last_stat=$(battery_status)
    case "$last_batt" in ''|0|*[!0-9]*) last_batt=100 ;; esac
    applied_ver=""

    while :; do
        [ -f "$DATA/stop" ] && break
        [ -d "$CPU/cpufreq" ] || { sleep 2; continue; }

        cfg_load
        if [ "$C_enable" != 1 ]; then
            [ -n "$cur" ] && { restore_all; cur=""; applied_ver=""; }
            sleep 3
            continue
        fi

        now=$(date +%s)

        # ---------------- 场景判定 ----------------
        want=""
        if screen_on; then
            fg=$(foreground_pkg 2>/dev/null)
            if [ "$fg" != "$last_fg" ] && [ -n "$fg" ]; then
                last_fg=$fg
                case "$C_launch_boost" in
                    1) burst_until=$(( now + C_boost_ms / 1000 )) ;;
                esac
            fi
            if [ "$C_mode" != auto ]; then
                want=$C_mode
            elif [ "$C_auto_game" = 1 ] && is_game "$fg"; then
                want=game
            else
                want=daily
                if [ $(( tick % 15 )) -eq 0 ]; then
                    last_batt=$(battery_cap); last_stat=$(battery_status)
                fi
                case "$last_batt" in ''|0|*[!0-9]*) last_batt=100 ;; esac
                if [ "$last_batt" -le 20 ] 2>/dev/null && \
                   [ "$last_stat" != "Charging" ] && [ "$last_stat" != "Full" ]; then
                    want=battery
                fi
            fi
        else
            if [ "$C_mode" = auto ] || [ "$C_screen_off_saver" = 1 ]; then
                want=sleep
            else
                want=$C_mode
            fi
        fi

        # ---------------- 迟滞 ----------------
        if [ "$want" = "$cand" ]; then
            cand_n=$((cand_n + 1))
        else
            cand=$want; cand_n=1
        fi
        need=2
        [ "$cur" = game ] && need=4
        [ -z "$cur" ] && need=1

        if { [ "$cand" != "$cur" ] && [ "$cand_n" -ge "$need" ]; } || \
           { [ -n "$cur" ] && [ "$applied_ver" != "$CFG_VER" ]; }; then
            apply_profile "$cand"
            cur=$cand
            applied_ver=$CFG_VER
            [ "$cur" != game ] && { rm -f "$DATA/.tgpid"; rm -f "$DATA"/.seen.* 2>/dev/null; }
        fi

        # ---------------- 启动瞬开（收紧上限后不牺牲手感）----------------
        if [ "$C_launch_boost" = 1 ] && [ -n "$cur" ] && [ "$cur" != game ] && \
           [ "$cur" != perf ] && [ "$now" -lt "$burst_until" ]; then
            [ -n "$PERF_POLICY" ]  && set_cap "$PERF_POLICY" 100 >/dev/null
            [ -n "$PRIME_POLICY" ] && set_cap "$PRIME_POLICY" 92 >/dev/null
        fi

        # ---------------- 上限漂移校正（厂商 thermal/limits 会改写）----------------
        if [ $((tick % 10)) -eq 0 ] && [ -n "$cur" ] && [ "$cur" != game ] && \
           [ "$cur" != perf ] && [ -n "$CAP_PERF" ]; then
            _now_cap=$(rdf "$PERF_POLICY/scaling_max_freq")
            case "$_now_cap" in ''|*[!0-9]*) _now_cap=0 ;; esac
            if [ "$_now_cap" -gt $(( CAP_PERF * 105 / 100 )) ] 2>/dev/null; then
                apply_profile "$cur"
                applied_ver=$CFG_VER
                logv "drift: 上限被外部改写(${_now_cap})，已重新应用"
            fi
        fi

        # ---------------- 线程归位（仅游戏）----------------
        if [ "$cur" = game ] && [ "$C_thread_opt" = 1 ]; then
            gpid=""
            for pid in $(head -4 "$CGRP/top-app/cgroup.procs" 2>/dev/null); do
                [ -r "$PROC/$pid/cmdline" ] || continue
                cl=$(tr '\0' '\n' < "$PROC/$pid/cmdline" 2>/dev/null | head -1)
                [ "$cl" = "$last_fg" ] && { gpid=$pid; break; }
            done
            [ -n "$gpid" ] && apply_threads "$gpid" "$last_fg"
        fi

        # ---------------- 状态落盘 ----------------
        state_begin
        state_put mode "$C_mode"
        state_put profile "$cur"
        state_put screen "$(screen_on && echo 1 || echo 0)"
        state_put fg "$last_fg"
        state_put game "$([ "$cur" = game ] && echo 1 || echo 0)"
        state_put battery "$last_batt"
        state_put temp "$(battery_temp)"
        state_put cap_perf "$CAP_PERF"
        state_put cap_prime "$CAP_PRIME"
        state_put max_perf "$PERF_MAX"
        state_put max_prime "$PRIME_MAX"
        state_put gov_perf "$(gov_of "$PERF_POLICY")"
        state_put gov_prime "$(gov_of "$PRIME_POLICY")"
        state_put hmbird "$HMBIRD_PRESENT"
        state_put hmbird_live "$(hmbird_live && echo 1 || echo 0)"
        state_put hmbird_tuned "$HMBIRD_TUNED"
        state_put threads "$THREADS_PINNED"
        state_put boost_killed "$BOOST_KILLED"
        state_put intensity "$C_intensity"
        state_put pid "$$"
        state_flush

        tick=$((tick + 1))
        if screen_on; then sleep 1; else sleep 3; fi
    done

    restore_all
    rm -f "$PIDFILE"
    log "daemon exit"
}

case "$1" in
    start)  do_start ;;
    stop)   do_stop ;;
    run)    run_loop ;;
    status) alive && echo "running $(cat "$PIDFILE")" || echo "stopped" ;;
    *)      echo "usage: $0 start|stop|run|status" ;;
esac
