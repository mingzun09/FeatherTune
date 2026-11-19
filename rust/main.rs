// ============================================================================
//  FeatherTune · 羽量调校 —— Rust 版守护进程（单线程 poll 循环）
//
//  Target : OnePlus 15 / SM8850（2x Oryon Prime + 6x Oryon Performance）
//  Host   : Android 15~17 arm64 · KernelSU / Magisk / APatch
//
//  与 C 版（src/feather.c）行为逐字对齐：命令输出、state 文件、sysfs 写入点、
//  日志文案都保持原样，测试套件（tests/run_tests_wsl.sh）可直接对着本二进制跑。
//
//  为什么重写成 Rust：
//   1. 所有路径/字符串都是 String，没有 PATH_MAX 截断和 snprintf 返回值陷阱
//   2. 文件句柄、socket 都是 RAII，不会漏 close（C 版 CLOEXEC 漏一处就泄漏）
//   3. 编译期就能挡住「格式串和参数类型不匹配」这类事故（C 版有过 %d 传 long）
//   4. 仍然是静态链接的单文件、零 fork、零外部依赖（正则自己实现，不用 crate）
// ============================================================================
#![allow(static_mut_refs)]

mod cfg;
mod charge;
mod cmd;
mod cpu;
mod net;
mod state;
mod util;

use std::fs;
use std::io::{Read, Write};
use std::os::unix::io::AsRawFd;
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, Ordering};

use cfg::*;
use charge::*;
use cmd::*;
use cpu::*;
use state::st;
use util::*;

static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn on_sig(_s: i32) {
    STOP.store(true, Ordering::SeqCst);
}

extern "C" {
    fn signal(sig: i32, handler: usize) -> usize;
}

// ---------------------------------------------------------------- daemon ----
fn run_daemon() -> i32 {
    // 单实例：多份守护进程会互相抢 socket/state，表现就是"有时连得上有时连不上"
    let lockp = format!("{}/.daemon.lock", st().p.data);
    mkdir_p(&st().p.data);
    // 注意：句柄必须活到函数结束，File 一 drop 锁就释放了
    let lock_file = fs::OpenOptions::new()
        .create(true)
        .read(true)
        .write(true)
        .open(&lockp)
        .ok();
    if let Some(f) = lock_file.as_ref() {
        if unsafe { flock(f.as_raw_fd(), LOCK_EX | LOCK_NB) } != 0 {
            eprintln!("featherd-daemon: already running, exit");
            // service.sh 的看护循环约定：rc=3 表示「已有实例」→ 停止重启，避免热循环
            return 3;
        }
    }

    mkdir_p(&st().p.data);
    log_rotate();
    // WebUI 的 fetch 超时 / curl 提前退出会让我们写已关闭的 socket，
    // 默认动作是 SIGPIPE 直接杀进程 → 忽略它
    unsafe {
        signal(SIGPIPE, 1); // SIG_IGN
        signal(SIGTERM, on_sig as extern "C" fn(i32) as usize);
        signal(SIGINT, on_sig as extern "C" fn(i32) as usize);
        signal(SIGHUP, on_sig as extern "C" fn(i32) as usize);
    }

    if let Ok(mut f) = fs::File::create("/proc/self/oom_score_adj") {
        let _ = f.write_all(b"-1000");
    }

    cfg_load();
    detect_paths();
    detect_topology();
    capture_syscaps();
    detect_refresh();
    hmbird_detect();
    games_load();
    rules_load();
    if st().c.daemon_pin != 0 {
        pin_self_to_perf();
    }

    logmsg(&format!(
        "daemon start {}: perf={}({}) prime={}({}) hmbird={} pid={}",
        state::APP_VER,
        if st().idx_perf >= 0 {
            st().pol[st().idx_perf as usize].name.clone()
        } else {
            "-".into()
        },
        if st().idx_perf >= 0 {
            st().pol[st().idx_perf as usize].max
        } else {
            0
        },
        if st().idx_prime >= 0 {
            st().pol[st().idx_prime as usize].name.clone()
        } else {
            "-".into()
        },
        if st().idx_prime >= 0 {
            st().pol[st().idx_prime as usize].max
        } else {
            0
        },
        if st().hb_present { 1 } else { 0 },
        unsafe { getpid() }
    ));

    if st().c.kill_boost != 0 {
        boost_off();
    }
    if st().c.io_tune != 0 {
        io_tune();
    }

    let lsock = net::sock_listen();
    let mut hsock = net::http_listen();
    match hsock {
        None => logmsg(&format!(
            "http: 端口 {} 绑定失败 errno={}（WebUI 暂走 ksu.exec，稍后自动重试）",
            st().http_port,
            st().http_errno
        )),
        Some(_) => logmsg(&format!("http: 已监听 127.0.0.1:{}", st().http_port)),
    }
    if lsock.is_none() {
        logmsg("socket: control.sock 绑定失败，CLI 将退回本地计算");
    }
    {
        let self_pid = unsafe { getpid() };
        let _ = fs::write(&st().p.pid.clone(), format!("{}\n", self_pid));
    }

    let mut cur = String::new();
    let mut cand = String::new();
    let mut cand_n = 0;
    let mut tick: i64 = 0;
    let mut last_fg = String::new();
    let mut burst_until: i64 = 0;
    let mut last_batt: i64 = 100;
    let start_ts = now_s();
    let mut last_stat = String::new();
    let interval_ms = 1000;

    while !STOP.load(Ordering::SeqCst) {
        let ufd = lsock.as_ref().map(|l| l.as_raw_fd());
        let hfd = hsock.as_ref().map(|l| l.as_raw_fd());
        let (u_ready, h_ready) = net::wait_readable(ufd, hfd, interval_ms);
        if u_ready {
            if let Some(l) = lsock.as_ref() {
                if let Ok((s, _)) = l.accept() {
                    net::sock_handle(s);
                }
            }
        }
        if h_ready {
            if let Some(l) = hsock.as_ref() {
                if let Ok((s, _)) = l.accept() {
                    net::http_handle(s);
                }
            }
        }
        if STOP.load(Ordering::SeqCst) {
            break;
        }

        // HTTP 绑定失败后定期重试：旧连接会留下 TIME_WAIT
        if hsock.is_none() && (tick % 15) == 7 {
            hsock = net::http_listen();
            if hsock.is_some() {
                logmsg(&format!(
                    "http: 重试成功，已监听 127.0.0.1:{}",
                    st().http_port
                ));
            }
        }

        // 命令里刚应用过 profile：立即同步状态，UI 不用等下一拍
        if !st().applied.is_empty() {
            cur = st().applied.clone();
            cand = st().applied.clone();
            cand_n = 99;
            st().applied.clear();
            state_write(
                &cur,
                &last_fg,
                cur == "game",
                last_batt,
                battery_temp(),
                screen_on(),
                now_s() - start_ts,
            );
        }

        let changed = cfg_load();
        apps_load();
        if st().c.enable == 0 {
            if !cur.is_empty() {
                restore_all();
                cur.clear();
            }
            tick += 1;
            continue;
        }

        let now = now_s();
        let scr = screen_on();
        let (want, fg) = decide_now();
        if scr && !fg.is_empty() && fg != last_fg {
            logmsg(&format!(
                "app: 检测到切换软件 {} -> {}",
                if last_fg.is_empty() { "(无)" } else { &last_fg },
                fg
            ));
            last_fg = fg.clone();
            if st().c.launch_boost != 0 {
                burst_until = now + (st().c.boost_ms / 1000) as i64;
                logmsg(&format!(
                    "app: 临时还原频率上限与调度参数 {}ms（{}%/{}%，期间不压 uclamp）",
                    st().c.boost_ms,
                    100,
                    92
                ));
            }
        }
        if !scr {
            last_fg.clear();
        }
        if tick % 5 == 0 {
            let b = battery_cap();
            if b > 0 {
                last_batt = b;
            }
            last_stat = battery_status();
        }

        if want == cand {
            cand_n += 1;
        } else {
            cand = want.clone();
            cand_n = 1;
        }

        // 进游戏要快，出游戏要稳
        let mut need = 2;
        if cand == "game" {
            need = 1;
        } else if cur == "game" {
            need = 3;
        }
        if cur.is_empty() {
            need = 1;
        }
        // 手动指定模式时不做防抖
        if st().c.mode != "auto" {
            need = 1;
        }

        if (cand != cur && cand_n >= need) || (!cur.is_empty() && changed) {
            let why = if cand != cur { "场景切换" } else { "配置变更" };
            let prev = if cur.is_empty() {
                "-".to_string()
            } else {
                cur.clone()
            };
            apply_profile(&cand);
            logmsg(&format!(
                "switch: {} -> {}  因{} (fg={}, {} 拍确认, 上限 {}/{})",
                prev,
                cand,
                why,
                if last_fg.is_empty() { "-" } else { &last_fg },
                cand_n,
                st().cap_perf,
                st().cap_prime
            ));
            cur = cand.clone();
            if cur != "game" {
                st().seen_pid = -1;
                st().seen.clear();
            }
        }

        if st().c.launch_boost != 0
            && !cur.is_empty()
            && cur != "game"
            && cur != "perf"
            && now < burst_until
        {
            set_cap(st().idx_perf, 100);
            set_cap(st().idx_prime, 92);
        }

        if tick % 10 == 0 && !cur.is_empty() && cur != "game" && cur != "perf" && st().cap_perf > 0 {
            let nowcap = policy_cap_now(st().idx_perf);
            if nowcap > st().cap_perf * 105 / 100 {
                apply_profile(&cur);
                logv(&format!("drift: 上限被外部改写({})，已重新应用", nowcap));
            }
        }

        charge_tick();
        {
            // 充电加速：用真实温度做我们自己那道闸，伪装只影响厂商侧
            let charging = battery_status().contains("Charging");
            accel_tick(battery_temp(), charging);
        }
        chg_power_tick(tick);

        if cur == "game" && st().c.thread_opt != 0 && st().c.game_mode == "aggressive" {
            let pid = find_game_pid(&last_fg);
            if pid > 0 {
                apply_threads(pid, &last_fg);
            }
        }

        if tick % 2 == 0 {
            state_write(
                &cur,
                &last_fg,
                cur == "game",
                last_batt,
                battery_temp(),
                scr,
                now - start_ts,
            );
        }

        tick += 1;
        if tick % 60 == 0 {
            mkdir_p(&st().p.data); // 目录被清理时自愈
        }
    }

    restore_all();
    let _ = fs::remove_file(&st().p.sock);
    let _ = fs::remove_file(&st().p.pid);
    let _ = &last_stat;
    logmsg("daemon exit");
    0
}

// ------------------------------------------------------------------- CLI ----
fn sock_client(cmd: &str) -> Option<String> {
    let p = st().p.sock.clone();
    let mut s = UnixStream::connect(&p).ok()?;
    if s.write_all(format!("{}\n", cmd).as_bytes()).is_err() {
        return None;
    }
    let mut out = String::new();
    let _ = s.read_to_string(&mut out);
    if out.is_empty() {
        None
    } else {
        Some(out)
    }
}

fn cli_daemonize() -> i32 {
    let pid = rdnum(&st().p.pid);
    if pid_alive(pid) {
        println!("already running ({})", pid);
        return 0;
    }
    let p = unsafe { fork() };
    if p < 0 {
        return 1;
    }
    if p > 0 {
        std::thread::sleep(std::time::Duration::from_millis(350));
        let np = rdnum(&st().p.pid);
        if np > 0 && pid_alive(np) {
            println!("started ({})", np);
            return 0;
        }
        println!("start failed");
        return 1;
    }
    unsafe {
        setsid();
        let _ = chdir(cstr("/").as_ptr());
        if let Ok(f) = fs::OpenOptions::new().read(true).write(true).open("/dev/null") {
            let fd = f.as_raw_fd();
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            std::mem::forget(f);
        }
        _exit(run_daemon());
    }
}

fn do_stop() {
    let pid = rdnum(&st().p.pid);
    if pid > 0 {
        unsafe {
            kill(pid as CInt, SIGTERM);
        }
        for _ in 0..20 {
            if !pid_alive(pid) {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(100));
        }
        unsafe {
            kill(pid as CInt, SIGKILL);
        }
    }
    let _ = fs::remove_file(&st().p.pid);
    detect_topology();
    capture_syscaps();
    hmbird_detect();
    restore_all();
}

fn do_restore() {
    detect_topology();
    capture_syscaps();
    hmbird_detect();
    restore_all();
}

fn do_reset() {
    let moddir = std::env::var("FEATHER_MODDIR")
        .unwrap_or_else(|_| "/data/adb/modules/feather_tune".to_string());
    for f in ["config.ini", "games.conf", "rules.conf"] {
        let src = format!("{}/etc/{}", moddir, f);
        let dst = format!("{}/{}", st().p.data, f);
        if let Ok(b) = fs::read(&src) {
            let _ = fs::write(&dst, b);
        }
    }
    println!("ok=reset");
}

fn cli_main() -> i32 {
    let argv: Vec<String> = std::env::args().collect();
    if argv.len() < 2 {
        println!("usage: feather <run|start|stop|restart|status|set|mode|doctor|pkg|apps|log|restore|reset|charge|boost|masks|version>");
        return 2;
    }
    let c = argv[1].as_str();
    match c {
        "run" => return run_daemon(),
        "start" => return cli_daemonize(),
        "stop" => {
            do_stop();
            println!("stopped");
            return 0;
        }
        "restart" => {
            do_stop();
            std::thread::sleep(std::time::Duration::from_millis(400));
            return cli_daemonize();
        }
        "restore" => {
            do_restore();
            println!("ok=restored");
            return 0;
        }
        "reset" => {
            do_reset();
            return 0;
        }
        _ => {}
    }

    // 其余命令：拼成一行发给 daemon
    let line = argv[1..].join(" ");
    if let Some(out) = sock_client(&line) {
        print!("{}", out);
        return 0;
    }
    // daemon 没起来：本地直接算（保证 WebUI 在守护进程挂掉时也能显示信息）
    let out = run_cmd(&line);
    print!(
        "{}",
        if out.is_empty() {
            "err=daemon_down\n".to_string()
        } else {
            out
        }
    );
    0
}

fn main() {
    // ps 里显示成 featherd-daemon，而不是含义模糊的 feather
    unsafe {
        let name = cstr("featherd-daemon");
        prctl(PR_SET_NAME, name.as_ptr() as u64, 0, 0, 0);
    }
    state::state_init();
    read_buildprops();
    init_tz(); // 日志时间戳跟手机一致（/data/adb 下默认是 UTC）
    std::process::exit(cli_main());
}
