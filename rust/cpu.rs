// ============================================================================
//  cpu.rs —— 拓扑识别 / 频率上限 / uclamp / 场景档位 / hmbird / 前台判定 / 线程归位
// ============================================================================
#![allow(static_mut_refs)]

use std::fs;

use crate::cfg::*;
use crate::state::{st, HmSave, Policy, MAX_FREQ, MAX_POL, MAX_SEEN};
use crate::util::*;

// ---------------------------------------------------------------- 拓扑 ----
pub fn detect_topology() {
    let dir = format!("{}/cpufreq", st().p.cpu);
    let mut pols: Vec<Policy> = Vec::new();
    if let Ok(rd) = fs::read_dir(&dir) {
        let mut names: Vec<String> = rd
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.starts_with("policy"))
            .collect();
        names.sort();
        for name in names {
            if pols.len() >= MAX_POL {
                break;
            }
            let path = format!("{}/{}", dir, name);
            let max = rdnum(&format!("{}/cpuinfo_max_freq", path));
            if max <= 0 {
                continue;
            }
            let min = rdnum(&format!("{}/cpuinfo_min_freq", path));
            let mut freq: Vec<i64> = Vec::new();
            if let Some(s) = rdline(&format!("{}/scaling_available_frequencies", path)) {
                for t in s.split([' ', '\t']) {
                    let v = parse_leading_num(t);
                    if v > 0 {
                        freq.push(v);
                    }
                }
            }
            if freq.is_empty() {
                freq.push(max);
            }
            freq.sort_unstable_by(|a, b| b.cmp(a));
            freq.truncate(MAX_FREQ);
            pols.push(Policy {
                path,
                name,
                max,
                min,
                freq,
                is_prime: false,
            });
        }
    }
    st().pol = pols;

    let mut idx_prime: isize = -1;
    for i in 0..st().pol.len() {
        if idx_prime < 0 || st().pol[i].max > st().pol[idx_prime as usize].max {
            idx_prime = i as isize;
        }
    }
    let mut idx_perf: isize = -1;
    for i in 0..st().pol.len() {
        if i as isize != idx_prime
            && (idx_perf < 0 || st().pol[i].max > st().pol[idx_perf as usize].max)
        {
            idx_perf = i as isize;
        }
    }
    if idx_prime >= 0 {
        st().pol[idx_prime as usize].is_prime = true;
    }
    if idx_perf < 0 {
        idx_perf = idx_prime;
    }
    st().idx_prime = idx_prime;
    st().idx_perf = idx_perf;
}

/// 基线只快照一次（守护进程启动时），之后谁改都不影响「我们不动手时的天花板」
pub fn capture_syscaps() {
    if st().base_ok {
        return;
    }
    let n = st().pol.len();
    for i in 0..n {
        let p = st().pol[i].path.clone();
        st().base_max[i] = rdnum(&format!("{}/scaling_max_freq", p));
        st().base_min[i] = rdnum(&format!("{}/scaling_min_freq", p));
    }
    st().base_ok = n > 0;
    let (m, mn) = (st().base_max[0], st().base_min[0]);
    logv(&format!("baseline: n={} max0={} min0={}", n, m, mn));
}

pub fn freq_le(idx: usize, target: i64) -> i64 {
    let p = &st().pol[idx];
    let mut best = 0;
    for &f in p.freq.iter() {
        if f <= target && f > best {
            best = f;
        }
    }
    if best == 0 {
        best = target;
    }
    if p.min > 0 && best < p.min {
        best = p.min;
    }
    best
}

/// 取不低于目标的**最低**档（用于抬底频）
pub fn freq_ge(idx: usize, target: i64) -> i64 {
    let p = &st().pol[idx];
    let mut best = 0;
    for &f in p.freq.iter() {
        if f >= target && (best == 0 || f < best) {
            best = f;
        }
    }
    if best != 0 {
        best
    } else {
        p.max
    }
}

pub fn policy_cap_now(idx: isize) -> i64 {
    if idx < 0 {
        return 0;
    }
    let v = rdnum(&format!("{}/scaling_max_freq", st().pol[idx as usize].path));
    if v > 0 {
        v
    } else {
        0
    }
}

/// 设上限；返回实际生效频率（0=失败）
pub fn set_cap(idx: isize, pct: i32) -> i64 {
    if idx < 0 {
        return 0;
    }
    let i = idx as usize;
    let mut pct = pct;
    if pct > 100 {
        pct = 100;
    }
    if pct < 5 {
        pct = 5;
    }
    let max = st().pol[i].max;
    let mut t = max / 100 * pct as i64;
    if st().base_ok && i < MAX_POL && st().base_max[i] > 0 && t > st().base_max[i] {
        t = st().base_max[i]; // 绝不抬到基线之上
    }
    let f = freq_le(i, t);
    let path = format!("{}/scaling_max_freq", st().pol[i].path);
    if wrl(&path, f) {
        f
    } else {
        0
    }
}

pub fn gov_of(idx: isize) -> String {
    if idx < 0 {
        return String::new();
    }
    rdline(&format!("{}/scaling_governor", st().pol[idx as usize].path)).unwrap_or_default()
}

pub fn set_policy_min(idx: isize, f: i64) {
    if idx < 0 {
        return;
    }
    let path = format!("{}/scaling_min_freq", st().pol[idx as usize].path);
    wrl(&path, f);
}

pub fn detect_refresh() {
    if st().refresh != 0 {
        return;
    }
    for c in ["/class/graphics/fb0/modes", "/class/graphics/fb0/mode"] {
        let p = format!("{}{}", st().p.sys, c);
        if let Some(b) = rdline(&p) {
            if let Some(dash) = b.rfind('-') {
                let v = parse_leading_num(&b[dash + 1..]);
                if (30..=240).contains(&v) {
                    st().refresh = v;
                }
            }
        }
        if st().refresh != 0 {
            return;
        }
    }
    // Android 上 fb0/modes 多数不可读，真正可靠的是 DRM 的模式表：
    //   1272x2772x165cmd   1272x2772x120cmd   1272x2772x60cmd
    for c in [
        "/class/drm/card0-DSI-1/modes",
        "/class/drm/card0-DSI-1/mode",
        "/class/drm/card0/modes",
    ] {
        let p = format!("{}{}", st().p.sys, c);
        let text = match read_all(&p) {
            Some(t) => t,
            None => continue,
        };
        let mut best = 0;
        for line in text.lines() {
            if let Some(x1) = line.find('x') {
                if let Some(x2) = line[x1 + 1..].find('x') {
                    let v = parse_leading_num(&line[x1 + 1 + x2 + 1..]);
                    if (30..=240).contains(&v) && v > best {
                        best = v;
                    }
                }
            }
        }
        if best > 0 {
            st().refresh = best;
            return;
        }
    }
}

// --------------------------------------------------------------- 屏幕 ----
pub fn detect_paths() {
    if let Ok(hp) = std::env::var("FEATHER_HTTP_PORT") {
        if let Ok(v) = hp.parse::<i32>() {
            if v > 0 && v < 65536 {
                st().http_port = v;
            }
        }
    }
    for c in [
        "/class/backlight/panel0-backlight/brightness",
        "/class/backlight/panel1-backlight/brightness",
        "/class/leds/lcd-backlight/brightness",
    ] {
        let p = format!("{}{}", st().p.sys, c);
        if readable(&p) {
            st().p.bright = p;
            break;
        }
    }
}

pub fn screen_on() -> bool {
    let b = st().p.bright.clone();
    if !b.is_empty() {
        let v = rdnum(&b);
        if v < 0 {
            return true;
        }
        return v > 0;
    }
    true
}

// -------------------------------------------------------------- uclamp ----
pub fn set_uclamp(group: &str, maxpct: i32) {
    if !st().uclamp_ok {
        return;
    }
    let path = format!("{}/{}/cpu.uclamp.max", st().p.cpuctl, group);
    if !writable(&path) {
        st().uclamp_ok = false;
        logv(&format!("uclamp: {} 不可写，停用", path));
        return;
    }
    let val = maxpct as i64 * 1024 / 100;
    if !wrl(&path, val) {
        st().uclamp_ok = false;
        logv("uclamp: 写入失败，停用");
        return;
    }
    if let Some(back) = rdline(&path) {
        if !back.is_empty() {
            let mut r: f64 = back.parse().unwrap_or(0.0);
            if r > 200.0 {
                r = r / 1024.0 * 100.0; // 读回 307 → 30%
            }
            if maxpct >= 5 && r < 2.0 {
                st().uclamp_ok = false;
                logv(&format!(
                    "uclamp: 读回 '{}' 与写入 {} 不符，格式不兼容，停用",
                    back, val
                ));
                return;
            }
        }
    }
    st().uclamp_touched = true;
}

pub fn uclamp_restore() {
    if !st().uclamp_touched {
        return;
    }
    for g in ["top-app", "foreground", "background"] {
        let path = format!("{}/{}/cpu.uclamp.max", st().p.cpuctl, g);
        wrp(&path, "max");
    }
    st().uclamp_touched = false;
}

// ------------------------------------------------------------- profiles ----
pub const PROTOS: [(&str, i32, i32); 5] = [
    ("sleep", 30, 25),
    ("battery", 50, 45),
    ("daily", 72, 62),
    ("game", 100, 100),
    ("perf", 100, 100),
];

pub fn profile_pct(name: &str) -> (i32, i32) {
    let mut pp = 72;
    let mut pr = 62;
    for (n, p, r) in PROTOS {
        if n == name {
            pp = p;
            pr = r;
            break;
        }
    }
    if name != "game" && name != "perf" {
        let mut iv = st().c.intensity;
        if iv < 0 {
            iv = 0;
        }
        if iv > 100 {
            iv = 100;
        }
        let delta = (50 - iv) * 2 / 5;
        pp += delta;
        pr += delta;
    }
    pp = pp.clamp(15, 100);
    pr = pr.clamp(15, 100);
    (pp, pr)
}

pub fn profile_uclamp(name: &str) -> (i32, i32, i32) {
    match name {
        "sleep" => (20, 25, 30),
        "battery" => (25, 40, 60),
        "game" => (25, 45, 100),
        "perf" => (40, 70, 100),
        _ => (30, 55, 100),
    }
}

fn bg_cpus_path() -> String {
    format!("{}/background/{}", st().p.cgrp, st().p.cset_node)
}

pub fn apply_game_extras() {
    if st().game_extras {
        return;
    }
    // 1) 给性能簇一个地板，掐掉 384/537MHz 这种深谷
    let ip = st().idx_perf;
    if st().c.game_min_pct > 0 && ip >= 0 {
        let max = st().pol[ip as usize].max;
        let t = max / 100 * st().c.game_min_pct as i64;
        let f = freq_ge(ip as usize, t);
        set_policy_min(ip, f);
    }
    // 2) 后台 cgroup 赶出超大核
    if st().c.game_bg_cpus != 0 {
        let p = bg_cpus_path();
        if let Some(cur) = rdline(&p) {
            if st().bg_cpus_orig.is_empty() {
                st().bg_cpus_orig = cur.clone();
            }
            if cur != "0-3" {
                wrp(&p, "0-3");
            }
        }
    }
    st().game_extras = true;
    logv(&format!(
        "game extras: min_perf={}% bg_cpus=0-3",
        st().c.game_min_pct
    ));
}

pub fn restore_game_extras() {
    if !st().game_extras {
        logv("extras: 已是干净状态，跳过还原");
        return;
    }
    let ip = st().idx_perf;
    if ip >= 0 {
        let v = if st().base_ok && st().base_min[ip as usize] > 0 {
            st().base_min[ip as usize]
        } else {
            st().pol[ip as usize].min
        };
        set_policy_min(ip, v);
    }
    if st().c.game_bg_cpus != 0 && !st().bg_cpus_orig.is_empty() {
        let p = bg_cpus_path();
        let o = st().bg_cpus_orig.clone();
        wrp(&p, &o);
    }
    st().game_extras = false;
    let v = if ip >= 0 {
        st().base_min[ip as usize]
    } else {
        -1
    };
    logv(&format!("extras: 已还原 min_perf={}", v));
}

// -------------------------------------------------------------- hmbird ----
pub fn hmbird_detect() {
    st().hb_present = is_dir(&st().p.hm.clone());
    if !st().hb_present {
        let g = gov_of(st().idx_prime);
        if g.contains("hmbird") || g.contains("scx") {
            st().hb_present = true;
        }
    }
}

pub fn hmbird_live() -> bool {
    for i in 0..st().pol.len() {
        let g = gov_of(i as isize);
        if g.contains("hmbird") || g.contains("scx") {
            return true;
        }
    }
    false
}

fn hm_write(node: &str, val: &str) {
    let path = format!("{}/{}", st().p.hm, node);
    wrp(&path, val);
}

fn hm_save_once(node: &str) {
    if st().hb_save.iter().any(|s| s.node == node) {
        return;
    }
    if st().hb_save.len() >= crate::state::HM_SAVE_MAX {
        return;
    }
    let path = format!("{}/{}", st().p.hm, node);
    if let Some(v) = rdline(&path) {
        st().hb_save.push(HmSave {
            node: node.to_string(),
            val: v,
        });
    }
}

fn hm_write_keep(node: &str, val: &str) {
    hm_save_once(node);
    hm_write(node, val);
}

/// hmbird 的 show 回调把条目打成 "<leader>::<comm>::<hex>"，只能按值是否出现判断
pub fn hm_selftest(node: &str, hexval: &str, me: i64) -> bool {
    let p = format!("{}/{}", st().p.hm, node);
    let v = format!("{} {}", me, hexval);
    if !wrp(&p, &v) {
        return false;
    }
    let hv = u64::from_str_radix(
        hexval.trim_start_matches("0x").trim_start_matches("0X"),
        16,
    )
    .unwrap_or(0);
    let want = if node == "prefer_preempt" {
        format!("::{}", hv) // %lu
    } else {
        format!("::{:x}", hv) // %lx
    };
    let ok = read_all(&p).map(|b| b.contains(&want)).unwrap_or(false);
    wrp(&p, &format!("{} 0", me)); // 清理自己写入的条目
    ok
}

pub fn hmbird_apply_frame_only() {
    if !st().hb_present {
        return;
    }
    if st().c.hmbird_frame < 0 {
        return;
    }
    let fps = if st().c.hmbird_frame > 0 {
        st().c.hmbird_frame as i64
    } else {
        st().refresh
    };
    if fps <= 0 {
        return;
    }
    let p = format!("{}/frame_per_sec", st().p.hm);
    hm_save_once("frame_per_sec");
    if wrverify(&p, &format!("{}", fps)) {
        logv(&format!("hmbird: frame_per_sec={}（light 模式，其余不动）", fps));
    }
}

pub fn hmbird_apply_game() {
    if !st().hb_present {
        return;
    }
    // 帧率窗口对齐面板刷新率（纯收益，不受 hmbird_tune 影响）
    if st().c.hmbird_frame >= 0 {
        let fps = if st().c.hmbird_frame > 0 {
            st().c.hmbird_frame as i64
        } else {
            st().refresh
        };
        if fps > 0 {
            let fp = format!("{}/frame_per_sec", st().p.hm);
            hm_save_once("frame_per_sec");
            if wrverify(&fp, &format!("{}", fps)) {
                logv(&format!("hmbird: frame_per_sec={}", fps));
            }
        }
    }
    if st().c.hmbird_tune == 0 {
        return;
    }
    let heavy = st().c.hm_heavy.replace(',', " ");
    let coeff = st().c.hm_coeff.clone();
    let high = st().c.hm_high.clone();
    let res = format!("{}", st().c.hmbird_reserved);
    hm_write_keep("coefficient", &coeff);
    hm_write_keep("perf_high_ratio", &high);
    hm_write_keep("cpus_reserved", &res);
    hm_write_keep("heavy_boost", &heavy);
    for i in 0..st().pol.len() {
        let p = format!("{}/hmbird/target_loads", st().pol[i].path);
        wrp(&p, "88");
        let p = format!("{}/hmbird/apply_freq_immediately", st().pol[i].path);
        wrp(&p, "0");
    }
    st().hb_tuned = true;
    logv("hmbird: game knobs applied");
}

pub fn hmbird_restore() {
    if !st().hb_present {
        st().hb_save.clear();
        st().hb_tuned = false;
        return;
    }
    if st().hb_save.is_empty() {
        st().hb_tuned = false;
        return;
    }
    let saves: Vec<HmSave> = st().hb_save.clone();
    for s in saves.iter() {
        let p = format!("{}/{}", st().p.hm, s.node);
        wrp(&p, &s.val);
    }
    logv(&format!("hmbird: 已还原 {} 个参数", saves.len()));
    st().hb_save.clear();
    st().hb_tuned = false;
}

/// 解析 `affected_cpus`（"0 1 2 3 4 5" 或 "0-5"）成掩码。
pub fn cpulist_mask(s: &str) -> u64 {
    let mut m = 0u64;
    for part in s.split_whitespace() {
        if let Some((a, b)) = part.split_once('-') {
            if let (Ok(a), Ok(b)) = (a.trim().parse::<u32>(), b.trim().parse::<u32>()) {
                for c in a..=b.min(63) {
                    m |= 1u64 << c;
                }
                continue;
            }
        }
        if let Ok(c) = part.trim().parse::<u32>() {
            if c < 64 {
                m |= 1u64 << c;
            }
        }
    }
    m
}

/// 性能簇（policy0）的 CPU 掩码；读不到就退回 SM8850 的 0x3F。
pub fn perf_mask() -> u64 {
    let ip = st().idx_perf;
    if ip >= 0 {
        let p = format!("{}/affected_cpus", st().pol[ip as usize].path);
        if let Some(s) = rdline(&p) {
            let m = cpulist_mask(&s);
            if m != 0 {
                return m;
            }
        }
    }
    0x3F
}

/// 守护进程只跑在性能簇上：1Hz 轮询不该把超大核从低频拉起来。
/// 子进程（dumpsys/pm）也会继承这个亲和性，同样不会惊动 cpu6-7。
pub fn pin_self_to_perf() -> bool {
    let m = perf_mask();
    if m == 0 {
        return false;
    }
    let ok = set_affinity(0, m);
    if ok {
        logv(&format!("daemon: 自身亲和性钉到 0x{:x}（性能簇）", m));
    } else {
        logmsg("daemon: 自身亲和性设置失败（继续运行）");
    }
    ok
}

// ------------------------------------------------------- boost / io / gpu ----
const BOOST_NODES: [&str; 8] = [
    "/sys/module/msm_performance/parameters/touch_boost",
    "/sys/devices/system/cpu/cpu_boost/input_boost_enabled",
    "/sys/module/cpu_boost/parameters/input_boost_enabled",
    "/sys/module/cpu_boost/parameters/boost_ms",
    "/sys/kernel/msm_performance/boost",
    "/proc/sys/kernel/sched_boost",
    "/sys/module/msm_perf_monitor/parameters/enable",
    "/sys/kernel/oplus_boost/enable",
];

/// 本机（一加 15 / SM8850）真正的触控加速在 WALT 里：
/// `kernel/sched/walt/sysctl.c` 的 `/proc/sys/walt/{input_boost_ms,sched_boost_on_input,sched_boost}`。
/// 通用的 `cpu_boost/input_boost_enabled` 在这台机器上大多不存在，只写那些等于没关。
/// `input_boost_freq` 是 8 个 u32 的数组，写 "0" 语义不明确，不动它。
const WALT_BOOST_NODES: [&str; 3] = ["input_boost_ms", "sched_boost_on_input", "sched_boost"];

/// 在 sysfs 里按目录名找节点（`touch_boost` 挂在 platform device 下面，
/// 具体 device 名随机型/DT 变，硬编码必然漏）。深度与数量都有上限，只在启动时跑一次。
fn find_dirs(root: &str, name: &str, max_depth: i32, out: &mut Vec<String>) {
    fn walk(dir: &str, name: &str, depth: i32, max_depth: i32, budget: &mut i32, out: &mut Vec<String>) {
        if depth > max_depth || out.len() >= 8 || *budget <= 0 {
            return;
        }
        let rd = match fs::read_dir(dir) {
            Ok(d) => d,
            Err(_) => return,
        };
        for e in rd.flatten() {
            if out.len() >= 8 || *budget <= 0 {
                return;
            }
            let p = e.path();
            // 必须跳过符号链接：sysfs 里 subsystem/driver 都是链，跟进去会绕到 /sys/bus 甚至成环
            let md = match fs::symlink_metadata(&p) {
                Ok(m) => m,
                Err(_) => continue,
            };
            if md.file_type().is_symlink() || !md.is_dir() {
                continue;
            }
            *budget -= 1;
            let n = e.file_name().to_string_lossy().to_string();
            let s = p.display().to_string();
            if n == name {
                out.push(s.clone());
            }
            walk(&s, name, depth + 1, max_depth, budget, out);
        }
    }
    let mut budget = 20000;
    walk(root, name, 0, max_depth, &mut budget, out);
}

/// 所有可能关掉触控/输入加速的节点（静态候选 + 运行时扫描）
pub fn boost_targets() -> Vec<String> {
    let mut t: Vec<String> = BOOST_NODES.iter().map(|s| s.to_string()).collect();
    let walt = format!("{}/sys/walt", st().p.proc_);
    for n in WALT_BOOST_NODES {
        t.push(format!("{}/{}", walt, n));
    }
    // OPLUS 自己的 touch_boost kobject：/sys/devices/platform/**/touch_boost/touch_boost_ms
    let mut dirs: Vec<String> = Vec::new();
    find_dirs(&format!("{}/devices/platform", st().p.sys), "touch_boost", 6, &mut dirs);
    for d in dirs {
        t.push(format!("{}/touch_boost_ms", d));
    }
    t
}

pub fn boost_off() {
    let targets = boost_targets();
    let total = targets.len();
    let mut n = 0;
    let mut probed = 0;
    let mut mode = 0;
    st().boost_save.clear();
    for p in targets {
        if !exists(&p) {
            continue;
        }
        probed += 1;
        if mode == 0 {
            mode = if p.contains("/sys/walt/") {
                1
            } else if p.contains("touch_boost") {
                2
            } else {
                3
            };
        }
        logv(&format!("boost: 找到 {} = {:?}", p, rdline(&p)));
        if !writable(&p) {
            logv(&format!("boost: {} 不可写，跳过", p));
            continue;
        }
        if let Some(old) = rdline(&p) {
            st().boost_save.push(HmSave {
                node: p.clone(),
                val: old,
            });
        }
        if wrp(&p, "0") {
            n += 1;
        }
    }
    st().boost_killed = n;
    st().boost_probed = probed;
    st().boost_mode = mode;
    logmsg(&format!(
        "boost: 候选 {} 个，存在 {} 个，已关 {} 个（mode={} 1=WALT 2=oplus_touch 3=legacy）",
        total, probed, n, mode
    ));
}

pub fn boost_restore() {
    let saves = st().boost_save.clone();
    let n = saves.len();
    for s in saves {
        wrp(&s.node, &s.val);
    }
    st().boost_save.clear();
    st().boost_killed = 0;
    st().boost_probed = 0;
    st().boost_mode = 0;
    if n > 0 {
        logv(&format!("boost: 已还原 {} 个加速节点", n));
    }
}

pub fn io_tune() {
    for i in 0..8u8 {
        let d = format!("/sys/block/sd{}/queue", (b'a' + i) as char);
        if exists(&d) {
            wrp(&format!("{}/read_ahead_kb", d), "256");
            wrp(&format!("{}/iostat", d), "0");
        }
    }
    wrp("/sys/block/mmcblk0/queue/read_ahead_kb", "256");
}

pub fn gpu_tune() {
    wrp("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0");
    wrp("/sys/class/kgsl/kgsl-3d0/bus_split", "1");
}

// ------------------------------------------------------------- 前台判定 ----
pub fn is_system_pkg(pkg: &str) -> bool {
    const PRE: [&str; 8] = [
        "android",
        "com.android.",
        "com.oplus.",
        "com.coloros.",
        "com.oneplus.",
        "com.google.android.",
        "com.qualcomm.",
        "com.qti.",
    ];
    PRE.iter().any(|p| pkg.starts_with(p))
}

fn is_ime_pkg(pkg: &str) -> bool {
    let ime = st().ime_pkg.clone();
    if !ime.is_empty() && pkg == ime {
        return true;
    }
    pkg.contains("inputmethod") || pkg.contains(".ime") || pkg.contains("sogou")
}

pub fn load_ime() {
    if !st().ime_pkg.is_empty() {
        return;
    }
    if let Some(line) = popen_first_line("settings get secure default_input_method 2>/dev/null", 3) {
        let pkg = match line.find('/') {
            Some(i) => &line[..i],
            None => &line[..],
        };
        if pkg.contains('.') {
            st().ime_pkg = pkg.trim().to_string();
        }
    }
}

fn collect_topapp() -> (bool, u32) {
    st().fg_cands.clear();
    let mut sig: u32 = 0;
    for suffix in ["/top-app/cgroup.procs", "/top-app/tasks"] {
        let p = format!("{}{}", st().p.cgrp, suffix);
        let text = match read_all(&p) {
            Some(t) => t,
            None => continue,
        };
        for tok in text.split('\n') {
            if st().fg_cands.len() >= 16 {
                break;
            }
            let pid = parse_leading_num(tok);
            if pid <= 0 {
                continue;
            }
            sig = sig.wrapping_mul(31).wrapping_add(pid as u32);
            let cf = format!("{}/{}/cmdline", st().p.proc_, pid);
            let cl = match rdline(&cf) {
                Some(c) if !c.is_empty() => c,
                _ => continue,
            };
            if !cl.contains('.') {
                continue;
            }
            if cl.contains(':') || cl.contains('/') {
                continue;
            }
            if !st().fg_cands.iter().any(|x| *x == cl) {
                st().fg_cands.push(cl);
            }
        }
        if !st().fg_cands.is_empty() {
            break;
        }
    }
    (!st().fg_cands.is_empty(), sig)
}

fn fg_from_cgroup() -> Option<String> {
    let (ok, _) = collect_topapp();
    if !ok {
        return None;
    }
    for c in st().fg_cands.clone() {
        if is_game(&c) {
            return Some(c);
        }
    }
    for c in st().fg_cands.clone() {
        if !is_system_pkg(&c) && !is_ime_pkg(&c) {
            return Some(c);
        }
    }
    None
}

/// 从一行 dumpsys 里抠出包名：... u0 com.pkg/.Act ...
fn pkg_from_line(line: &str) -> Option<String> {
    let tok = match line.find("u0 ") {
        Some(i) => &line[i + 3..],
        None => line,
    };
    let b = tok.as_bytes();
    let mut i = 0;
    while i < b.len() && !is_alnum(b[i]) {
        i += 1;
    }
    let mut tmp = String::new();
    while i < b.len() {
        let c = b[i];
        if c == b' ' || c == b'\t' || c == b'}' || c == b'\r' || c == b'\n' {
            break;
        }
        tmp.push(c as char);
        i += 1;
    }
    if let Some(slash) = tmp.find('/') {
        tmp.truncate(slash);
    }
    if !tmp.contains('.') {
        return None;
    }
    if tmp.starts_with("Window") || tmp.starts_with("ActivityRecord") {
        return None;
    }
    Some(tmp)
}

fn fg_from_cmd(cmd: &str) -> Option<String> {
    let line = popen_first_line(cmd, 4)?;
    pkg_from_line(&line)
}

fn fg_from_window() -> Option<String> {
    fg_from_cmd("dumpsys window 2>/dev/null | grep -m1 -E 'mCurrentFocus|mFocusedApp'")
}

fn fg_from_activity() -> Option<String> {
    fg_from_cmd("dumpsys activity activities 2>/dev/null | grep -m1 -E 'topResumedActivity|mResumedActivity'")
}

/// 盯 /proc 的新进程：新起来的进程命中游戏名单就立刻重算
fn new_game_process() -> bool {
    let rd = match fs::read_dir(&st().p.proc_) {
        Ok(d) => d,
        Err(_) => return false,
    };
    let mut mx = 0i64;
    for e in rd.flatten() {
        let v = parse_leading_num(&e.file_name().to_string_lossy());
        if v > mx {
            mx = v;
        }
    }
    if mx <= 0 {
        return false;
    }
    if st().max_pid <= 0 {
        st().max_pid = mx;
        return false;
    }
    if mx <= st().max_pid {
        return false;
    }
    let from = st().max_pid;
    st().max_pid = mx;
    if mx - from > 256 {
        return true; // pid 大跳变（重启/清理），直接重算一次
    }
    for pid in (from + 1)..=mx {
        let cf = format!("{}/{}/cmdline", st().p.proc_, pid);
        let cl = match rdline(&cf) {
            Some(c) if !c.is_empty() => c,
            _ => continue,
        };
        if !cl.contains('.') {
            continue;
        }
        if is_game(&cl) {
            return true;
        }
    }
    false
}

pub fn foreground_pkg() -> Option<String> {
    if !st().ime_loaded {
        st().ime_loaded = true;
        load_ime();
    }
    let now = now_s();
    let (sig_ok, sig) = collect_topapp();
    let mut forced = false;
    if new_game_process() {
        forced = true;
    }
    if sig_ok && st().topapp_sig != 0 && sig != st().topapp_sig {
        forced = true; // App 切换
    }
    if sig_ok {
        st().topapp_sig = sig;
    }
    // 缓存里的那个包已经不在 top-app 了 → 说明它被切走了，必须重算
    if !forced && sig_ok && !st().fg_cache.is_empty() {
        let cache = st().fg_cache.clone();
        if !st().fg_cands.iter().any(|c| *c == cache) {
            forced = true;
        }
    }
    if !forced && !st().fg_cache.is_empty() && now - st().fg_ts < 10 {
        return Some(st().fg_cache.clone());
    }

    st().fg_source = 0;
    let mut res: Option<String> = None;
    if let Some(v) = fg_from_window() {
        st().fg_source = 1;
        res = Some(v);
    } else if let Some(v) = fg_from_activity() {
        st().fg_source = 2;
        res = Some(v);
    } else if let Some(v) = fg_from_cgroup() {
        st().fg_source = 3;
        res = Some(v);
    }
    if let Some(v) = res {
        if !v.is_empty() {
            st().fg_cache = v.clone();
            st().fg_ts = now;
            return Some(v);
        }
    }
    // 全失败：保留上次结果，别让状态在空与有之间抖
    if !st().fg_cache.is_empty() {
        return Some(st().fg_cache.clone());
    }
    None
}

// ------------------------------------------------------------ 线程归位 ----
fn seen_has(tid: i64) -> bool {
    st().seen.iter().any(|&x| x == tid)
}

fn pin_tid(tid: i64, hexmask: &str) {
    let mm = parse_hex(hexmask);
    if st().hb_present {
        let p = format!("{}/prefer_cpu", st().p.hm);
        if popcount(mm) == 1 {
            // 单核目标必须给精确核：prefer_cluster 只表达「哪个簇」
            if wrverify(&p, &format!("{} {}", tid, hexmask)) {
                st().pin_kind = 2;
                return;
            }
        } else {
            // 1) 只给簇，让 hmbird 自己挑核
            let pc = format!("{}/prefer_cluster", st().p.hm);
            if wrverify(&pc, &format!("{} {}", tid, cluster_of(hexmask))) {
                st().pin_kind = 1;
                return;
            }
            // 2) 精确核掩码
            if wrverify(&p, &format!("{} {}", tid, hexmask)) {
                st().pin_kind = 2;
                return;
            }
        }
    }
    // 3) 单核目标：钉死那一颗
    if popcount(mm) == 1 {
        if set_affinity(tid, mm) {
            st().pin_kind = 3;
            return;
        }
    }
    // 4) 多核目标：整体给掩码
    set_affinity(tid, mm);
    st().pin_kind = 4;
}

pub fn apply_threads(pid: i64, pkg: &str) {
    if pid != st().seen_pid {
        st().seen_pid = pid;
        st().seen.clear();
        st().threads_pinned = 0;
    }
    let tdir = format!("{}/{}/task", st().p.proc_, pid);
    let rd = match fs::read_dir(&tdir) {
        Ok(d) => d,
        Err(_) => return,
    };
    let mut added = 0;
    let mut tids: Vec<String> = rd
        .flatten()
        .map(|e| e.file_name().to_string_lossy().to_string())
        .collect();
    tids.sort();
    for name in tids {
        let tid = parse_leading_num(&name);
        if tid <= 0 || seen_has(tid) {
            continue;
        }
        if st().seen.len() < MAX_SEEN {
            st().seen.push(tid);
        }
        let comm = match rdline(&format!("{}/{}/comm", tdir, name)) {
            Some(c) if !c.is_empty() => c,
            _ => continue,
        };
        let mut target: Option<String> = None;
        // 精确包名优先，其次 *
        for pass in 0..2 {
            if target.is_some() {
                break;
            }
            for i in 0..st().rules.len() {
                if !st().rules[i].valid {
                    continue;
                }
                let exact = st().rules[i].pkg != "*";
                if pass == 0 && !exact {
                    continue;
                }
                if pass == 1 && exact {
                    continue;
                }
                if exact && st().rules[i].pkg != pkg {
                    continue;
                }
                if st().rules[i].re.is_match(&comm) {
                    target = Some(st().rules[i].target.clone());
                }
            }
        }
        if let Some(t) = target {
            let mut pinned_any = false;
            for part in t.split('+') {
                if let Some(rest) = part.strip_prefix("uclamp:") {
                    let mut val = atoi_safe(rest, 0);
                    if val < 1 {
                        val = 1;
                    }
                    if val > 1024 {
                        val = 1024;
                    }
                    let node = format!("{}/set_uclamp", st().p.hm);
                    if wrp(&node, &format!("{} {}", tid, val)) {
                        st().schedprop_pinned += 1;
                    } else if st().log_level >= 2 {
                        logv(&format!("uclamp: {} 写入失败 ({})", tid, part));
                    }
                    continue;
                }
                if part == "preempt" || part == "idle" {
                    let node = format!("{}/prefer_{}", st().p.hm, part);
                    if wrp(&node, &format!("{} 1", tid)) {
                        st().schedprop_pinned += 1;
                    }
                    continue;
                }
                let mut m = mask_of(part);
                if m.is_none() && part.starts_with("0x") {
                    // 规则里可能直接写掩码
                    let owned = part.to_string();
                    let leaked: &'static str = Box::leak(owned.into_boxed_str());
                    m = Some(leaked);
                }
                if let Some(mm) = m {
                    pin_tid(tid, mm);
                    pinned_any = true;
                }
            }
            if pinned_any || !t.is_empty() {
                added += 1;
            }
        }
    }
    if added > 0 {
        st().threads_pinned += added;
        logv(&format!(
            "threads: +{} pinned ({}, total {})",
            added,
            pkg,
            st().threads_pinned
        ));
    }
}

/// 找 top-app 里与前台包名一致的主进程 pid
pub fn find_game_pid(pkg: &str) -> i64 {
    let p = format!("{}/top-app/cgroup.procs", st().p.cgrp);
    let text = match read_all(&p) {
        Some(t) => t,
        None => return -1,
    };
    for tok in text.split('\n') {
        let pp = parse_leading_num(tok);
        if pp <= 0 {
            continue;
        }
        let cf = format!("{}/{}/cmdline", st().p.proc_, pp);
        if let Some(cl) = rdline(&cf) {
            if !cl.is_empty() && cl == pkg {
                return pp;
            }
        }
    }
    -1
}

// ---------------------------------------------------------------- apply ----
pub fn apply_profile(name: &str) {
    let (mut pp, mut pr) = profile_pct(name);
    if name == "custom" && st().cur_app >= 0 {
        let a = &st().apps[st().cur_app as usize];
        pp = a.perf;
        pr = a.prime;
    }
    let (ip, ipr) = (st().idx_perf, st().idx_prime);
    st().cap_perf = if pp < 100 && ip >= 0 {
        set_cap(ip, pp)
    } else {
        policy_cap_now(ip)
    };
    st().cap_prime = if pr < 100 && ipr >= 0 {
        set_cap(ipr, pr)
    } else {
        policy_cap_now(ipr)
    };

    let (bg, fg, top) = profile_uclamp(name);
    set_uclamp("background", bg);
    set_uclamp("foreground", fg);
    set_uclamp("top-app", top);
    set_uclamp("system-background", bg + 10);

    if name == "custom" && st().cur_app >= 0 && st().apps[st().cur_app as usize].floor > 0 && ip >= 0
    {
        let floor = st().apps[st().cur_app as usize].floor;
        let max = st().pol[ip as usize].max;
        let t = max / 100 * floor as i64;
        let f = freq_ge(ip as usize, t);
        set_policy_min(ip, f);
        if !st().game_extras {
            st().game_extras = true; // 借用同一套还原逻辑
        }
    }
    if name == "game" {
        /* 官方 hmbird 本来就按帧预算自己算频率，再叠一层地板和绑核只会对打。
         *   official   —— 什么都不写（默认）
         *   light      —— 只对齐 frame_per_sec
         *   aggressive —— 地板 + bg cpuset + hmbird 系数 + 线程归位
         */
        let gm = st().c.game_mode.clone();
        if gm == "aggressive" {
            hmbird_apply_game();
            apply_game_extras();
        } else {
            hmbird_restore();
            restore_game_extras();
            if gm == "light" {
                hmbird_apply_frame_only();
            }
        }
    } else {
        hmbird_restore();
        restore_game_extras();
    }
    if st().c.gpu_tune != 0 {
        gpu_tune();
    }

    st().prof = name.to_string();
    let (cp, cpv) = (st().cap_perf, st().cap_prime);
    logv(&format!(
        "apply: {} perf={}%({}) prime={}%({})",
        name, pp, cp, pr, cpv
    ));
}

pub fn restore_all() {
    uclamp_restore();
    crate::charge::charge_restore();
    crate::charge::accel_restore();
    let (n, ge) = (st().pol.len(), st().game_extras);
    logv(&format!("restore_all: npol={} extras={}", n, ge));
    for i in 0..st().pol.len() {
        let v = if st().base_ok && st().base_max[i] > 0 {
            st().base_max[i]
        } else {
            st().pol[i].max
        };
        let f = format!("{}/scaling_max_freq", st().pol[i].path);
        wrl(&f, v);
    }
    set_uclamp("background", 100);
    set_uclamp("foreground", 100);
    set_uclamp("top-app", 100);
    set_uclamp("system-background", 100);
    hmbird_restore();
    restore_game_extras();
    boost_restore();
    st().hb_tuned = false;
    logmsg("restore: 已还原频率上限与调度参数");
}

/// 即时判定（不含迟滞），loop 与 apply/命令共用
pub fn decide_now() -> (String, String) {
    if !screen_on() {
        return ("sleep".to_string(), String::new());
    }
    let fg = foreground_pkg().unwrap_or_default();
    // 应用级策略优先于场景：用户设了就是白名单
    let ar = app_find(&fg);
    st().cur_app = ar;
    if ar >= 0 {
        let m = st().apps[ar as usize].mode.clone();
        return (m, fg);
    }
    if st().c.mode != "auto" {
        return (st().c.mode.clone(), fg);
    }
    if st().c.auto_game != 0 && is_game(&fg) {
        return ("game".to_string(), fg);
    }
    let b = crate::charge::battery_cap();
    let stt = crate::charge::battery_status();
    if b > 0 && b <= 20 && stt != "Charging" && stt != "Full" {
        return ("battery".to_string(), fg);
    }
    ("daily".to_string(), fg)
}
