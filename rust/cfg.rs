// ============================================================================
//  cfg.rs —— config.ini / apps.conf / labels.conf / games.conf / rules.conf
//            的解析与写回（含 mtime 变更检测，避免每拍重复解析）
// ============================================================================
#![allow(static_mut_refs)]

use std::fs;
use std::io::Write;

use crate::state::{st, AppRule, Cfg, Label, Rule, MAX_APPS, MAX_LABELS, MAX_RULES};
use crate::util::*;

pub fn cfg_load() -> bool {
    let pconf = st().p.conf.clone();
    let mt = mtime(&pconf);
    if mt == st().conf_mtime && st().conf_mtime.0 != 0 {
        return false;
    }
    st().conf_mtime = mt;

    st().c = Cfg::default();
    if let Ok(text) = fs::read_to_string(&pconf) {
        for line in text.lines() {
            let s = line.trim_start_matches([' ', '\t']);
            if s.is_empty() || s.starts_with('#') || s.starts_with(';') {
                continue;
            }
            let eq = match s.find('=') {
                Some(i) => i,
                None => continue,
            };
            let key = s[..eq].trim_end_matches([' ', '\t']).to_string();
            let mut val = s[eq + 1..].trim_start_matches([' ', '\t']).to_string();
            // 去掉行尾注释
            if let Some(h) = val.find('#') {
                val = val[..h].to_string();
            }
            let val = val.trim_matches(['\n', '\r', ' ', '\t']).to_string();
            let v = val.as_str();
            let c = &mut st().c;
            match key.as_str() {
                "enable" => c.enable = atoi_safe(v, 1),
                "mode" => c.mode = v.to_string(),
                "intensity" => c.intensity = atoi_safe(v, 50),
                "auto_game" => c.auto_game = atoi_safe(v, 1),
                "screen_off_saver" => c.screen_off_saver = atoi_safe(v, 1),
                "thread_opt" => c.thread_opt = atoi_safe(v, 1),
                "kill_boost" => c.kill_boost = atoi_safe(v, 1),
                "io_tune" => c.io_tune = atoi_safe(v, 1),
                "gpu_tune" => c.gpu_tune = atoi_safe(v, 1),
                "launch_boost" => c.launch_boost = atoi_safe(v, 1),
                "boost_ms" => c.boost_ms = atoi_safe(v, 4000),
                "log_level" => c.log_level = atoi_safe(v, 1),
                "hmbird_tune" => c.hmbird_tune = atoi_safe(v, 0),
                "hmbird_frame_per_sec" => c.hmbird_frame = atoi_safe(v, 0),
                "game_min_pct" => c.game_min_pct = atoi_safe(v, 42),
                "game_bg_cpus" => c.game_bg_cpus = atoi_safe(v, 1),
                "hmbird_reserved" => c.hmbird_reserved = atoi_safe(v, 192),
                "hmbird_coeff" => c.hm_coeff = v.to_string(),
                "hmbird_high_ratio" => c.hm_high = v.to_string(),
                "hmbird_heavy" => c.hm_heavy = v.to_string(),
                "game_mode" => c.game_mode = v.to_string(),
                "charge_ctrl" => c.charge_ctrl = atoi_safe(v, 1),
                "charge_knee_soc" => c.charge_knee_soc = atoi_safe(v, 67),
                "charge_temp_wall" => c.charge_temp_wall = atoi_safe(v, 50),
                "charge_resume_temp" => c.charge_resume_temp = atoi_safe(v, 45),
                "charge_curve" => c.chg_curve = v.to_string(),
                "accel_ctrl" => c.accel_ctrl = atoi_safe(v, 1),
                "accel_tbat" => c.accel_tbat = atoi_safe(v, 0),
                "accel_stop_temp" => c.accel_stop_temp = atoi_safe(v, 45),
                "accel_resume_temp" => c.accel_resume_temp = atoi_safe(v, 42),
                "accel_gentle" => c.accel_gentle = atoi_safe(v, 0),
                "accel_gauge" => c.accel_gauge = atoi_safe(v, 0),
                "battery_cells" => c.battery_cells = atoi_safe(v, 0),
                "daemon_pin" => c.daemon_pin = atoi_safe(v, 1),
                _ => {}
            }
        }
    }
    // v1.3.0 的老配置里 accel_tbat=360，落在 warm/表外那一段，写它等于反向优化。
    // 这里把越界的手动值统一退回自动档（用 DT 分桶表算），不让老配置继续拖后腿。
    if st().c.accel_tbat > 340 {
        let v = st().c.accel_tbat;
        logv(&format!(
            "accel: accel_tbat={} 落在 warm/表外（有效范围 150~340），已退回自动档",
            v
        ));
        st().c.accel_tbat = 0;
    }
    // v1.4.1：v1.2.6 的默认曲线末尾多了一个 ",43:0"，按「取最后一个 <= 温度的点」
    // 会把 43℃ 以上全部算成 0%（等于停充）。加载时清洗一遍，老配置不用手改。
    {
        let raw = st().c.chg_curve.clone();
        let mut out = String::new();
        let mut last_t = i64::MIN;
        let mut dropped = 0;
        for tok in raw.split(',') {
            let mut it = tok.split(':');
            let t = match it.next().and_then(|s| s.trim().parse::<i64>().ok()) {
                Some(v) => v,
                None => continue,
            };
            let p = match it.next().and_then(|s| s.trim().parse::<i32>().ok()) {
                Some(v) => v,
                None => continue,
            };
            if t < last_t || t > 90 || p < 20 || p > 100 {
                dropped += 1;
                continue;
            }
            last_t = t;
            if !out.is_empty() {
                out.push(',');
            }
            out.push_str(&format!("{}:{}", t, p));
        }
        if dropped > 0 && !out.is_empty() {
            logv(&format!(
                "charge: charge_curve 丢弃 {} 个非法点（乱序/0%/越界），有效曲线 {}",
                dropped, out
            ));
            st().c.chg_curve = out;
        }
    }
    st().log_level = st().c.log_level;
    true
}

pub fn cfg_set(key: &str, val: &str) -> bool {
    let pconf = st().p.conf.clone();
    let tmp = format!("{}/.cfg.tmp", st().p.data);
    let old = fs::read_to_string(&pconf).unwrap_or_default();
    let mut out = String::new();
    let mut done = false;
    for line in old.lines() {
        let s = line.trim_start_matches([' ', '\t']);
        let mut matched = false;
        if !s.starts_with('#') && !s.starts_with(';') {
            if let Some(eq) = s.find('=') {
                let k = s[..eq].trim_end_matches([' ', '\t']);
                if k == key {
                    matched = true;
                }
            }
        }
        if matched {
            out.push_str(&format!("{}={}\n", key, val));
            done = true;
        } else {
            out.push_str(line);
            out.push('\n');
        }
    }
    if !done {
        out.push_str(&format!("{}={}\n", key, val));
    }
    let mut f = match fs::File::create(&tmp) {
        Ok(f) => f,
        Err(_) => return false,
    };
    if f.write_all(out.as_bytes()).is_err() {
        return false;
    }
    drop(f);
    if fs::rename(&tmp, &pconf).is_err() {
        return false;
    }
    st().conf_mtime = (0, 0);
    cfg_load();
    true
}

// ------------------------------------------------------------------ apps ----
pub fn apps_load() {
    let papps = st().p.apps.clone();
    let mt = mtime(&papps);
    if mt == st().apps_mtime && st().apps_mtime.0 != 0 {
        return;
    }
    st().apps_mtime = mt;
    st().apps.clear();
    let text = fs::read_to_string(&papps).unwrap_or_default();
    for line in text.lines() {
        let line = match line.find('#') {
            Some(i) => &line[..i],
            None => line,
        };
        let f: Vec<&str> = line.split_whitespace().collect();
        if f.len() < 2 {
            continue;
        }
        if f[1] == "follow" {
            continue; // follow 等于没配置
        }
        if st().apps.len() >= MAX_APPS {
            break;
        }
        let perf = f.get(2).map(|s| atoi_safe(s, 0)).unwrap_or(0);
        let prime = f.get(3).map(|s| atoi_safe(s, 0)).unwrap_or(0);
        let floor = f.get(4).map(|s| atoi_safe(s, 0)).unwrap_or(0);
        let threads = f.get(5).map(|s| atoi_safe(s, 0)).unwrap_or(0);
        st().apps.push(AppRule {
            pkg: f[0].to_string(),
            mode: f[1].to_string(),
            perf: if perf > 0 { perf } else { 100 },
            prime: if prime > 0 { prime } else { 100 },
            floor,
            threads,
        });
    }
    let n = st().apps.len();
    logv(&format!("apps: 载入 {} 条应用策略", n));
}

pub fn app_find(pkg: &str) -> isize {
    if pkg.is_empty() {
        return -1;
    }
    for (i, a) in st().apps.iter().enumerate() {
        if a.pkg == pkg {
            return i as isize;
        }
    }
    -1
}

// ---------------------------------------------------------------- labels ----
pub fn labels_load() {
    let pl = st().p.labels.clone();
    let mt = mtime(&pl);
    if mt == st().labels_mtime && st().labels_mtime.0 != 0 {
        return;
    }
    st().labels_mtime = mt;
    st().labels.clear();
    let text = fs::read_to_string(&pl).unwrap_or_default();
    for line in text.lines() {
        let line = match line.find('#') {
            Some(i) => &line[..i],
            None => line,
        };
        if st().labels.len() >= MAX_LABELS {
            break;
        }
        let eq = match line.find('=') {
            Some(i) => i,
            None => continue,
        };
        let k = line[..eq].trim();
        let v = line[eq + 1..].trim();
        if k.is_empty() || v.is_empty() {
            continue;
        }
        st().labels.push(Label {
            pkg: k.to_string(),
            name: v.to_string(),
        });
    }
}

pub fn label_of(pkg: &str) -> Option<String> {
    for l in st().labels.iter() {
        if l.pkg == pkg {
            return Some(l.name.clone());
        }
    }
    None
}

// ------------------------------------------------------------------ games ----
pub fn games_load() {
    let pg = st().p.games.clone();
    let mt = mtime(&pg);
    if mt == st().games_mtime && st().games_mtime.0 != 0 {
        return;
    }
    st().games_mtime = mt;
    st().games.clear();
    let text = fs::read_to_string(&pg).unwrap_or_default();
    for line in text.lines() {
        let s = line.trim();
        if s.is_empty() || s.starts_with('#') {
            continue;
        }
        st().games.push(s.to_string());
    }
}

pub fn is_game(pkg: &str) -> bool {
    if pkg.is_empty() {
        return false;
    }
    st().games.iter().any(|g| g == pkg)
}

// ------------------------------------------------------------------ rules ----
/// 掩码：prime=CPU6-7 等
pub fn mask_of(t: &str) -> Option<String> {
    crate::cpu::target_mask(t)
}

pub fn rules_load() {
    let pr = st().p.rules.clone();
    let mt = mtime(&pr);
    if mt == st().rules_mtime && st().rules_mtime.0 != 0 {
        return;
    }
    st().rules_mtime = mt;
    st().rules.clear();
    let text = fs::read_to_string(&pr).unwrap_or_default();
    for line in text.lines() {
        if st().rules.len() >= MAX_RULES {
            break;
        }
        let line = match line.find('#') {
            Some(i) => &line[..i],
            None => line,
        };
        let s = line.trim();
        if s.is_empty() {
            continue;
        }
        // 最后一个空白分隔字段是目标，中间整段都是正则（正则里可以带空格）
        let last = match s.rfind([' ', '\t']) {
            Some(i) => i,
            None => continue,
        };
        let tgt = s[last + 1..].trim();
        if tgt.is_empty() {
            continue;
        }
        let head = s[..last].trim_end();
        let sp2 = match head.find([' ', '\t']) {
            Some(i) => i,
            None => continue,
        };
        let pkg = head[..sp2].to_string();
        let pat = head[sp2..].trim().to_string();
        if pat.is_empty() {
            continue;
        }

        // 目标支持 '+' 组合：核掩码 + hmbird 调度属性
        let mask_part = tgt.split('+').next().unwrap_or("");
        let mask = mask_of(mask_part);

        // 非核掩码目标：preempt / idle 交给 hmbird 自己的调度属性
        if mask.is_none() && (tgt == "preempt" || tgt == "idle") {
            let re = Regex::new(&pat);
            if !re.valid {
                continue;
            }
            st().rules.push(Rule {
                pkg,
                pat,
                target: tgt.to_string(),
                re,
                valid: true,
            });
            continue;
        }
        if mask.is_none() {
            logmsg(&format!("rules: 跳过未知目标 '{}' 的行", tgt));
            continue;
        }
        let re = Regex::new(&pat);
        if !re.valid {
            logmsg(&format!("rules: 正则编译失败，跳过 '{}'", pat));
            continue;
        }
        st().rules.push(Rule {
            pkg,
            pat,
            target: tgt.to_string(),
            re,
            valid: true,
        });
    }
}

// ------------------------------------------------------------- build.prop ----
/// 机型/系统信息：先扫 build.prop（快），缺的再用 getprop 补齐（准）。
/// ColorOS 的 ro.product.marketname / ro.product.model 常在 /my_product/build.prop，
/// 老实现只扫 /system 与 /vendor，结果 model 为空、WebUI 退回硬编码的「OnePlus 15」。
pub fn read_buildprops() {
    fn set_prop(k: &str, v: &str) {
        let v = v.trim();
        if v.is_empty() {
            return;
        }
        match k {
            "ro.product.marketname" if st().market.is_empty() => st().market = v.to_string(),
            "ro.product.model" if st().model.is_empty() => st().model = v.to_string(),
            "ro.product.device" if st().device.is_empty() => st().device = v.to_string(),
            "ro.board.platform" if st().platform.is_empty() => st().platform = v.to_string(),
            "ro.build.version.release" if st().android.is_empty() => st().android = v.to_string(),
            _ => {}
        }
    }

    let files = [
        "/my_product/build.prop",
        "/system/build.prop",
        "/vendor/build.prop",
        "/odm/etc/build.prop",
        "/product/build.prop",
        "/system_ext/build.prop",
    ];
    for f in files {
        let text = match fs::read_to_string(f) {
            Ok(t) => t,
            Err(_) => continue,
        };
        for line in text.lines() {
            let s = line.trim_start_matches([' ', '\t']);
            if s.is_empty() || s.starts_with('#') {
                continue;
            }
            let eq = match s.find('=') {
                Some(i) => i,
                None => continue,
            };
            set_prop(&s[..eq], &s[eq + 1..]);
        }
    }

    // build.prop 里没有的（或机型把值放在别的分区）再问一次系统。
    // 非 Android 环境（测试机/qemu）没有 getprop，跳过，免得每个命令都往 stderr 打一串错。
    if !exists("/system/bin/getprop") && !exists("/system/bin/getprop64") {
        return;
    }
    for k in [
        "ro.product.marketname",
        "ro.product.model",
        "ro.product.device",
        "ro.board.platform",
        "ro.build.version.release",
    ] {
        let need = match k {
            "ro.product.marketname" => st().market.is_empty(),
            "ro.product.model" => st().model.is_empty(),
            "ro.product.device" => st().device.is_empty(),
            "ro.board.platform" => st().platform.is_empty(),
            _ => st().android.is_empty(),
        };
        if !need {
            continue;
        }
        if let Some(v) = popen_all(&format!("getprop {}", k), 2) {
            set_prop(k, &v);
        }
    }
}
