# 风驰（hmbird_II）源码分析笔记

来源：`OnePlusOSS/android_kernel_oneplus_sm8850` @ `oneplus/sm8850_b_16.0.0_oneplus_15`，
`vendor/oplus/kernel/cpu/sched_ext/hmbird_II/`。真机：PLK110 / Android 17 / kernel 6.12.69。

## 1. 它是什么

一个 sched_ext（BPF）调度器 + 一组内核侧 sysctl。**真正做决策的代码在编译好的 BPF 对象里**，
内核树里只有：

- `hmbird_II.c`：注册 scx ops、任务属性位、`hmbird_task_change_cpumask()`
- `hmbird_II_freqgov.c`：频率调速器侧接口
- `hmbird_II_sysctl.c`：参数/任务属性的入口
- `hmbird_kfunc.c`：导出给 BPF 的 kfunc（`hb_bpf_cfg_cpus_get`、`hb_bpf_set_prefer_cpu` …）

这一点决定了优化策略：**能精确控制的是"任务属性"和"输入给 BPF 的参数"，不是算法本身**。

## 2. 参数表（内核侧真实格式）

| 节点 | 格式 | 默认 | 谁消费 |
|---|---|---|---|
| `coefficient` | `v0,v1`（每簇一个，范围 8–4096） | `1024,1024` | 只经 kfunc 交给 BPF |
| `perf_high_ratio` | `v0,v1` | `60,60` | 同上 |
| `freq_policy` | `v0,v1`（无范围校验） | `0,0` | 同上 |
| `cpus_reserved` | 十进制或 0x，按位掩码 | `0xf0` | 同上（`cfg_cpus_get`） |
| `cpus_exclusive` | 同上 | `0` | 同上 |
| `frame_per_sec` | 整数 | `120` | BPF 的帧预算 |
| `heavy_boost` | `type bottom_perf boost_weight` | `0 400 140` | 重载任务抬频 |
| `highres_tick_enable` | 0/1 | — | 高精度 tick |

读回的格式和写入不同，**校验时必须按值解析，不能字符串比对**（这就是 v1.2.2 自检误报的原因）：

```
coefficient=1024 1024
cpus_reserved=cpus_reserved=0xf0
heavy_boost=type=0, bottom_perf=400, boost_weight=140
```

## 3. 任务属性（可以精确打的"标签"）

`cfg_task_sched_prop_handler` 的写入口支持两种寻址：

```
<pid> <value>        # 按 pid
<线程名> <value>     # 按 comm，内核 find_task_by_name() 找
```

| 节点 | 取值 | 含义 |
|---|---|---|
| `prefer_cpu` | 0–0xff 掩码（hex） | 偏好哪几颗核 |
| `prefer_cluster` | 0 … (1<<nr_cluster)-1 | 偏好哪个簇 |
| `prefer_preempt` | 0/1 | 标记为"可抢占他人"（延迟敏感） |
| `prefer_idle` | 0/1 | 标记为"偏好空闲核"（可以让路） |
| `set_uclamp` | 1–1024 | per-task uclamp_max，**直接影响该任务把频率顶多高** |
| `uclamp_keep_freq` | 0/1 | uclamp 是否参与选频 |

读回是 `group_leader_comm::comm::value` 的列表：

```
prefer_cluster:
UnityMain::UnityMain::2
```

## 4. 亲和性会被改写（关键约束）

`hmbird_task_change_cpumask()` 在 ops 生效期间会把任务掩码扩到 `task_cpu_possible_mask`。
所以：

- 多核 `sched_setaffinity` / `taskset` 基本无效
- **单核掩码**才会被尊重
- 想"钉在某颗核"必须走 `prefer_cpu` + 单核掩码，或直接单核亲和

## 5. 我们据此做的优化（v1.4 规则）

| 手段 | 依据 |
|---|---|
| 串行链两段分核（CPU7 / CPU6） | 实测 pair 串行耗时 6.105ms > 6.06ms 预算，两个超大核峰值只用到 145%/200% |
| `prefer_preempt` 打在主线程与图形设备线程 | 这两条是每帧必跑的关键路径，抢占优先减少排队 |
| `Job.worker` 加 `set_uclamp=640`（62.5%） | 并行 Job 是纯吞吐，跑满会把整簇频率需求顶上去；压 uclamp 保吞吐不掉帧但少耗电 |
| `AudioTrack/AudioOut_*` → `low+idle` | 音频不需要和渲染抢核，给 idle 让路 |
| `frame_per_sec` 跟面板 165Hz | 默认 120 会让 BPF 把帧预算放大 37%（8.33ms vs 6.06ms） |

## 6. 只能实测的部分（不要凭想象调）

`coefficient` / `perf_high_ratio` / `freq_policy` / `cpus_reserved` / `heavy_boost` 在内核侧**没有实现体**，
语义全在 BPF 里。已知只有"读回格式"和"取值范围"。所以：

- 模块默认**不写**这些值（`hmbird_tune=0`），避免把厂商调好的协同值搞坏
- 想 A/B 时用 `hmbird_tune=1` + 单个参数改动，一次只动一个
- 每次改动都要在同一路线（同地图/同帧率）录 10 分钟，看 perf 簇 p50 / 19–30ms 长尾秒数 / 电流

上一轮的教训：v1.1 盲调 `coefficient=1152`，结果 perf p50 从 1113MHz 掉到 787MHz —— 方向都是错的。


## 7. 实测复盘：为什么"给更多频率"反而更差（v1.2.7 → v1.2.8）

王者荣耀 165fps，官方调度 vs 模块激进档（同机同路线）：

| 指标 | 官方 | 模块激进(v1.2.7) | 差 |
|---|---:|---:|---|
| perf 簇 CPU0-5 均值 | 1231 MHz | 1716 MHz | **+39.5%** |
| perf 簇 min | 787 MHz | 1555 MHz | 低档被锁死 |
| prime 簇 CPU6-7 | **1017 MHz（95.5% 秒数恒定）** | 768 MHz（78% 秒数） | **−10%** |
| FPS 均值 | 164.80 | 161.32 | −2.1% |
| FPS p1 | 161.0 | 135.8 | −15.6% |
| JANK /100s | 0.67 | 5.78 | **8.7×** |
| 16–33ms 秒数 | 17 | 81 | 4.8× |
| CPU 周期/帧 | 27.52 M | 35.21 M | +27.9% |
| 能耗/帧 | 29.26 mJ | 35.05 mJ | +19.8% |
| CPU 温度 | 52.8℃ | 59.9℃ | +7.2℃ |
| 功率 | 4821 mW | 5655 mW | +17.3% |
| UnityMain 绑核 | **6-7（100% 秒数）** | 0-7（100% 秒数） | 绑核被拆掉 |

三个结论：

1. **频率给错了簇**。真正扛 UnityMain 的是 CPU7（占用 49.8%），官方把它钉在 prime 簇
   并保底 1017MHz；我们把 prime 压到 768，同时把 perf 抬到 1555/2227——给不干活的簇加频率，
   给干活的簇减频率。
2. **官方的绑核和它的频率策略是配套的**。它把 UnityMain 钉 6-7、UnityGfxDeviceW 钉 0-5，
   所以 prime 必须保底。我们改成"单核钉 CPU7/CPU6 + prefer_preempt"，实测线程反而全部退回 0-7
   （`hmbird_task_change_cpumask()` 会把掩码扩开），等于把官方的绑核拆了，还顺手降了 prime。
3. **瓶颈是等待不是算力**。线程合计 CPU% 只涨 2.4，但每帧周期 +27.9%、能耗 +19.8%；
   CoreThread +35%，渲染/Job 链全部下降。CPU 占用 43%、GPU 占用 70%，没有任何一侧饱和。
   这种状态下加频率只会把等待也一起烧掉。

所以 v1.2.8 起：**游戏档默认 `official`**——不写频率、不绑核、不动 hmbird 参数。
三档可在 WebUI「场景」页切：

| 模式 | 行为 |
|---|---|
| `official`（默认） | 什么都不写，等于装了个空模块 |
| `light` | 只把 `frame_per_sec` 对齐面板刷新率 |
| `aggressive` | v1.2.7 的旧行为（地板 + 绑核 + uclamp），仅用于 A/B |

另外修掉一个隐患：`hmbird_restore()` 原来会把参数还原成**硬编码的猜测值**（`1024,1024` /
`0 400 140`），等于每次出游戏都把厂商调好的协同值覆盖掉。现在改成写前存原值、还原按原值写回。
