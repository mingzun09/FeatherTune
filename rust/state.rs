// ============================================================================
//  FeatherTune · 羽量调校 —— Rust 版共享状态
//
//  这个文件只放「所有模块都要读写」的东西：路径、配置、拓扑、充电/加速节点、
//  前台检测缓存、hmbird 快照、规则表。C 版是散落的 static 全局，这里收进一个
//  State 结构，用 st() 取可变引用。
//
//  为什么用 static mut 而不是加锁：守护进程是单线程 poll 循环（和 C 版一样），
//  所有入口都在同一个线程里串行调用，加锁只会引入额外开销。
// ============================================================================
#![allow(static_mut_refs)]

// ------------------------------------------------------------------ 常量 ----
pub const APP_VER: &str = "v1.5.0";
pub const APP_VC: i32 = 10100;
pub const MAX_FREQ: usize = 64;
pub const MAX_POL: usize = 4;
pub const MAX_RULES: usize = 64;
pub const MAX_SEEN: usize = 2048;
pub const MAX_APPS: usize = 128;
pub const MAX_LABELS: usize = 512;
pub const HM_SAVE_MAX: usize = 8;

// ------------------------------------------------------------------- 路径 ----
#[derive(Default, Clone)]
pub struct Paths {
    pub data: String,
    pub cpu: String,
    pub cgrp: String,
    pub cpuctl: String,
    pub sys: String,
    pub proc_: String,
    pub hm: String,
    pub conf: String,
    pub games: String,
    pub rules: String,
    pub apps: String,
    pub labels: String,
    pub icons: String,
    pub state: String,
    pub log: String,
    pub pid: String,
    pub sock: String,
    pub web: String,
    pub bright: String,
    pub cset_node: String,
}

fn env_or(k: &str, def: &str) -> String {
    match std::env::var(k) {
        Ok(v) if !v.is_empty() => v,
        _ => def.to_string(),
    }
}

pub fn path_init() -> Paths {
    let mut p = Paths::default();
    p.data = env_or("FEATHER_DATA", "/data/adb/feather");
    p.cpu = env_or("FEATHER_CPU_ROOT", "/sys/devices/system/cpu");

    // cpuset 根：Android 13+ 多数机型在 /dev/cpuset（cgroup v1 语义，节点名 cpus），
    // 少数在 /sys/fs/cgroup。uclamp 则在 /dev/cpuctl（cpu 控制器）。三者不能混用。
    p.cgrp = match std::env::var("FEATHER_CGROUP_ROOT") {
        Ok(v) if !v.is_empty() => v,
        _ => {
            if std::path::Path::new("/dev/cpuset/top-app").exists() {
                "/dev/cpuset".to_string()
            } else {
                "/sys/fs/cgroup".to_string()
            }
        }
    };
    p.cpuctl = match std::env::var("FEATHER_CPUCTL_ROOT") {
        Ok(v) if !v.is_empty() => v,
        _ => {
            if std::path::Path::new("/dev/cpuctl/top-app").exists() {
                "/dev/cpuctl".to_string()
            } else {
                p.cgrp.clone()
            }
        }
    };
    // cgroup v1 里叫 cpus，v2 里叫 cpuset.cpus
    p.cset_node = if std::path::Path::new(&format!("{}/background/cpus", p.cgrp)).exists() {
        "cpus".to_string()
    } else {
        "cpuset.cpus".to_string()
    };

    p.sys = env_or("FEATHER_SYS_ROOT", "/sys");
    p.proc_ = env_or("FEATHER_PROC_ROOT", "/proc");
    p.hm = env_or("FEATHER_HMBIRD_SYS", "/proc/sys/hmbird_II");
    p.conf = format!("{}/config.ini", p.data);
    p.games = format!("{}/games.conf", p.data);
    p.rules = format!("{}/rules.conf", p.data);
    p.apps = format!("{}/apps.conf", p.data);
    p.labels = format!("{}/labels.conf", p.data);
    p.icons = format!("{}/icons", p.data);
    p.state = format!("{}/state", p.data);
    p.log = format!("{}/feather.log", p.data);
    p.pid = format!("{}/featherd-daemon.pid", p.data);
    p.sock = format!("{}/control.sock", p.data);
    p.web = env_or("FEATHER_WEBROOT", "/data/adb/modules/feather_tune/webroot");
    p
}

// ------------------------------------------------------------------ 配置 ----
#[derive(Clone)]
pub struct Cfg {
    pub enable: i32,
    pub intensity: i32,
    pub auto_game: i32,
    pub screen_off_saver: i32,
    pub thread_opt: i32,
    pub kill_boost: i32,
    pub io_tune: i32,
    pub gpu_tune: i32,
    pub launch_boost: i32,
    pub boost_ms: i32,
    pub log_level: i32,
    pub hmbird_tune: i32,
    pub hmbird_reserved: i32,
    pub hmbird_frame: i32,
    pub game_min_pct: i32,
    pub game_bg_cpus: i32,
    pub mode: String,
    pub hm_coeff: String,
    pub hm_high: String,
    pub hm_heavy: String,
    pub charge_ctrl: i32,
    pub charge_knee_soc: i32,
    pub charge_temp_wall: i32,
    pub charge_resume_temp: i32,
    pub accel_ctrl: i32,
    pub accel_tbat: i32,
    pub accel_stop_temp: i32,
    pub accel_resume_temp: i32,
    pub accel_gentle: i32,
    pub accel_gauge: i32,
    pub game_mode: String,
    pub chg_curve: String,
    /// 电芯串联数覆盖：0 = 自动（DT oplus,batt_num → voltage_max 兜底）
    pub battery_cells: i32,
    /// 1 = 守护进程把自己钉在性能簇（cpu0-5），不去唤醒超大核
    pub daemon_pin: i32,
}

impl Default for Cfg {
    fn default() -> Self {
        Cfg {
            enable: 1,
            intensity: 50,
            auto_game: 1,
            screen_off_saver: 1,
            thread_opt: 1,
            kill_boost: 1,
            io_tune: 1,
            gpu_tune: 1,
            launch_boost: 1,
            boost_ms: 4000,
            log_level: 1,
            hmbird_tune: 0,
            hmbird_reserved: 192,
            hmbird_frame: 0,
            game_min_pct: 42,
            game_bg_cpus: 1,
            mode: "auto".into(),
            hm_coeff: "1152,1024".into(),
            hm_high: "56,60".into(),
            hm_heavy: "0,360,128".into(),
            charge_ctrl: 1,
            charge_knee_soc: 67,
            charge_temp_wall: 50,
            charge_resume_temp: 45,
            accel_ctrl: 1,
            accel_tbat: 0,
            accel_stop_temp: 45,
            accel_resume_temp: 42,
            accel_gentle: 0,
            accel_gauge: 0,
            game_mode: "official".into(),
            chg_curve: "36:100,38:88,40:78,42:66,44:55,50:50".into(),
            battery_cells: 0,
            daemon_pin: 1,
        }
    }
}

// ---------------------------------------------------------------- 拓扑 ----
#[derive(Clone)]
pub struct Policy {
    pub path: String,
    pub name: String, // "policy0" 这类，日志里直接用
    pub max: i64,
    pub min: i64,
    pub freq: Vec<i64>,
    pub is_prime: bool,
    /// 这个 policy 实际管的 CPU 列表（来自 affected_cpus）；规则目标据此动态换算掩码
    pub cpus: Vec<u32>,
}

// -------------------------------------------------------------- 充电节点 ----
/// kind: 0 = 未知/µA 节点，1 = mA 节点，2 = charger v2 的 battery/current_now
/// （HIDL 智能充电限流，单位 mA，只在息屏时生效，0 表示解除）
#[derive(Clone, Default)]
pub struct Node {
    pub path: String,
    pub orig: String,
    pub valid: bool,
    pub kind: i32,
}

// ---------------------------------------------------------------- 规则 ----
#[derive(Clone)]
pub struct Rule {
    pub pkg: String,
    pub pat: String,
    pub target: String,
    pub re: crate::util::Regex,
    pub valid: bool,
}

#[derive(Clone)]
pub struct AppRule {
    pub pkg: String,
    pub mode: String,
    pub perf: i32,
    pub prime: i32,
    pub floor: i32,
    pub threads: i32,
}

#[derive(Clone)]
pub struct Label {
    pub pkg: String,
    pub name: String,
}

#[derive(Clone)]
pub struct HmSave {
    pub node: String,
    pub val: String,
}

// ---------------------------------------------------------------- 状态 ----
pub struct State {
    pub p: Paths,
    pub c: Cfg,
    pub log_level: i32,

    pub pol: Vec<Policy>,
    pub idx_perf: isize,
    pub idx_prime: isize,
    pub base_max: [i64; MAX_POL],
    pub base_min: [i64; MAX_POL],
    pub base_ok: bool,

    pub refresh: i64,
    pub model: String,
    /// 厂商营销名（ColorOS: ro.product.marketname，如「一加 15T」）
    pub market: String,
    /// ro.product.device，如 OP5D0B / PLK110
    pub device: String,
    /// ro.board.platform，如 sun / canoe
    pub platform: String,
    pub android: String,

    pub uclamp_ok: bool,
    pub uclamp_touched: bool,
    pub game_extras: bool,
    pub bg_cpus_orig: String,

    // 充电控制
    pub chg_cur: Node,
    pub chg_suspend: Node,
    pub chg_cd: Node,
    pub chg_ready: bool,
    pub chg_active: bool,
    pub chg_suspended: bool,
    pub chg_last_pct: i32,
    pub chg_gear: i32,
    pub chg_orig_cur: i64,
    /// 息屏智能充电接口的基准电流（mA）：本段充电里见过的最大值（高水位）
    pub chg_base_ma: i64,
    /// 本次充电是否已经向 current_now 写过限流（息屏语义专用）
    pub chg_v2_written: i32,

    // 充电加速
    pub acc_shell: Node,
    pub acc_batt: Node,
    pub acc_sub: Node,
    pub acc_ready: bool,
    pub acc_spoofing: bool,
    pub acc_blocked: bool,
    pub acc_written: i32,
    pub acc_dt_val: i32,
    /// 上次读到的 DT 温度分桶表（0.1℃），`charge why` 用它算“真实/伪装落在哪一段”
    pub acc_dt: Vec<i32>,
    /// 0=未知 1=伪装把曲线换到了更高电流段 2=与真实温度同段（无增益）
    pub accel_eff: i32,

    // 功率采样
    pub chg_peak_w10: i64,
    /// 分亮屏/息屏记录峰值：厂商「亮屏限流」只能靠两个数对比看出来
    pub chg_peak_on_w10: i64,
    pub chg_peak_off_w10: i64,
    pub chg_last_log: i64,
    pub cells: i32,

    // hmbird
    pub hb_present: bool,
    pub hb_tuned: bool,
    pub hb_save: Vec<HmSave>,
    pub pm_errno: i32,
    pub pin_kind: i32,

    pub boost_killed: i32,
    /// boost 候选里实际存在的节点数（区分「没有节点」和「有节点但写不进」）
    pub boost_probed: i32,
    /// 1=WALT 2=oplus touch_boost 3=legacy；0=一个都没找到
    pub boost_mode: i32,
    /// 被 boost_off() 改过的节点原值（退出时逐个还原，而不是统一写 1/0）
    pub boost_save: Vec<HmSave>,

    // 线程归位
    pub seen: Vec<i64>,
    pub seen_pid: i64,
    pub threads_pinned: i64,
    pub schedprop_pinned: i64,

    // 前台判定
    pub fg_cands: Vec<String>,
    pub fg_source: i32,
    pub fg_cache: String,
    pub fg_ts: i64,
    pub topapp_sig: u32,
    pub max_pid: i64,
    pub ime_pkg: String,
    pub ime_loaded: bool,

    // 文件表
    pub apps: Vec<AppRule>,
    pub labels: Vec<Label>,
    pub games: Vec<String>,
    pub rules: Vec<Rule>,
    pub apps_mtime: (i64, i64),
    pub labels_mtime: (i64, i64),
    pub games_mtime: (i64, i64),
    pub rules_mtime: (i64, i64),
    pub conf_mtime: (i64, i64),

    pub cur_app: isize, // 当前生效的 apps[] 下标，-1 = 无
    pub prof: String,
    pub cap_perf: i64,
    pub cap_prime: i64,
    pub applied: String,

    pub http_port: i32,
    pub http_errno: i32,
    pub unzip: String,
    /// 所有解压工具都试过且都不可用：别再每取一个图标就重试 8 遍
    pub unzip_none: bool,
    /// 图标缓存版本检查只做一次（换算法时自动清旧缓存）
    pub icon_ver_checked: bool,
}

impl State {
    fn fresh() -> State {
        State {
            p: Paths::default(),
            c: Cfg::default(),
            log_level: 1,
            pol: Vec::new(),
            idx_perf: -1,
            idx_prime: -1,
            base_max: [0; MAX_POL],
            base_min: [0; MAX_POL],
            base_ok: false,
            refresh: 0,
            model: String::new(),
            market: String::new(),
            device: String::new(),
            platform: String::new(),
            android: String::new(),
            uclamp_ok: true,
            uclamp_touched: false,
            game_extras: false,
            bg_cpus_orig: String::new(),
            chg_cur: Node::default(),
            chg_suspend: Node::default(),
            chg_cd: Node::default(),
            chg_ready: false,
            chg_active: false,
            chg_suspended: false,
            chg_last_pct: -1,
            chg_gear: 0,
            chg_orig_cur: 0,
            chg_base_ma: 0,
            chg_v2_written: 0,
            acc_shell: Node::default(),
            acc_batt: Node::default(),
            acc_sub: Node::default(),
            acc_ready: false,
            acc_spoofing: false,
            acc_blocked: false,
            acc_written: 0,
            acc_dt_val: 0,
            acc_dt: Vec::new(),
            accel_eff: 0,
            chg_peak_w10: 0,
            chg_peak_on_w10: 0,
            chg_peak_off_w10: 0,
            chg_last_log: 0,
            cells: 0,
            hb_present: false,
            hb_tuned: false,
            hb_save: Vec::new(),
            pm_errno: 0,
            pin_kind: 0,
            boost_killed: 0,
            boost_probed: 0,
            boost_mode: 0,
            boost_save: Vec::new(),
            seen: Vec::new(),
            seen_pid: -1,
            threads_pinned: 0,
            schedprop_pinned: 0,
            fg_cands: Vec::new(),
            fg_source: 0,
            fg_cache: String::new(),
            fg_ts: 0,
            topapp_sig: 0,
            max_pid: 0,
            ime_pkg: String::new(),
            ime_loaded: false,
            apps: Vec::new(),
            labels: Vec::new(),
            games: Vec::new(),
            rules: Vec::new(),
            apps_mtime: (0, 0),
            labels_mtime: (0, 0),
            games_mtime: (0, 0),
            rules_mtime: (0, 0),
            conf_mtime: (0, 0),
            cur_app: -1,
            prof: String::new(),
            cap_perf: 0,
            cap_prime: 0,
            applied: String::new(),
            http_port: 7891,
            http_errno: 0,
            unzip: String::new(),
            unzip_none: false,
            icon_ver_checked: false,
        }
    }
}

static mut ST: Option<State> = None;

/// 全局状态。调用前必须先 state_init()。
pub fn st() -> &'static mut State {
    unsafe {
        if ST.is_none() {
            ST = Some(State::fresh());
        }
        ST.as_mut().unwrap()
    }
}

pub fn state_init() {
    unsafe {
        ST = Some(State::fresh());
    }
    let p = path_init();
    st().p = p;
}
