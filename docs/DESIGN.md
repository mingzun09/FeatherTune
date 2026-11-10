# 设计文档：SM8850 调度分析 → 模块决策

本文记录“为什么这么做”。所有代码引用都来自一加开源的内核与 vendor 模块：

- `OnePlusOSS/android_kernel_oneplus_sm8850` @ `oneplus/sm8850_b_16.0.0_oneplus_15`
- `OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8850` @ 同分支
  - `vendor/oplus/kernel/cpu/sched_ext/hmbird_II/*`（风驰 sched_ext 实现）
  - `vendor/oplus/kernel/cpu/sched_ext/hmbird_common_sysctl.c`、`hmbird_kfunc.c`
  - `vendor/oplus/kernel/cpu/game_opt/cpufreq_scx_main.h`、`sched/sched_assist/sa_hmbird.c`
  - `kernel_platform/qcom/opensource/devicetree/qcom/canoe*.dtsi`

---

## 1. SM8850（Canoe）的形态

从 `canoe.dtsi` 里能直接读出的硬事实：

| 项目 | 结论 |
|---|---|
| 簇拓扑 | `cpu-map cluster0 = CPU0..CPU5`，`cluster1 = CPU6,7`（**2 + 6 全大核 Oryon Gen3，没有小核**） |
| 频率 | prime 最高 4.6 GHz（4608000 kHz），perf 最高 3.62 GHz（3628800 kHz） |
| 调频接口 | 走 SCMI perf domain（`clocks = <&pdp_scmi_perf 1>`），DTS 里**没有 OPP 表**，频率表由固件给 |
| WALT | `canoe-walt.dtsi` 只留了 `qcom,gclk` 窗口计数，调度统计走 WALT（非 PELT） |
| 开机参数 | `cpufreq.default_governor=performance`、`sched_pelt_multiplier=4`、`irqaffinity=0-1`、`rcu_nocbs=0-7` |

**由此得到的两个设计约束：**

1. **频率必须运行时读**，不能写死。`scaling_available_frequencies` 会随固件/机型版本变，所以模块里所有上限都用
   “读 cpuinfo_max_freq → 算百分比 → 在可用表里取不超过目标的最高档”，而不是硬编码 2.6 GHz 这类数字。
2. **没有小核 = 后台任务没地方塞**。这是 SM8850 与 8 Gen3 及更早机型的最大区别：
   以前“后台扔小核”那套 cpuset 策略在这里等价于“后台抢大核”，所以本模块**不靠 cpuset 挪核**，
   而是用 **uclamp 限上限 + 频率上限**来控制后台的实际功耗。这也是为什么删掉了 MW_CTS 那类
   “background=0-2”的写法——在 2+6 且 0-5 同簇的拓扑上，0-2 和 0-5 是同一个频点域，写它没有意义。

---

## 2. “风驰”到底是什么：hmbird 的两套实现

社区说的 “scx / 风驰游戏内核”，源码里其实有两个不同实现（`Lunation-Studio/lunarkernel_sched_extention` 的 README 也印证了这点）：

| | hmbird | hmbird_gki |
|---|---|---|
| 调度本体 | 改过的 sched_ext 调度类 | 在高通 sched-walt 模块里插 `SCX_CALL_OP` 钩子 |
| sysctl | `/proc/hmbird_sched` | `/proc/sys/oplus_sched_ext`（本机型落在 `/proc/sys/hmbird_II`） |
| 适用 | 6.1 / MTK | 6.6+ 高通（**SM8850 属于这一支**） |
| 侵入性 | 大（改了 task_struct） | 小（靠 `oplus_task_struct` 挂在 `android_oem_data1`） |

在本机型上实际是 `hmbird_II`（`sched_ext/hmbird_II/`）：
`hmbird_II.c`（ops/状态机）、`hmbird_II_freqgov.c`（**cpufreq governor，名字就叫 `hmbird`**）、
`hmbird_II_sysctl.c`、`hmbird_II_shadow_tick.c`（影子 tick）、`hmbird_II_critical_task_monitor.c`。

所以“游戏场景默认调用 scx/hmbird 调速器”= 游戏启动后 Oplus 把 `scaling_governor` 切成 `hmbird`，
由它按帧预算算每个簇的目标 perf，再换算成频率。

### 2.1 它对外暴露的可调项（我们唯一能合法插手的地方）

`/proc/sys/hmbird_II/`（`hmbird_II_sysctl.c`）：

| 节点 | 默认 | 范围/格式 | 含义 |
|---|---|---|---|
| `coefficient` | 1024,1024 | 8..4096，逗号分簇 | perf 值→频率的换算系数 |
| `perf_high_ratio` | 60,60 | 百分比 | 进入 high-perf 档的利用率门限 |
| `freq_policy` | 0,0 | 逗号分簇 | 调频策略选择 |
| `cpus_reserved` | 0xf0 | 位图 | 留给关键任务的核（默认 CPU4-7） |
| `cpus_exclusive` | 0x0 | 位图 | 独占核（默认不启用） |
| `frame_per_sec` | — | int | 帧率窗口 |
| `heavy_boost` | "0 400 140" | type bottom_perf weight | 重载任务的频率托底与加权 |
| `prefer_idle / prefer_preempt / prefer_cluster / prefer_cpu / set_uclamp / uclamp_keep_freq` | — | `"<pid> <值>"`（cpu 掩码按十六进制） | 按线程打调度属性 |
| `critical_task` | — | `pid type name mask prop`，`#` 分隔 | 批量登记关键线程 |

调速器自身（`hmbird_II_freqgov.c`，governor attrs 挂在 `policyX/hmbird/`）：
`target_loads`（默认 90）、`mul_tl`（默认 `2;70:90:10`）、`soft_freq_max/min`、`soft_freq_cur`、`apply_freq_immediately`。

> **必须说清楚的一点**：`coefficient` 与 `perf_high_ratio` 在 OSS 树里没有消费方——
> 它们只通过 kfunc 暴露给 BPF 程序（`hmbird_kfunc.c` 的 `hb_bpf_cfg_coefficient_get()`），
> 真正的“perf → 频率”换算在预编译的 sched_ext BPF 镜像里，开源代码看不到。
> 因此本模块给的 `1152,1024 / 56,60 / 0xC0 / 0,360,128` 是**基于语义的保守假设值**，
> 不是从公式推导出的最优点；它们全部可配（`hmbird_*` 四项），也可以 `hmbird_tune=0` 整个关掉。
> 想验证就按下面的 A/B 做，不要相信任何“作者说省电 XX%”的数字。

### 2.2 A/B 方法（自测建议）

```sh
# 采样 60 秒内各簇实际频率分布（游戏里跑同一段路/训练场）
for i in $(seq 60); do
  echo "$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq) \
$(cat /sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq)" >> /sdcard/freq.log
  sleep 1
done
```

对比两组：`hmbird_tune=0`（纯风驰）vs `hmbird_tune=1`（本模块副调），
看 **prime 簇曲线下面积**（≈ 电量）和 **帧时间抖动**（Scene / Perfetto 的 jank 数）。
只有“电量降 + jank 不升”才算这次副调有效；任何一个变差就把对应那项调回默认。

### 2.3 一个必须知道的坑：它会重写线程亲和性

`hmbird_II.c` 里：

```c
static void hmbird_task_change_cpumask(struct task_struct *p) {
    ...
    cpumask_copy(&p->cpus_mask, task_cpu_possible_mask(p));   // 扩到全核
}
```

而 `trace_set_cpus_allowed_common()` 里只有两种情况会被尊重：

```c
if (ctx->flags & SCA_USER || cpumask_weight(ctx->new_mask) == 1) {
    cpumask_copy(&p->cpus_mask, ctx->new_mask);   // 单核掩码 / 用户态主动设置 -> 认
} else {
    cpumask_copy(&p->cpus_mask, task_cpu_possible_mask(p));  // 其他 -> 抹掉
}
```

结论：**游戏里 `taskset` 绑多核是无效的**（会被扩回全核），只有绑单核或走 `prefer_*` 接口才真正生效。
这就是 AsoulOpt 那类纯 taskset 方案在风驰机型上“看起来没效果”的根因，也是本模块线程归位的实现依据：
hmbird 在跑时写 `prefer_cpu`，不在跑时才回落 `taskset`。

---

## 3. 调度策略文献里，哪些结论被用进来了

| 结论 | 出处/依据 | 落到本模块 |
|---|---|---|
| EAS/schedutil 靠 util 选频，**上限（cap）是最省电且最可控的旋钮**，比改 governor 稳 | ARM EAS 文档、schedutil 设计 | 用 `scaling_max_freq` 做场景上限，不动 governor |
| uclamp 能在不改频率表的前提下限制某类任务能“要到”多少算力 | `Documentation/scheduler/sched-util-clamp.rst` | 后台 30%、前台 55%、熄屏 20~30% |
| 帧感知调度（fas-rs）说明**游戏里频率该由帧预算决定，而不是由通用 util 决定** | shadow3aaa/fas-rs | 游戏场景**不设上限**，把决定权还给 hmbird |
| sched_ext 调度器的收益集中在“任务放置 + 唤醒路径”，用户态改不了本体，但可以改它读的参数 | sched-ext/scx 设计文档 | 改 coefficient / perf_high_ratio / cpus_reserved / target_loads |
| 迁移成本与缓存局部性：绑核要绑“音视频/渲染”这类稳定线程，抢核的是后台 | WALT/sched 文档、AsoulOpt 实践 | 只对名字可识别的线程做归位，未知线程一律不动 |
| 频率抖动比频率高低更费电（每次 ramp 都要过一遍电压档） | 高通 DCVS/WALT 调参实践 | `target_loads` 88、`apply_freq_immediately=0`（游戏）、日常用 cap 而不是频繁写频 |

---

## 4. 三态权重：为什么日常压、游戏放

```
        ┌────────── 日常 ──────────┐        ┌──── 游戏 ────┐
功率    │ 上限 72%/62%             │        │ 不设上限      │
        │ 后台 uclamp 30%          │        │ 后台 uclamp 25%│
        │ 厂商 boost 全关          │        │ hmbird 系数微调│
        └──────────────────────────┘        └───────────────┘
              ↑ 收益：待机/轻载电流               ↑ 收益：帧率稳定 + 少一次簇跳变
```

- **日常压上限不会牺牲流畅度**，因为：
  1. 2 颗 prime 只在单线程突发时有用，UI 滚动主要吃 perf 簇；
  2. 切 App/启动窗口有 4 秒“瞬开”兜底（100%/92%）；
  3. uclamp 保证前台任务的 util 不会被后台任务淹没。
- **游戏不压上限**，因为 hmbird 已经按帧预算算好了；外加 cap 只会让它在需要冲一下时冲不上去，
  帧时间抖动变大。此时把 `cpus_reserved` 从 0xF0 改成 0xC0（把 CPU4-5 释放给普通任务）
  反而能减少“因为关键核被占而抬高整簇频率”的情况。

---

## 5. 模块工程上的取舍

| 取舍 | 原因 |
|---|---|
| **纯 shell，不带 .ko / 不注入** | 适配 Android 15~17 + KSU/Magisk/APatch + 元模块，二进制最容易踩 ABI 与 SELinux 的坑 |
| **1Hz 轮询 cgroup 而不是 `dumpsys activity`** | `dumpsys` 单次 80~300ms，是这类模块最典型的“自己耗电”；读 `top-app/cgroup.procs` 只要一次 read |
| **配置缓存到 shell 变量** | 热路径每 tick fork 十几次 awk/cat 会稳定吃掉 2~4% CPU；改完实测每 tick ≈ 0 fork |
| **线程扫描增量 + 单次 awk 匹配** | 200 线程 × 20 条规则 = 4000 次 grep，在手机上要 8 秒；现在固定 1 个 awk |
| **写前探测、失败即跳过** | 不同固件节点名不一致（`msm_performance` / `cpu_boost` / `sched_boost`），探测失败绝不能影响开机 |
| **不改 governor 名、不碰温控** | 风驰/厂商热管理会跟模块打架；一旦互相覆盖就是“发热又掉帧” |
| **卸载时还原上限** | 避免“模块删了但频率还被压着”这类事故 |
| **`oom_score_adj=-1000`** | 厂商进程回收器会顺手杀 root 守护进程，表现就是“过一会儿省电失效了” |

---

## 6. 待真机验证的点（贴 `doctor` 输出即可逐条确认）

1. `/proc/sys/hmbird_II` 是否存在（`hmbird=1`）；游戏时 `gov_*` 是否真的变成 `hmbird`。
2. `scaling_available_frequencies` 的实际档位，验证百分比换算落在预期档。
3. cgroup v2 的 `cpu.uclamp.max` 是否可写（部分 ROM 关掉 `CONFIG_UCLAMP_TASK_GROUP`）。
4. `taskset` 是否存在（决定非风驰场景的归位上限）。
5. 日常档的实际电流：用同一场景（刷信息流 10 分钟）对比开关模块的 `mAh` 与 `avg freq`。

---

## 7. 为什么把守护进程写成 C（v1.1）

### 7.1 触发点

装到真机后 WebUI 一直转圈：页面渲染了，但 `ksu.exec` 的回调一次都没回来
（BakaSU 把 WebUI 的 shell 接口挡掉了）。同时实测 shell 守护进程每 tick 有 10~15 次 fork，
在 1Hz 常驻的前提下这是不可接受的固定开销。

### 7.2 结构

```
src/feather.c
├── path_init()        所有路径都可由 FEATHER_* 环境变量覆盖（测试用）
├── cfg_load()         mtime 变化才重解析，热路径零 fork
├── detect_topology()  policy→cluster，最大频簇=prime，次之=perf
├── set_cap()          百分比→变频档，且绝不高于 syscap（启动时快照）
├── hmbird_*()         /proc/sys/hmbird_II 写入 + 默认值还原
├── apply_threads()    增量扫描 /proc/<pid>/task，POSIX regexec 匹配，零 fork
├── run_daemon()       poll(socket, http, timeout=1s) 单线程状态机
├── sock_handle()      UNIX socket，一行命令 → key=value 文本
└── http_handle()      127.0.0.1:7891，/status /doctor /games /set /mode …
                       另外直接托管 webroot（浏览器打开即可用）
```

协议刻意保持和 shell 版一致（`key=value` 文本），所以 WebUI 的解析逻辑零改动。

### 7.3 三个坑

1. **`-Wl,--gc-sections` + `-static` + aarch64 = 静默废掉 main**。
   x86 上同样参数正常；aarch64 上程序能启动、退出码 0、但什么都不做。
   定位方法：给一个必然输出的命令（`version`）看是否为空。
2. **并发探测竞态**：页面里 `refresh()` 与“切到游戏页”会同时触发通道探测，
   早期版本用 `probing` 布尔量导致第二个调用者拿到空 transport，界面显示“通道未连接”。
   改成共享同一个 Promise。
3. **守护进程的 data 目录被清掉**：`/data` 正常不会发生，但 WSL 下 `/tmp` 会被回收，
   表现为 pid/state 全部消失而进程仍在。加了一个每 60 tick 的 `mkdir` 自愈。

### 7.4 验证方式

`tests/run_tests_wsl.sh` 在 Linux 里用假文件树跑**真正的二进制**（x86 原生，或 arm64 靠 qemu-user），
覆盖 29 项：拓扑识别、限频档位、场景迟滞、hmbird 写入/还原、线程归位（看写入 trace）、
UNIX socket 命令、HTTP 桥（含 CORS）、熄屏、退出还原。
交付前这 29 项全绿，且其中一次直接用 `module/bin/feather`（arm64 静态）跑通。

---

## 8. 王者荣耀实测复盘（v1.1 → v1.2）

### 8.1 数据（Scene 每秒采样，600s，165fps 档）

| 指标 | 基准 hp调度 | v1.1 游戏档 |
|---|---|---|
| FPS 均值 | 164.80 | 164.62 |
| FPS p1 / 最低 | **161 / 156** | **157 / 144** |
| JANK / BigJANK | 4 / **0** | 7 / **1** |
| Max FT ≤6ms 秒数 | 159 | 348 |
| Max FT 19–30ms 秒数 | **7** | **45** |
| perf 簇 p50 | 1113 MHz | **787 MHz** |
| perf 簇利用率 | 52.3% | **66.9%** |
| prime 簇 | 1017 MHz | 768 MHz |
| 电流 / 温度 | 1325 mA / 52.8℃ | 1228 mA / 50.5℃ |
| UnityMain 落核 | 6-7 | **0-7** |
| UnityGfxDeviceW 落核 | 0-5 | **0-7** |

### 8.2 结论

1. **归位没生效，且规则写错**：`UnityGfx` 是前缀匹配，把 `UnityGfxDeviceW` 也抢到了
   prime 簇，与 UnityMain 抢同一簇；同时三条线程最终都在 0-7 —— 说明 `prefer_cpu` 写失败后
   回落 `taskset`，又被 hmbird 的扩掩码逻辑抹平。
2. **频率压过头**：同样 165fps，基准 1113MHz/52% 能跑完，v1.1 用 787MHz/67% 硬扛，
   每帧执行时间变长，遇到突发补不回来 → 19~30ms 长尾 7s → 45s。
3. 省电（−7%）是压频压出来的，不是效率优化来的，所以必然用帧时间换。

### 8.3 v1.2 改动

| 改动 | 依据 |
|---|---|
| 规则拆开：`^UnityMain$`→prime、`^UnityGfxDeviceW$`→perf、GfxRenderWorker 不强制 | 与基准落核一致 |
| 规则匹配改为「包名精确优先 → `*` 首匹配」 | 原来 `*` 规则是「最后一条命中生效」，顺序敏感且难推理 |
| `pin` 四级降级：prefer_cluster → prefer_cpu → **单核亲和** → 多核亲和 | hmbird 只尊重单核掩码，钉死一个核也比被撒满全核稳 |
| 游戏档抬 `scaling_min_freq` 地板（`game_min_pct=42`，≈1.52GHz） | 掐掉 384/537MHz 深谷，比整体抬上限省电 |
| 游戏档 background cgroup 收窄到 `0-3` | 后台线程别插进超大核抢主线程 |
| `hmbird_tune` 默认 **0**（不再改系数） | 系数往 1152 调后 p50 掉到 787MHz，方向假设被数据推翻 |
| `frame_per_sec` 跟随面板刷新率（读 fb0/modes） | 165fps 面板配 120 的窗口，帧预算算不准 |

### 8.4 下一轮验收指标

同一路线、同一 165fps、10 分钟：

- perf 簇 p50 ≥ 1050 MHz，p5 ≥ 900 MHz（不再出现 384/537）
- Max FT 19–30ms 的秒数 **< 15**（当前 45，基准 7）
- BigJANK = 0，FPS p1 ≥ 160
- 电流目标 1250–1290 mA（比基准低 3~5%，比 v1.1 高 2~5% —— 用这点功耗换长尾）

调不动的旋钮依次试：`game_min_pct` 35 / 42 / 50；`hmbird_frame_per_sec` 固定 165；
`hmbird_tune=1` 只把 `apply_freq_immediately` 设 1（其余系数保持默认）。

---

## 9. v1.2.1：三个把游戏档直接废掉的 bug

用户实测「WebUI 前台应用一直是空的」，顺着查出三个连环 bug——它们合起来让**游戏档从来没有真正生效过**：

| # | bug | 后果 |
|---|---|---|
| 1 | `read_all()` 成功返回字节数，调用处却写成 `!= 0` 才 continue | 反过来判断，等于"有数据就跳过"，永远拿不到 pid |
| 2 | 用读首行的 `rdline()` 去读多行的 `top-app/cgroup.procs` | 只看第一个 pid（真机常是 systemui），前台包名永远是错的 |
| 3 | `find_game_pid()` 同样只读首行 | 永远找不到游戏主进程 → 线程归位一次都没执行 |

连带效应：`fg` 为空 → `is_game()` 永远 false → 打游戏时跑的是**日常档 72% 上限**（实测 perf 簇 max 2611MHz，正好是 72% 档）→ 长尾 19~30ms 从 7 秒涨到 45 秒。

另外修了一个更隐蔽的：

| # | bug | 后果 |
|---|---|---|
| 4 | `capture_syscaps()` 每次命令都重采基线 | 把"我们刚设的上限"当成硬件上限，卸载/停止时还原到被压过的值（实测还原成 1305600） |

现在基线只快照一次、单独存数组（`detect_topology()` 每次 memset pol[] 也不会冲掉），并且**绝不把当前值当基线**。

前台包名也加了启发式：top-app 里所有 pid 全收 → 1) 命中游戏名单优先 2) 跳过系统包（com.android./com.oplus./...）3) 兜底第一个；全失败再退 `dumpsys`（6 秒节流）。

验证：34 项测试全绿，其中新增「前台包名绕过 systemui」「游戏抬底频」「后台 cpuset 收窄」「退出后 min/max 还原」。
