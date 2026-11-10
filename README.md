# 羽量调校 · FeatherTune

面向 **OnePlus 15 / SM8850（骁龙 8 Elite Gen 5，2×Oryon Prime + 6×Oryon Performance，无小核）** 的
场景化功耗与调度调校模块。

不是无脑降频：日常按负载收紧频率上限、压掉厂商的频繁 boost、给后台降 uclamp；游戏时把频率控制权
让给 Oplus 风驰（`hmbird` / sched_ext），只做它管不到的事——线程归位与帧预算对齐。

- 适配：Android 15 / 16 / 17（ARM64）
- ROOT：KernelSU / Magisk / APatch；可与元模块（meta-overlayfs 等）共存
- WebUI：`webroot/`，模块卡片直接打开；管理器禁用了 `exec` 时用浏览器访问 `http://127.0.0.1:7891/`

> v1.2.2 · versionCode 10004 · C 静态守护进程（193 KB，零依赖）

![首页](shots/1home-light.png)

---

## 一、为什么是守护进程

v1.0 是 shell 守护进程，实测有两个问题：热路径每 tick 十几次 fork（手机上 2~4% CPU），
以及 WebUI 只剩 `ksu.exec` 一条通道——BakaSU / KSU-Next / SukiSU 会禁掉这个接口，
表现就是**页面能开但一直转圈**。

v1.2 是单个静态 arm64 二进制（`bin/feather`）：1 Hz 轮询、热路径零 fork、`sched_setaffinity()`
直接改亲和性，同时自带 UNIX socket 与 `127.0.0.1:7891` HTTP 两条取数通道，HTTP 那条还会直接托管 WebUI 页面。

| | v1.0 shell | v1.2 C |
|---|---|---|
| 每 tick 进程创建 | 10~15 次 fork | 0 |
| WebUI 取数 | `sh ctl.sh status`（~100ms） | UNIX socket（~1ms） |
| 兜底通道 | 无 | daemon 自带 HTTP + 托管页面 |
| 线程亲和 | `taskset` 子进程 | `sched_setaffinity()` |
| 二进制跑不起来 | — | 自动回落 `bin/shell/`（旧实现保留） |

---

## 二、做了什么

### 日常（daily / battery / sleep）

| 手段 | 说明 |
|---|---|
| 频率上限 | 按簇百分比：日常 72% / 超大核 62%，省电 50%/45%，熄屏 30%/25%（随“省电强度”浮动） |
| uclamp | 后台 30%、前台 55%、top-app 不限；熄屏 20~30% |
| 厂商 boost | 关 touch/input boost、`sched_boost`、`cpu_boost` 等节点 |
| 启动瞬开 | 切 App 后 4 秒内临时放开（100%/92%），避免“点开卡一下” |
| 漂移校正 | 厂商 thermal 改写上限后 10 秒内自动纠正 |
| 基线保护 | 只快照一次原始上限/下限，退出或卸载只还原到“动手前”，绝不抬高别人设的值 |
| I/O / GPU | read_ahead 256K、关 iostats；GPU `force_clk_on=0`、`bus_split=1` |

### 游戏（game）

| 手段 | 说明 |
|---|---|
| 频率上限 | **不压**，把控制权还给 hmbird |
| 频率地板 | 只抬 `scaling_min_freq`（约 1.55 GHz），掐掉 384/537 MHz 深谷 |
| 帧预算 | `frame_per_sec` 跟随面板刷新率（默认 120 → 165 面板实际 6.06ms） |
| 线程归位 | 见下方规则表，串行链两段分核 |
| 后台隔离 | background cgroup 收窄、uclamp 压低 |

**为什么绑核要绕路**：`hmbird_II.c` 的 `hmbird_task_change_cpumask()` 在 ops 启用期间会把任务掩码
扩到 `task_cpu_possible_mask`，多核 `taskset` 基本无效；只有「单核掩码」会被尊重。所以规则的降级顺序是
`prefer_cluster → prefer_cpu → 单核亲和 → 多核亲和`，单核目标直接走 `prefer_cpu`。

### 线程规则（`rules.conf`）

```
<包名|*>  <线程名 POSIX ERE>  <目标>
目标： prime=CPU6-7  prime1=CPU7  prime2=CPU6  perf=CPU0-5  mid=CPU4-5  low=CPU0-3  bg=CPU0-1
```

三个坑（都踩过，写进注释了）：

1. **最后一个空白字段是目标，中间整段是正则** —— 所以正则里可以带空格，`^Job\.worker` 才能匹配
   内核 comm 为 `Job.worker 0` 的线程。
2. **POSIX ERE 不是 PCRE**：`\d` 在 glibc 下等于字面 `d`，数字要写 `[0-9]`。
3. **默认锚定 `^...$`**：写 `UnityGfx` 是前缀匹配，会把 `UnityGfxDeviceW` 一起拉进超大核。

默认规则：

| 线程 | 目标 | 依据 |
|---|---|---|
| `^UnityMain$` | `prime1`（CPU7） | 串行链第一段，独占一颗大核 |
| `^UnityGfxDeviceW$` | `prime2`（CPU6） | 串行链第二段，与主线程分核不排队 |
| `^GfxRenderWorker$` | `mid`（CPU4-5） | 厂商基准不限核，只收到同簇中段 |
| `^Job\.worker` | `perf` | 纯吞吐型 C# Job，不占超大核 |
| `^AudioTrack$\|^AudioOut_[0-9]` | `low` | 音频不抢渲染核 |

未命中的 `Thread-xxxx` 故意不绑——容量够的前提下乱绑只会把热线程挪到更差的核上。

---

## 三、实测（王者荣耀 165 fps，600 s）

| 指标 | 基准（hp调度） | v1.1（问题版） |
|---|---:|---:|
| 平均 FPS | 164.80 | 164.62 |
| FPS p1 | 161 | 157 |
| JANK | 4 | 7 |
| Max FT 19–30 ms 的秒数 | 7 | 45 |
| perf 簇 p50 | 1113 MHz | 787 MHz |
| 全进程 CPU | 172.3% | 213.8% |
| 电流 | 1325 mA | 1228 mA |

v1.1 的 7% 电流是**靠压频硬压**出来的：所有线程同比膨胀 24%，而 UnityMain + UnityGfxDeviceW
两核合并需求只有 100.7%（离 2 核 200% 差得远）——不是抢不到核，是频率不够。v1.2 因此改成
「抬地板 + 修线程归位 + 对齐帧预算」，不再继续压频。

> v1.2 的真机复测数据待补（见「已知问题」）。

---

## 四、安装

1. 下载 `FeatherTune_v1.2.2.zip`，在 KernelSU / Magisk / APatch 里 **从本地安装**
2. 重启（或在 WebUI 里点“重启守护进程”）
3. 自检：

```sh
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh doctor'
```

配置在 `/data/adb/feather/`，升级不覆盖：`config.ini` / `games.conf` / `rules.conf`。

---

## 五、命令行

```sh
ctl=/data/adb/modules/feather_tune/bin/ctl.sh
sh $ctl status            # 状态
sh $ctl doctor            # 环境诊断（提 issue 请附这个）
sh $ctl mode battery      # auto/daily/battery/game/perf/sleep
sh $ctl set intensity 70  # 省电强度
sh $ctl pkg add com.xxx.yyy
sh $ctl restore           # 立刻还原
sh $ctl restart
```

---

## 六、已知问题（欢迎提 PR）

真机（OnePlus 15 / ColorOS 16）上 doctor 目前会看到：

| 现象 | 原因 | 现状 |
|---|---|---|
| `selftest prefer_cluster=FAIL` / `prefer_cpu=FAIL` | `/proc/sys/hmbird_II/prefer_*` 写入被拒 | v1.2.2 起 doctor 打印 `prefer_errno=` 定位是 SELinux 还是格式；失败时降级到单核亲和（单核掩码 hmbird 会尊重） |
| `refresh=0Hz` | 该机型不允许读 `fb0/modes` | `frame_per_sec` 保持内核默认 120；可在 `config.ini` 里显式写 `hmbird_frame_per_sec=165` |
| `fg=-` `cand=0` | cgroup 布局与标准路径不同 | v1.2.2 起 doctor 打印 `cgroup_root` 探针与 `/proc/self/cgroup`，据此再适配 |
| `bg_cpuset=n/a` | `background/cpuset.cpus` 不存在 | 同上，探针会指出实际路径 |

这些问题不影响日常档/省电档生效，但会让**游戏档的线程归位退化**。

---

## 七、构建与测试

```sh
# 交叉编译（WSL + aarch64-linux-gnu-gcc）
aarch64-linux-gnu-gcc -Os -s -static -Wno-format-truncation -Wno-unused-result \
  -o module/bin/feather src/feather.c

# 端到端测试：假 /sys /proc /cgroup 树 + 真实 arm64 二进制（binfmt/qemu-user）
FEATHER_TEST_BIN=$PWD/module/bin/feather bash tests/run_tests_wsl.sh   # 37 项

# 打包
python build.py
```

```
src/feather.c          单文件 C 守护进程（daemon + CLI + socket + HTTP + WebUI 托管）
module/                Magisk/KSU/APatch 模块骨架（含 webroot/）
module/bin/shell/      v1.0 的 shell 实现，仅作二进制跑不起来时的兜底
tests/                 37 项端到端测试 + 假文件树 + 场景 CSV 分析脚本
docs/DESIGN.md         设计取舍、源码分析、每一轮实测复盘
```

> 交叉编译坑：`aarch64-linux-gnu-gcc -static` 加 `-Wl,--gc-sections` 会得到一个
> **能启动、退出码 0、但 main 不执行** 的二进制（x86 正常）。所以不带 gc-sections。

---

## 八、许可与致谢

GPL-3.0（见 `LICENSE`）。模块会与内核侧 sched_ext/风驰调度器交互，沿用社区惯例使用 GPL。

参考与致谢：

- [OnePlusOSS/android_kernel_oneplus_sm8850](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8850)
  —— 拓扑、`hmbird_II` sysctl 与 `hmbird_task_change_cpumask()` 行为均据此分析
- [MoWei-2077/MW_CpuTurboScheduler](https://github.com/MoWei-2077/MW_CpuTurboScheduler) —— 模块形态参考
- AsoulOpt —— 游戏线程归位的思路来源
- sched_ext / scx_hmbird —— 风驰底层
- KernelSU 模块文档 —— WebUI 与模块规范

本项目来自对上述**公开源码的分析**与真机实测，未包含任何厂商专有代码。
