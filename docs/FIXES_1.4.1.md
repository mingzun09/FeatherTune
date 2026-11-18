# v1.4.1 — 三个线上问题的根因与修复

> 背景：v1.4.0 在真机上暴露三个问题 —— SukiSU 刷不进去、WebUI 应用管理的图标不显示、
> 充电功率控制改了没反应。三个问题互相独立，但都能在仓库里找到确定性证据。
> 下面按「证据 → 机制 → 改法 → 怎么验收」写。

| 现象 | 直接原因 | 修复 |
|---|---|---|
| SukiSU/KernelSU 安装直接失败 | `customize.sh` 被打包成 CRLF，Android 的 sh 解析到 `then\r` 就语法错误 | 强制 LF + 打包时归一化并自检；顺手修掉解压权限与 `rm -rf bin` 的坑 |
| 应用图标全是首字母 | ① WebUI 被 SukiSU 的 http 服务器托管时 `HTTP_BASE` 被置空 ② 解压工具探测必错 ③ 只认 png、排序选错层 ④ 负缓存永不过期 | 逐条修，见 §2 |
| 充电限流没反应 | 旧实现写的节点在 charger v2 上根本不存在；本机唯一可写的 `battery/current_now` 又是「仅息屏」语义 | 按内核源码改走 current_now，并说清楚边界，见 §3 |

---

## 1. SukiSU 无法刷入：`customize.sh` 是 CRLF

### 证据

仓库工作区的 `module/customize.sh` 每一行都是 CRLF（83 个 CR / 83 个 LF），
`module/module.prop` 也是（6 个 CR）。而 `build.py` 是**按字节读盘**打进 zip 的，
所以 `dist/FeatherTune_v1.4.0.zip` 里的 `customize.sh` 同样是 CRLF。

`git status` 早就提示过：

```
warning: in the working copy of 'module/customize.sh', CRLF will be replaced by LF
         the next time Git touches it
```

（`.gitattributes` 里写了 `*.sh text eol=lf`，但它只在 git 触碰文件时生效，
不改变工作区现状，也管不到打包。）

### 为什么必失败

Android 的 `sh`（mksh / busybox ash）按行解析。CRLF 下每个 token 后面多一个 `\r`：

```sh
if [ "$(getprop ro.product.cpu.abi)" != "arm64-v8a" ]; then\r
```

`then\r` 不是关键字，直接语法错误；解释器连第一个 `if` 都过不去。
在 Linux 上复现同样如此：

```
$ dash customize.sh
customize.sh: 5: Syntax error: end of file unexpected (expecting "then")
```

管理器能给的提示只有「安装失败」，看不出是脚本坏了。

### 改法

1. `customize.sh` / `module.prop` 重写为 LF。
2. `build.py` 升级为「打包器 + 守门员」：
   - 文本后缀（`.sh/.prop/.ini/.conf/.js/.css/.html/...`）自动 `CRLF/CR → LF`、去 BOM，
     并把归一化过的文件打印出来；
   - 归一化后仍含 `\r` 直接拒绝打包；
   - 校验 `module.prop` 必须有 `id=` / `version=` / `versionCode=`；
   - 打完**重新读回来**自检一遍。
3. 顺手修掉两个会跟着 CRLF 一起发作的隐患：
   - 旧脚本先 `rm -rf "$MODPATH/bin"` 再 `unzip`。部分 SukiSU/KernelSU 版本的安装环境里
     没有 `unzip`，这一步等于把刚解压好的模块掏空 → 现在改成非破坏性，且没有 unzip 时直接跳过。
   - Python `zipfile` 手工构造 `ZipInfo` 时默认 `create_system = 0`（MS-DOS）。
     此时 `external_attr` 里的 Unix 权限位会被 Info-ZIP 等解压器忽略，
     `bin/feather` 解出来是 `0644`、不可执行。现在显式 `create_system = 3`。

### 验收

```sh
python build.py                      # 末尾应打印 self-check ok ... mode=0o755
python - <<'PY'
import zipfile
z = zipfile.ZipFile('dist/FeatherTune_v1.4.1.zip')
bad = [i.filename for i in z.infolist()
       if i.filename.endswith(('.sh','.prop')) and b'\r' in z.read(i.filename)]
print('CRLF files:', bad)            # 必须是 []
PY
```

刷入后 `su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh doctor'` 能出结果即为正常。

---

## 2. 应用管理图标不显示

截图里 158 个应用全是「首字母色块」，说明不是个别 APK 的问题，而是整条抠图链路断了。
四个原因叠加，任何一个都足以让图标全灭：

### 2.1 WebUI 被 SukiSU 托管时，图标请求打错了服务器（SukiSU 专属）

旧代码：

```js
const HTTP_BASE = (location.protocol === 'http:') ? '' : 'http://127.0.0.1:7891';
```

它假设「协议是 http ⇒ 页面就是 daemon 提供的」。但 SukiSU Ultra / KernelSU 的 WebUI
本身就是 `http://127.0.0.1:<另一个端口>` 的本地服务，于是 `HTTP_BASE` 被置空，
`/icon?p=...` 请求发到了 WebUI 服务器（404）。

为什么其它功能看着正常：状态、应用列表都走 `ksu.exec`（`TRANSPORT=exec`），
只有 `<img>` 没法走 exec —— 于是「列表正常、图标全空」。
这也解释了为什么这个现象在 MagicNet/Magisk 上不明显。

**修复**：`HTTP_BASE` 默认就是 `http://127.0.0.1:7891`，只有 probe 时实测
**同源 `/version` 真的能通**（页面确实由 daemon 托管）才改成同源 `''`。

### 2.2 解压工具探测必错

旧代码：

```rust
if command -v unzip ... { "unzip" }
else if command -v busybox ... { "busybox unzip" }
else if command -v toybox ... { "toybox unzip" }
```

真实情况是：

- Android 的 **toybox 没有 unzip 子命令**，`command -v unzip` 在真机上通常是空；
- `command -v toybox` 却一定成功 → 旧代码稳定选中 `toybox unzip`；
- 每条 `toybox unzip -l` 都失败 → 所有应用都进负缓存。

另外守护进程的 `PATH` 由管理器给，部分管理器给的环境里没有 `/system/bin`，
`pm`、`awk` 都找不到。

**修复**：

- 候选改成「KSU / APatch / Magisk 的 busybox 绝对路径 + `busybox unzip` + `unzip` + `toybox unzip`」；
- **不信 `command -v`，拿真 APK 跑一次 `-l`，输出里出现 `res/` 才算可用**，并把结果缓存在进程里；
- 子进程显式补全 `PATH`（并保留管理器原本的 PATH），不再依赖环境；
- 全部候选都失败时记 `unzip_none`，避免每个应用都重试 8 遍。

### 2.3 只认 `.png`，而且排序会选到透明前景层

旧实现的候选：`res/*.png` 里匹配 `ic_launcher|icon`，然后 `sort -n | tail -1` 取体积最大的。

两个问题：

1. 现代 APK 大量把图标转成 **WebP**（AGP 的 `convertPngsToWebp`），`.png` 直接没有；
2. 自适应图标里 `ic_launcher_foreground.png` 往往是**体积最大的那张**，但它是透明前景层，
   单独显示就是空白/一块纯色。

**修复**：

- 收 `.png/.webp/.jpg/.jpeg`；
- 用打分选条目，而不是只看体积：
  `ic_launcher` 本体（1000）> `icon`（900）> `ic_launcher*`（700）> 名字含 icon/logo（500）
  > `res/mipmap*`（300）> `res/drawable*`（100），再按 `xxxhdpi > xxhdpi > …` 加密度分；
  显式排除 `foreground` / `background` / `monochrome` 层；
- `/icon` 按**文件魔数**给 `Content-Type`（PNG / WebP / JPEG），不再写死 `image/png`；
- 缓存文件不再带 `.png` 后缀（`icons/<包名>`），内容是什么格式就发什么格式。

### 2.4 负缓存永不过期

旧 `.miss` 只写包名、没有时间戳，命中一次就永远返回「没有图标」。
开机早期 `pm` 还没起来时扫一轮，之后就一直是首字母。

**修复**：`.miss` 写成 `包名\tUnix 时间戳`，TTL 30 分钟；旧格式（只有包名）视为过期，
自动重试一次；工具本身不可用时不写负缓存；`feather icon <包名> force` 可以强制重抠。

### 验收

```sh
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh doctor' | grep '^icon '
# icon tool=/data/adb/ksu/bin/busybox unzip cached=37 miss=2 dir=/data/adb/feather/icons
su -c 'sh /data/adb/modules/feather_tune/bin/ctl.sh icon com.tencent.mm'
# ok=/data/adb/feather/icons/com.tencent.mm
```

`tests/run_tests_wsl.sh` 里新增了 10 条用例：自造 APK（png + 更大的 webp + 更大的透明前景层）、
只有前景层时退回 `drawable/icon.png`、负缓存时间戳、`force` 重抠、`/icon` 的 Content-Type。

---

## 3. 充电功率控制无效

### 3.1 旧实现的限流链路在 PLK110 上是空转

真机回传（2026-11-11，一加 15 / PLK110）：

```
enabled=1 active=0 suspended=0 last_pct=-1
node=- orig=            ← 电流节点不存在
```

内核源码印证（`vendor/oplus/kernel/charger/v2/oplus_chg_gki.c`）：

- `battery_props[]` 里**没有** `CONSTANT_CHARGE_CURRENT_MAX`、`CURRENT_MAX`、
  `INPUT_SUSPEND`、`CHARGING_ENABLED`；`CHARGE_CONTROL_LIMIT` 在 6.1+ 被 `#if` 掉了；
- `battery_psy_prop_is_writeable()` 只把 `CURRENT_NOW` / `CHARGE_CONTROL_LIMIT` /
  `TIME_TO_FULL_NOW` 标成可写，其中 `CHARGE_CONTROL_LIMIT` 的 `set_property` 是空实现。

所以 v1.2.6 那套 `constant_charge_current_max` / `oplus_chg/battery/cool_down` 候选，
在 charger v2 机器上一条都命不中 —— 配置怎么改都不会有反应。

### 3.2 本机唯一能用的是 `battery/current_now`，而且是「息屏」语义

同一个文件里：

```c
case POWER_SUPPLY_PROP_CURRENT_NOW:                 // battery_psy_set_prop()
    if (!chip->smart_charging_screenoff) return -ENOTSUPP;
    val = pval->intval & 0xFFFF;                    // 单位 mA
    if (chip->led_on) {                             // led_on == 屏幕亮
        vote(wired_icl/vooc_curr/ufcs_curr/pps_curr, HIDL_VOTER, false, 0, …);  // 取消限流
    } else {
        vote(wired_icl, HIDL_VOTER, true, val, …);  // 生效
        vote(vooc_curr / ufcs_curr / pps_curr, HIDL_VOTER, true, val, …);
    }
```

- `led_on` 不是「充电灯」，是 **panel notifier** 给的「屏幕亮/灭」（`UNBLANK → true`）；
- `smart_charging_screenoff` 来自 `/soc/oplus_chg_core` 的 `oplus,smart_charging_screenoff`，
  一加 15 的 DT（`oplus-chg-24831.dtsi`）里**有这个属性**，所以接口是开的；
- 于是：**息屏写 = 限流；亮屏写 = 把限流撤掉**。
  旧实现每秒都写，亮屏时不但无效，还会撤销厂商 HAL 自己的限流票。

**修复**：

- 新增节点类型 `kind=2`（`battery/current_now`，单位 mA，息屏语义）；
- 只在**息屏**时接管；亮屏时主动写 `0` 释放，把控制权交还厂商 HAL，并在日志说明一次；
- 百分比 100% 也写 `0`（取消投票）。否则刚插上时的小电流（比如 2 A）会被当成「100% 上限」，
  充电器再也升不到 100 W —— 这是这套接口最容易踩的坑；
- 基准电流取「本段充电见过的最大电池电流」（高水位），而不是 `init` 那一刻的瞬时值；
- 真机 `current_now` 充电时是**负数**（如 `-8158 mA`），统一取绝对值；
- `charge_release()` 对 kind=2 写 0、对老节点写回原值。

### 3.3 曲线里藏了一个 `43:0`

`module/etc/config.ini` 里原本是：

```
charge_curve=36:100,38:88,40:78,42:66,44:55,50:50,43:0
```

解析规则是「按温度升序，取最后一个 `<= 当前温度` 的点」。末尾这个乱序的 `43:0`
会让 **43℃ 以上全部算成 0%**，再被下限钳成 100 mA —— 等于停充。

之前没暴露，只是因为节点根本不存在。一旦按 §3.2 修好节点，这个 bug 会立刻咬人。

**修复**：默认曲线删掉 `,43:0`；加载时清洗曲线（丢弃乱序 / 0% / 越界点并记日志）；
`customize.sh` 升级时顺带改写老配置里的这个尾巴。

### 3.4 「充电加速」在常温下本来就是空操作

真机 DT：

```
oplus,temp_range = <0 50 120 160 210 380 440>;   // 7 个点 = 6 段
```

按 `puc_get_temp_region()` 的分段（`[range[i-1], range[i])` → 第 i-1 段）：

| 段 | 温度 |
|---|---|
| little_cold | 0 ~ 4.9℃ |
| cool | 5.0 ~ 11.9℃ |
| little_cool | 12.0 ~ 15.9℃ |
| normal_low | 16.0 ~ 20.9℃ |
| **normal_high（最高电流）** | **21.0 ~ 37.9℃** |
| warm（整条曲线降流） | 38.0 ~ 43.9℃ |

也就是说：常温机身（约 29℃）**已经在 normal_high 段**，把伪装值写成 22.0℃ 不改变任何东西；
只有当机身被晒热 / 边充边玩进到 38℃ 以上时，这条伪装才会把曲线拉回最高电流档。
这是这台机器的分桶表决定的，不是代码 bug。UI 文案已改成「主要对发烫场景有效」。

### 3.5 仍然做不到的部分（明确写下，避免再被当成 bug）

| 需求 | 为什么做不到 | 可能的代价 |
|---|---|---|
| 亮屏时限流 | charger v2 没有给用户态的电流接口；`current_now` 亮屏即反向 | 无解 |
| 用 `oplus_chg/battery/cool_down` 限流 | v2 已经没有这个 class 节点 | 无解 |
| 走 `oplus_mms` 的 `overwrite` 改 `COMM_ITEM_COOL_DOWN` | 需要 `CONFIG_OPLUS_CHG_MMS_DEBUG` + `CONFIG_OPLUS_DEBUG_AUTH` 的证书 | 生产内核基本没开 |
| 伪装电池温度限流（`gauge_dbg_tbat`） | 可行，但会把过温保护一起骗掉 | 默认关闭（`accel_gauge=0`），只作为最后手段 |

### 验收

```sh
ctl=/data/adb/modules/feather_tune/bin/ctl.sh
su -c "sh $ctl charge probe" | grep -E 'current_now|input_suspend|constant_charge'
su -c "sh $ctl charge"
#   node=/sys/class/power_supply/battery/current_now orig=-8158
#   cur: kind=v2-current-now base=8158mA written=6363
#   cur: charger v2 的息屏智能充电接口：只有熄屏时才真正限流，亮屏会被内核反向取消
```

熄屏、充电中、SOC ≥ 67% 时，`/sys/class/power_supply/battery/current_now`
的**读数是真实电流、不会等于写入值**（它不是上限节点），要看效果请对比熄屏前后的
`dumpsys battery` 电流或实测功率。

---

## 4. 顺手修掉的两个测试环境坑

1. **`std::process::Command` 在 qemu-user 下子进程不 exec**。Rust 的 `Command` 走
   `posix_spawn`（`clone(CLONE_VM|CLONE_VFORK)`），qemu-aarch64-static 下子进程直接消失，
   表现是「arm64 回归里 dumpsys/pm 全部读不到」的假失败。改用 libc `popen`（fork+exec，
   也是 C 版一直用的）后，同一个 arm64 产物在 qemu 下 **84/84 全过**。
2. **PATH 里有空格/括号时 `sh` 语法错误**。`build_arm64` 的测试环境里 Windows PATH 会带
   `Program Files (x86)`，`PATH=...` 不加引号会让 dash 在 `(` 处报
   `Syntax error: "(" unexpected`。现在整段加单引号。

## 5. 变更清单

| 文件 | 变更 |
|---|---|
| `build.py` | LF 归一化 + 自检、`create_system=3`、module.prop 校验 |
| `module/customize.sh` | LF 重写；非破坏性解压；权限兜底；`43:0` 迁移；`exit 0` |
| `module/module.prop` | LF；版本 v1.4.1 / 10018 |
| `rust/cmd.rs` | 图标：工具实测探测、PATH、webp、打分、负缓存 TTL、force；doctor 增加 icon/charge 段；charge 增加 kind/base 输出 |
| `rust/net.rs` | `/icon` 按魔数给 Content-Type；`&r=1` 强制重抠 |
| `rust/charge.rs` | 节点 kind、v2 `current_now` 路径、息屏策略、释放写 0、高水位基准、曲线清洗 |
| `rust/cfg.rs` | 默认温度墙/回落值对齐；`charge_curve` 加载清洗 |
| `rust/state.rs` | `Node.kind`、`chg_base_ma`、`chg_v2_written`、`unzip_none` |
| `module/webroot/app.js` | `HTTP_BASE` 改为实测同源；`httpGetOn`；`<img>` 尺寸/异步解码 |
| `module/webroot/index.html` | 充电两行文案改成与本机能力一致 |
| `module/etc/config.ini` | 删除 `,43:0`；补 charger v2 / DT 分桶说明 |
| `tests/run_tests_wsl.sh` | 新增 11 条图标用例；假 `pm` |
