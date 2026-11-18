# 《SM8850 与一加 15 调度优化 Agent 提示词》逐条核对

> 核对对象：`D:\QQ\下载\SM8850 与一加 15 调度优化 Agent 提示词.md`
> 核对依据：**`OnePlusOSS/android_kernel_oneplus_sm8850` @ `oneplus/sm8850_b_16.0.0_oneplus_15`
> 的内核源码**（`kernel/sched/walt/**`，已下载到 `research/walt/`）+ 本项目在 PLK110 上的真机实测。
>
> 结论：**提示词关于 WALT 的判断是对的** —— 这份内核确实带完整 WALT
> （`kernel/sched/walt/` 40 个文件，含 `cpufreq_walt.c` 这个名为 `walt` 的 governor），
> `/proc/sys/walt/*` 也在。此前本项目只盯着 sched_ext/hmbird，漏了 WALT 这一层（v1.4.2 已补上
> 触控加速的关闭与探测）。

## 1. 本机其实是两层调度栈

```
基础：CFS + WALT
        governor = "walt"            (kernel/sched/walt/cpufreq_walt.c)
        调参   = /proc/sys/walt/*    (kernel/sched/walt/sysctl.c)
        负载跟踪 = walt.c / walt_cfs.c / sched_avg.c
   │
   ├─ 叠加：sched_ext / hmbird_II（风驰）
   │        /proc/sys/hmbird_II/*   (vendor/oplus/kernel/cpu/sched_ext/hmbird_II/)
   │        scx ops 挂上后接管选核/选频，WALT 的 CFS 路径被绕过
   │
   └─ 叠加：OPLUS frame_boost
            /proc/oplus_frame_boost/* (vendor/oplus/kernel/cpu/uad/ua_ioctl/frame_ioctl.c)
```

所以「提示词说 WALT」和「本项目说 hmbird」**不矛盾**：平时是 WALT，游戏时风驰（sched_ext）
接管；两者各有各的调参入口，不能只认一边。

## 2. 逐条核对

| # | 提示词的说法 | 内核源码 / 真机证据 | 判定 | 模块现状 |
|---|---|---|---|---|
| 1 | 2+6 全大核；`policy0=cpu0-5`、`policy6=cpu6-7` | `doctor`：`perf_policy=policy0:3628800`、`prime_policy=policy6:4608000` | ✅ 一致 | 掩码 `0x3F`/`0xC0`，按真实拓扑探测 |
| 2 | policy0 3.63GHz、policy6 4.61GHz | 同上 | ✅ 一致 | 百分比按 `cpuinfo_max_freq` 算 |
| 3 | `cpu_capacity = 762 / 1024` | 离线无法确认真机数值（WALT 里 `walt_cpu_capacity` 走 DT/驱动） | ⚠️ 待真机 | `doctor` 新增 `cpu_capacity:` 行，直接读 `cpu0/6/cpu_capacity` |
| 4 | Prime 97%~100% 时间停 883MHz 地板 | 本项目实测 prime **保底 1017MHz**（README 三、600s 游戏表）；WALT 有 `hispeed_freq`/`hispeed_load(90)` 机制，具体驻留要看 `time_in_state` | ⚠️ 待复核 | 不假设 prime 常驻低频；命令见文末 |
| 5 | **绝不**把游戏主线程/RenderThread 钉死 cpu6-7 | 本项目 A/B：`UnityMain→CPU7` 是收益项；但**默认 `official` 完全不绑核**，只有 `aggressive` 才绑 | ⚠️ 结论相反、默认行为不冲突 | 默认 official；绑核只在 aggressive，README 标注“仅 A/B” |
| 6 | 优先 uclamp + 全核亲和，不要绑核 | `hmbird_task_change_cpumask()` 让多核 `taskset` 失效，只认单核掩码 | ✅ 方向一致 | 日常用 cgroup uclamp；绑核走 `prefer_cpu` 单核 |
| 7 | 抬高 `sched_upmigrate/downmigrate` 把任务压在 cpu0-5 | **源码确认存在**：`sysctl.c` 有 `sched_upmigrate`、`sched_downmigrate`，另有按 cgroup 的 `sched_{topapp,foreground,background,other_cgroup}_updownmigrate` 数组 | ✅ 存在 | 模块**尚未**使用（见 §3 计划） |
| 8 | 不要 rmmod `oplus_bsp_sched_assist` / `frame_boost` | 源码是内建模块；模块从不 rmmod | ✅ 一致 | 只写 sysctl/cpufreq |
| 9 | 真实的 frame_boost 接口 | `frame_ioctl.c`：`proc_mkdir("oplus_frame_boost")`，节点 `info/sys_ctrl/cfbt_*` | ✅ 存在 | v1.4.2 `doctor` 新增 `frame_boost=yes <info 首行>`；参数语义未验证，暂不写 |
| 10 | thermal daemon 周期改写 `scaling_max_freq`，要持久化 | 本项目实测到漂移，`cpu.rs` 每 10 拍（1s）比对后重写 | ✅ 一致 | 不用 `chmod 0444`（会挡住自己），用“比对后重写” |
| 11 | WALT 调参在 `policy*/walt/up_rate_limit_us` | `cpufreq_walt.c` 注册的 governor 名就是 `walt`，tunable 在 `policy*/walt/` | ✅ 正确 | 模块现在**不写** `schedutil/*` 或 `walt/*`（测试树里旧的 `schedutil/` 已改成 `walt/`） |
| 12 | 不要关 `input_boost`（WALT 的触控加速） | `sysctl.c`：`/proc/sys/walt/input_boost_ms`（u32）、`sched_boost_on_input`、`sched_boost`；`input_boost_freq` 是 8×u32 数组 | ✅ 存在 | **v1.4.2 修复**：`kill_boost` 现在会关这三个节点并保存/还原原值（原来只写通用 `cpu_boost/*`，真机上多数不存在） |
| 13 | GPU 1.2GHz / 18MB HPM / LPDDR5X 5300 / SLC 8MB | 与调度模块无关 | — | 只 `force_clk_on=0`、`bus_split=1` |
| 14 | `sched_setattr(SCHED_FLAG_UTIL_CLAMP_*)` | 需要逐线程 syscall | — | 用 hmbird `set_uclamp` + cgroup uclamp 覆盖同等效果 |

## 3. 本轮实际改动（v1.4.2）

1. **`kill_boost` 补上 WALT**：`/proc/sys/walt/{input_boost_ms,sched_boost_on_input,sched_boost}`
   → 写 0；原值存进 `boost_save`，退出/卸载时**逐节点还原**（以前是统一写死 1/0）。
   `input_boost_freq` 是数组，写 "0" 语义不明，故意不碰。
2. **可观测性**（`doctor` / `probe` 新增 4 行）：

```
walt=yes upmigrate=762 downmigrate=500
governor: policy0=walt policy6=walt
walt_boost: input_boost_ms=0 sched_boost_on_input=0 sched_boost=0
scx_ops=hmbird_II hmbird_live=1
frame_boost=yes <info 首行>
cpu_capacity: policy0=762 policy6=1024
```

3. 测试树从 `schedutil/` 改为 `walt/`，并新增 `/proc/sys/walt/*` 与 4 条断言
   （关触控加速、doctor 报 WALT、退出还原）。

## 4. 还没做、需要真机数据才能做的

| 项目 | 为什么先不做 | 验证方式 |
|---|---|---|
| `sched_upmigrate/downmigrate` 打包调参（压在 cpu0-5 省电） | 会直接改变调度器行为，没有 A/B 数据不敢默认开 | 先看 `time_in_state` 里 policy6 的驻留，再用 `sched_upmigrate` 700→900 各跑 10 分钟对比功耗/帧率 |
| `sched_pipeline*` / `sched_low_latency` / `sched_smart_freq*` | 语义只有源码，缺实测 | 逐个 A/B |
| `frame_boost` 的 `cfbt_*` 参数 | 与 165Hz 帧同步耦合，写错会掉帧 | 先只读 `info`/`sys_ctrl` |
| `core_ctl`（`kernel/sched/walt/core_ctl.c`） | 限制在线核数对续航影响大、且与 hmbird 的 CPU 掩码可能冲突 | 先看 `core_ctl` 的 `max_cpus`/`min_cpus` 现值 |

## 5. 真机回传（决定下一步）

```sh
# 1) 两层调度栈各是什么状态
su -c 'cat /sys/kernel/sched_ext/root/ops 2>/dev/null; ls /proc/sys/walt | head -20'
su -c 'for p in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do echo -n "$p="; cat $p; done'

# 2) prime 簇真实频率驻留（核对提示词的 883MHz 说法）
su -c 'cat /sys/devices/system/cpu/cpufreq/policy6/stats/time_in_state'

# 3) WALT 触控加速与 capacity
su -c 'cat /proc/sys/walt/input_boost_ms /proc/sys/walt/sched_upmigrate /proc/sys/walt/sched_downmigrate'
su -c 'cat /sys/devices/system/cpu/cpu0/cpu_capacity /sys/devices/system/cpu/cpu6/cpu_capacity'

# 4) frame_boost / core_ctl
su -c 'cat /proc/oplus_frame_boost/info 2>/dev/null; ls /sys/devices/system/cpu/core_ctl* 2>/dev/null'
```

有了 2) 和 4) 才能决定要不要动 `sched_upmigrate` 与 `core_ctl`；1) 决定 hmbird 是否真的挂上。
