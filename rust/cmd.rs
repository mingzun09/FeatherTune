// ============================================================================
//  cmd.rs —— state 落盘 + 所有 CLI 命令（输出协议与 C 版逐字一致）
// ============================================================================
#![allow(static_mut_refs)]

use std::fs;
use std::io::Write;

use crate::cfg::*;
use crate::charge::*;
use crate::cpu::*;
use crate::state::{st, MAX_APPS};
use crate::util::*;

// ---------------------------------------------------------------- state ----
pub fn state_write(cur: &str, fg: &str, game: bool, batt: i64, temp: i64, screen: bool, up: i64) {
    let tmp = format!("{}/.state.tmp", st().p.data);
    let mut f = match fs::File::create(&tmp) {
        Ok(f) => f,
        Err(_) => return,
    };
    let ip = st().idx_perf;
    let ipr = st().idx_prime;
    let mut s = String::new();
    let mode = st().c.mode.clone();
    s.push_str(&format!("mode={}\n", mode));
    s.push_str(&format!("profile={}\n", cur));
    s.push_str(&format!("screen={}\n", if screen { 1 } else { 0 }));
    s.push_str(&format!("fg={}\n", fg));
    s.push_str(&format!("game={}\n", if game { 1 } else { 0 }));
    s.push_str(&format!("battery={}\n", batt));
    s.push_str(&format!("temp={}\n", temp));
    s.push_str(&format!("cap_perf={}\n", st().cap_perf));
    s.push_str(&format!("cap_prime={}\n", st().cap_prime));
    s.push_str(&format!(
        "max_perf={}\n",
        if ip >= 0 { st().pol[ip as usize].max } else { 0 }
    ));
    s.push_str(&format!(
        "max_prime={}\n",
        if ipr >= 0 { st().pol[ipr as usize].max } else { 0 }
    ));
    s.push_str(&format!("gov_perf={}\n", gov_of(ip)));
    s.push_str(&format!("gov_prime={}\n", gov_of(ipr)));
    s.push_str(&format!("hmbird={}\n", if st().hb_present { 1 } else { 0 }));
    s.push_str(&format!(
        "hmbird_live={}\n",
        if hmbird_live() { 1 } else { 0 }
    ));
    s.push_str(&format!("hmbird_tuned={}\n", if st().hb_tuned { 1 } else { 0 }));
    s.push_str(&format!("threads={}\n", st().threads_pinned));
    s.push_str(&format!(
        "accel={}\n",
        if st().acc_spoofing { 1 } else { 0 }
    ));
    s.push_str(&format!("accel_spoof={}\n", st().acc_written));
    s.push_str(&format!(
        "accel_blocked={}\n",
        if st().acc_blocked { 1 } else { 0 }
    ));
    s.push_str(&format!("accel_eff={}\n", st().accel_eff));
    let anode = if st().acc_shell.valid {
        "oplus_dbg_tbat"
    } else if st().acc_batt.valid {
        "gauge_dbg_tbat"
    } else {
        "-"
    };
    s.push_str(&format!("accel_node={}\n", anode));
    let (mv, ma) = chg_read_power();
    let w10 = power_w10(mv, ma);
    // 充电状态只读一次：WebUI 用它决定显示「功率」还是「未充电/已充满」
    let chg_stt = battery_status();
    let charging = chg_stt.contains("Charging");
    s.push_str(&format!(
        "chg_mv={}\nchg_ma={}\nchg_w={}\nchg_peak={}\nchg_peak_on={}\nchg_peak_off={}\n",
        mv,
        ma,
        w10,
        st().chg_peak_w10,
        st().chg_peak_on_w10,
        st().chg_peak_off_w10
    ));
    // 电芯数与充电节点类型：WebUI 需要它们才能把功率换算对、并解释“为什么限流没生效”
    s.push_str(&format!(
        "charging={}\ncells={}\nchg_status={}\nchg_gear={}\nchg_node={}\n",
        if charging { 1 } else { 0 },
        battery_cells(),
        chg_stt,
        st().chg_gear,
        if st().chg_cur.valid {
            chg_kind_name(st().chg_cur.kind)
        } else {
            "none"
        }
    ));
    s.push_str(&format!("boost_killed={}\n", st().boost_killed));
    s.push_str(&format!(
        "boost_probed={}\nboost_mode={}\n",
        st().boost_probed,
        st().boost_mode
    ));    s.push_str(&format!("intensity={}\n", st().c.intensity));
    // WebUI 的开关/分段控件全靠这些 cfg_* 回填；缺了它们，页面上的开关永远是关的、
    // 充电策略永远显示“已关闭”——之前“改了没反应”有一半是这个原因。
    s.push_str(&format!(
        "cfg_enable={}\ncfg_mode={}\ncfg_intensity={}\ncfg_auto_game={}\ncfg_thread_opt={}\n\
         cfg_screen_off={}\ncfg_kill_boost={}\ncfg_io={}\ncfg_gpu={}\ncfg_launch_boost={}\n\
         cfg_charge_ctrl={}\ncfg_accel_ctrl={}\ncfg_accel_stop={}\ncfg_game_mode={}\n",
        st().c.enable,
        st().c.mode,
        st().c.intensity,
        st().c.auto_game,
        st().c.thread_opt,
        st().c.screen_off_saver,
        st().c.kill_boost,
        st().c.io_tune,
        st().c.gpu_tune,
        st().c.launch_boost,
        st().c.charge_ctrl,
        st().c.accel_ctrl,
        st().c.accel_stop_temp,
        st().c.game_mode
    ));
    s.push_str(&format!("uptime={}\n", up));
    s.push_str(&format!("updated={}\n", now_s()));
    let _ = f.write_all(s.as_bytes());
    drop(f);
    let _ = fs::rename(&tmp, &st().p.state);
}

// --------------------------------------------------------------- apply ----
pub fn apply_now() -> String {
    let (want, fg) = decide_now();
    apply_profile(&want);
    st().applied = want.clone();
    // 命令路径立刻落盘：WebUI 点一下就要看到新状态
    let b = battery_cap();
    state_write(
        &want,
        &fg,
        want == "game",
        if b > 0 { b } else { 100 },
        battery_temp(),
        screen_on(),
        0,
    );
    format!(
        "applied={} perf={} prime={}\n",
        want,
        st().cap_perf,
        st().cap_prime
    )
}

// ----------------------------------------------------------------- icon ----
fn pkg_ok(pkg: &str) -> bool {
    if pkg.is_empty() || pkg.len() > 120 {
        return false;
    }
    pkg.bytes()
        .all(|c| is_alnum(c) || c == b'.' || c == b'_' || c == b'-')
}

/* 为什么不用 `command -v unzip`：
 *   Android 的 toybox **没有 unzip 子命令**，`command -v unzip` 在真机上基本是空的；
 *   而 `command -v toybox` 一定成功 —— 旧实现因此选中了 "toybox unzip"，
 *   每条命令都失败，最后所有应用都退化成首字母色块。
 *   现在改成「拿真 APK 跑一次 -l，输出里出现 res/ 才算可用」，并且把
 *   KernelSU / APatch / Magisk 自带的 busybox 绝对路径也列进来。 */
const UNZIP_CANDS: [&str; 8] = [
    "/data/adb/ksu/bin/busybox unzip",
    "/data/adb/ap/bin/busybox unzip",
    "/data/adb/magisk/busybox unzip",
    "/system/bin/busybox unzip",
    "busybox unzip",
    "unzip",
    "/system/bin/unzip",
    "toybox unzip",
];

/// 管理器给守护进程的 PATH 经常缺 /system/bin（不同管理器差异很大），
/// 而 pm / busybox / timeout 全在那里。抠图这条链路显式补全 PATH。
const TOOL_PATH: &str = "/data/adb/ksu/bin:/data/adb/ap/bin:/data/adb/magisk:/system/bin:\
/system/xbin:/vendor/bin:/odm/bin:/sbin:/system/sbin:/usr/bin:/bin";

fn shell_run(cmd: &str, secs: i32) -> Option<(String, i32)> {
    let inherited = std::env::var("PATH").unwrap_or_default();
    // PATH 里可能有空格/括号（Windows 版 WSL 的 PATH 就有 "Program Files (x86)"），
    // 赋值必须整体加引号，否则 sh 在 '(' 处直接语法错误。
    let full = format!(
        "PATH='{}:{}'; export PATH; timeout {} {}",
        TOOL_PATH, inherited, secs, cmd
    );
    popen_capture(&full)
}

fn apk_paths(pkg: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    if let Some((s, rc)) = shell_run(&format!("pm path {} 2>/dev/null", pkg), 4) {
        if rc == 0 {
            for line in s.lines() {
                if let Some(p) = line.trim().strip_prefix("package:") {
                    if !p.is_empty() && !out.iter().any(|x| x == p) {
                        out.push(p.to_string());
                    }
                }
            }
        }
    }
    // base.apk 一定带 resources；split 里只有代码/资源补丁，图标不在里面
    out.sort_by_key(|p| if p.ends_with("/base.apk") { 0 } else { 1 });
    out
}

fn try_list(prefix: &str, apk: &str) -> Option<String> {
    let (s, rc) = shell_run(&format!("{} -l \"{}\" 2>/dev/null", prefix, apk), 8)?;
    if rc != 0 {
        return None;
    }
    if s.contains("res/") || s.contains(".png") || s.contains(".webp") {
        Some(s)
    } else {
        None
    }
}

fn unzip_list(apk: &str) -> Option<(String, String)> {
    let cached = st().unzip.clone();
    if !cached.is_empty() {
        if let Some(l) = try_list(&cached, apk) {
            return Some((cached, l));
        }
        st().unzip.clear(); // 缓存的工具这次不灵了（被卸载/路径变了），重新探测
    }
    if st().unzip_none {
        return None;
    }
    for c in UNZIP_CANDS {
        if let Some(l) = try_list(c, apk) {
            st().unzip = c.to_string();
            logv(&format!("icon: 解压工具 = {}", c));
            return Some((c.to_string(), l));
        }
    }
    st().unzip_none = true;
    logv("icon: 没有可用的 unzip（busybox / toybox / unzip 全失败），图标退化为首字母");
    None
}

/// 给一个 APK 条目打分：优先「图标本体」，排除自适应图标的前景/背景层、
/// 通知栏小图标、启动图/横幅这些「长得像图标但不是图标」的资源。
fn icon_score(low: &str) -> i32 {
    let file = low.rsplit('/').next().unwrap_or(low);
    let stem = file.split('.').next().unwrap_or(file);
    for bad in [
        "foreground",
        "background",
        "monochrome",
        "notification",
        "statusbar",
        "stat_sys",
        "qs_",
        "widget",
        "splash",
        "banner",
        "shadow",
        "watermark",
        "_bg",
        "bg_",
        "badge",
        "-bg",
        "_mask",
    ] {
        if stem.contains(bad) {
            return -1;
        }
    }
    let mut s = if stem == "ic_launcher" {
        1000
    } else if stem.starts_with("ic_launcher") {
        800 // ic_launcher_round / ic_launcher_forever 之类
    } else if stem == "icon" || stem.starts_with("app_icon") {
        900
    } else if stem.contains("icon") || stem.contains("logo") {
        600
    } else if low.contains("/mipmap") {
        400
    } else if low.contains("/drawable") {
        200
    } else {
        50 // 资源名被混淆（res/ab.png）时只能靠尺寸/形状判断，先给个底分
    };
    for (d, v) in [
        ("xxxhdpi", 60),
        ("xxhdpi", 50),
        ("xhdpi", 40),
        ("hdpi", 30),
        ("mdpi", 20),
        ("ldpi", 10),
    ] {
        if low.contains(d) {
            s += v;
            break;
        }
    }
    if stem.ends_with("_round") {
        s -= 150; // 有方图标时优先方的，实在没有才用圆形
    }
    s
}

#[derive(Clone)]
struct Cand {
    apk: String,
    name: String,
    score: i32,
    size: i64,
}

/// 从 `unzip -l` 输出里收集候选，按 (名字分, 体积) 排序
fn icon_candidates(list: &str, apk: &str) -> Vec<Cand> {
    let mut v: Vec<Cand> = Vec::new();
    for line in list.lines() {
        let cols: Vec<&str> = line.split_whitespace().collect();
        if cols.len() < 2 {
            continue;
        }
        let name = *cols.last().unwrap();
        let low = name.to_ascii_lowercase();
        if !low.starts_with("res/") {
            continue;
        }
        if !(low.ends_with(".png")
            || low.ends_with(".webp")
            || low.ends_with(".jpg")
            || low.ends_with(".jpeg"))
        {
            continue;
        }
        let size = parse_leading_num(cols[0]);
        if size <= 0 {
            continue;
        }
        let score = icon_score(&low);
        if score <= 0 {
            continue;
        }
        v.push(Cand {
            apk: apk.to_string(),
            name: name.to_string(),
            score,
            size,
        });
    }
    v.sort_by(|a, b| b.score.cmp(&a.score).then(b.size.cmp(&a.size)));
    v
}

// ---- 图片尺寸：只读文件头，不依赖任何解码库 ----
fn be32(b: &[u8], o: usize) -> i64 {
    if o + 4 > b.len() {
        return 0;
    }
    ((b[o] as i64) << 24) | ((b[o + 1] as i64) << 16) | ((b[o + 2] as i64) << 8) | b[o + 3] as i64
}

fn be16(b: &[u8], o: usize) -> i64 {
    if o + 2 > b.len() {
        return 0;
    }
    ((b[o] as i64) << 8) | b[o + 1] as i64
}

fn le16(b: &[u8], o: usize) -> i64 {
    if o + 2 > b.len() {
        return 0;
    }
    (b[o] as i64) | ((b[o + 1] as i64) << 8)
}

fn le24(b: &[u8], o: usize) -> i64 {
    if o + 3 > b.len() {
        return 0;
    }
    (b[o] as i64) | ((b[o + 1] as i64) << 8) | ((b[o + 2] as i64) << 16)
}

fn le32(b: &[u8], o: usize) -> i64 {
    le16(b, o) | (le16(b, o + 2) << 16)
}

/// 返回 (宽, 高)。支持 PNG / WebP(VP8,VP8L,VP8X) / JPEG。看不懂就 None。
fn img_dims(b: &[u8]) -> Option<(i64, i64)> {
    if b.len() > 24 && &b[0..4] == b"\x89PNG" {
        let (w, h) = (be32(b, 16), be32(b, 20));
        if w > 0 && h > 0 {
            return Some((w, h));
        }
    }
    if b.len() > 30 && &b[0..4] == b"RIFF" && &b[8..12] == b"WEBP" {
        let chunk = &b[12..16];
        if chunk == b"VP8X" {
            let (w, h) = (le24(b, 24) + 1, le24(b, 27) + 1);
            if w > 0 && h > 0 {
                return Some((w, h));
            }
        } else if chunk == b"VP8L" && b[20] == 0x2f {
            let bits = le32(b, 21);
            return Some(((bits & 0x3fff) + 1, ((bits >> 14) & 0x3fff) + 1));
        } else if chunk == b"VP8 " {
            for i in 20..b.len().saturating_sub(9) {
                if b[i] == 0x9d && b[i + 1] == 0x01 && b[i + 2] == 0x2a {
                    let w = le16(b, i + 3) & 0x3fff;
                    let h = le16(b, i + 5) & 0x3fff;
                    if w > 0 && h > 0 {
                        return Some((w, h));
                    }
                }
            }
        }
    }
    if b.len() > 16 && b[0] == 0xFF && b[1] == 0xD8 {
        let mut i = 2;
        while i + 9 < b.len() {
            if b[i] != 0xFF {
                i += 1;
                continue;
            }
            let m = b[i + 1];
            if m == 0xD8 || m == 0xD9 || (0xD0..=0xD7).contains(&m) {
                i += 2;
                continue;
            }
            let len = be16(b, i + 2) as usize;
            if len < 2 {
                break;
            }
            let sof = (0xC0..=0xC3).contains(&m)
                || (0xC5..=0xC7).contains(&m)
                || (0xC9..=0xCB).contains(&m)
                || (0xCD..=0xCF).contains(&m);
            if sof {
                let (h, w) = (be16(b, i + 5), be16(b, i + 7));
                if w > 0 && h > 0 {
                    return Some((w, h));
                }
                return None;
            }
            i += 2 + len;
        }
    }
    None
}

const ICON_MISS_TTL: i64 = 30 * 60; // 失败缓存 30 分钟；旧格式（只有包名）视为过期，自动重试

fn icon_missed(pkg: &str) -> bool {
    let mf = format!("{}/.miss", st().p.icons);
    let text = match fs::read_to_string(&mf) {
        Ok(t) => t,
        Err(_) => return false,
    };
    let now = now_s();
    for l in text.lines() {
        let mut it = l.trim().split('\t');
        if it.next().unwrap_or("") != pkg {
            continue;
        }
        let ts = it.next().and_then(|s| s.parse::<i64>().ok()).unwrap_or(0);
        return ts > 0 && now >= ts && now - ts < ICON_MISS_TTL;
    }
    false
}

fn icon_mark_miss(pkg: &str) {
    let mf = format!("{}/.miss", st().p.icons);
    let now = now_s();
    let old = fs::read_to_string(&mf).unwrap_or_default();
    let mut out = String::new();
    let mut n = 0;
    for l in old.lines() {
        let mut it = l.trim().split('\t');
        let p = it.next().unwrap_or("");
        if p.is_empty() || p == pkg {
            continue;
        }
        let ts = it.next().and_then(|s| s.parse::<i64>().ok()).unwrap_or(0);
        if ts == 0 || now - ts >= ICON_MISS_TTL || n >= 400 {
            continue;
        }
        out.push_str(&format!("{}\t{}\n", p, ts));
        n += 1;
    }
    out.push_str(&format!("{}\t{}\n", pkg, now));
    let _ = fs::write(&mf, out);
}

fn icon_unmark(pkg: &str) {
    let mf = format!("{}/.miss", st().p.icons);
    let old = fs::read_to_string(&mf).unwrap_or_default();
    let mut out = String::new();
    for l in old.lines() {
        let p = l.trim().split('\t').next().unwrap_or("");
        if !p.is_empty() && p != pkg {
            out.push_str(&format!("{}\n", l.trim()));
        }
    }
    let _ = fs::write(&mf, out);
}

const ICON_CACHE_VER: &str = "3";

/// 抠图算法变了就清一次缓存：v1.4.1 之前缓存里可能躺着透明前景层或不相干的 drawable，
/// 不清掉的话「修好了」用户也看不到。
fn icon_ver_check() {
    if st().icon_ver_checked {
        return;
    }
    st().icon_ver_checked = true;
    let dir = st().p.icons.clone();
    mkdir_p(&dir);
    let vp = format!("{}/.ver", dir);
    let cur = fs::read_to_string(&vp).unwrap_or_default();
    if cur.trim() == ICON_CACHE_VER {
        return;
    }
    let mut wiped = 0;
    if let Ok(rd) = fs::read_dir(&dir) {
        for e in rd.flatten() {
            if fs::remove_file(e.path()).is_ok() {
                wiped += 1;
            }
        }
    }
    let _ = fs::write(&vp, format!("{}\n", ICON_CACHE_VER));
    logmsg(&format!(
        "icon: 缓存版本 {} -> {}，清理 {} 个旧文件",
        if cur.trim().is_empty() {
            "无"
        } else {
            cur.trim()
        },
        ICON_CACHE_VER,
        wiped
    ));
}

/// 把 APK 里的图标抠出来；成功返回缓存文件路径。
/// force=true 时先丢掉旧缓存和负缓存（WebUI 的点「刷新」/ CLI `icon <pkg> force`）。
pub fn icon_get(pkg: &str, force: bool) -> Option<String> {
    if !pkg_ok(pkg) {
        return None;
    }
    icon_ver_check();
    mkdir_p(&st().p.icons);
    let out = format!("{}/{}", st().p.icons, pkg);
    let legacy = format!("{}/{}.png", st().p.icons, pkg);
    if force {
        let _ = fs::remove_file(&out);
        let _ = fs::remove_file(&legacy);
        icon_unmark(pkg);
    } else {
        if let Ok(md) = fs::metadata(&out) {
            if md.len() > 0 {
                return Some(out); // 已缓存
            }
        }
        if let Ok(md) = fs::metadata(&legacy) {
            if md.len() > 0 {
                let _ = fs::rename(&legacy, &out); // 老版本缓存的是 <pkg>.png
                return Some(out);
            }
        }
        if icon_missed(pkg) {
            return None;
        }
    }
    let apks = apk_paths(pkg);
    if apks.is_empty() {
        icon_mark_miss(pkg);
        logv(&format!("icon: {} 的 pm path 为空", pkg));
        return None;
    }
    let mut cands: Vec<Cand> = Vec::new();
    let mut tool = String::new();
    for apk in &apks {
        if let Some((t, list)) = unzip_list(apk) {
            tool = t;
            for c in icon_candidates(&list, apk) {
                if !cands.iter().any(|x| x.name == c.name) {
                    cands.push(c);
                }
            }
            if !cands.is_empty() {
                break; // base.apk 里已经找到，split 不用再看
            }
        }
    }
    if cands.is_empty() {
        icon_mark_miss(pkg);
        logv(&format!("icon: {} 的 APK 里没有可用图片资源", pkg));
        return None;
    }
    cands.sort_by(|a, b| b.score.cmp(&a.score).then(b.size.cmp(&a.size)));
    let pool: Vec<Cand> = cands.into_iter().take(5).collect();
    let tmp = format!("{}/{}.part", st().p.icons, pkg);
    let mut best: Option<(i64, String, i32)> = None; // (面积, 条目名, 分数)
    for c in &pool {
        let cmd = format!(
            "{} -p \"{}\" \"{}\" > \"{}\" 2>/dev/null",
            tool, c.apk, c.name, tmp
        );
        let ok = shell_run(&cmd, 10).map(|(_, rc)| rc == 0).unwrap_or(false);
        if !ok {
            continue;
        }
        let bytes = match fs::read(&tmp) {
            Ok(b) if b.len() >= 64 => b,
            _ => continue,
        };
        let dims = img_dims(&bytes);
        let square = dims.map(|(w, h)| w == h && w >= 48).unwrap_or(false);
        let confident = c.score >= 600; // 名字就写着 ic_launcher/icon
        if !confident && !square {
            continue; // 名字不可信、形状也不像图标 → 不要拿它冒充
        }
        if confident {
            let tiny = dims.map(|(w, h)| w < 16 || h < 16).unwrap_or(false);
            if tiny {
                continue;
            }
            let _ = fs::rename(&tmp, &out);
            logv(&format!(
                "icon: {} <- {} ({} B, score={}, {:?})",
                pkg,
                c.name,
                bytes.len(),
                c.score,
                dims
            ));
            return Some(out);
        }
        let area = dims.map(|(w, h)| w * h).unwrap_or(0);
        if best.as_ref().map(|b| area > b.0).unwrap_or(true) {
            let _ = fs::rename(&tmp, &out);
            best = Some((area, c.name.clone(), c.score));
        }
    }
    if let Some((area, name, score)) = best {
        logv(&format!(
            "icon: {} <- {} (按尺寸选中, {}px², score={})",
            pkg, name, area, score
        ));
        return Some(out);
    }
    let _ = fs::remove_file(&tmp);
    icon_mark_miss(pkg);
    logv(&format!(
        "icon: {} 没有合适的位图（可能是纯自适应图标/矢量图标），退回首字母",
        pkg
    ));
    None
}

// ----------------------------------------------------------- run_cmd ----
pub fn run_cmd(line: &str) -> String {
    let args: Vec<&str> = line.split_whitespace().collect();
    if args.is_empty() {
        return "err=empty\n".to_string();
    }
    let cmd = args[0];

    cfg_load();
    detect_topology();
    capture_syscaps();
    detect_refresh();
    detect_paths();
    hmbird_detect();
    games_load();
    rules_load();
    apps_load();
    labels_load();

    match cmd {
        "status" => return cmd_status(),
        "doctor" => return cmd_doctor(),
        "apply" => return apply_now(),
        "log" => return cmd_log(args.get(1).map(|s| atoi_safe(s, 40)).unwrap_or(40)),
        "games" | "pkg" => return cmd_pkg(&args),
        "set" => {
            if args.len() >= 3 {
                if !cfg_set(args[1], args[2]) {
                    return "err=write\n".to_string();
                }
                return apply_now();
            }
            return "err=usage\n".to_string();
        }
        "mode" => {
            if args.len() >= 2 {
                let ok = ["auto", "daily", "battery", "game", "perf", "sleep"];
                if !ok.contains(&args[1]) {
                    return "err=bad_mode\n".to_string();
                }
                cfg_set("mode", args[1]);
                return apply_now();
            }
            return "err=usage\n".to_string();
        }
        "apps" | "app" => return cmd_apps(&args),
        "labels" => {
            let mut out = String::new();
            for l in st().labels.clone() {
                if out.len() + 200 < 65536 {
                    out.push_str(&format!("{}={}\n", l.pkg, l.name));
                }
            }
            return out;
        }
        "icon" => {
            if args.len() < 2 {
                return "err=missing_pkg\n".to_string();
            }
            let force = args
                .get(2)
                .map(|s| *s == "force" || *s == "1" || *s == "-f")
                .unwrap_or(false);
            return match icon_get(args[1], force) {
                Some(p) => format!("ok={}\n", p),
                None => "err=no_icon\n".to_string(),
            };
        }
        "apprule" => {
            let mut out = String::new();
            for a in st().apps.clone() {
                if out.len() + 200 < 65536 {
                    out.push_str(&format!(
                        "{}|{}|{}|{}|{}|{}\n",
                        a.pkg, a.mode, a.perf, a.prime, a.floor, a.threads
                    ));
                }
            }
            return out;
        }
        "charge" => return cmd_charge(args.get(1).copied().unwrap_or(""), args.get(2).copied()),
        "boost" => return cmd_boost(),
        "version" => return format!("{} ({})\n", crate::state::APP_VER, crate::state::APP_VC),
        "power" => return cmd_power(),
        "probe" => return cmd_probe(),
        _ => {}
    }
    "err=unknown_cmd\n".to_string()
}

fn cmd_log(want: i32) -> String {
    let p = st().p.log.clone();
    let text = fs::read_to_string(&p).unwrap_or_default();
    if text.is_empty() {
        return "(空)\n".to_string();
    }
    let lines: Vec<&str> = text.lines().collect();
    let want = want.max(0) as usize;
    let skip = lines.len().saturating_sub(want);
    let mut out = String::new();
    for l in &lines[skip..] {
        out.push_str(l);
        out.push('\n');
    }
    out
}

fn cmd_pkg(args: &[&str]) -> String {
    let a1 = args.get(1).copied().unwrap_or("");
    if args.len() == 1 || a1 == "list" {
        let mut out = String::new();
        for g in st().games.clone() {
            out.push_str(&format!("{}\n", g));
        }
        return out;
    }
    if a1 == "add" && args.len() >= 3 {
        let p = st().p.games.clone();
        if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(&p) {
            let _ = writeln!(f, "{}", args[2]);
        }
        st().games_mtime = (0, 0);
        return format!("ok=added:{}\n", args[2]);
    }
    if a1 == "del" && args.len() >= 3 {
        let p = st().p.games.clone();
        let tmp = format!("{}/.g.tmp", st().p.data);
        let old = fs::read_to_string(&p).unwrap_or_default();
        let mut out = String::new();
        for line in old.lines() {
            if line.trim() != args[2] {
                out.push_str(line);
                out.push('\n');
            }
        }
        let _ = fs::write(&tmp, out);
        let _ = fs::rename(&tmp, &p);
        st().games_mtime = (0, 0);
        return format!("ok=removed:{}\n", args[2]);
    }
    "err=usage\n".to_string()
}

fn cmd_apps(args: &[&str]) -> String {
    let a1 = args.get(1).copied().unwrap_or("");
    if a1 == "set" && args.len() >= 3 {
        // app set <pkg> <mode> [perf prime floor threads]
        let pkg = args[2];
        let mode = args.get(3).copied().unwrap_or("custom");
        let mut pp = args.get(4).map(|s| atoi_safe(s, 100)).unwrap_or(100);
        let mut pr = args.get(5).map(|s| atoi_safe(s, 100)).unwrap_or(100);
        let mut fl = args.get(6).map(|s| atoi_safe(s, 0)).unwrap_or(0);
        let th = args.get(7).map(|s| atoi_safe(s, 1)).unwrap_or(1);
        pp = pp.clamp(20, 100);
        pr = pr.clamp(20, 100);
        fl = fl.clamp(0, 95);
        let papps = st().p.apps.clone();
        let tmp = format!("{}/.a.tmp", st().p.data);
        let old = fs::read_to_string(&papps).unwrap_or_default();
        let mut out = String::new();
        for line in old.lines() {
            let t = line.trim_start_matches([' ', '\t']);
            let first = t.split([' ', '\t', '\r', '\n']).next().unwrap_or("");
            if first == pkg {
                continue; // 旧行丢掉，末尾重写
            }
            out.push_str(line);
            out.push('\n');
        }
        out.push_str(&format!("{} {} {} {} {} {}\n", pkg, mode, pp, pr, fl, th));
        if fs::write(&tmp, out).is_err() {
            return "err=write_failed\n".to_string();
        }
        let _ = fs::rename(&tmp, &papps);
        st().apps_mtime = (0, 0);
        apps_load();
        labels_load();
        return format!(
            "ok=app:{} mode={} perf={} prime={} floor={} thr={}\n",
            pkg, mode, pp, pr, fl, th
        );
    }
    if a1 == "del" && args.len() >= 3 {
        let papps = st().p.apps.clone();
        let tmp = format!("{}/.a.tmp", st().p.data);
        let old = fs::read_to_string(&papps).unwrap_or_default();
        let mut out = String::new();
        for line in old.lines() {
            let t = line.trim_start_matches([' ', '\t']);
            let first = t.split([' ', '\t', '\r', '\n']).next().unwrap_or("");
            if first == args[2] {
                continue;
            }
            out.push_str(line);
            out.push('\n');
        }
        if fs::write(&tmp, out).is_err() {
            return "err=write_failed\n".to_string();
        }
        let _ = fs::rename(&tmp, &papps);
        st().apps_mtime = (0, 0);
        apps_load();
        return format!("ok=deleted:{}\n", args[2]);
    }
    // apps [list]：本机第三方应用 + 它们的策略
    if let Some(t) = popen_all("pm list packages -3 2>/dev/null | sed 's/^package://'", 8) {
        let mut out = String::new();
        for ln in t.lines() {
            if ln.is_empty() || out.len() + 200 >= 65536 {
                continue;
            }
            let ar = app_find(ln);
            let mode = if ar >= 0 {
                st().apps[ar as usize].mode.clone()
            } else {
                "-".to_string()
            };
            let lb = label_of(ln).unwrap_or_default();
            out.push_str(&format!(
                "{}|{}|{}|{}\n",
                ln,
                mode,
                if is_game(ln) { 1 } else { 0 },
                lb
            ));
        }
        if !out.is_empty() {
            return out;
        }
    }
    // pm 读不到时至少把已配置的应用列出来
    let mut out = String::new();
    for a in st().apps.clone() {
        let lb = label_of(&a.pkg).unwrap_or_default();
        out.push_str(&format!(
            "{}|{}|{}|{}\n",
            a.pkg,
            a.mode,
            if is_game(&a.pkg) { 1 } else { 0 },
            lb
        ));
    }
    for g in st().games.clone() {
        if app_find(&g) >= 0 {
            continue;
        }
        let lb = label_of(&g).unwrap_or_default();
        out.push_str(&format!("{}|follow|1|{}\n", g, lb));
    }
    out
}

/// 直接回答「为什么充电功率没有提升」：把真实温度、伪装值、DT 分桶表、限流节点
/// 一起摆出来，并给出结论。
fn charge_why() -> String {
    charge_init();
    accel_init();
    let mut out = String::new();
    let soc = battery_cap();
    let temp = battery_temp();
    let stt = battery_status();
    let (mv, ma) = chg_read_power();
    let w10 = power_w10(mv, ma);
    let cells = battery_cells();
    out.push_str(&format!(
        "soc={}% temp={}℃ status={} screen={}\n",
        soc,
        temp,
        stt,
        if screen_on() { 1 } else { 0 }
    ));
    out.push_str(&format!(
        "power={}.{}W cells={} vbat={}mV ibat={:+}mA peak={}.{}W\n",
        w10 / 10,
        w10 % 10,
        cells,
        mv / 1000,
        current_ma(ma),
        st().chg_peak_w10 / 10,
        st().chg_peak_w10 % 10
    ));
    // 适配器/协议天花板：67W 到底是模块没生效，还是这条线就只能给这么多
    let adapter = adapter_power_w10();
    let input = input_power_w10();
    if adapter > 0 {
        out.push_str(&format!(
            "adapter: {}.{}W (usb voltage_max={}mV × current_max={}mA)\n",
            adapter / 10,
            adapter % 10,
            rdnum(&format!("{}/class/power_supply/usb/voltage_max", st().p.sys)) / 1000,
            rdnum(&format!("{}/class/power_supply/usb/current_max", st().p.sys)) / 1000
        ));
    }
    if input > 0 {
        out.push_str(&format!("input: {}.{}W\n", input / 10, input % 10));
    }
    // 亮屏/息屏峰值分开看：120W 这种档位基本只在息屏时给
    out.push_str(&format!(
        "peak: on={}.{}W off={}.{}W\n",
        st().chg_peak_on_w10 / 10,
        st().chg_peak_on_w10 % 10,
        st().chg_peak_off_w10 / 10,
        st().chg_peak_off_w10 % 10
    ));
    if adapter > 0 && adapter < 900 && stt.contains("Charging") {
        out.push_str(
            "note: 适配器/协议上限低于 90W —— 本次充电不可能超过它；温度伪装只解除温度限流\n",
        );
    }
    out.push_str(&format!(
        "limit: node={} kind={} written={}\n",
        if st().chg_cur.valid {
            st().chg_cur.path.clone()
        } else {
            "-".to_string()
        },
        if st().chg_cur.valid {
            chg_kind_name(st().chg_cur.kind)
        } else {
            "none"
        },
        st().chg_v2_written
    ));
    if !st().chg_cur.valid {
        out.push_str("limit: 本机没有可写电流节点 → charge_ctrl 不可能生效\n");
    } else if st().chg_cur.kind == 2 {
        if screen_on() {
            out.push_str(
                "limit: 亮屏 → charger v2 把 current_now 写入当成“取消限流”，模块已释放；只有熄屏才限流\n",
            );
        } else {
            out.push_str("limit: 息屏 → 可限流（SOC≥拐点且温度到墙才降）\n");
        }
    }
    out.push_str(&format!(
        "accel: node={} target={} auto_dt={} spoofing={} blocked={}\n",
        if st().acc_shell.valid {
            "oplus_dbg_tbat"
        } else if st().acc_batt.valid {
            "gauge_dbg_tbat"
        } else {
            "MISSING"
        },
        accel_target(),
        st().acc_dt_val,
        if st().acc_spoofing { 1 } else { 0 },
        if st().acc_blocked { 1 } else { 0 }
    ));
    // 真实机身/电池温度（内核 pps_ufcs 曲线默认用 shell_temp）
    let shell = thermal_by("shell");
    let batt = thermal_by("batt");
    if let Some((n, t)) = &shell {
        out.push_str(&format!(
            "real: shell {}={}℃ → {} ({})\n",
            n,
            fmt_t10(*t),
            region_name(region_of(*t)),
            region_of(*t)
        ));
    }
    if let Some((n, t)) = &batt {
        out.push_str(&format!(
            "real: batt {}={}℃ → {} ({})\n",
            n,
            fmt_t10(*t),
            region_name(region_of(*t)),
            region_of(*t)
        ));
    }
    // 距离「电池温度降流线」还有多少度：34.8℃ 时这条线根本没碰到，加速自然没东西可解
    if temp > 0 {
        let line = batt_derate_temp();
        let mine = temp * 10;
        out.push_str(&format!(
            "headroom: batt {}℃ → 降流线 {}℃（{}）\n",
            temp,
            fmt_t10(line),
            if mine < line {
                format!("还有 {}℃", fmt_t10(line - mine))
            } else {
                "已过线，厂商正在降流".to_string()
            }
        ));
    }
    // 亮屏充电：厂商 HAL 自己限流，用户态没有可写的开关（提示词里的 led_on 节点不存在）
    if stt.contains("Charging")
        && screen_on()
        && w10 > 0
        && w10 < 800
        && (adapter == 0 || adapter >= 900)
        && temp > 0
        && temp * 10 < batt_derate_temp()
    {
        out.push_str(
            "note: 亮屏 + 无温度降流 + 适配器够 → 剩下的解释就是厂商的亮屏限流；锁屏 3~5 分钟再看 peak.off\n",
        );
    }
    if st().acc_dt.len() >= 3 {
        let r = st().acc_dt.clone();
        out.push_str(&format!(
            "dt: {:?}  normal_high=[{}, {}) = {}~{}℃\n",
            r,
            r[r.len() - 3],
            r[r.len() - 2],
            fmt_t10(r[r.len() - 3] as i64),
            fmt_t10(r[r.len() - 2] as i64 - 1)
        ));
        let tgt = accel_target() as i64;
        out.push_str(&format!(
            "fake: {}℃ → {} ({})\n",
            fmt_t10(tgt),
            region_name(region_of(tgt)),
            region_of(tgt)
        ));
        if !stt.contains("Charging") {
            out.push_str("verdict: 当前没在充电，加速不参与\n");
        } else if !st().acc_shell.valid && !st().acc_batt.valid {
            out.push_str("verdict: 没有 oplus_chg 温度参数节点，加速不可用\n");
        } else {
            let real = shell.as_ref().or(batt.as_ref()).map(|(_, t)| region_of(*t));
            match real {
                Some(rr) if rr >= 0 => {
                    if rr == region_of(tgt) {
                        out.push_str(&format!(
                            "verdict: 真实 {} 与伪装 {} 同段，电流档不变 → 没有增益\n",
                            region_name(rr),
                            region_name(region_of(tgt))
                        ));
                    } else {
                        out.push_str(&format!(
                            "verdict: 真实 {} → 伪装 {}，曲线换到更高电流段（这条才会让功率变高）\n",
                            region_name(rr),
                            region_name(region_of(tgt))
                        ));
                    }
                }
                _ => out.push_str("verdict: 读不到 thermal_zone 的 shell/batt 温度，无法判断段位\n"),
            }
        }
    } else {
        out.push_str("verdict: 没读到 DT oplus,temp_range，加速档位未知（可用 accel_tbat 手动指定）\n");
    }
    out
}

fn cmd_boost() -> String {
    let mut out = String::new();
    let walt = format!("{}/sys/walt", st().p.proc_);
    out.push_str(&format!(
        "walt_dir={} exists={}\n",
        walt,
        if exists(&walt) { 1 } else { 0 }
    ));
    if let Ok(rd) = fs::read_dir(&walt) {
        let mut names: Vec<String> = rd
            .flatten()
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.contains("boost"))
            .collect();
        names.sort();
        out.push_str(&format!("walt_boost_nodes={}\n", names.join(",")));
    }
    let targets = boost_targets();
    out.push_str(&format!(
        "candidates={} mode_now={} probed={} killed={}\n",
        targets.len(),
        st().boost_mode,
        st().boost_probed,
        st().boost_killed
    ));
    // 只读探测其它厂商 boost 入口（frame_boost 不关：关掉会破坏 165Hz 帧同步）
    for d in [
        "/proc/oplus_frame_boost",
        "/sys/kernel/oplus_boost",
        "/sys/module/cpu_boost",
        "/sys/module/msm_performance",
    ] {
        out.push_str(&format!("probe {} exists={}\n", d, if exists(d) { 1 } else { 0 }));
    }
    if let Some(v) = rdline("/proc/oplus_frame_boost/sys_ctrl") {
        out.push_str(&format!("frame_boost sys_ctrl={}\n", v));
    }
    for p in targets {
        if !exists(&p) {
            continue;
        }
        out.push_str(&format!(
            "{}\n  val={} writable={}\n",
            p,
            rdline(&p).unwrap_or_else(|| "-".to_string()),
            if writable(&p) { 1 } else { 0 }
        ));
    }
    out
}

fn cmd_charge(sub: &str, arg: Option<&str>) -> String {
    let mut out = String::new();
    if sub == "why" {
        return charge_why();
    }
    if sub == "sweep" {
        let ms = arg.and_then(|s| s.parse::<u64>().ok()).unwrap_or(4000);
        return accel_sweep(ms);
    }
    if sub == "probe" {
        out.push_str(&format!(
            "soc={} temp={}\n",
            battery_cap(),
            battery_temp()
        ));
        out.push_str(&format!("status={}\n", battery_status()));
        // 只读探测：这台机器到底开放了哪些限流接口。
        // charger v2（OnePlus 15）上只有 battery/current_now 可写，且仅息屏生效。
        const ALL: [&str; 22] = [
            "/class/power_supply/battery/constant_charge_current_max",
            "/class/power_supply/battery/constant_charge_current",
            "/class/power_supply/battery/current_max",
            "/class/power_supply/battery/input_current_limit",
            "/class/power_supply/battery/current_now",
            "/class/power_supply/battery/input_suspend",
            "/class/power_supply/battery/charge_disable",
            "/class/power_supply/battery/charging_enabled",
            "/class/power_supply/battery/charge_control_limit",
            "/class/power_supply/battery/voltage_max",
            "/class/power_supply/usb/current_max",
            "/class/power_supply/usb/input_current_limit",
            "/class/power_supply/usb/online",
            "/class/oplus_chg/battery/current_max",
            "/class/oplus_chg/battery/chg_current_max",
            "/class/oplus_chg/battery/fastchg_current_max",
            "/class/oplus_chg/battery/input_suspend",
            "/class/oplus_chg/battery/cool_down",
            "/class/oplus_chg/battery/normal_cool_down",
            "/module/oplus_chg_v2/parameters/oplus_dbg_tbat",
            "/module/oplus_chg_v2/parameters/gauge_dbg_tbat",
            "/class/oplus_mms/common/overwrite",
        ];
        for c in ALL {
            let p = format!("{}{}", st().p.sys, c);
            let w = writable(&p);
            let v = rdline(&p);
            out.push_str(&format!(
                "{} {} {}\n",
                c,
                if v.is_some() { "read" } else { "----" },
                if w { "WRITE" } else { "-" }
            ));
            if let Some(v) = v {
                out.push_str(&format!("    = {}\n", v));
            }
        }
        return out;
    }

    charge_init();
    out.push_str(&format!(
        "enabled={} active={} suspended={} last_pct={}\n",
        st().c.charge_ctrl,
        if st().chg_active { 1 } else { 0 },
        if st().chg_suspended { 1 } else { 0 },
        st().chg_last_pct
    ));
    out.push_str(&format!(
        "node={} orig={}\n",
        if st().chg_cur.valid {
            st().chg_cur.path.clone()
        } else {
            "-".to_string()
        },
        st().chg_cur.orig
    ));
    out.push_str(&format!(
        "cur: kind={} base={}{} written={}\n",
        if st().chg_cur.valid {
            chg_kind_name(st().chg_cur.kind)
        } else {
            "none"
        },
        if st().chg_cur.kind == 2 {
            chg_base_ma()
        } else {
            st().chg_orig_cur
        },
        if st().chg_cur.valid && st().chg_cur.kind == 0 {
            "uA"
        } else {
            "mA"
        },
        st().chg_v2_written
    ));
    if !st().chg_cur.valid {
        out.push_str(
            "cur: 本机没有可写的电流节点，限流阶梯不生效（charger v2 只开放 battery/current_now，\n\
             cur: 若它也不可写，说明内核没编 oplus,smart_charging_screenoff，用户态无法限流）\n",
        );
    } else if st().chg_cur.kind == 2 {
        out.push_str(
            "cur: charger v2 的息屏智能充电接口：只有熄屏时才真正限流，亮屏会被内核反向取消\n",
        );
    }
    out.push_str(&format!(
        "suspend_node={}\n",
        if st().chg_suspend.valid {
            st().chg_suspend.path.clone()
        } else {
            "-".to_string()
        }
    ));
    out.push_str(&format!(
        "soc={} temp={} knee={} wall={} resume={} curve={}\n",
        battery_cap(),
        battery_temp(),
        st().c.charge_knee_soc,
        st().c.charge_temp_wall,
        st().c.charge_resume_temp,
        st().c.chg_curve
    ));
    accel_init();
    out.push_str(&format!(
        "accel: enabled={} gauge={} target={} auto_dt={} stop={} gentle={}\n",
        st().c.accel_ctrl,
        st().c.accel_gauge,
        accel_target(),
        st().acc_dt_val,
        st().c.accel_stop_temp,
        st().c.accel_gentle
    ));
    out.push_str(&format!(
        "accel_node shell={} orig={} | batt={} orig={} | sub={}\n",
        if st().acc_shell.valid {
            st().acc_shell.path.clone()
        } else {
            "-".to_string()
        },
        orig_or_dash(&st().acc_shell),
        if st().acc_batt.valid {
            st().acc_batt.path.clone()
        } else {
            "-".to_string()
        },
        orig_or_dash(&st().acc_batt),
        if st().acc_sub.valid {
            st().acc_sub.path.clone()
        } else {
            "-".to_string()
        }
    ));
    // 运行时状态只能来自守护进程写的 state 文件：这条命令是独立进程
    let mut rt_s: i32 = -1;
    let mut rt_v: i32 = -1;
    let mut rt_b: i32 = -1;
    let mut rt_ok = false;
    if let Ok(text) = fs::read_to_string(&st().p.state) {
        for line in text.lines() {
            if let Some(v) = line.strip_prefix("accel_spoof=") {
                rt_v = atoi_safe(v, 0);
            } else if let Some(v) = line.strip_prefix("accel_blocked=") {
                rt_b = atoi_safe(v, 0);
            } else if let Some(v) = line.strip_prefix("accel=") {
                rt_s = atoi_safe(v, 0);
            } else if line.starts_with("updated=") {
                rt_ok = true;
            }
        }
    }
    out.push_str(&format!(
        "accel_rt: spoofing={} value={} blocked={} (state {})\n",
        rt_s,
        rt_v,
        rt_b,
        if rt_ok { "ok" } else { "缺失/守护进程没跑" }
    ));

    let stt = battery_status();
    let (uv, ua) = chg_read_power();
    let w10 = power_w10(uv, ua);
    out.push_str(&format!(
        "power: status={} {}S vbat={}mV ibat={:+}mA → {}.{}W（{}）\n",
        stt,
        battery_cells(),
        uv / 1000,
        current_ma(ua),
        w10 / 10,
        w10 % 10,
        if ua < 0 {
            "充入电池"
        } else {
            "放电/入不敷出，PC USB 口常见，测加速请用原装充电头"
        }
    ));
    out
}

fn cmd_power() -> String {
    let mut out = String::new();
    let stt = battery_status();
    let (mv, ma) = chg_read_power();
    let w10 = power_w10(mv, ma);
    out.push_str(&format!(
        "soc={} temp={} status={} cells={}\n",
        battery_cap(),
        battery_temp(),
        stt,
        battery_cells()
    ));
    out.push_str(&format!(
        "battery: vbat={}mV ibat={:+}mA → {}.{}W\n",
        mv / 1000,
        current_ma(ma),
        w10 / 10,
        w10 % 10
    ));
    let dir = format!("{}/class/power_supply", st().p.sys);
    let rd = match fs::read_dir(&dir) {
        Ok(d) => d,
        Err(_) => {
            out.push_str("err=no_power_supply\n");
            return out;
        }
    };
    const ATTRS: [&str; 14] = [
        "online",
        "status",
        "type",
        "real_type",
        "voltage_now",
        "voltage_max",
        "current_now",
        "current_max",
        "input_current_limit",
        "charge_type",
        "constant_charge_current_max",
        "capacity",
        "temp",
        "cycle_count",
    ];
    let mut names: Vec<String> = rd
        .flatten()
        .map(|e| e.file_name().to_string_lossy().to_string())
        .filter(|n| !n.starts_with('.') && is_dir(&format!("{}/{}", dir, n)))
        .collect();
    names.sort();
    for n in names.iter().take(12) {
        let pd = format!("{}/{}", dir, n);
        let mut line = String::new();
        for a in ATTRS {
            if let Some(v) = rdline(&format!("{}/{}", pd, a)) {
                line.push_str(&format!("{}={} ", a, v));
            }
        }
        if !line.is_empty() {
            out.push_str(&format!("{}: {}\n", n, line));
        }
    }
    out
}

/// 调度栈只读探测：WALT / sched_ext(hmbird) / 厂商 frame_boost / cpu_capacity。
///
/// 背景：给一加 15 做调优时经常拿到「WALT 那一套」的通用建议（/proc/sys/walt/*），
/// 但这台机器的实际调度器是 sched_ext（风驰 hmbird_II）。先把「哪套接口真的存在」
/// 打出来，再决定要不要写，避免对着不存在的节点调半天。
fn scx_ops_name() -> String {
    for p in ["/sys/kernel/sched_ext/root/ops", "/proc/sys/kernel/sched_ext/ops"] {
        if let Some(v) = rdline(p) {
            if !v.is_empty() {
                return v;
            }
        }
    }
    "-".to_string()
}

fn sched_probe_lines() -> String {
    let mut o = String::new();
    let wdir = format!("{}/sys/walt", st().p.proc_);
    let up = format!("{}/sched_upmigrate", wdir);
    o.push_str(&format!(
        "walt={} upmigrate={} downmigrate={}\n",
        if exists(&up) { "yes" } else { "no" },
        rdline(&up).unwrap_or_else(|| "-".to_string()),
        rdline(&format!("{}/sched_downmigrate", wdir)).unwrap_or_else(|| "-".to_string())
    ));
    // 本机的 cpufreq governor：WALT 机器上是 "walt"（kernel/sched/walt/cpufreq_walt.c），
    // 不是 schedutil；写 tunable 时目录名要跟着变。
    let mut govs = String::new();
    for i in 0..st().pol.len() {
        govs.push_str(&format!("{}={} ", st().pol[i].name, gov_of(i as isize)));
    }
    o.push_str(&format!("governor: {}\n", govs.trim_end()));
    o.push_str(&format!(
        "walt_boost: input_boost_ms={} sched_boost_on_input={} sched_boost={}\n",
        rdline(&format!("{}/input_boost_ms", wdir)).unwrap_or_else(|| "-".to_string()),
        rdline(&format!("{}/sched_boost_on_input", wdir)).unwrap_or_else(|| "-".to_string()),
        rdline(&format!("{}/sched_boost", wdir)).unwrap_or_else(|| "-".to_string())
    ));
    o.push_str(&format!(
        "scx_ops={} hmbird_live={}\n",
        scx_ops_name(),
        if hmbird_live() { 1 } else { 0 }
    ));
    let fb = "/proc/oplus_frame_boost/info";
    o.push_str(&format!(
        "frame_boost={} {}\n",
        if exists(fb) { "yes" } else { "no" },
        rdline(fb).unwrap_or_else(|| "-".to_string())
    ));
    o.push_str(&format!(
        "daemon_pin={} perf_mask=0x{:x}\n",
        st().c.daemon_pin,
        perf_mask()
    ));
    let mut caps = String::new();
    for i in 0..st().pol.len() {
        let n = st().pol[i].name.clone();
        let cpu = n.trim_start_matches("policy");
        let c = rdnum(&format!("{}/cpu{}/cpu_capacity", st().p.cpu, cpu));
        caps.push_str(&format!("{}={} ", n, c));
    }
    o.push_str(&format!("cpu_capacity: {}\n", caps.trim_end()));
    o
}

fn cmd_status() -> String {
    let mut out = String::new();
    match fs::read_to_string(&st().p.state) {
        Ok(t) => out.push_str(&t),
        Err(_) => out.push_str("profile=unknown\n"),
    }
    let pid = rdnum(&st().p.pid);
    out.push_str(&format!(
        "runner={}\n",
        if pid_alive(pid) { "running" } else { "stopped" }
    ));
    out.push_str(&format!("pid={}\n", if pid > 0 { pid } else { 0 }));

    if let Some(b) = rdline("/proc/sys/kernel/osrelease") {
        out.push_str(&format!("kernel={}\n", b));
    }
    if let Some(b) = rdline("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq") {
        out.push_str(&format!("gpu_cur={}\n", b));
    }
    let ip = st().idx_perf;
    let ipr = st().idx_prime;
    out.push_str(&format!(
        "perf_policy={}:{}\n",
        if ip >= 0 { policy_name(ip) } else { "-".to_string() },
        if ip >= 0 { st().pol[ip as usize].max } else { 0 }
    ));
    out.push_str(&format!(
        "prime_policy={}:{}\n",
        if ipr >= 0 {
            policy_name(ipr)
        } else {
            "-".to_string()
        },
        if ipr >= 0 {
            st().pol[ipr as usize].max
        } else {
            0
        }
    ));
    for i in 0..st().pol.len() {
        let n = st().pol[i].name.clone();
        out.push_str(&format!("gov_{}={}\n", n, gov_of(i as isize)));
        out.push_str(&format!("max_{}={}\n", n, policy_cap_now(i as isize)));
    }
    out.push_str(&format!(
        "hmbird_present={}\n",
        if st().hb_present { 1 } else { 0 }
    ));
    out.push_str(&sched_probe_lines());
    out.push_str(&format!(
        "model={}\n",
        if st().model.is_empty() {
            "OnePlus 15"
        } else {
            &st().model
        }
    ));
    out.push_str(&format!("android={}\n", st().android));
    let c = &st().c;
    out.push_str(&format!("cfg_enable={}\n", c.enable));
    out.push_str(&format!("cfg_mode={}\n", c.mode));
    out.push_str(&format!("cfg_intensity={}\n", c.intensity));
    out.push_str(&format!("cfg_auto_game={}\n", c.auto_game));
    out.push_str(&format!("cfg_thread_opt={}\n", c.thread_opt));
    out.push_str(&format!("cfg_screen_off={}\n", c.screen_off_saver));
    out.push_str(&format!("cfg_kill_boost={}\n", c.kill_boost));
    out.push_str(&format!("cfg_io={}\n", c.io_tune));
    out.push_str(&format!("cfg_gpu={}\n", c.gpu_tune));
    out.push_str(&format!("cfg_launch_boost={}\n", c.launch_boost));
    out.push_str(&format!("cfg_log_level={}\n", c.log_level));
    out.push_str(&format!("cfg_charge_ctrl={}\n", c.charge_ctrl));
    out.push_str(&format!("cfg_accel_ctrl={}\n", c.accel_ctrl));
    out.push_str(&format!("cfg_accel_tbat={}\n", c.accel_tbat));
    out.push_str(&format!("cfg_accel_stop={}\n", c.accel_stop_temp));
    out.push_str(&format!("cfg_accel_gentle={}\n", c.accel_gentle));
    out.push_str(&format!("cfg_accel_gauge={}\n", c.accel_gauge));
    out.push_str(&format!("cfg_game_mode={}\n", c.game_mode));
    out.push_str(&format!("chg_last_pct={}\n", st().chg_last_pct));
    out.push_str(&format!(
        "chg_suspend={}\n",
        if st().chg_suspended { 1 } else { 0 }
    ));
    out.push_str(&format!("chg_gear={}\n", st().chg_gear));
    out.push_str(&format!(
        "chg_node={}\n",
        if st().chg_cur.valid {
            st().chg_cur.path.clone()
        } else {
            "-".to_string()
        }
    ));
    let cdv = if st().chg_cd.valid {
        rdline(&st().chg_cd.path).unwrap_or_else(|| "-".to_string())
    } else {
        "-".to_string()
    };
    out.push_str(&format!("chg_cool_down={}\n", cdv));
    out.push_str(&format!("games={}\n", st().games.len()));
    out.push_str(&format!("version={}\n", crate::state::APP_VER));
    out
}

fn policy_name(idx: isize) -> String {
    if idx < 0 {
        return "-".to_string();
    }
    let p = &st().pol[idx as usize].path;
    p.rsplit('/').next().unwrap_or("-").to_string()
}

fn cmd_doctor() -> String {
    let mut out = String::new();
    out.push_str(&format!(
        "== FeatherTune doctor {} ==\n",
        crate::state::APP_VER
    ));
    let kern = rdline("/proc/sys/kernel/osrelease").unwrap_or_else(|| "?".to_string());
    out.push_str(&format!("kernel={}\n", kern));
    let (ip, ipr) = (st().idx_perf, st().idx_prime);
    out.push_str(&format!(
        "policies={} perf={}({}) prime={}({})\n",
        st().pol.len(),
        if ip >= 0 { policy_name(ip) } else { "-".to_string() },
        if ip >= 0 { st().pol[ip as usize].max } else { 0 },
        if ipr >= 0 {
            policy_name(ipr)
        } else {
            "-".to_string()
        },
        if ipr >= 0 {
            st().pol[ipr as usize].max
        } else {
            0
        }
    ));
    for i in 0..st().pol.len() {
        let base = if st().base_ok && i < crate::state::MAX_POL {
            st().base_max[i]
        } else {
            0
        };
        out.push_str(&format!(
            "{}: gov={} max={}/{} min={} freqs={} base_max={}\n",
            st().pol[i].name,
            gov_of(i as isize),
            policy_cap_now(i as isize),
            st().pol[i].max,
            st().pol[i].min,
            st().pol[i].freq.len(),
            base
        ));
    }
    out.push_str(&format!(
        "hmbird present={} live={} tuned={} sysctl={}\n",
        if st().hb_present { 1 } else { 0 },
        if hmbird_live() { 1 } else { 0 },
        if st().hb_tuned { 1 } else { 0 },
        st().p.hm
    ));
    for k in [
        "coefficient",
        "perf_high_ratio",
        "cpus_reserved",
        "cpus_exclusive",
        "freq_policy",
        "frame_per_sec",
        "heavy_boost",
    ] {
        let p = format!("{}/{}", st().p.hm, k);
        if let Some(b) = rdline(&p) {
            out.push_str(&format!("  {}={}\n", k, b));
        }
    }
    for g in ["top-app", "foreground", "background", "system-background"] {
        let p = format!("{}/{}/cpu.uclamp.max", st().p.cgrp, g);
        out.push_str(&format!(
            "{} uclamp_max={}\n",
            g,
            rdline(&p).unwrap_or_else(|| "n/a".to_string())
        ));
    }
    out.push_str(&sched_probe_lines());
    let bright = if st().p.bright.is_empty() {
        "n/a".to_string()
    } else {
        rdline(&st().p.bright.clone()).unwrap_or_else(|| "?".to_string())
    };
    out.push_str(&format!("brightness={}\n", bright));

    out.push_str(&format!(
        "battery={}% {} temp={}\n",
        battery_cap(),
        battery_status(),
        battery_temp()
    ));
    {
        accel_init();
        let an = if st().acc_shell.valid {
            Some(st().acc_shell.clone())
        } else if st().acc_batt.valid {
            Some(st().acc_batt.clone())
        } else {
            None
        };
        let av = an
            .as_ref()
            .and_then(|n| rdline(&n.path))
            .unwrap_or_else(|| "-".to_string());
        let node = if st().acc_shell.valid {
            "oplus_dbg_tbat"
        } else if st().acc_batt.valid {
            "gauge_dbg_tbat"
        } else {
            "MISSING"
        };
        out.push_str(&format!(
            "accel ctrl={} gauge={} node={} now={} orig={} target={} dt={} spoofing={} blocked={}\n",
            st().c.accel_ctrl,
            st().c.accel_gauge,
            node,
            av,
            an.as_ref().map(|n| n.orig.clone()).unwrap_or_else(|| "-".to_string()),
            accel_target(),
            st().acc_dt_val,
            if st().acc_spoofing { 1 } else { 0 },
            if st().acc_blocked { 1 } else { 0 }
        ));
        if !st().acc_shell.valid && !st().acc_batt.valid {
            out.push_str("  note: 没有 oplus_chg 的温度参数节点（非 OPPO 机型），充电加速不生效\n");
        } else if !st().acc_shell.valid {
            out.push_str("  note: 只有 gauge_dbg_tbat（会连电池保护一起骗），需要 accel_gauge=1 才启用\n");
        } else if st().acc_dt_val == 0 {
            out.push_str("  note: 没读到 DT oplus,temp_range，用默认 25.0℃；没效果就试 set accel_tbat 200\n");
        }
    }
    {
        charge_init();
        out.push_str(&format!(
            "charge ctrl={} knee={} wall={} node={} kind={} suspend={}\n",
            st().c.charge_ctrl,
            st().c.charge_knee_soc,
            st().c.charge_temp_wall,
            if st().chg_cur.valid {
                st().chg_cur.path.clone()
            } else {
                "-".to_string()
            },
            if st().chg_cur.valid {
                chg_kind_name(st().chg_cur.kind)
            } else {
                "none"
            },
            if st().chg_suspend.valid {
                st().chg_suspend.path.clone()
            } else {
                "-".to_string()
            },
        ));
        if !st().chg_cur.valid {
            out.push_str("  note: 没有可写的电流节点 → charge_ctrl 在本机不生效（内核没给用户态限流接口）\n");
        } else if st().chg_cur.kind == 2 {
            out.push_str("  note: 走 charger v2 的 battery/current_now，仅息屏限流（亮屏由厂商 HAL 接管）\n");
        }
    }
    {
        let idir = st().p.icons.clone();
        let mut cached = 0;
        let mut miss = 0;
        if let Ok(rd) = fs::read_dir(&idir) {
            for e in rd.flatten() {
                let n = e.file_name().to_string_lossy().to_string();
                if n == ".miss" {
                    miss = fs::read_to_string(e.path())
                        .map(|t| t.lines().filter(|l| !l.trim().is_empty()).count())
                        .unwrap_or(0);
                } else if !n.starts_with('.') {
                    cached += 1;
                }
            }
        }
        out.push_str(&format!(
            "icon tool={} cached={} miss={} dir={}\n",
            if st().unzip.is_empty() {
                "-".to_string()
            } else {
                st().unzip.clone()
            },
            cached,
            miss,
            idir
        ));
        if st().unzip.is_empty() && st().unzip_none {
            out.push_str("  note: 没找到可用的 unzip（busybox/toybox/unzip 全失败），图标只能退化\n");
        }
    }
    out.push_str(&format!(
        "boost_killed={}\ntaskset={}\n",
        st().boost_killed,
        if exists("/system/bin/taskset") {
            "/system/bin/taskset"
        } else {
            "none"
        }
    ));
    out.push_str(&format!(
        "boost: mode={} (1=WALT 2=oplus_touch 3=legacy) probed={} killed={}\n",
        st().boost_mode, st().boost_probed, st().boost_killed
    ));
    for p in boost_targets() {
        if exists(&p) {
            out.push_str(&format!(
                "  boost node {} = {} ({})\n",
                p,
                rdline(&p).unwrap_or_else(|| "?".to_string()),
                if writable(&p) { "writable" } else { "readonly" }
            ));
        }
    }
    out.push_str(&format!("refresh={}Hz\n", st().refresh));
    if ip >= 0 {
        let mp = format!("{}/scaling_min_freq", st().pol[ip as usize].path);
        out.push_str(&format!(
            "perf_min={} (baseline {})\n",
            rdnum(&mp),
            if st().base_ok {
                st().base_min[ip as usize]
            } else {
                -1
            }
        ));
    }
    {
        let bp = format!("{}/background/cpuset.cpus", st().p.cgrp);
        out.push_str(&format!(
            "bg_cpuset={}\n",
            rdline(&bp).unwrap_or_else(|| "n/a".to_string())
        ));
    }
    {
        let fg = foreground_pkg().unwrap_or_else(|| "-".to_string());
        out.push_str(&format!(
            "fg={} (source={} 1=window 2=activity 3=cgroup 0=none) cands={} ime={}\n",
            fg,
            st().fg_source,
            st().fg_cands.len(),
            if st().ime_pkg.is_empty() {
                "-".to_string()
            } else {
                st().ime_pkg.clone()
            }
        ));
        for (k, c) in st().fg_cands.clone().iter().enumerate() {
            out.push_str(&format!(
                "  cand{}={}{}\n",
                k,
                c,
                if is_game(c) { " [game]" } else { "" }
            ));
        }
    }
    {
        out.push_str(&format!(
            "cgroup_root={} (node={})\n",
            st().p.cgrp,
            st().p.cset_node
        ));
        out.push_str(&format!(
            "cpuctl_root={} uclamp_ok={}\n",
            st().p.cpuctl,
            if st().uclamp_ok { 1 } else { 0 }
        ));
        for c in [
            "/top-app/cgroup.procs",
            "/top-app/tasks",
            "/background/cpuset.cpus",
            "/cgroup.controllers",
        ] {
            let p = format!("{}{}", st().p.cgrp, c);
            out.push_str(&format!(
                "  probe {} = {}\n",
                c,
                if readable(&p) { "readable" } else { "MISSING" }
            ));
        }
        let bgp = format!("{}/background/{}", st().p.cgrp, st().p.cset_node);
        out.push_str(&format!(
            "  bg_cpus {} = {}\n",
            bgp,
            if writable(&bgp) { "writable" } else { "MISSING" }
        ));
        let up = format!("{}/foreground/cpu.uclamp.max", st().p.cpuctl);
        out.push_str(&format!(
            "  uclamp {} = {}\n",
            up,
            if writable(&up) { "writable" } else { "MISSING" }
        ));
        if let Ok(t) = fs::read_to_string("/proc/self/cgroup") {
            for (k, l) in t.lines().enumerate() {
                if k >= 4 {
                    break;
                }
                out.push_str(&format!("  self_cgroup: {}\n", l));
            }
        }
    }
    out.push_str(&format!(
        "prefer_errno={} (13=EACCES 22=EINVAL 2=ENOENT)\n",
        st().pm_errno
    ));
    {
        out.push_str(&format!("rules={}\n", st().rules.len()));
        for (i, r) in st().rules.iter().take(10).enumerate() {
            out.push_str(&format!(
                "  rule{}: {} | {} | {}\n",
                i,
                r.pkg,
                r.pat,
                if r.target.is_empty() { "?" } else { &r.target }
            ));
        }
    }
    out.push_str(&format!(
        "pin_kind={} (1=prefer_cluster 2=prefer_cpu 3=single-core 4=multi)\n",
        st().pin_kind
    ));
    if st().hb_present {
        let me = unsafe { getpid() } as i64;
        out.push_str(&format!(
            "selftest prefer_cluster={}\n",
            if hm_selftest("prefer_cluster", "0x2", me) {
                "ok"
            } else {
                "FAIL"
            }
        ));
        out.push_str(&format!(
            "selftest prefer_cpu={}\n",
            if hm_selftest("prefer_cpu", "0x40", me) {
                "ok"
            } else {
                "FAIL"
            }
        ));
        out.push_str(&format!(
            "selftest prefer_preempt={}\n",
            if hm_selftest("prefer_preempt", "1", me) {
                "ok"
            } else {
                "FAIL"
            }
        ));
        let pp = format!("{}/frame_per_sec", st().p.hm);
        out.push_str(&format!(
            "frame_per_sec_now={}\n",
            rdline(&pp).unwrap_or_else(|| "n/a".to_string())
        ));
    }
    let pid = rdnum(&st().p.pid);
    out.push_str(&format!(
        "daemon pid={} alive={}\n",
        if pid > 0 { pid } else { 0 },
        if pid_alive(pid) { 1 } else { 0 }
    ));
    out
}

fn cmd_probe() -> String {
    let mut out = String::new();
    out.push_str(&format!("version={}\n", crate::state::APP_VER));
    out.push_str(&format!("cgroup_root={}\n", st().p.cgrp));
    out.push_str(&format!("cset_node={}\n", st().p.cset_node));
    out.push_str(&format!("cpuctl_root={}\n", st().p.cpuctl));
    {
        let bgp = format!("{}/background/{}", st().p.cgrp, st().p.cset_node);
        out.push_str(&format!(
            "bg_cpus={} {}\n",
            bgp,
            if writable(&bgp) { "W" } else { "-" }
        ));
        out.push_str(&format!(
            "bg_cpus_val={}\n",
            rdline(&bgp).unwrap_or_else(|| "?".to_string())
        ));
        let tp = format!("{}/top-app/cgroup.procs", st().p.cgrp);
        let (n, bytes) = match read_all(&tp) {
            Some(t) => (t.matches('\n').count(), t.len()),
            None => (0, 0),
        };
        out.push_str(&format!("topapp_procs={} read={}\n", n, bytes));
        for g in ["top-app", "foreground", "background"] {
            let p = format!("{}/{}/cpu.uclamp.max", st().p.cpuctl, g);
            out.push_str(&format!(
                "uclamp_{}={}\n",
                g,
                if writable(&p) { "W" } else { "-" }
            ));
        }
    }
    out.push_str(&format!("refresh={}Hz\n", st().refresh));
    out.push_str(&format!(
        "hmbird_dir={}\n",
        if st().hb_present { 1 } else { 0 }
    ));
    {
        let fp = format!("{}/frame_per_sec", st().p.hm);
        out.push_str(&format!(
            "frame_per_sec={}\n",
            rdline(&fp).unwrap_or_else(|| "?".to_string())
        ));
        let pc = format!("{}/prefer_cpu", st().p.hm);
        out.push_str(&format!(
            "prefer_cpu_entries={}\n",
            if read_all(&pc).is_some() {
                "readable"
            } else {
                "unreadable"
            }
        ));
    }
    out.push_str(&format!("rules={}\n", st().rules.len()));
    out.push_str(&format!(
        "fg={}\n",
        foreground_pkg().unwrap_or_default()
    ));
    let ip = st().idx_perf;
    out.push_str(&format!(
        "perf={} cap={}/{}\n",
        if ip >= 0 {
            st().pol[ip as usize].path.clone()
        } else {
            "-".to_string()
        },
        if ip >= 0 { policy_cap_now(ip) } else { 0 },
        if ip >= 0 { st().pol[ip as usize].max } else { 0 }
    ));
    out
}

fn orig_or_dash(n: &crate::state::Node) -> String {
    if n.valid {
        n.orig.clone()
    } else {
        "-".to_string()
    }
}

// 兼容：apps 上限在 state 里定义
#[allow(dead_code)]
fn _max_apps() -> usize {
    MAX_APPS
}
