// ============================================================================
//  charge.rs —— 电池读数 / 充电电流阶梯 / 充电加速（伪装温度）/ 功率采样
// ============================================================================
#![allow(static_mut_refs)]

use std::fs;

use crate::cpu::screen_on;
use crate::state::{st, Node};
use crate::util::*;

// ---------------------------------------------------------------- 电池读数 ----
pub fn battery_cap() -> i64 {
    rdnum(&format!("{}/class/power_supply/battery/capacity", st().p.sys))
}

pub fn battery_temp() -> i64 {
    let v = rdnum(&format!("{}/class/power_supply/battery/temp", st().p.sys));
    if v > 0 {
        v / 10
    } else {
        -1
    }
}

pub fn battery_status() -> String {
    rdline(&format!("{}/class/power_supply/battery/status", st().p.sys)).unwrap_or_default()
}

/// 双电芯机型的 voltage_now 是单芯电压、current_now 是串联回路电流，
/// 真实功率必须乘电芯串联数。
///
/// 判定顺序（v1.4.2）：
///   1. config `battery_cells`（0 = 自动）
///   2. DT `oplus,batt_num`（一加 15 是 2）
///   3. 老办法：`voltage_max > 6V` 视为 2S
/// 第 3 条在 charger v2 上**一定会误判成 1S**：v2 的 VOLTAGE_MAX 返回的是
/// 单芯电压上限（~4.5V），不是整包终止电压，所以旧版功率永远少算一半。
fn dt_prop(dir: &str, depth: i32, name: &str) -> Option<Vec<u8>> {
    if depth > 5 {
        return None;
    }
    let p = format!("{}/{}", dir, name);
    if let Ok(md) = fs::metadata(&p) {
        if md.is_file() && md.len() > 0 && md.len() <= 256 {
            if let Ok(b) = fs::read(&p) {
                return Some(b);
            }
        }
    }
    let rd = match fs::read_dir(dir) {
        Ok(d) => d,
        Err(_) => return None,
    };
    let mut subs: Vec<String> = Vec::new();
    for e in rd.flatten() {
        let n = e.file_name().to_string_lossy().to_string();
        if n.starts_with('.') || n == "aliases" || n == "__symbols__" {
            continue;
        }
        subs.push(format!("{}/{}", dir, n));
    }
    for sub in subs {
        if is_dir(&sub) {
            if let Some(v) = dt_prop(&sub, depth + 1, name) {
                return Some(v);
            }
        }
    }
    None
}

fn dt_u32_be(b: &[u8]) -> i32 {
    if b.len() < 4 {
        return 0;
    }
    ((b[0] as i32) << 24) | ((b[1] as i32) << 16) | ((b[2] as i32) << 8) | (b[3] as i32)
}

pub fn battery_cells() -> i32 {
    if st().cells != 0 {
        return st().cells;
    }
    let mut n = 0;
    if st().c.battery_cells > 0 {
        n = st().c.battery_cells;
    }
    if n <= 0 {
        let dt = format!("{}/firmware/devicetree/base", st().p.sys);
        if let Some(b) = dt_prop(&dt, 0, "oplus,batt_num") {
            n = dt_u32_be(&b);
            logv(&format!("power: DT oplus,batt_num = {}", n));
        }
    }
    if n <= 0 {
        let vmax = rdnum(&format!(
            "{}/class/power_supply/battery/voltage_max",
            st().p.sys
        ));
        n = if vmax > 9_000_000 {
            3
        } else if vmax > 6_000_000 {
            2
        } else {
            1
        };
    }
    if n < 1 {
        n = 1;
    }
    if n > 4 {
        n = 4;
    }
    st().cells = n;
    n
}

/// 有的机型 current_now 是 µA（qc 系），有的是 mA（OPPO gauge）：>20A 一律按 µA 归一
pub fn current_ma(ma: i64) -> i64 {
    if ma > 20000 || ma < -20000 {
        ma / 1000
    } else {
        ma
    }
}

/// 返回 0.1W（mv 是单芯电压，双电芯要乘串联数）
pub fn power_w10(uv: i64, ma: i64) -> i64 {
    let mv = uv / 1000;
    let mut ima = current_ma(ma);
    if mv <= 0 {
        return 0;
    }
    if ima < 0 {
        ima = -ima;
    }
    let mvp = mv * battery_cells() as i64;
    mvp * ima / 100000
}

pub fn chg_read_power() -> (i64, i64) {
    let mv = rdnum(&format!(
        "{}/class/power_supply/battery/voltage_now",
        st().p.sys
    ));
    let ma = rdnum(&format!(
        "{}/class/power_supply/battery/current_now",
        st().p.sys
    ));
    (mv, ma)
}

// ------------------------------------------------------------- 充电控制 ----
/* 电流节点候选：(路径, kind)
 *   kind 0 = µA（power_supply 的 constant_charge_current_max / current_max / input_current_limit）
 *   kind 1 = mA （oplus_chg v1 的 class 节点、fastchg_current_max）
 *   kind 2 = mA （charger v2：battery/current_now —— HIDL 智能充电限流，仅息屏生效）
 *
 * 为什么顺序是这样：先兼容老机型/通用节点，最后才是 v2 的 current_now。
 * OnePlus 15（charger v2）的 battery power_supply 只暴露 current_now 这一个可写电流属性
 * （见 vendor/oplus/kernel/charger/v2/oplus_chg_gki.c: battery_psy_prop_is_writeable），
 * 所以旧版的 constant_charge_current_max 那条链路在 PLK110 上一直是空转。 */
const CHG_CUR_CANDS: [(&str, i32); 8] = [
    ("/class/power_supply/battery/constant_charge_current_max", 0),
    ("/class/power_supply/battery/constant_charge_current", 0),
    ("/class/power_supply/battery/current_max", 0),
    ("/class/power_supply/battery/input_current_limit", 0),
    ("/class/oplus_chg/battery/current_max", 1),
    ("/class/oplus_chg/battery/chg_current_max", 1),
    ("/class/oplus_chg/battery/fastchg_current_max", 1),
    ("/class/power_supply/battery/current_now", 2),
];

const CHG_SUSPEND_CANDS: [&str; 4] = [
    "/class/power_supply/battery/input_suspend",
    "/class/power_supply/battery/charge_disable",
    "/class/power_supply/battery/charging_enabled",
    "/class/oplus_chg/battery/input_suspend",
];

fn chg_pick(cands: &[&str]) -> Option<Node> {
    for c in cands {
        let p = format!("{}{}", st().p.sys, c);
        if !writable(&p) {
            continue;
        }
        if let Some(v) = rdline(&p) {
            return Some(Node {
                path: p,
                orig: v,
                valid: true,
                kind: 0,
            });
        }
    }
    None
}

fn chg_pick_cur() -> Option<Node> {
    for (c, kind) in CHG_CUR_CANDS {
        let p = format!("{}{}", st().p.sys, c);
        if !writable(&p) {
            continue;
        }
        if let Some(v) = rdline(&p) {
            return Some(Node {
                path: p,
                orig: v,
                valid: true,
                kind,
            });
        }
    }
    None
}

pub fn chg_kind_name(kind: i32) -> &'static str {
    match kind {
        1 => "mA",
        2 => "v2-current-now",
        _ => "uA",
    }
}

pub fn charge_init() {
    if st().chg_ready {
        return;
    }
    st().chg_ready = true;
    st().chg_base_ma = 0;
    st().chg_v2_written = 0;
    if let Some(n) = chg_pick_cur() {
        st().chg_cur = n;
    } else {
        logv("charge: 没有可写的电流节点，充电控制停用（这台机器的内核没给用户态留接口）");
        return;
    }
    // 暂停开关不是必须的：没有就只靠电流兜底
    if let Some(n) = chg_pick(&CHG_SUSPEND_CANDS) {
        st().chg_suspend = n;
    }
    // ColorOS 上标准电流节点常常整个不存在，厂商真正的入口是 oplus_chg 的 cool_down
    if let Some(n) = chg_pick(&[
        "/class/oplus_chg/battery/cool_down",
        "/class/oplus_chg/battery/normal_cool_down",
    ]) {
        st().chg_cd = n;
    }
    st().chg_orig_cur = parse_leading_num(&st().chg_cur.orig).abs();
    let (cp, co, sp, cdp) = (
        st().chg_cur.path.clone(),
        st().chg_cur.orig.clone(),
        if st().chg_suspend.valid {
            st().chg_suspend.path.clone()
        } else {
            "无".to_string()
        },
        if st().chg_cd.valid {
            st().chg_cd.path.clone()
        } else {
            "无".to_string()
        },
    );
    logv(&format!(
        "charge: 电流节点 {} ({}, kind={}) 暂停节点 {} cool_down {}",
        cp,
        co,
        chg_kind_name(st().chg_cur.kind),
        sp,
        cdp
    ));
}

fn gear_pct(gear: i32) -> i32 {
    match gear {
        g if g <= 0 => 100,
        1 => 75,
        2 => 50,
        _ => 0, // 3 = 暂停
    }
}

fn gear_for_temp(t: i64) -> i32 {
    let wall = st().c.charge_temp_wall as i64;
    if t >= wall + 4 {
        return 3;
    }
    if t >= wall + 2 && st().chg_gear < 2 {
        return 2;
    }
    if t >= wall && st().chg_gear < 1 {
        return 1;
    }
    if t <= wall - 5 {
        return 0;
    }
    st().chg_gear // 维持当前档
}

/// "36:100,38:88,40:78,42:66" —— 按温度升序，取最后一个 <= 当前温度的点。
/// 乱序/倒退的条目直接忽略（v1.2.6 的默认曲线末尾多了一个 ",43:0"，
/// 按"最后一个覆盖前面的"会把 43℃ 以上全部算成 0%），百分比钳在 20~100。
fn charge_pct_for_temp(temp: i64) -> i32 {
    let mut pct = 100;
    let curve = st().c.chg_curve.clone();
    let mut last_t = i64::MIN;
    for tok in curve.split(',') {
        let mut it = tok.split(':');
        let t = match it.next().and_then(|s| s.trim().parse::<i64>().ok()) {
            Some(v) => v,
            None => continue,
        };
        let p = match it.next().and_then(|s| s.trim().parse::<i32>().ok()) {
            Some(v) => v,
            None => continue,
        };
        if t < last_t || t > 90 {
            continue; // 乱序或明显越界的点
        }
        last_t = t;
        let p = p.clamp(20, 100);
        if temp >= t {
            pct = p; // 温度越高，后面覆盖前面的
        }
    }
    pct
}

/// charger v2 的电流基准（mA）：优先 usb 侧协议电流，其次本次充电见过的最大电池电流
pub fn chg_base_ma() -> i64 {
    if st().chg_base_ma > 0 {
        return st().chg_base_ma;
    }
    let mut base = 0;
    if let Some(v) = rdline(&format!(
        "{}/class/power_supply/usb/current_max",
        st().p.sys
    )) {
        let ua = parse_leading_num(&v);
        if ua > 0 {
            base = ua / 1000;
        }
    }
    if base <= 0 {
        base = st().chg_orig_cur;
        if base > 20000 {
            base /= 1000;
        }
    }
    if base <= 0 {
        base = 3000;
    }
    base
}

/// 写百分比上限。三种节点单位不同，下限也不同：
///  µA 节点最小 100mA；mA 节点最小 500mA；v2 的 current_now 写 0 等于"取消限流"，
///  所以 0% 也只能落到最低档，真正的停充交给 suspend 节点。
fn charge_set_pct(pct: i32) {
    if pct == st().chg_last_pct {
        return;
    }
    st().chg_last_pct = pct;
    if !st().chg_cur.valid {
        return;
    }
    let kind = st().chg_cur.kind;
    let pct = pct.clamp(0, 100);
    let v: i64 = match kind {
        2 => {
            // v2：写 0 = 取消投票，也就是"不限"。100% 必须走这条路，否则会把
            // 刚插上时的小电流当成上限，充电器再也升不上去。
            if pct >= 100 {
                0
            } else {
                let base = chg_base_ma();
                let v = base * pct as i64 / 100;
                if v < 500 {
                    500
                } else {
                    v
                }
            }
        }
        1 => {
            let base = if st().chg_orig_cur > 0 {
                st().chg_orig_cur
            } else {
                3000
            };
            let v = base * pct as i64 / 100;
            if v < 500 {
                500
            } else {
                v
            }
        }
        _ => {
            let base = if st().chg_orig_cur > 0 {
                st().chg_orig_cur
            } else {
                3_000_000
            };
            let v = base / 100 * pct as i64;
            if v < 100000 {
                100000 // 别设成 0，0 在很多机型上等于"不限"
            } else {
                v
            }
        }
    };
    let p = st().chg_cur.path.clone();
    if wrl(&p, v) && kind == 2 {
        st().chg_v2_written = v as i32;
    }
}

/// 交还控制权：v2 的 current_now 必须写 0（= 取消 HIDL 投票），
/// 其它节点写回动手前的原值。
fn charge_release() {
    if !st().chg_cur.valid {
        return;
    }
    let p = st().chg_cur.path.clone();
    if st().chg_cur.kind == 2 {
        if st().chg_v2_written != 0 {
            wrl(&p, 0);
            st().chg_v2_written = 0;
        }
        st().chg_last_pct = -1; // 释放后重新算，否则下次同百分比会被去重跳过
    } else {
        let o = st().chg_cur.orig.clone();
        wrp(&p, &o);
    }
}

fn charge_suspend(on: bool) {
    if st().chg_suspended == on {
        return;
    }
    st().chg_suspended = on;
    if !st().chg_suspend.valid {
        return;
    }
    let invert = st().chg_suspend.path.contains("charging_enabled");
    let v = if invert {
        if on {
            "0"
        } else {
            "1"
        }
    } else if on {
        "1"
    } else {
        "0"
    };
    let p = st().chg_suspend.path.clone();
    wrp(&p, v);
}

/// 每个 tick 调一次（1Hz）；不充电时什么都不做
pub fn charge_tick() {
    if st().c.charge_ctrl == 0 {
        if st().chg_active {
            charge_suspend(false);
            charge_release();
            st().chg_active = false;
            st().chg_last_pct = -1;
        }
        return;
    }
    charge_init();
    if !st().chg_cur.valid {
        return;
    }
    let charging = battery_status().contains("Charging");
    if !charging {
        if st().chg_active {
            charge_suspend(false);
            charge_release();
            st().chg_active = false;
            st().chg_last_pct = -1;
            st().chg_base_ma = 0;
            let o = st().chg_cur.orig.clone();
            logv(&format!("charge: 已停止充电，释放限流（原值 {}）", o));
        }
        return;
    }

    // ---- charger v2：battery/current_now 是「息屏智能充电」接口 ----
    // oplus_chg_gki.c 的 battery_psy_set_prop(CURRENT_NOW) 里：
    //   息屏（led_on=false）→ vote(HIDL_VOTER, 我们写的 mA) 生效
    //   亮屏（led_on=true） → 反向 unvote，写它等于取消限流
    // 所以这边只在息屏时接管；亮屏时主动释放，绝不写无意义的票。
    if st().chg_cur.kind == 2 {
        let raw = rdnum(&format!(
            "{}/class/power_supply/battery/current_now",
            st().p.sys
        ))
        .abs();
        let ma = current_ma(raw).abs();
        if ma > st().chg_base_ma {
            st().chg_base_ma = ma; // 本段充电的高水位，作为百分比基准
        }
        if screen_on() {
            if st().chg_v2_written != 0 {
                charge_release();
                logmsg("charge: 亮屏，已释放息屏限流（本机内核亮屏不接受 current_now 限流）");
            }
            return;
        }
    }

    let soc = battery_cap();
    let temp = battery_temp();
    if soc <= 0 {
        return;
    }
    st().chg_active = true;

    // 1) 温度阶梯：到墙才降档，不是一碰就停
    let g = gear_for_temp(temp);
    if g != st().chg_gear {
        let old = st().chg_gear;
        st().chg_gear = g;
        let wall = st().c.charge_temp_wall;
        if g == 3 {
            if st().chg_suspend.valid {
                logmsg(&format!("charge: {}℃ 超过安全线 {}℃，暂停充电", temp, wall + 4));
            } else {
                logmsg(&format!(
                    "charge: {}℃ 超过安全线 {}℃，电流压到最低档（本机没有暂停充电的节点）",
                    temp,
                    wall + 4
                ));
            }
        } else if g == 0 {
            logmsg(&format!(
                "charge: {}℃ 回落到 {}℃ 以下，恢复满档",
                temp,
                wall - 5
            ));
        } else {
            logmsg(&format!(
                "charge: {}℃ 到温度墙 {}℃，降档 {}% → {}%（约 {}W）",
                temp,
                wall,
                gear_pct(old),
                gear_pct(g),
                120 * gear_pct(g) / 100
            ));
        }
    }
    if st().chg_gear == 3 {
        charge_suspend(true);
        charge_set_pct(0);
        return;
    }
    if st().chg_suspended {
        logmsg(&format!("charge: 恢复充电（档位 {}%）", gear_pct(st().chg_gear)));
        charge_suspend(false);
    }

    // 2) 电量区间：拐点以下满功率（除非温度档位要求更低）
    let mut pct = 100;
    if soc >= st().c.charge_knee_soc as i64 {
        pct = charge_pct_for_temp(temp);
    }
    let cap = gear_pct(st().chg_gear);
    if cap < pct {
        pct = cap;
    }
    if pct != st().chg_last_pct {
        logmsg(&format!(
            "charge: SOC {}% {}℃ → 电流上限 {}%（拐点 {}%，温度档 {}%）",
            soc, temp, pct, st().c.charge_knee_soc, cap
        ));
    }
    charge_set_pct(pct);
}

pub fn charge_restore() {
    if st().chg_suspend.valid && st().chg_suspended {
        let invert = st().chg_suspend.path.contains("charging_enabled");
        let v = if invert { "1" } else { "0" };
        let p = st().chg_suspend.path.clone();
        wrp(&p, v);
        st().chg_suspended = false;
    }
    if st().chg_cur.valid {
        charge_release();
        if st().chg_cur.kind == 2 {
            logv("charge: 已释放息屏限流（写 0 = 取消 HIDL 投票）");
        } else {
            let o = st().chg_cur.orig.clone();
            logv(&format!("charge: 已还原电流 {}", o));
        }
    }
    st().chg_active = false;
    st().chg_last_pct = -1;
    st().chg_gear = 0;
    st().chg_base_ma = 0;
}

// ------------------------------------------------------- 充电加速 ----
/// 适配器/协议上限（0.1W）：usb 侧 `voltage_max`（协商到的最高电压）× `current_max`
/// （SPEC_VOTER 的电流）。两者相乘就是这条线协议能喂进来的天花板。
/// 一加 15 上 120W 是 11V×11A≈121W，65W PD/PPS 是 20V×3.25A≈65W —— 一眼能看出
/// 「为什么只有 67W」到底是模块没生效，还是适配器就到这了。
pub fn adapter_power_w10() -> i64 {
    let v = rdnum(&format!("{}/class/power_supply/usb/voltage_max", st().p.sys));
    let i = rdnum(&format!("{}/class/power_supply/usb/current_max", st().p.sys));
    if v <= 0 || i <= 0 {
        return 0;
    }
    (v / 1000) * (i / 1000) / 100000
}

/// usb 侧实测输入功率（0.1W）
pub fn input_power_w10() -> i64 {
    let v = rdnum(&format!("{}/class/power_supply/usb/voltage_now", st().p.sys));
    let i = rdnum(&format!("{}/class/power_supply/usb/current_now", st().p.sys));
    if v <= 0 || i == 0 {
        return 0;
    }
    (v / 1000) * current_ma(i).abs() / 100000
}

/// 按 DT 分桶表扫一遍「伪装温度」候选，实测每档的充入功率，找出最优值。
///
/// 这就是把逆向出来的原理用足：不猜哪个温度落哪一段，直接试。
/// 阻塞执行（每档默认 4s），只在用户显式调用 `charge sweep` 时跑。
pub fn accel_sweep(settle_ms: u64) -> String {
    accel_init();
    let node = if st().acc_shell.valid {
        st().acc_shell.clone()
    } else if st().acc_batt.valid {
        st().acc_batt.clone()
    } else {
        return "err=no_accel_node\n".to_string();
    };
    let stt = battery_status();
    if !stt.contains("Charging") {
        return format!("err=not_charging status={}\n", stt);
    }
    let settle: u64 = settle_ms.clamp(50, 15000);
    let mut out = String::new();
    out.push_str(&format!(
        "sweep: node={} orig={} settle={}ms\n",
        node.path, node.orig, settle
    ));
    let mut cands: Vec<i32> = Vec::new();
    let r = st().acc_dt.clone();
    if r.len() >= 3 {
        for j in 0..r.len() - 1 {
            let mid = (r[j] + r[j + 1]) / 2;
            let v = mid.clamp(150, 340);
            if !cands.contains(&v) {
                cands.push(v);
            }
        }
    }
    if cands.is_empty() {
        cands = vec![170, 220, 280, 340];
    }
    let mut best_v = 0;
    let mut best_w = -1i64;
    let mut worst_w = i64::MAX;
    for v in &cands {
        if !wrp(&node.path, &v.to_string()) {
            out.push_str(&format!("try={} write_failed\n", v));
            continue;
        }
        std::thread::sleep(std::time::Duration::from_millis(settle));
        let (mv, ma) = chg_read_power();
        let w = power_w10(mv, ma);
        if w > best_w {
            best_w = w;
            best_v = *v;
        }
        if w < worst_w {
            worst_w = w;
        }
        out.push_str(&format!(
            "try={} temp={}℃ ibat={:+}mA power={}.{}W\n",
            v,
            fmt_t10(*v as i64),
            current_ma(ma),
            w / 10,
            w % 10
        ));
    }
    let _ = wrp(&node.path, &node.orig);
    out.push_str(&format!(
        "best={} best_power={}.{}W worst_power={}.{}W\n",
        best_v,
        best_w / 10,
        best_w % 10,
        worst_w.max(0) / 10,
        worst_w.max(0) % 10
    ));
    out.push_str(&format!("restore={}\n", node.orig));
    let adapter = adapter_power_w10();
    if best_w <= 0 {
        out.push_str("verdict: 读不到充电电流，无法判断\n");
    } else if best_w - worst_w < best_w * 3 / 100 {
        out.push_str("verdict: 各温度档电流差异 <3%，温度不是当前瓶颈");
        if adapter > 0 {
            out.push_str(&format!(
                "（适配器/协议上限约 {}.{}W）\n",
                adapter / 10,
                adapter % 10
            ));
        } else {
            out.push_str("（多半是适配器/协议上限，或电流还没上来）\n");
        }
    } else {
        out.push_str(&format!(
            "verdict: 温度档对电流有影响，最优是 {}（{}℃），可用 set accel_tbat {} 固定\n",
            best_v,
            fmt_t10(best_v as i64),
            best_v
        ));
    }
    out
}

/// 0.1℃ → DT 段号（与内核 puc_get_temp_region 同构：落在 [range[i-1], range[i]) 就是 i-1）
pub fn region_of(t10: i64) -> i32 {
    let r = st().acc_dt.clone();
    if r.len() < 2 {
        return -1;
    }
    for (i, v) in r.iter().enumerate() {
        if t10 < *v as i64 {
            return i as i32 - 1;
        }
    }
    -1
}

pub fn region_name(i: i32) -> &'static str {
    match i {
        0 => "little_cold",
        1 => "cool",
        2 => "little_cool",
        3 => "normal_low",
        4 => "normal_high",
        5 => "warm",
        6 => "warm+",
        _ => "表外",
    }
}

pub fn fmt_t10(v: i64) -> String {
    format!("{}.{}", v / 10, (v % 10).abs())
}

/// 读 thermal_zone 里名字含 sub 的那一路（shell / batt），返回 (名字, 0.1℃)
pub fn thermal_by(sub: &str) -> Option<(String, i64)> {
    let dir = format!("{}/class/thermal", st().p.sys);
    let rd = fs::read_dir(&dir).ok()?;
    let mut cands: Vec<String> = Vec::new();
    for e in rd.flatten() {
        let p = e.path();
        let n = p
            .file_name()
            .map(|x| x.to_string_lossy().to_string())
            .unwrap_or_default();
        if !n.starts_with("thermal_zone") {
            continue;
        }
        let ty = rdline(&format!("{}/type", p.display())).unwrap_or_default();
        if ty.to_lowercase().contains(sub) {
            cands.push(p.display().to_string());
        }
    }
    cands.sort();
    for p in cands {
        let t = rdnum(&format!("{}/temp", p));
        if t > 0 {
            let ty = rdline(&format!("{}/type", p)).unwrap_or_default();
            return Some((ty, t / 100));
        }
    }
    None
}

/// UFCS/PPS 的电池温度降流起点（DT `oplus,ufcs_strategy_batt_high_temp` 的第一个值，0.1℃）。
/// 一加 15 是 `<400 420 430 390 410 420>` → 40.0℃ 起降流。
pub fn batt_derate_temp() -> i64 {
    let dt = format!("{}/firmware/devicetree/base", st().p.sys);
    if let Some(b) = dt_prop(&dt, 0, "oplus,ufcs_strategy_batt_high_temp") {
        let v = dt_u32_be(&b) as i64;
        if v > 0 {
            return v;
        }
    }
    400
}

fn accel_open_node(slot: &mut Node, module: &str, param: &str) {
    if slot.valid {
        return;
    }
    let p = format!("{}/module/{}/parameters/{}", st().p.sys, module, param);
    if !writable(&p) {
        return;
    }
    if let Some(v) = rdline(&p) {
        slot.path = p;
        slot.orig = v;
        slot.valid = true;
    }
}

/* UFCS/PPS 电流来自「温度分桶曲线」(oplus_strategy_pps_ufcs_curve.c)：
 *   puc_get_temp_region(): 取第一个 temp < range[i] 的 i，段号 = i-1
 *   6 段依次 little_cold / cool / little_cool / normal_low / normal_high / warm，
 *   最高电流在 normal_high = [range[n-3], range[n-2])。
 * 直接读 DT 的分桶表，把伪装值定在 normal_high 段里。 */
fn accel_dt_scan(dir: &str, depth: i32) -> i32 {
    if depth > 4 {
        return 0;
    }
    let path = format!("{}/oplus,temp_range", dir);
    if let Ok(md) = fs::metadata(&path) {
        if md.is_file() && md.len() >= 6 * 4 {
            if let Ok(buf) = fs::read(&path) {
                let cnt = (buf.len() / 4).min(16);
                if cnt >= 6 {
                    let mut b: Vec<i32> = Vec::new();
                    for i in 0..cnt {
                        let v = ((buf[i * 4] as i32) << 24)
                            | ((buf[i * 4 + 1] as i32) << 16)
                            | ((buf[i * 4 + 2] as i32) << 8)
                            | (buf[i * 4 + 3] as i32);
                        b.push(v);
                    }
                    let mut v = b[cnt - 3] + 10; // normal_high 段起点 +1℃
                    if v >= b[cnt - 2] - 5 {
                        v = b[cnt - 2] - 10;
                    }
                    if v < 150 {
                        v = 150;
                    }
                    st().acc_dt = b.clone();
                    logv(&format!(
                        "accel: DT {} cnt={} [{}..{}] → 目标 {}",
                        path,
                        cnt,
                        b[0],
                        b[cnt - 1],
                        v
                    ));
                    return v;
                }
            }
        }
    }
    let rd = match fs::read_dir(dir) {
        Ok(d) => d,
        Err(_) => return 0,
    };
    let mut subs: Vec<String> = Vec::new();
    for e in rd.flatten() {
        let n = e.file_name().to_string_lossy().to_string();
        if n.starts_with('.') {
            continue;
        }
        subs.push(format!("{}/{}", dir, n));
    }
    for sub in subs {
        if is_dir(&sub) {
            let found = accel_dt_scan(&sub, depth + 1);
            if found != 0 {
                return found;
            }
        }
    }
    0
}

pub fn accel_init() {
    if st().acc_ready {
        return;
    }
    st().acc_ready = true;
    st().acc_shell = Node::default();
    st().acc_batt = Node::default();
    st().acc_sub = Node::default();

    for m in ["oplus_chg_v2", "oplus_chg"] {
        accel_open_node(&mut st().acc_shell, m, "oplus_dbg_tbat");
        accel_open_node(&mut st().acc_batt, m, "gauge_dbg_tbat");
        accel_open_node(&mut st().acc_sub, m, "sub_gauge_dbg_tbat");
    }
    // 模块名可能带后缀（oplus_chg_v2_xxx），扫 /sys/module 兜底
    let mdir = format!("{}/module", st().p.sys);
    if let Ok(rd) = fs::read_dir(&mdir) {
        let names: Vec<String> = rd
            .flatten()
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.contains("oplus_chg"))
            .collect();
        for n in names {
            accel_open_node(&mut st().acc_shell, &n, "oplus_dbg_tbat");
            accel_open_node(&mut st().acc_batt, &n, "gauge_dbg_tbat");
            accel_open_node(&mut st().acc_sub, &n, "sub_gauge_dbg_tbat");
        }
    }

    let dt = format!("{}/firmware/devicetree/base", st().p.sys);
    let mut v = accel_dt_scan(&dt, 0);
    if v == 0 {
        let dt2 = format!("{}/device-tree", st().p.proc_);
        v = accel_dt_scan(&dt2, 0);
    }
    st().acc_dt_val = v;

    if st().acc_shell.valid || st().acc_batt.valid || st().acc_sub.valid {
        logv(&format!(
            "accel: shell={}({}) batt={}({}) sub={}({}) dt={}",
            yn(st().acc_shell.valid),
            orig_or_dash(&st().acc_shell),
            yn(st().acc_batt.valid),
            orig_or_dash(&st().acc_batt),
            yn(st().acc_sub.valid),
            orig_or_dash(&st().acc_sub),
            st().acc_dt_val
        ));
    } else {
        logv("accel: 没有 oplus_chg 的温度参数节点，充电加速停用（非 OPPO 机型正常）");
    }
}

fn yn(b: bool) -> &'static str {
    if b {
        "Y"
    } else {
        "-"
    }
}

fn orig_or_dash(n: &Node) -> String {
    if n.valid {
        n.orig.clone()
    } else {
        "-".to_string()
    }
}

pub fn accel_target() -> i32 {
    let mut v = if st().c.accel_tbat > 0 {
        st().c.accel_tbat
    } else if st().acc_dt_val > 0 {
        st().acc_dt_val
    } else {
        250
    };
    if st().c.accel_gentle != 0 {
        v += 20; // 温和档：整体抬 2℃，尽量仍留在同一段
    }
    if v < 150 {
        v = 150; // 再低就掉进 cool 段，电流照样被限
    }
    if v > 340 {
        v = 340; // warm 段是 250~350，>350 直接掉出表 → 一律钳住
    }
    v
}

fn accel_put(n: &Node, v: i32) -> i32 {
    if !n.valid {
        return 0;
    }
    let b = format!("{}", v);
    if !wrp(&n.path, &b) {
        return -1;
    }
    if let Some(rb) = rdline(&n.path) {
        if parse_leading_num(&rb) != v as i64 {
            return -1; // 回读校验
        }
    }
    1
}

/// 写回原值并清状态（why 只用于日志，重复调用无副作用）
pub fn accel_off(why: &str) {
    if !st().acc_spoofing {
        return;
    }
    let (sh, ba, su) = (
        st().acc_shell.clone(),
        st().acc_batt.clone(),
        st().acc_sub.clone(),
    );
    if sh.valid {
        wrp(&sh.path, &sh.orig);
    }
    if ba.valid {
        wrp(&ba.path, &ba.orig);
    }
    if su.valid {
        wrp(&su.path, &su.orig);
    }
    st().acc_spoofing = false;
    st().acc_written = 0;
    logmsg(&format!("accel: 退出温度伪装（{}），已还原原值", why));
}

pub fn accel_restore() {
    if st().acc_spoofing {
        accel_off("退出");
    }
    st().acc_blocked = false;
}

/// 每拍调一次；temp = 真实电池温度（℃），charging = status 含 Charging
pub fn accel_tick(temp: i64, charging: bool) {
    if st().c.enable == 0 || st().c.accel_ctrl == 0 {
        accel_off("已关闭");
        return;
    }
    accel_init();
    let use_gauge = !st().acc_shell.valid && st().c.accel_gauge != 0 && st().acc_batt.valid;
    if !st().acc_shell.valid && !use_gauge {
        return;
    }
    if !charging {
        accel_off("未充电");
        return;
    }
    // 真实温度到线：停伪装，让温度阶梯（读真实温度）自己去降档
    if temp >= st().c.accel_stop_temp as i64 {
        if !st().acc_blocked {
            st().acc_blocked = true;
            logmsg(&format!(
                "accel: 电池 {}℃ 到线 {}℃，停止伪装温度，交温度阶梯降档",
                temp,
                st().c.accel_stop_temp
            ));
        }
        accel_off("过热");
        return;
    }
    if st().acc_blocked {
        if temp > st().c.accel_resume_temp as i64 {
            return; // 没凉下来就保持退出，别来回抖
        }
        st().acc_blocked = false;
        logmsg(&format!(
            "accel: 电池回落到 {}℃（≤{}℃），恢复温度伪装",
            temp,
            st().c.accel_resume_temp
        ));
    }

    let v = accel_target();
    if st().acc_spoofing && st().acc_written == v {
        return;
    }
    let mut ok = false;
    if st().acc_shell.valid {
        let n = st().acc_shell.clone();
        ok = accel_put(&n, v) > 0;
    } else if use_gauge {
        let n = st().acc_batt.clone();
        ok = accel_put(&n, v) > 0;
        let sub = st().acc_sub.clone();
        if sub.valid {
            accel_put(&sub, v);
        }
    }
    if ok {
        st().acc_spoofing = true;
        st().acc_written = v;
        // 伪装值到底有没有把曲线换到更高电流段？直接比两边的温度段号。
        // 常温下一加 15 的 normal_high 覆盖 21~37.9℃，写 22℃ 与真实同段 → 无增益。
        st().accel_eff = if st().acc_dt.len() >= 3 {
            let real = thermal_by("shell")
                .map(|(_, t)| t)
                .or_else(|| thermal_by("batt").map(|(_, t)| t));
            match real {
                Some(t) if region_of(t) >= 0 => {
                    if region_of(t) == region_of(v as i64) {
                        2
                    } else {
                        1
                    }
                }
                _ => 0,
            }
        } else {
            0
        };
        let which = if st().acc_shell.valid { "机身" } else { "电池" };
        logmsg(&format!(
            "accel: 伪装{}温度 {:.1}℃（真实电池 {}℃），曲线切到 normal_high 段",
            which,
            v as f64 / 10.0,
            temp
        ));
    }
}

// ------------------------------------------------------- 功率采样（诊断） ----
pub fn chg_power_tick(tick: i64) {
    let charging = battery_status().contains("Charging");
    let (mv, ma) = chg_read_power();

    if !charging {
        if st().chg_peak_w10 > 0 {
            let (p, c, t) = (st().chg_peak_w10, battery_cap(), battery_temp());
            logmsg(&format!(
                "charge: 本次充电峰值 {}.{}W（{}% {}℃，亮屏峰值 {}.{}W / 息屏峰值 {}.{}W）",
                p / 10,
                p % 10,
                c,
                t,
                st().chg_peak_on_w10 / 10,
                st().chg_peak_on_w10 % 10,
                st().chg_peak_off_w10 / 10,
                st().chg_peak_off_w10 % 10
            ));
            st().chg_peak_w10 = 0;
            st().chg_peak_on_w10 = 0;
            st().chg_peak_off_w10 = 0;
        }
        return;
    }
    if mv <= 0 {
        return;
    }
    let watt10 = power_w10(mv, ma);
    if ma < 0 {
        if watt10 > st().chg_peak_w10 {
            st().chg_peak_w10 = watt10;
        }
        // 亮屏/息屏分开记：厂商的「亮屏限流」只有把两个峰值摆在一起才看得出来
        if screen_on() {
            if watt10 > st().chg_peak_on_w10 {
                st().chg_peak_on_w10 = watt10;
            }
        } else if watt10 > st().chg_peak_off_w10 {
            st().chg_peak_off_w10 = watt10;
        }
    }
    if tick - st().chg_last_log >= 30 {
        st().chg_last_log = tick;
        let (p, c, t) = (st().chg_peak_w10, battery_cap(), battery_temp());
        let fake = if st().acc_spoofing { "开" } else { "关" };
        logmsg(&format!(
            "charge: {}% {}℃ {}mV {:+}mA → {}.{}W（峰值 {}.{}W，伪装 {}）",
            c,
            t,
            mv / 1000,
            ma,
            watt10 / 10,
            watt10 % 10,
            p / 10,
            p % 10,
            fake
        ));
    }
}
