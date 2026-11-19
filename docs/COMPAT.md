# 机型兼容与风驰协同（v1.5.0）

这一版处理三件事：**机型/系统信息按真实环境显示**、**不再假设一加 15 的 CPU 拓扑**、
**修掉「打完游戏/软重启后再也起不来」的守护进程死锁**。最后附调度文献的阅读结论。

## 1. 分支与机型盘点（OnePlusOSS/android_kernel_oneplus_sm8850）

同一套 SM8850（canoe）内核，按机型分了 4 个分支，分支内是**累积**的板级 DTS：

| 分支 | 分支内可见的板号（board） | 充电 DTS |
|---|---|---|
| `oneplus/sm8850_b_16.0.0_oneplus_15` | infiniti 24831 | `oplus-chg-24831.dtsi` |
| `oneplus/sm8850_b_16.0_oneplus_15t` | fairlady 25821 / 25871，macan 24855 / 24877 | `oplus-chg-25821.dtsi`（166 KB fw 变体） |
| `oneplus/sm8850_b_16.0_pad_3_pro` | dengta 25021 / 25211，iceland 25926 / 25927 / 25978 | `oplus-chg-24624*.dtsi` / 24863 |
| `oneplus/sm8850_b_16.0_pad_4` | 同上（平板共用板号） | 同上 |

结论：**SoC 调度框架（WALT + sched_ext/hmbird_II）是同一套**，但
`/sys/devices/system/cpu/cpufreq/policy*` 的数量、编号、每簇核数、最高频都可能不同，
充电 DT（电芯数、温度分桶、UFCS 曲线）也不同。所以工程上只有一条路：**全部运行时探测**。

## 2. 机型与系统版本：不再硬编码

旧实现只扫 `/system`、`/vendor`、`/odm`、`/product` 的 build.prop，且缺值时 WebUI 回落
`'OnePlus 15'`——15T 用户看到的就是「OnePlus 15 · Android 17」。

现在的取值顺序（`rust/cfg.rs:read_buildprops`）：

1. `/my_product/build.prop`（ColorOS 把 `ro.product.marketname` 放这里）→ `/system` → `/vendor` → `/odm` → `/product` → `/system_ext`
2. 还缺的键用 `getprop` 兜底（非 Android 环境没有 getprop，自动跳过，不污染 stderr）

WebUI 顶部副标题：`ro.product.marketname` → `ro.product.model` → `ro.product.device` → `未知机型`，
后面拼 `Android <ro.build.version.release>`，风驰存在时再拼「风驰内核」。
长按（title）能看到 market / model / device / platform 四个原始值。

## 3. 拓扑自适应：`masks` 命令

规则里的 `prime / prime1 / prime2 / perf / mid / low / bg / all` 以前是写死的掩码
（`0xC0 / 0x80 / 0x40 / 0x3F / 0x30 / 0x0F / 0x03`），只有「6+2、policy0=0-5」的一加 15 成立。

现在 `detect_topology()` 解析每个 policy 的 `affected_cpus` 得到真实 CPU 集合，
`target_mask()` 按集合现算：

- `perf` = 性能簇全部核；`prime` = 超大核簇全部核
- `prime1` = 簇内编号最大的核（一加 15 = CPU7）；`prime2` = 次大的核（CPU6）
- `mid` = 性能簇上 1/3；`low` = 下 2/3；`bg` = 性能簇前 2 核（不足则取全部）
- `all` = 两簇并集

新增 `ctl.sh masks` 直接打印：

```
pol policy0 cpus=0,1,2,3,4,5 max=3628800 role=perf
pol policy6 cpus=6,7 max=4608000 role=prime
targets: perf=0x3f prime=0xc0 prime1=0x80 prime2=0x40 mid=0x30 low=0xf bg=0x3 all=0xff
```

4+4 或 3 簇的机器会自动得到不同的值（测试里用自造的 `policy0=0-3 / policy4=4-7` 验证过）。
`hmbird` 的 `prefer_cluster` 位图也改成按 policy 序号动态换算，不再是「有 cpu6/7 就算 0x2」。

## 4. 越狱/临时 root 下「起不来」的根因与修复（issue #1）

三个独立问题叠在一起：

1. **看护锁是死锁**：`service.sh` 用 `mkdir .sup.lock` 当互斥，进程被游戏/系统杀掉后目录留下，
   下次开机 `service.sh` 直接 `exit 0` → 永远不启动。表现就是「软重启只能重装」。
2. **退出码不匹配**：`feather run` 发现已有实例时返回 0，而看护脚本只把 3 当「已有实例」，
   于是每 2 秒重启一次失败实例（热循环）。
3. **看护进程会被一起杀**：`service.sh` 的 `( ... ) &` 子 shell 仍在模块的进程组里。

修复：

- 锁目录里写 `pid`，只有 `kill -0 <pid>` 成功才算「已有实例」，否则清理残留锁；
- `post-fs-data.sh` 开机清 `.sup.lock`；`boot-completed.sh` 再兜底一次；
- 看护脚本写到 `$DATA/supervisor.sh`，用 `setsid` 拉起，脱离模块/管理器进程组；
- `run_daemon()` 遇到已有实例返回 **3**，看护按约定退出，不再热循环。

临时 root（越狱）环境如果**重启后 root 本身没恢复**，模块脚本不会执行，这不是模块能修的；
但「root 还在、模块脚本执行了」的情况下，现在不会再被残留锁卡死。

## 5. 与风驰（hmbird / sched_ext）的协同

读过 Android 的 uclamp 用法、Qualcomm post_boot 的 WALT 调参、以及 sched_ext 的帧调度讨论后，
保留三条原则（也是本模块现在的做法）：

1. **uclamp 是建议、affinity 是强约束**：hmbird 在多核目标上会按自己的 cpumask 展开，
   所以对渲染/主逻辑线程优先写 `set_uclamp` + `prefer_cluster`，只对单核目标才写 `prefer_cpu`
   （[UtilClamp on Android](https://linuxplumbersconf.com/event/4/contributions/412/attachments/328/552/UtilClamp_usage_on_Android.pdf)、
   [OEM 调度与游戏输入](https://github.com/Gracker/android-internals-wiki/blob/master/src/part4-system/ch19-oem/03-oem-scheduling-game-input.md)）。
2. **不抄 post_boot 的 WALT 阈值**：`sched_upmigrate/downmigrate` 是百分比，和簇容量、
   频率表强相关，换了机型就是另一套数值；模块默认不写这些全局阈值
   （[init.qcom.post_boot.sh 示例](https://gerrit-public.fairphone.software/plugins/gitiles/device/qcom/common/+/824e487b9ed34af41ee974bd78a534f38ef9cfea/rootdir/etc/init.qcom.post_boot.sh)）。
3. **帧预算不是硬保证**：高优先级调度类、阻塞、抢占都会提前结束一次运行，
   所以只做「按刷新率对齐 frame_per_sec + 关键线程 uclamp」，不承诺 20ms 连续 CPU 时间；
   游戏默认 `official`，把频率决策交回风驰（[hmbird 讨论](https://xdaforums.com/t/info-on-the-hmbird-governor.4798030/)）。

## 6. 真机自检

```sh
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh masks'          # 拓扑与目标掩码
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh doctor' | grep -E '^(pol |targets|market|model|platform|android)'
```

把这两段发出来，就能判断某台机器（15T/平板）的 policy 划分是否被正确识别。
