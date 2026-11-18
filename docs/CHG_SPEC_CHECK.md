# 《一加 15 满血快充 Rust 守护进程提示词》逐条核对

> 核对对象：`D:\QQ\下载\一加 15 (SM8850) 满血快充 Rust 守护进程与 Agent 开发提示词.md`
> 核对依据：`vendor/oplus/kernel/charger/v2/**` 源码（本仓库 `research/charger/v2/`）、
> 一加 15 充电 DT（`oplus-chg-24831.dtsi`）、以及本项目在 PLK110 上的真机回传。
>
> **结论：这份蓝图的 sysfs 路径全部属于旧的 v1 充电栈，这台机器上是 v2，`/sys/class/oplus_chg/`
> 整个不存在。** 按它写代码会得到一串 ENOENT；能实现同样效果的入口只有模块参数和
> `power_supply/{battery,usb}`，也就是本模块已经在用的那几个。

## 1. 设备是哪套充电栈

DT 里 `/soc/oplus_chg_core` 明确写着 `oplus,chg_framework_v2;`，驱动是
`vendor/oplus/kernel/charger/v2/oplus_chg_gki.c`（注册 `battery` / `usb` / `wireless`
三个 power_supply）+ `oplus_mms`（`class_create("oplus_mms")`）。
`oplus_chg_core.c` 是 votable 框架，**不创建任何 sysfs 节点**。

所以 `/sys/class/oplus_chg/...` 在这台机器上不存在（v1 的 `oplus_charger.c` 才有）。

## 2. 逐节点对照

| 蓝图节点 | 本机实际 | 说明 |
|---|---|---|
| `/sys/class/oplus_chg/battery/cool_down` | **不存在** | v2 的 `cool_down` 只是 `cool_down_votable` + `COMM_ITEM_COOL_DOWN`，没有 class 属性 |
| `.../battery/normal_cool_down` | **不存在** | 同上（`oplus_smart_chg` 的 `normal_cool_down` 字段，无 sysfs） |
| `.../battery/shell_temp` | **不可写** | shell_temp 由 `thermal_zone` 读入后 publish；用户态唯一的覆盖是 module_param `oplus_dbg_tbat` |
| `/sys/class/oplus_chg/common/led_on`、`.../battery/led_on` | **不存在** | `led_on` 是 `COMM_ITEM_LED_ON`，由 panel notifier（屏幕亮灭）维护，用户态改不了 |
| `.../battery/bcc_current` | **不存在** | v2 只有 `BCC_VOTER` 投票，没有 sysfs；`oplus_smart_chg` 里 BCC 是内部 buffer |
| `.../battery/batt_temp` / `batt_soc` | 对应 `power_supply/battery/{temp,capacity}` | 单位 0.1℃ / % |
| `.../battery/wired_online` | 对应 `power_supply/usb/online` | |
| `.../wireless/online` | 对应 `power_supply/wireless/online` | |
| （未提）最终电流 | `power_supply/battery/current_now`（**唯一可写**，仅息屏） | v1.4.1 已实现，见 `docs/FIXES_1.4.1.md` §3 |

## 3. 为什么 MS_BIND 那套在这里用不上

蓝图的核心手法是「写一次真实节点 → bind-mount 一个 0444 的假文件盖住它，让 `horae` 写不进去」。
在本机：

1. 要盖的节点（cool_down / shell_temp / led_on / bcc_current）**根本不存在**，没东西可盖；
2. 唯一有效的 `oplus_dbg_tbat` 是 **module_param**，只有本模块和用户会写，`horae` 不碰它，
   不存在「被抢回去」的竞争；
3. 给 module_param 做 bind mount 只会在自己升级/调试时造成「写了没反应」，收益为负。

真正会被 `horae`/thermal 改写的是 `scaling_max_freq`（CPU 频率），那个本模块用
「每秒比对 + 重写」处理（`cpu.rs` 的 drift 校正），比 bind mount 温和，也不会挡住自己。

## 4. 本机真正的「解锁」手段与边界

| 手段 | 能做什么 | 不能做什么 |
|---|---|---|
| `oplus_dbg_tbat`（shell temp） | 把 PPS/UFCS 的温度分桶拉回 `normal_high` | 常温本来就在这一段，没有增益；不改变 SVOOC 的电池温度投票 |
| `gauge_dbg_tbat`（电池 temp） | 会把 `BATT_TEMP_VOTER` 一起骗，热天能维持高电流 | **过温保护读到的也是假值**，默认关闭（`accel_gauge=0`） |
| `battery/current_now` | 息屏时按温度/SOC 限流 | 只能降不能升；亮屏写入等于取消限流 |
| `oplus_mms` 的 `overwrite` | 理论上能改 `COMM_ITEM_COOL_DOWN` | 需要 `CONFIG_OPLUS_DEBUG_AUTH` 证书，生产内核没开 |

因此「满血 120W」在工程上只能靠「不发烫」：把机身温度压到 `normal_high`（21~37.9℃）以内，
厂商自己就会给满电流。蓝图的 43.5℃ 熔断点与模块现有的 45℃ 停伪装 / 50℃ 降档 / 54℃ 暂停
是同一套思路，模块更保守一点。

## 5. 采纳了什么

**只采纳一条：守护进程自钉性能簇。**

- 蓝图与上一份调度提示词都要求「后台守护进程不许唤醒超大核」；
- 实现：`cpu.rs: pin_self_to_perf()`，掩码从 `policy0/affected_cpus` 解析（读不到退回 `0x3F`），
  在 `detect_topology()` 之后调用一次；子进程（dumpsys/pm）继承亲和性，同样不惊动 cpu6-7；
- 配置：`daemon_pin=1`（默认开，排查调度问题可关）；
- `doctor` 新增一行：`daemon_pin=1 perf_mask=0x3f`。

蓝图里「不要每秒写」这条本模块本来就合规：只有档位/百分比变化时才写节点，不是轮询写入。

## 6. 如果要更激进（风险自负）

```sh
# 让电池温度也参与伪装（会骗过温保护，45℃ 闸失效）
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh set accel_gauge 1'
# 手动指定伪装温度（150~340 = 15.0~34.0℃）
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh set accel_tbat 200'
```

用 `ctl.sh charge why` 看真实/伪装温度各落在哪个分桶段；用
`/sys/class/power_supply/battery/current_now` 与充电头标称值核对是否真的跑满。

## 7. 真机自检（本机路径）

```sh
su -c 'cat /sys/class/power_supply/battery/{temp,capacity,current_now,voltage_now}'
su -c 'cat /sys/module/oplus_chg_v2/parameters/oplus_dbg_tbat /sys/module/oplus_chg_v2/parameters/gauge_dbg_tbat'
su -c 'ls /sys/class/oplus_mms 2>/dev/null; ls /sys/class/oplus_chg 2>/dev/null || echo "no oplus_chg class (v2)"'
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh charge why'
```
