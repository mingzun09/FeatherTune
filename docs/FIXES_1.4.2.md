# v1.4.2 — 图标覆盖、双电芯功率、充电功率诊断

> 承接 [FIXES_1.4.1.md](FIXES_1.4.1.md)。v1.4.1 解决了「图标全灭 / 限流节点不存在 / 刷机失败」，
> v1.4.2 解决「图标只对一部分 / 功率显示减半 / 加速看不出效果」。

| 现象 | 根因 | 修复 |
|---|---|---|
| 只有部分应用有图标，其余是字母或错图 | 只扫 `pm path` 第一条；资源名被混淆后名字匹配失效；自适应图标只有透明前景层 | 多 APK 扫描 + 图片尺寸嗅探 + 方形判定 + 缓存版本号 |
| 双电芯机器峰值功率只有真实一半 | `voltage_max` 在 charger v2 上是**单芯**电压（~4.5V），旧逻辑据此判成 1S | 读 DT `oplus,batt_num`，`battery_cells` 可覆盖 |
| 充电加速「没有明显提升」 | 常温下厂商本来就在最高电流档，没有可提升空间 | 新增 `charge why` 把证据摊开 |

---

## 1. 图标：从「能抠出来」到「抠对」

### 1.1 只扫了第一个 APK

`pm path <pkg>` 对带 split 的应用会返回多行（`base.apk` + `split_config.*.apk`）。
旧实现取第一行，万一拿到的是 split，里面没有 `resources.arsc`，清单里自然没有 `ic_launcher`。
现在会遍历所有 APK，**优先 `base.apk`**，在一个 APK 里找到候选就停。

### 1.2 资源名混淆后，名字匹配全失效（“异常图标”的主因）

AGP 的 `shrinkResources` / 资源名混淆会把 `res/mipmap-xxxhdpi-v4/ic_launcher.png` 改名成
`res/ab.png`。旧逻辑按 `ic_launcher|icon` 匹配不到，就退化成「`res/drawable*` 里随便一张」——
于是出现了通知栏小图标、启动图、logo、甚至纯色块。

现在：

- 先按名字打分（`ic_launcher` 1000 > `icon` 900 > 含 icon/logo 600 > `mipmap` 400 > 其他 50）；
- 名字分 ≥600 的直接采信（真正的大概率是图标）；
- 名字分低的（混淆名）改用**图片头里的宽高**判断：必须是**正方形且 ≥48px**，取面积最大的那张；
- 排除名单扩到 `foreground/background/monochrome/notification/statusbar/stat_sys/qs_/widget/splash/banner/shadow/watermark/_bg/bg_/badge/mask`。

宽高解析只读文件头，不依赖任何解码库：PNG(IHDR)、WebP(VP8/VP8L/VP8X)、JPEG(SOF0/1/2)。

### 1.3 纯自适应图标

有些应用只发 `mipmap-anydpi-v26/ic_launcher.xml` + 矢量前景层，**没有位图本体**。
这种情况下任何「抠一张图」的做法都只能拿到透明前景层——显示出来就是空白或一块色。
现在的选择是：**退回首字母，不拿前景层冒充图标**。这是有意的取舍（宁可字母，也不要错图）。

### 1.4 旧缓存自动失效

抠图算法变了，`/data/adb/feather/icons/` 里可能还躺着上一版抠错的图。
新增 `icons/.ver`，版本不一致就整目录清一次（日志里会写 `icon: 缓存版本 X -> 3`）。

### 1.5 v1.4.4：改用管理器原生接口（最省事也最准）

参考 `KOWX712/Tricky-Addon-Update-Target-List` 的做法后确认，KernelSU 系管理器（含 SukiSU、
APatch）的 WebView 已经内置了两件事，根本不需要我们自己抠图：

| 能力 | 接口 | 说明 |
|---|---|---|
| 应用名 | `ksu.getPackagesInfo(JSON.stringify([...]))` → `appLabel` | 系统 PackageManager 的显示名，支持多语言 |
| 图标 | `ksu://icon/<包名>` | 管理器用 `AppIconCache.loadIconSync(..., 512)` 渲染成 **512px PNG**，自适应图标/主题图标都对 |
| 包列表 | `ksu.listPackages('user')` | 等价 `pm list packages -3`，但走 Binder，不依赖 shell |

（源码：`tiann/KernelSU` 的 `WebViewInterface.kt` / `WebViewHelper.kt`；新 API 在 `ksu.pm()` 上，
旧 API 直接在 `ksu` 上，两套都做了兼容。）

所以 v1.4.4 的顺序是：

1. **名字**：系统 `appLabel`（ColorOS 上本身就是中文）→ `labels.conf`（人工校对，兜底）→ 包名末段；
2. **图标**：`ksu://icon/<pkg>` → daemon `/icon`（浏览器直连 7891 或管理器没给图时）→ 首字母；
3. **列表**：`ksu.listPackages('user')` 与 daemon 的 `apps` 取并集，新装应用立刻能单独设策略。

自己抠图那套仍然保留：Magisk 没有 WebUI，用户用浏览器打开 `127.0.0.1:7891` 时没有
`window.ksu`，只能靠 daemon；系统应用通常不在管理器的 `webUIApps()` 里，`ksu://icon` 会 404，
也会回落到 daemon。

### 1.6 验收

```sh
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh doctor' | grep '^icon '
# icon tool=/data/adb/ksu/bin/busybox unzip cached=NN miss=N dir=...
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh icon com.tencent.mm force'
```

---

## 2. 双电芯：峰值功率只有一半

### 2.1 根因

`power_w10()` 用 `battery/voltage_max > 6V` 判断 2S。但 charger v2 的
`POWER_SUPPLY_PROP_VOLTAGE_MAX` 返回的是 `GAUGE_ITEM_VOL_MAX`——**单芯**电压上限
（约 4.5V），不是整包终止电压。于是 `cells=1`，功率少算一半。

真机 2026-11-11 的回传就是这个数：`voltage_now=4301000µV, current_now=-8158mA` →
旧逻辑 35.0W，实际 2S 应是 **70.1W**。

### 2.2 修复

判定顺序（`charge.rs: battery_cells()`）：

1. `config.ini` 的 `battery_cells`（0 = 自动）
2. DT `oplus,batt_num`（一加 15 的 `oplus-chg-24831.dtsi` 里是 `<2>`）
3. 兜底：`voltage_max > 6V` 视为 2S

```sh
su -c 'sh .../ctl.sh power'
# soc=78 temp=33 status=Charging cells=2
# battery: vbat=4247mV ibat=-4421mA → 37.5W      ← 旧版这里会是 18.8W
```

WebUI「充电功率」一行会带上 `4.25V×2S` 与 `4.42A`，一眼能看出倍率是怎么来的。

---

## 3. 充电功率为什么「没有明显提升」

先把结论放前面：**这台机器在常温下已经没有可提升的空间了。**

### 3.1 证据（都在 DT / 内核源码里）

1. UFCS/PPS 曲线用**机身温度**分桶（`oplus,temp_type = <OPLUS_STRATEGY_USE_SHELL_TEMP>`），
   一加 15 的分桶表是 `oplus,temp_range = <0 50 120 160 210 380 440>`：
   `normal_high = [210, 380) = 21.0~37.9℃`。常温机身（~29℃）**本来就在最高电流段**，
   把伪装值写成 22.0℃ 不会换段 → 不产生任何增益。
2. 电池温度那条单独的限流表 `oplus,ufcs_strategy_batt_high_temp = <400 420 430 ...>` +
   `oplus,ufcs_strategy_high_current = <6000 5000 3000 ...>`：只有电池 **≥40℃** 才开始降流。
3. `oplus,curr_max_ma = <10900>`：10.9A 就是 DT 给 UFCS 的上限，再快也不可能超过它。
4. 熄屏智能充电接口（`battery/current_now`）只能**限流**，不能加速。

所以「加速」真正有意义的场景只有：机身被晒热 / 边充边玩把 shell 顶到 38℃ 以上，
这时伪装能把曲线拉回 normal_high。常温下没有变化是**预期行为**，不是没生效。

### 3.2 新增 `charge why`

```sh
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh charge why'
```

输出示例（真机字段含义）：

```
soc=78% temp=33℃ status=Charging screen=1
power=70.1W cells=2 vbat=4301mV ibat=-8158mA peak=71.0W
limit: node=/sys/class/power_supply/battery/current_now kind=v2-current-now written=0
limit: 亮屏 → charger v2 把 current_now 写入当成“取消限流”，模块已释放；只有熄屏才限流
accel: node=oplus_dbg_tbat target=220 auto_dt=220 spoofing=0 blocked=0
real: shell shell_back=29.0℃ → normal_high (4)
dt: [0, 50, 120, 160, 210, 380, 440]  normal_high=[210, 380) = 21.0~37.9℃
fake: 22.0℃ → normal_high (4)
verdict: 真实 normal_high 与伪装 normal_high 同段，电流档不变 → 没有增益
```

发热时会变成：

```
real: shell shell_back=39.2℃ → warm (5)
verdict: 真实 warm → 伪装 normal_high，曲线换到更高电流段（这条才会让功率变高）
```

### 3.3 WebUI 一直显示「已关闭」/ 开关永远是关的

排查时发现的第二个问题：WebUI 的开关和分段控件全靠 `state.cfg_*` 回填，但守护进程的
`state` 文件里**从来没写过这些键**（只有 `MOCK_STATE` 演示数据里有）。后果是：

- 页面上的开关全部显示为关闭（实际 `charge_ctrl=1`、`accel_ctrl=1`）；
- 充电策略永远走 `state.cfg_charge_ctrl !== '1'` 分支 → 显示「已关闭 / 不干预厂商充电曲线」；
- 用户按“开启”只是又写了一遍 1，看不到任何变化 → 这就是「改了没反应」的另一半原因。

现在 `state_write()` 会写 `cfg_enable / cfg_mode / cfg_intensity / cfg_auto_game /
cfg_thread_opt / cfg_screen_off / cfg_kill_boost / cfg_io / cfg_gpu / cfg_launch_boost /
cfg_charge_ctrl / cfg_accel_ctrl / cfg_accel_stop / cfg_game_mode`，开关与充电状态
都和实际配置一致了。

同时 `accel_eff` 会被写进 state：`1` = 伪装确实把曲线换到了更高电流段，`2` = 与真实温度同段
（UI 显示「加速中·同段」，直接说明“功率不会变”）。

### 3.4 建议的 A/B 方式

1. 先确认计量正确（`cells=2`，功率与充电头标称量级一致）；
2. 关掉加速（`ctl.sh set accel_ctrl 0`）充 5 分钟，记 `peak`；
3. 打开加速再充 5 分钟，比 `peak`；
4. 想让电池温度也参与，才考虑 `ctl.sh set accel_gauge 1`——代价是过温保护读到的也是伪装值，
   模块的 45℃ 闸会失效（默认不开）。

---

## 4. 变更清单

| 文件 | 变更 |
|---|---|
| `rust/charge.rs` | `battery_cells()` 走 DT `oplus,batt_num` + 配置覆盖；DT 属性扫描；记录 `acc_dt` |
| `rust/cmd.rs` | 图标：多 APK、尺寸嗅探、打分、缓存版本；`charge why`；`charge`/state 增加 cells/节点类型 |
| `rust/state.rs` | `Cfg.battery_cells`、`acc_dt`、`icon_ver_checked` |
| `rust/cfg.rs` | 解析 `battery_cells` |
| `module/webroot/*` | 新增「充电功率」行；`chg_mv/chg_ma/chg_w/chg_peak/cells/chg_node` |
| `module/etc/config.ini` | 新增 `battery_cells=0` 说明 |
| `tests/mkfake.sh` | DT `oplus,batt_num`、thermal_zone(shell/batt) |
| `tests/run_tests_wsl.sh` | 新增 7 条：电芯判定/覆盖、`charge why`、混淆资源名、纯自适应图标 |
