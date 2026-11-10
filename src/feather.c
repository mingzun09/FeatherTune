// ============================================================================
//  FeatherTune · 羽量调校 — single static daemon + cli  (v1.1.0)
//
//  Target : OnePlus 15 / SM8850 (2x Oryon Prime + 6x Oryon Performance)
//  Host   : Android 15~17 arm64 · KernelSU / Magisk / APatch
//
//  为什么把 shell 换成 C：
//   1. 1Hz 常驻，shell 每 tick 十几次 fork（手机上 2~4% CPU）；这里全程 0 fork
//   2. 线程亲和直接 sched_setaffinity()，不再调 taskset
//   3. WebUI 每次查询走 ctl.sh 要 ~100ms；现在走 UNIX socket ~1ms
//   4. 自带 127.0.0.1 HTTP 桥 —— 有些管理器（BakaSU / KSU-Next）
//      会禁掉 WebUI 的 ksu.exec，这时 WebUI 可以退回 fetch 本机端口
//
//  原则：写入先探测、失败即跳过；绝不抬高别人设的频率上限；绝不阻塞开机。
// ============================================================================
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <regex.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/file.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_VER   "v1.2.9"
#define APP_VC    10011
#define MAX_FREQ  64
#define MAX_POL   4
#define MAX_RULES 64
#define MAX_SEEN  2048
static int g_http_port = 7891;   // 可由 FEATHER_HTTP_PORT 覆盖（测试隔离端口）

// ------------------------------------------------------------------ paths --
static char P_DATA[PATH_MAX], P_CPU[PATH_MAX], P_CGRP[PATH_MAX], P_CPUCTL[PATH_MAX],
            P_SYS[PATH_MAX],
            P_PROC[PATH_MAX], P_HM[PATH_MAX], P_CONF[PATH_MAX], P_GAMES[PATH_MAX],
            P_RULES[PATH_MAX], P_APPS[PATH_MAX], P_LABELS[PATH_MAX], P_ICONS[PATH_MAX], P_STATE[PATH_MAX], P_LOG[PATH_MAX], P_PID[PATH_MAX],
            P_SOCK[PATH_MAX];
static char g_cset_node[16] = "cpuset.cpus";   // cpuset v1 用 cpus，v2 用 cpuset.cpus

static void path_init(void) {
    const char *e;
    snprintf(P_DATA, sizeof P_DATA, "%s", (e = getenv("FEATHER_DATA")) ? e : "/data/adb/feather");
    snprintf(P_CPU, sizeof P_CPU, "%s", (e = getenv("FEATHER_CPU_ROOT")) ? e : "/sys/devices/system/cpu");
    // cpuset 根：Android 13+ 多数机型在 /dev/cpuset（cgroup v1 语义，节点名 cpus），
    // 少数在 /sys/fs/cgroup。uclamp 则在 /dev/cpuctl（cpu 控制器）。三者不能混用。
    if ((e = getenv("FEATHER_CGROUP_ROOT"))) {
        snprintf(P_CGRP, sizeof P_CGRP, "%s", e);
    } else if (access("/dev/cpuset/top-app", F_OK) == 0) {
        snprintf(P_CGRP, sizeof P_CGRP, "%s", "/dev/cpuset");
    } else {
        snprintf(P_CGRP, sizeof P_CGRP, "%s", "/sys/fs/cgroup");
    }
    if ((e = getenv("FEATHER_CPUCTL_ROOT"))) {
        snprintf(P_CPUCTL, sizeof P_CPUCTL, "%s", e);
    } else if (access("/dev/cpuctl/top-app", F_OK) == 0) {
        snprintf(P_CPUCTL, sizeof P_CPUCTL, "%s", "/dev/cpuctl");
    } else {
        snprintf(P_CPUCTL, sizeof P_CPUCTL, "%s", P_CGRP);
    }
    {
        char probe[PATH_MAX];
        snprintf(probe, sizeof probe, "%s/background/cpus", P_CGRP);
        snprintf(g_cset_node, sizeof g_cset_node, "%s",
                 access(probe, R_OK) == 0 ? "cpus" : "cpuset.cpus");
    }
    snprintf(P_SYS, sizeof P_SYS, "%s", (e = getenv("FEATHER_SYS_ROOT")) ? e : "/sys");
    snprintf(P_PROC, sizeof P_PROC, "%s", (e = getenv("FEATHER_PROC_ROOT")) ? e : "/proc");
    snprintf(P_HM, sizeof P_HM, "%s", (e = getenv("FEATHER_HMBIRD_SYS")) ? e : "/proc/sys/hmbird_II");
    snprintf(P_CONF, sizeof P_CONF, "%s/config.ini", P_DATA);
    snprintf(P_GAMES, sizeof P_GAMES, "%s/games.conf", P_DATA);
    snprintf(P_RULES, sizeof P_RULES, "%s/rules.conf", P_DATA);
    snprintf(P_APPS, sizeof P_APPS, "%s/apps.conf", P_DATA);
    snprintf(P_LABELS, sizeof P_LABELS, "%s/labels.conf", P_DATA);
    snprintf(P_ICONS, sizeof P_ICONS, "%s/icons", P_DATA);
    snprintf(P_STATE, sizeof P_STATE, "%s/state", P_DATA);
    snprintf(P_LOG, sizeof P_LOG, "%s/feather.log", P_DATA);
    snprintf(P_PID, sizeof P_PID, "%s/featherd-daemon.pid", P_DATA);
    snprintf(P_SOCK, sizeof P_SOCK, "%s/control.sock", P_DATA);
}

static int  read_all(const char *path, char *buf, size_t n);
static int  rdline(const char *path, char *buf, size_t n);

// -------------------------------------------------------------------- log --
static int g_log_level = 1;

static void logmsg(const char *fmt, ...) {
    char ts[32], buf[1024];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%m-%d %H:%M:%S", &tm);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    FILE *f = fopen(P_LOG, "a");
    if (!f) return;
    fprintf(f, "%s %s\n", ts, buf);
    fclose(f);
}

static void logv(const char *fmt, ...) {
    if (g_log_level < 2) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logmsg("%s", buf);
}

static void log_rotate(void) {
    struct stat st;
    if (stat(P_LOG, &st) == 0 && st.st_size > 256 * 1024) {
        char bak[PATH_MAX];
        snprintf(bak, sizeof bak, "%s.1", P_LOG);
        rename(P_LOG, bak);
    }
}

// --------------------------------------------------------------------- io --
static int rdline(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) return -1;
    out[r] = 0;
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    char *s = out;
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
    size_t len = strlen(out);
    while (len && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '\r')) out[--len] = 0;
    return 0;
}

static long rdnum(const char *path) {
    char b[64];
    if (rdline(path, b, sizeof b) != 0 || !b[0]) return -1;
    char *end = NULL;
    long v = strtol(b, &end, 10);
    if (end == b) return -1;
    return v;
}

static int wrp(const char *path, const char *value) {
    if (access(path, F_OK) != 0) return -1;
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = strlen(value);
    ssize_t w = write(fd, value, n);
    close(fd);
    if (w != (ssize_t)n) return -1;
    const char *tr = getenv("FEATHER_WR_TRACE");
    if (tr && *tr) {
        FILE *f = fopen(tr, "a");
        if (f) { fprintf(f, "%s <- %s\n", path, value); fclose(f); }
    }
    return 0;
}

static int wrl(const char *path, long v) {
    char b[32];
    snprintf(b, sizeof b, "%ld", v);
    return wrp(path, b);
}

static int wrverify(const char *path, const char *value) {
    if (wrp(path, value) != 0) return -1;
    char b[128];
    if (rdline(path, b, sizeof b) != 0) return -1;
    return strstr(b, value) ? 0 : -1;
}

// ---------------------------------------------------------------- topology --
typedef struct {
    char path[PATH_MAX];
    long max, min;
    long freq[MAX_FREQ];
    int nfreq;
    bool is_prime;
} policy_t;

static policy_t pol[MAX_POL];
static int npol, idx_perf = -1, idx_prime = -1;

static int cmp_long_desc(const void *a, const void *b) {
    long x = *(const long *)a, y = *(const long *)b;
    return (x < y) - (x > y);
}

static void detect_topology(void) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/cpufreq", P_CPU);
    npol = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && npol < MAX_POL) {
        if (strncmp(de->d_name, "policy", 6) != 0) continue;
        policy_t *p = &pol[npol];
        memset(p, 0, sizeof *p);
        snprintf(p->path, sizeof p->path, "%s/%s", dir, de->d_name);
        char f[PATH_MAX];
        snprintf(f, sizeof f, "%s/cpuinfo_max_freq", p->path);
        p->max = rdnum(f);
        if (p->max <= 0) continue;
        snprintf(f, sizeof f, "%s/cpuinfo_min_freq", p->path);
        p->min = rdnum(f);
        snprintf(f, sizeof f, "%s/scaling_available_frequencies", p->path);
        char buf[2048];
        if (rdline(f, buf, sizeof buf) == 0) {
            char *save = NULL;
            for (char *t = strtok_r(buf, " \t", &save); t && p->nfreq < MAX_FREQ;
                 t = strtok_r(NULL, " \t", &save)) {
                long v = strtol(t, NULL, 10);
                if (v > 0) p->freq[p->nfreq++] = v;
            }
        }
        if (p->nfreq == 0) { p->freq[0] = p->max; p->nfreq = 1; }
        qsort(p->freq, p->nfreq, sizeof(long), cmp_long_desc);
        npol++;
    }
    closedir(d);

    idx_prime = idx_perf = -1;
    for (int i = 0; i < npol; i++)
        if (idx_prime < 0 || pol[i].max > pol[idx_prime].max) idx_prime = i;
    for (int i = 0; i < npol; i++)
        if (i != idx_prime && (idx_perf < 0 || pol[i].max > pol[idx_perf].max)) idx_perf = i;
    if (idx_prime >= 0) pol[idx_prime].is_prime = true;
    if (idx_perf < 0) idx_perf = idx_prime;
}

// 动手之前的天花板/地板，只快照一次（守护进程启动时）。
// 注意：detect_topology() 每次都会 memset pol[]，所以基线必须单独存。
static long g_base_max[MAX_POL], g_base_min[MAX_POL];
static int  g_base_ok;

static void capture_syscaps(void) {
    if (g_base_ok) return;                 // 只认第一次，后面谁改都不影响基线
    for (int i = 0; i < npol; i++) {
        char f[PATH_MAX];
        snprintf(f, sizeof f, "%s/scaling_max_freq", pol[i].path);
        g_base_max[i] = rdnum(f);
        snprintf(f, sizeof f, "%s/scaling_min_freq", pol[i].path);
        g_base_min[i] = rdnum(f);
    }
    g_base_ok = npol > 0;
    logv("baseline: n=%d max0=%ld min0=%ld", npol, g_base_max[0], g_base_min[0]);
}

static long freq_le(policy_t *p, long target) {
    long best = 0;
    for (int i = 0; i < p->nfreq; i++)
        if (p->freq[i] <= target && p->freq[i] > best) best = p->freq[i];
    if (best == 0) best = target;
    if (p->min > 0 && best < p->min) best = p->min;
    return best;
}

typedef struct {
    int enable, intensity, auto_game, screen_off_saver, thread_opt;
    int kill_boost, io_tune, gpu_tune, launch_boost, boost_ms, log_level;
    int hmbird_tune, hmbird_reserved, hmbird_frame;
    int game_min_pct, game_bg_cpus;
    char mode[16];
    char hm_coeff[48], hm_high[48], hm_heavy[64];
    int  charge_ctrl, charge_knee_soc, charge_temp_wall, charge_resume_temp;
    char game_mode[16];     // official / light / aggressive
    char chg_curve[128];
} cfg_t;

static cfg_t C;

// 取不低于目标的**最低**档（用于抬底频）
static long freq_ge(policy_t *p, long target) {
    long best = 0;
    for (int i = 0; i < p->nfreq; i++)
        if (p->freq[i] >= target && (best == 0 || p->freq[i] < best)) best = p->freq[i];
    return best ? best : p->max;
}

static int g_refresh;          // Hz，0=未知

static void detect_refresh(void) {
    char b[4096];
    static const char *cands[] = {"/class/graphics/fb0/modes", "/class/graphics/fb0/mode"};
    for (size_t i = 0; i < sizeof cands / sizeof cands[0] && !g_refresh; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s%s", P_SYS, cands[i]);
        if (rdline(path, b, sizeof b) != 0) continue;
        // "U:1080x2354p-165" / "1080x2354-165" 都取最后一段数字
        char *dash = strrchr(b, '-');
        if (!dash) continue;
        int v = atoi(dash + 1);
        if (v >= 30 && v <= 240) g_refresh = v;
    }
    // Android 上 fb0/modes 多数不可读，真正可靠的是 DRM 的模式表：
    //   1272x2772x165cmd   1272x2772x120cmd   1272x2772x60cmd
    // 取其中最大的刷新率（游戏档要的是它能跑到多少）
    if (!g_refresh) {
        static const char *drm[] = {
            "/class/drm/card0-DSI-1/modes", "/class/drm/card0-DSI-1/mode",
            "/class/drm/card0/modes",
        };
        for (size_t i = 0; i < sizeof drm / sizeof drm[0] && !g_refresh; i++) {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s%s", P_SYS, drm[i]);
            if (read_all(path, b, sizeof b) < 0) continue;
            int best = 0;
            char *line = b;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                // 形如 1272x2772x165cmd：取第二个 'x' 之后的数字
                char *x1 = strchr(line, 'x');
                if (x1) {
                    char *x2 = strchr(x1 + 1, 'x');
                    if (x2) {
                        int v = atoi(x2 + 1);
                        if (v >= 30 && v <= 240 && v > best) best = v;
                    }
                }
                line = nl ? nl + 1 : NULL;
            }
            if (best) g_refresh = best;
        }
    }
}

static char g_bg_cpus_orig[64];

static void bg_cpus_path(char *p, size_t n) {
    snprintf(p, n, "%s/background/%s", P_CGRP, g_cset_node);
}
static int g_game_extras;

static void set_policy_min(int idx, long f) {
    if (idx < 0) return;
    char path[PATH_MAX], v[32];
    snprintf(path, sizeof path, "%s/scaling_min_freq", pol[idx].path);
    snprintf(v, sizeof v, "%ld", f);
    wrp(path, v);
}

static void apply_game_extras(void) {
    if (g_game_extras) return;
    // 1) 给性能簇一个地板，掐掉 384/537MHz 这种深谷
    if (C.game_min_pct > 0 && idx_perf >= 0) {
        long t = pol[idx_perf].max / 100 * C.game_min_pct;
        set_policy_min(idx_perf, freq_ge(&pol[idx_perf], t));
    }
    // 2) 后台 cgroup 赶出超大核
    if (C.game_bg_cpus) {
        char p[PATH_MAX], cur[64];
        bg_cpus_path(p, sizeof p);
        if (rdline(p, cur, sizeof cur) == 0) {
            if (!g_bg_cpus_orig[0]) snprintf(g_bg_cpus_orig, sizeof g_bg_cpus_orig, "%s", cur);
            if (strcmp(cur, "0-3")) wrp(p, "0-3");
        }
    }
    g_game_extras = 1;
    logv("game extras: min_perf=%d%% bg_cpus=0-3", C.game_min_pct);
}

static void restore_game_extras(void) {
    if (!g_game_extras) { logv("extras: 已是干净状态，跳过还原"); return; }
    if (idx_perf >= 0)
        set_policy_min(idx_perf, (g_base_ok && g_base_min[idx_perf] > 0)
                                 ? g_base_min[idx_perf] : pol[idx_perf].min);
    if (C.game_bg_cpus && g_bg_cpus_orig[0]) {
        char p[PATH_MAX];
        bg_cpus_path(p, sizeof p);
        wrp(p, g_bg_cpus_orig);
    }
    g_game_extras = 0;
    logv("extras: 已还原 min_perf=%ld", idx_perf >= 0 ? g_base_min[idx_perf] : -1);
}

static long policy_cap_now(int idx) {
    if (idx < 0) return 0;
    char f[PATH_MAX];
    snprintf(f, sizeof f, "%s/scaling_max_freq", pol[idx].path);
    long v = rdnum(f);
    return v > 0 ? v : 0;
}

// 设上限；返回实际生效频率（0=失败）
static long set_cap(int idx, int pct) {
    if (idx < 0) return 0;
    policy_t *p = &pol[idx];
    if (pct > 100) pct = 100;
    if (pct < 5) pct = 5;
    long t = p->max / 100 * pct;
    int bi = (int)(p - pol);
    if (g_base_ok && bi >= 0 && bi < MAX_POL && g_base_max[bi] > 0 && t > g_base_max[bi])
        t = g_base_max[bi];                              // 绝不抬到基线之上
    long f = freq_le(p, t);
    char path[PATH_MAX], v[32];
    snprintf(path, sizeof path, "%s/scaling_max_freq", p->path);
    snprintf(v, sizeof v, "%ld", f);
    return wrp(path, v) == 0 ? f : 0;
}

static const char *gov_of(int idx) {
    static char b[64];
    if (idx < 0) return "";
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/scaling_governor", pol[idx].path);
    return rdline(path, b, sizeof b) == 0 ? b : "";
}

// ------------------------------------------------------------------ config --
static struct timespec g_conf_mtime;

static void cfg_defaults(void) {
    memset(&C, 0, sizeof C);
    C.enable = 1; C.intensity = 50; C.auto_game = 1; C.screen_off_saver = 1;
    C.thread_opt = 1; C.kill_boost = 1; C.io_tune = 1; C.gpu_tune = 1;
    C.launch_boost = 1; C.boost_ms = 4000; C.log_level = 1;
    C.hmbird_tune = 0; C.hmbird_reserved = 192; C.hmbird_frame = 0;
    C.game_min_pct = 42; C.game_bg_cpus = 1;
    snprintf(C.mode, sizeof C.mode, "auto");
    snprintf(C.hm_coeff, sizeof C.hm_coeff, "1152,1024");
    snprintf(C.hm_high, sizeof C.hm_high, "56,60");
    snprintf(C.hm_heavy, sizeof C.hm_heavy, "0,360,128");
    snprintf(C.game_mode, sizeof C.game_mode, "official");
    C.charge_ctrl = 1; C.charge_knee_soc = 67; C.charge_temp_wall = 50;
    C.charge_resume_temp = 45;
    snprintf(C.chg_curve, sizeof C.chg_curve, "36:100,38:88,40:78,42:66,44:55,50:50");
}

static int atoi_safe(const char *s, int def) {
    if (!s || !*s) return def;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    return end == s ? def : (int)v;
}

static int timespec_eq(struct timespec a, struct timespec b) {
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

static bool cfg_load(void) {   // 返回 true 表示内容有更新
    struct stat st;
    struct timespec mt = {0, 0};
    if (stat(P_CONF, &st) == 0) {
        mt.tv_sec = st.st_mtime;
        mt.tv_nsec = st.st_mtim.tv_nsec;
        if (timespec_eq(mt, g_conf_mtime) && g_conf_mtime.tv_sec != 0) return false;
    }
    g_conf_mtime = mt;

    cfg_defaults();
    FILE *f = fopen(P_CONF, "r");
    if (!f) { g_log_level = C.log_level; return true; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';' || !*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = s, *val = eq + 1;
        char *kc = key + strlen(key);
        while (kc > key && (kc[-1] == ' ' || kc[-1] == '\t')) *--kc = 0;
        while (*val == ' ' || *val == '\t') val++;
        char *vc = val + strlen(val);
        while (vc > val && (vc[-1] == '\n' || vc[-1] == '\r' || vc[-1] == ' ' || vc[-1] == '\t')) *--vc = 0;
        char *hash = strchr(val, '#');
        if (hash) {
            *hash = 0;
            char *q = hash;
            while (q > val && (q[-1] == ' ' || q[-1] == '\t')) *--q = 0;
        }
        if (!strcmp(key, "enable")) C.enable = atoi_safe(val, 1);
        else if (!strcmp(key, "mode")) snprintf(C.mode, sizeof C.mode, "%s", val);
        else if (!strcmp(key, "intensity")) C.intensity = atoi_safe(val, 50);
        else if (!strcmp(key, "auto_game")) C.auto_game = atoi_safe(val, 1);
        else if (!strcmp(key, "screen_off_saver")) C.screen_off_saver = atoi_safe(val, 1);
        else if (!strcmp(key, "thread_opt")) C.thread_opt = atoi_safe(val, 1);
        else if (!strcmp(key, "kill_boost")) C.kill_boost = atoi_safe(val, 1);
        else if (!strcmp(key, "io_tune")) C.io_tune = atoi_safe(val, 1);
        else if (!strcmp(key, "gpu_tune")) C.gpu_tune = atoi_safe(val, 1);
        else if (!strcmp(key, "launch_boost")) C.launch_boost = atoi_safe(val, 1);
        else if (!strcmp(key, "boost_ms")) C.boost_ms = atoi_safe(val, 4000);
        else if (!strcmp(key, "log_level")) C.log_level = atoi_safe(val, 1);
        else if (!strcmp(key, "hmbird_tune")) C.hmbird_tune = atoi_safe(val, 0);
        else if (!strcmp(key, "hmbird_frame_per_sec")) C.hmbird_frame = atoi_safe(val, 0);
        else if (!strcmp(key, "game_min_pct")) C.game_min_pct = atoi_safe(val, 42);
        else if (!strcmp(key, "game_bg_cpus")) C.game_bg_cpus = atoi_safe(val, 1);
        else if (!strcmp(key, "hmbird_reserved")) C.hmbird_reserved = atoi_safe(val, 192);
        else if (!strcmp(key, "hmbird_coeff")) snprintf(C.hm_coeff, sizeof C.hm_coeff, "%s", val);
        else if (!strcmp(key, "hmbird_high_ratio")) snprintf(C.hm_high, sizeof C.hm_high, "%s", val);
        else if (!strcmp(key, "hmbird_heavy")) snprintf(C.hm_heavy, sizeof C.hm_heavy, "%s", val);
        else if (!strcmp(key, "game_mode")) snprintf(C.game_mode, sizeof C.game_mode, "%s", val);
        else if (!strcmp(key, "charge_ctrl")) C.charge_ctrl = atoi_safe(val, 1);
        else if (!strcmp(key, "charge_knee_soc")) C.charge_knee_soc = atoi_safe(val, 67);
        else if (!strcmp(key, "charge_temp_wall")) C.charge_temp_wall = atoi_safe(val, 43);
        else if (!strcmp(key, "charge_resume_temp")) C.charge_resume_temp = atoi_safe(val, 40);
        else if (!strcmp(key, "charge_curve")) snprintf(C.chg_curve, sizeof C.chg_curve, "%s", val);
    }
    fclose(f);
    g_log_level = C.log_level;
    return true;
}

static int cfg_set(const char *key, const char *val) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/.cfg.tmp", P_DATA);
    FILE *in = fopen(P_CONF, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return -1; }
    bool done = false;
    if (in) {
        char line[512];
        while (fgets(line, sizeof line, in)) {
            char copy[512];
            snprintf(copy, sizeof copy, "%s", line);
            char *s = copy;
            while (*s == ' ' || *s == '\t') s++;
            char *eq = strchr(s, '=');
            bool match = false;
            if (*s != '#' && *s != ';' && eq) {
                char k[128];
                size_t n = (size_t)(eq - s);
                if (n < sizeof k) { memcpy(k, s, n); k[n] = 0; match = !strcmp(k, key); }
            }
            if (match) { fprintf(out, "%s=%s\n", key, val); done = true; }
            else fputs(line, out);
        }
        fclose(in);
    }
    if (!done) fprintf(out, "%s=%s\n", key, val);
    fclose(out);
    rename(tmp, P_CONF);
    g_conf_mtime.tv_sec = 0;
    g_conf_mtime.tv_nsec = 0;
    cfg_load();
    return 0;
}

// ---------------------------------------------------------------- profiles --
typedef struct { const char *name; int perf, prime; } proto_t;
static const proto_t PROTOS[] = {
    {"sleep", 30, 25}, {"battery", 50, 45}, {"daily", 72, 62},
    {"game", 100, 100}, {"perf", 100, 100},
};

static void profile_pct(const char *name, int *perf, int *prime) {
    int pp = 72, pr = 62;
    for (size_t i = 0; i < sizeof PROTOS / sizeof PROTOS[0]; i++)
        if (!strcmp(PROTOS[i].name, name)) { pp = PROTOS[i].perf; pr = PROTOS[i].prime; break; }
    if (strcmp(name, "game") && strcmp(name, "perf")) {
        int iv = C.intensity;
        if (iv < 0) iv = 0;
        if (iv > 100) iv = 100;
        int delta = (50 - iv) * 2 / 5;
        pp += delta;
        pr += delta;
    }
    if (pp > 100) pp = 100;
    if (pp < 15) pp = 15;
    if (pr > 100) pr = 100;
    if (pr < 15) pr = 15;
    *perf = pp;
    *prime = pr;
}

static void profile_uclamp(const char *name, int *bg, int *fg, int *top) {
    if (!strcmp(name, "sleep"))        { *bg = 20; *fg = 25; *top = 30; }
    else if (!strcmp(name, "battery")) { *bg = 25; *fg = 40; *top = 60; }
    else if (!strcmp(name, "game"))    { *bg = 25; *fg = 45; *top = 100; }
    else if (!strcmp(name, "perf"))    { *bg = 40; *fg = 70; *top = 100; }
    else                                { *bg = 30; *fg = 55; *top = 100; }
}

static int g_uclamp_ok = 1;      // 0=已确认不兼容/不可写，本次运行不再尝试
static int g_uclamp_touched;

// uclamp 在不同内核上有两种读法：整数 0-1024，或百分数字符串 "30.00"。
// 这里写入后读回校验，对不上就整体停用并在日志/doctor 里说明，不去盲猜格式。
static void set_uclamp(const char *group, int maxpct) {
    if (!g_uclamp_ok) return;
    char path[PATH_MAX], v[32], back[64];
    snprintf(path, sizeof path, "%s/%s/cpu.uclamp.max", P_CPUCTL, group);
    if (access(path, W_OK) != 0) {
        g_uclamp_ok = 0;
        logv("uclamp: %s 不可写，停用", path);
        return;
    }
    long val = (long)maxpct * 1024 / 100;
    snprintf(v, sizeof v, "%ld", val);
    if (wrp(path, v) != 0) { g_uclamp_ok = 0; logv("uclamp: 写入失败，停用"); return; }
    if (rdline(path, back, sizeof back) == 0 && back[0]) {
        double r = atof(back);
        if (r > 200.0) r = r / 1024.0 * 100.0;      // 读回 307 → 30%
        if (maxpct >= 5 && r < 2.0) {
            g_uclamp_ok = 0;
            logv("uclamp: 读回 '%s' 与写入 %ld 不符，格式不兼容，停用", back, val);
            return;
        }
    }
    g_uclamp_touched = 1;
}

static void uclamp_restore(void) {
    if (!g_uclamp_touched) return;
    static const char *groups[] = {"top-app", "foreground", "background"};
    for (size_t i = 0; i < sizeof groups / sizeof groups[0]; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s/cpu.uclamp.max", P_CPUCTL, groups[i]);
        wrp(path, "max");
    }
    g_uclamp_touched = 0;
}

static long battery_cap(void);
static long battery_temp(void);
static void battery_status(char *out, size_t n);

/* ------------------------------------------------------------ 充电控制 --
 * 方案 A：只做「少要一点电流」，不碰温度上报、不碰 DT。
 *
 *   SOC <  charge_knee_soc   → 不干预（厂商曲线自己跑，这段电池本身不热）
 *   SOC >= charge_knee_soc   → 按电池温度查 charge_curve 降电流
 *   温度 >= charge_temp_wall → 直接暂停充电，降到 charge_resume_temp 再恢复
 *
 * 作用点是充电 IC 的输入/恒流上限，SVOOC / UFCS / PPS 都要经过它，
 * 所以三种协议同时生效。只能降不能升——这是方案 A 的边界。
 * 所有写过的节点都在退出时还原成动手前的值。
 */
typedef struct {
    char path[PATH_MAX];
    char orig[64];
    int  valid;
} chg_node_t;

static chg_node_t g_chg_cur;        // 恒流/输入电流上限
static chg_node_t g_chg_suspend;    // 暂停充电开关
static chg_node_t g_chg_cd;         // oplus_chg 的 cool_down 等级（0=不限，越大越限）
static int  g_chg_ready;            // 节点已探测
static int  g_chg_active;           // 当前正被我们接管
static int  g_chg_suspended;
static int  g_chg_last_pct = -1;    // 上次设置的百分比
static int  g_chg_gear;             // 0=满档，1/2=降档，3=暂停
static long g_chg_orig_cur;         // 原始电流值（用于按比例算）

// 候选节点：不同 ColorOS 版本开放的不一样，按顺序挑第一个可写的
static const char *CHG_CUR_CANDS[] = {
    "/class/power_supply/battery/constant_charge_current_max",
    "/class/power_supply/battery/constant_charge_current",
    "/class/power_supply/battery/current_max",
    "/class/power_supply/battery/input_current_limit",
    "/class/oplus_chg/battery/current_max",
    "/class/oplus_chg/battery/chg_current_max",
    "/class/oplus_chg/battery/fastchg_current_max",
};
static const char *CHG_SUSPEND_CANDS[] = {
    "/class/power_supply/battery/input_suspend",
    "/class/power_supply/battery/charge_disable",
    "/class/power_supply/battery/charging_enabled",
    "/class/oplus_chg/battery/input_suspend",
};

static int chg_pick(chg_node_t *n, const char **cands, size_t cnt) {
    memset(n, 0, sizeof *n);
    for (size_t i = 0; i < cnt; i++) {
        char p[PATH_MAX], v[64];
        snprintf(p, sizeof p, "%s%s", P_SYS, cands[i]);
        if (access(p, W_OK) != 0) continue;
        if (rdline(p, v, sizeof v) != 0) continue;
        snprintf(n->path, sizeof n->path, "%s", p);
        snprintf(n->orig, sizeof n->orig, "%s", v);
        n->valid = 1;
        return 0;
    }
    return -1;
}

static void charge_init(void) {
    if (g_chg_ready) return;
    g_chg_ready = 1;
    if (chg_pick(&g_chg_cur, CHG_CUR_CANDS,
                 sizeof CHG_CUR_CANDS / sizeof CHG_CUR_CANDS[0]) != 0) {
        logv("charge: 没有可写的电流节点，充电控制停用");
        return;
    }
    // 暂停开关不是必须的：没有就只靠电流兜底（设成最小值而不是 0）
    chg_pick(&g_chg_suspend, CHG_SUSPEND_CANDS,
             sizeof CHG_SUSPEND_CANDS / sizeof CHG_SUSPEND_CANDS[0]);
    // ColorOS 上标准电流节点常常整个不存在（真机实测：constant_charge_current* 全没有），
    // 厂商真正的入口是 oplus_chg 的 cool_down 等级。这里只探测+显示，写不写由后续实测决定。
    chg_pick(&g_chg_cd, (const char *[]){
        "/class/oplus_chg/battery/cool_down",
        "/class/oplus_chg/battery/normal_cool_down",
    }, 2);
    g_chg_orig_cur = strtol(g_chg_cur.orig, NULL, 10);
    logv("charge: 电流节点 %s (%s) 暂停节点 %s cool_down %s",
         g_chg_cur.valid ? g_chg_cur.path : "无", g_chg_cur.orig,
         g_chg_suspend.valid ? g_chg_suspend.path : "无",
         g_chg_cd.valid ? g_chg_cd.path : "无");
}

/* 温度阶梯（相对温度墙）：
 *   墙-5 以下 → 回到满档
 *   ≥ 墙       → 降一档（默认 75%，约 120W→90W）
 *   ≥ 墙+2     → 再降一档（默认 50%，约 60W）
 *   ≥ 墙+4     → 暂停（保护）
 * 档位只朝一个方向走到阈值，避免在边界反复横跳。
 */
static int gear_pct(int gear) {
    if (gear <= 0) return 100;
    if (gear == 1) return 75;
    if (gear == 2) return 50;
    return 0;                       // 3 = 暂停
}

static int gear_for_temp(long t) {
    int wall = C.charge_temp_wall;
    if (t >= wall + 4) return 3;
    if (t >= wall + 2 && g_chg_gear < 2) return 2;
    if (t >= wall && g_chg_gear < 1) return 1;
    if (t <= wall - 5) return 0;
    return g_chg_gear;              // 维持当前档
}

// "36:100,38:88,40:78,42:66" —— 按温度升序，取最后一个 <= 当前温度的点
static int charge_pct_for_temp(long temp) {
    int pct = 100;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", C.chg_curve);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int t = 0, p = 100;
        if (sscanf(tok, "%d:%d", &t, &p) != 2) continue;
        if (temp >= t) pct = p;      // 温度越高，后面覆盖前面的
    }
    return pct;
}

static void charge_set_pct(int pct) {
    if (pct == g_chg_last_pct) return;
    g_chg_last_pct = pct;
    if (!g_chg_cur.valid) return;
    long base = g_chg_orig_cur > 0 ? g_chg_orig_cur : 3000000;
    long v = base / 100 * pct;
    if (v < 100000) v = 100000;      // 别设成 0，0 在很多机型上等于"不限"
    char b[32];
    snprintf(b, sizeof b, "%ld", v);
    wrp(g_chg_cur.path, b);
}

static void charge_suspend(int on) {
    if (g_chg_suspended == on) return;
    g_chg_suspended = on;
    if (!g_chg_suspend.valid) return;
    int invert = strstr(g_chg_suspend.path, "charging_enabled") != NULL;
    wrp(g_chg_suspend.path, invert ? (on ? "0" : "1") : (on ? "1" : "0"));
}

// 每个 tick 调一次（1Hz）；不充电时什么都不做
static void charge_tick(void) {
    if (!C.charge_ctrl) {
        if (g_chg_active) { charge_suspend(0); charge_set_pct(100); g_chg_active = 0; }
        return;
    }
    charge_init();
    if (!g_chg_cur.valid) return;

    char st[32] = "";
    battery_status(st, sizeof st);
    int charging = strstr(st, "Charging") != NULL;
    if (!charging) {
        if (g_chg_active) {
            charge_suspend(0);
            charge_set_pct(100);
            g_chg_active = 0;
            logv("charge: 已停止充电，还原电流 %s", g_chg_cur.orig);
        }
        return;
    }

    long soc = battery_cap();
    long temp = battery_temp();
    if (soc <= 0) return;
    g_chg_active = 1;

    // 1) 温度阶梯：到墙才降档，不是一碰就停
    int g = gear_for_temp(temp);
    if (g != g_chg_gear) {
        int old = g_chg_gear;
        g_chg_gear = g;
        if (g == 3)
            logmsg("charge: %.0f℃ 超过安全线 %d℃，暂停充电", (double)temp, C.charge_temp_wall + 4);
        else if (g == 0)
            logmsg("charge: %.0f℃ 回落到 %d℃ 以下，恢复满档", (double)temp, C.charge_temp_wall - 5);
        else
            logmsg("charge: %.0f℃ 到温度墙 %d℃，降档 %d%% → %d%%（约 %dW）",
                   (double)temp, C.charge_temp_wall, gear_pct(old), gear_pct(g),
                   120 * gear_pct(g) / 100);
    }
    if (g_chg_gear == 3) {
        charge_suspend(1);
        charge_set_pct(0);
        return;
    }
    if (g_chg_suspended) {
        logmsg("charge: 恢复充电（档位 %d%%）", gear_pct(g_chg_gear));
        charge_suspend(0);
    }

    // 2) 电量区间：拐点以下满功率（除非温度档位要求更低）
    int pct = 100;
    if (soc >= C.charge_knee_soc) pct = charge_pct_for_temp(temp);
    int cap = gear_pct(g_chg_gear);
    if (cap < pct) pct = cap;

    if (pct != g_chg_last_pct)
        logmsg("charge: SOC %ld%% %.0f℃ → 电流上限 %d%%（拐点 %d%%，温度档 %d%%）",
               soc, (double)temp, pct, C.charge_knee_soc, cap);
    charge_set_pct(pct);
}

static void charge_restore(void) {
    if (g_chg_suspend.valid && g_chg_suspended) {
        int invert = strstr(g_chg_suspend.path, "charging_enabled") != NULL;
        wrp(g_chg_suspend.path, invert ? "1" : "0");
        g_chg_suspended = 0;
    }
    if (g_chg_cur.valid) {
        wrp(g_chg_cur.path, g_chg_cur.orig);
        logv("charge: 已还原电流 %s", g_chg_cur.orig);
    }
    g_chg_active = 0;
    g_chg_last_pct = -1;
    g_chg_gear = 0;
}

// ------------------------------------------------------------------ hmbird --
// 判断 pid 是否真的活着：僵尸（Z）虽然 kill(pid,0) 成功，但其实已经死了
static int pid_alive(long pid) {
    if (pid <= 0) return 0;
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return kill((pid_t)pid, 0) == 0 ? 1 : 0;
    int alive = 0;
    if (fgets(buf, sizeof buf, f)) {
        char *rp = strrchr(buf, ')');        // comm 里可能有空格，用最后一个 ')' 定位
        if (rp && rp[1] == ' ') {
            char st = rp[2];
            alive = (st != 'Z' && st != 'X');
        }
    }
    fclose(f);
    return alive;
}

static int g_hb_present, g_hb_tuned;

static void hmbird_detect(void) {
    struct stat st;
    g_hb_present = (stat(P_HM, &st) == 0 && S_ISDIR(st.st_mode));
    if (!g_hb_present) {
        const char *g = gov_of(idx_prime);
        if (strstr(g, "hmbird") || strstr(g, "scx")) g_hb_present = 1;
    }
}

// hmbird 的 show 回调把所有已设置的条目打成 "<leader>::<comm>::<hex>"，
// 拿原文比对必然误报，只能按「值是否出现」判断，测完写 0 清掉自己的条目。
static int hm_selftest(const char *node, const char *hexval, long me) {
    char p[PATH_MAX], v[64], b[2048], want[32];
    snprintf(p, sizeof p, "%s/%s", P_HM, node);
    snprintf(v, sizeof v, "%ld %s", me, hexval);
    if (wrp(p, v) != 0) return 0;
    unsigned long hv = strtoul(hexval, NULL, 16);
    const char *fmt = strcmp(node, "prefer_preempt") == 0 ? "::%lu" : "::%lx";
    snprintf(want, sizeof want, fmt, strcmp(node, "prefer_preempt") == 0 ? hv : hv);
    int ok = 0;
    if (read_all(p, b, sizeof b) >= 0 && strstr(b, want)) ok = 1;
    snprintf(v, sizeof v, "%ld 0", me);      // 清理自己写入的条目
    wrp(p, v);
    return ok;
}

static int hmbird_live(void) {
    for (int i = 0; i < npol; i++) {
        const char *g = gov_of(i);
        if (strstr(g, "hmbird") || strstr(g, "scx")) return 1;
    }
    return 0;
}

static int g_pm_errno;      // 最近一次 prefer_* 写入的 errno（0=成功）

static void hm_write(const char *node, const char *val) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", P_HM, node);
    wrp(path, val);
}

// 只写帧预算，不碰系数/地板
/* hmbird 的每个参数在写之前都把原值存下来，退出/切模式时按原值还原。
 * 之前是还原成硬编码的猜测值（1024,1024 / 0 400 140），等于把厂商调好的
 * 协同值直接覆盖掉——这是错的。 */
#define HM_SAVE_MAX 8
typedef struct { char node[32]; char val[128]; } hmsave_t;
static hmsave_t g_hb_save[HM_SAVE_MAX];
static int g_hb_nsave;

static void hm_save_once(const char *node) {
    for (int i = 0; i < g_hb_nsave; i++)
        if (!strcmp(g_hb_save[i].node, node)) return;
    if (g_hb_nsave >= HM_SAVE_MAX) return;
    char p[PATH_MAX], v[128];
    snprintf(p, sizeof p, "%s/%s", P_HM, node);
    if (rdline(p, v, sizeof v) != 0) return;
    snprintf(g_hb_save[g_hb_nsave].node, sizeof g_hb_save[0].node, "%s", node);
    snprintf(g_hb_save[g_hb_nsave].val, sizeof g_hb_save[0].val, "%s", v);
    g_hb_nsave++;
}

// 写 hmbird 参数（带原值保存）
static void hm_write_keep(const char *node, const char *val) {
    hm_save_once(node);
    hm_write(node, val);
}

static void hmbird_apply_frame_only(void) {
    if (!g_hb_present) return;
    if (C.hmbird_frame < 0) return;
    int fps = C.hmbird_frame > 0 ? C.hmbird_frame : g_refresh;
    if (fps <= 0) return;
    char v[16], p[PATH_MAX];
    snprintf(v, sizeof v, "%d", fps);
    snprintf(p, sizeof p, "%s/frame_per_sec", P_HM);
    hm_save_once("frame_per_sec");
    if (wrverify(p, v) == 0) logv("hmbird: frame_per_sec=%d（light 模式，其余不动）", fps);
}

static void hmbird_apply_game(void) {
    if (!g_hb_present) return;
    // 帧率窗口对齐：跟面板刷新率一致，hmbird 的帧预算才算得准（纯收益，不受 hmbird_tune 影响）
    if (C.hmbird_frame >= 0) {
        int fps = C.hmbird_frame > 0 ? C.hmbird_frame : g_refresh;
        if (fps > 0) {
            char v[16], fp[PATH_MAX];
            snprintf(v, sizeof v, "%d", fps);
            snprintf(fp, sizeof fp, "%s/frame_per_sec", P_HM);
            hm_save_once("frame_per_sec");
            if (wrverify(fp, v) == 0) logv("hmbird: frame_per_sec=%d", fps);
        }
    }
    if (!C.hmbird_tune) return;
    char heavy[64];
    snprintf(heavy, sizeof heavy, "%s", C.hm_heavy);
    for (char *p = heavy; *p; p++) if (*p == ',') *p = ' ';
    hm_write_keep("coefficient", C.hm_coeff);
    hm_write_keep("perf_high_ratio", C.hm_high);
    char res[16];
    snprintf(res, sizeof res, "%d", C.hmbird_reserved);
    hm_write_keep("cpus_reserved", res);
    hm_write_keep("heavy_boost", heavy);
    for (int i = 0; i < npol; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/hmbird/target_loads", pol[i].path);
        wrp(p, "88");
        snprintf(p, sizeof p, "%s/hmbird/apply_freq_immediately", pol[i].path);
        wrp(p, "0");
    }
    g_hb_tuned = 1;
    logv("hmbird: game knobs applied");
}

static void hmbird_restore(void) {
    if (!g_hb_present) { g_hb_nsave = 0; g_hb_tuned = 0; return; }
    if (!g_hb_nsave) { g_hb_tuned = 0; return; }
    for (int i = 0; i < g_hb_nsave; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", P_HM, g_hb_save[i].node);
        wrp(p, g_hb_save[i].val);
    }
    logv("hmbird: 已还原 %d 个参数", g_hb_nsave);
    g_hb_nsave = 0;
    g_hb_tuned = 0;
}

// ------------------------------------------------------------------ boosts --
static int g_boost_killed;

static void boost_off(void) {
    static const char *nodes[] = {
        "/sys/module/msm_performance/parameters/touch_boost",
        "/sys/devices/system/cpu/cpu_boost/input_boost_enabled",
        "/sys/module/cpu_boost/parameters/input_boost_enabled",
        "/sys/module/cpu_boost/parameters/boost_ms",
        "/sys/kernel/msm_performance/boost",
        "/proc/sys/kernel/sched_boost",
        "/sys/module/msm_perf_monitor/parameters/enable",
        "/sys/kernel/oplus_boost/enable",
    };
    int n = 0;
    for (size_t i = 0; i < sizeof nodes / sizeof nodes[0]; i++)
        if (wrp(nodes[i], "0") == 0) n++;
    g_boost_killed = n;
    logmsg("boost: 关闭 %d 个厂商加速节点", n);
}

static void boost_restore(void) {
    static const char *nodes[] = {
        "/sys/module/msm_performance/parameters/touch_boost",
        "/sys/devices/system/cpu/cpu_boost/input_boost_enabled",
        "/sys/module/cpu_boost/parameters/input_boost_enabled",
        "/sys/kernel/msm_performance/boost",
        "/sys/module/msm_perf_monitor/parameters/enable",
    };
    for (size_t i = 0; i < sizeof nodes / sizeof nodes[0]; i++) wrp(nodes[i], "1");
    wrp("/proc/sys/kernel/sched_boost", "0");
}

static void io_tune(void) {
    for (int i = 0; i < 8; i++) {
        char d[PATH_MAX];
        snprintf(d, sizeof d, "/sys/block/sd%c/queue", 'a' + i);
        if (access(d, F_OK) == 0) {
            char f[PATH_MAX];
            snprintf(f, sizeof f, "%s/read_ahead_kb", d);
            wrp(f, "256");
            snprintf(f, sizeof f, "%s/iostats", d);
            wrp(f, "0");
        }
    }
    wrp("/sys/block/mmcblk0/queue/read_ahead_kb", "256");
}

static void gpu_tune(void) {
    wrp("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0");
    wrp("/sys/class/kgsl/kgsl-3d0/bus_split", "1");
}

// ------------------------------------------------------------------ detect --
static char BRIGHT[PATH_MAX];
static char P_WEB[PATH_MAX];

static void detect_paths(void) {
    const char *hp = getenv("FEATHER_HTTP_PORT");
    if (hp && *hp) { int v = atoi(hp); if (v > 0 && v < 65536) g_http_port = v; }
    const char *wr = getenv("FEATHER_WEBROOT");
    snprintf(P_WEB, sizeof P_WEB, "%s", wr ? wr : "/data/adb/modules/feather_tune/webroot");
    static const char *cands[] = {
        "/class/backlight/panel0-backlight/brightness",
        "/class/backlight/panel1-backlight/brightness",
        "/class/leds/lcd-backlight/brightness",
    };
    for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s%s", P_SYS, cands[i]);
        if (access(p, R_OK) == 0) { snprintf(BRIGHT, sizeof BRIGHT, "%s", p); break; }
    }
}

static int screen_on(void) {
    if (BRIGHT[0]) {
        long v = rdnum(BRIGHT);
        if (v < 0) return 1;
        return v > 0;
    }
    return 1;
}

static int is_game(const char *pkg);   // 前置声明

// 整文件读取（不按行截断）——cgroup.procs 是多行的，读首行会漏
static int read_all(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) return -1;
    out[r] = 0;
    return (int)r;
}

static int is_system_pkg(const char *pkg) {
    static const char *pre[] = {
        "android", "com.android.", "com.oplus.", "com.coloros.", "com.oneplus.",
        "com.google.android.", "com.qualcomm.", "com.qti."
    };
    for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++)
        if (!strncmp(pkg, pre[i], strlen(pre[i]))) return 1;
    return 0;
}

/* ------------------------------------------------------------------ 前台判定 --
 * 真机的 top-app cgroup 里同时有 systemui / 输入法 / 前台 App，只按 cgroup 猜
 * 会把输入法当"前台应用"（实测 com.sohu.inputmethod.sogouoem 就顶上来过）。
 * 现在按可靠性排序：
 *   1) 焦点窗口 dumpsys window mCurrentFocus   —— 唯一正确的"用户正在看的 App"
 *   2) dumpsys activity 的 topResumedActivity  —— 没有窗口时（全屏/悬浮）
 *   3) top-app cgroup 扫描（排除输入法/系统包） —— 最便宜的兜底
 * 触发时机：top-app pid 集合变化、出现新进程且是游戏、缓存超过 10s、屏幕亮起。
 * 平时直接吃缓存，dumpsys 不会每秒都跑。
 */
static int is_game(const char *pkg);

static char g_fg_cands[16][128];
static int  g_fg_ncand;
static int  g_fg_source;          // 1=焦点窗口 2=activity 3=cgroup 0=失败
static char g_fg_cache[256];      // 上次成功结果
static long g_fg_ts;              // 上次重算时间
static unsigned g_topapp_sig;     // top-app pid 集合的指纹
static long g_max_pid;            // 见过的最大的 /proc pid
static char g_ime_pkg[128];       // 默认输入法包名（真机实测会顶到 top-app）

static int is_ime_pkg(const char *pkg) {
    if (g_ime_pkg[0] && !strcmp(pkg, g_ime_pkg)) return 1;
    if (strstr(pkg, "inputmethod") || strstr(pkg, ".ime") || strstr(pkg, "sogou")) return 1;
    return 0;
}

static void load_ime(void) {
    if (g_ime_pkg[0]) return;
    FILE *pp = popen("settings get secure default_input_method 2>/dev/null", "r");
    if (!pp) return;
    char line[256] = "";
    if (fgets(line, sizeof line, pp)) {
        char *slash = strchr(line, '/');
        if (slash) *slash = 0;
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strchr(line, '.')) snprintf(g_ime_pkg, sizeof g_ime_pkg, "%s", line);
    }
    pclose(pp);
}

static int collect_topapp(unsigned *sig) {
    static const char *paths[] = { "/top-app/cgroup.procs", "/top-app/tasks" };
    g_fg_ncand = 0;
    *sig = 0;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        char p[PATH_MAX], buf[4096];
        snprintf(p, sizeof p, "%s%s", P_CGRP, paths[i]);
        if (read_all(p, buf, sizeof buf) < 0) continue;
        char *save = NULL;
        for (char *t = strtok_r(buf, "\n", &save); t && g_fg_ncand < 16; t = strtok_r(NULL, "\n", &save)) {
            long pid = strtol(t, NULL, 10);
            if (pid <= 0) continue;
            *sig = *sig * 31u + (unsigned)pid;
            char cf[PATH_MAX], cl[256];
            snprintf(cf, sizeof cf, "%s/%ld/cmdline", P_PROC, pid);
            if (rdline(cf, cl, sizeof cl) != 0 || !cl[0]) continue;
            if (!strchr(cl, '.')) continue;
            if (strchr(cl, ':') || strchr(cl, '/')) continue;
            int dup = 0;
            for (int k = 0; k < g_fg_ncand; k++)
                if (!strcmp(g_fg_cands[k], cl)) { dup = 1; break; }
            if (!dup) snprintf(g_fg_cands[g_fg_ncand++], 128, "%s", cl);
        }
        if (g_fg_ncand) break;
    }
    return g_fg_ncand ? 0 : -1;
}

static int fg_from_cgroup(char *out, size_t n) {
    unsigned sig;
    if (collect_topapp(&sig) != 0) return -1;
    for (int k = 0; k < g_fg_ncand; k++)
        if (is_game(g_fg_cands[k])) { snprintf(out, n, "%s", g_fg_cands[k]); return 0; }
    for (int k = 0; k < g_fg_ncand; k++)
        if (!is_system_pkg(g_fg_cands[k]) && !is_ime_pkg(g_fg_cands[k])) {
            snprintf(out, n, "%s", g_fg_cands[k]);
            return 0;
        }
    return -1;
}

// 从一行 dumpsys 里抠出包名：... u0 com.pkg/.Act ...
static int pkg_from_line(const char *line, char *out, size_t n) {
    const char *u = strstr(line, "u0 ");
    const char *tok = u ? u + 3 : line;
    // 跳过 Window{ / ActivityRecord{ 之类的前缀
    while (*tok && !isalnum((unsigned char)*tok)) tok++;
    char tmp[256];
    size_t i = 0;
    for (; tok[i] && i < sizeof tmp - 1; i++) {
        if (tok[i] == ' ' || tok[i] == '\t' || tok[i] == '}' || tok[i] == '\r' || tok[i] == '\n') break;
        tmp[i] = tok[i];
    }
    tmp[i] = 0;
    char *slash = strchr(tmp, '/');
    if (slash) *slash = 0;
    if (!strchr(tmp, '.')) return -1;
    if (!strncmp(tmp, "Window", 6) || !strncmp(tmp, "ActivityRecord", 14)) return -1;
    snprintf(out, n, "%s", tmp);
    return 0;
}

static int fg_from_cmd(char *out, size_t n, const char *cmd) {
    FILE *pp = popen(cmd, "r");
    if (!pp) return -1;
    char line[512];
    int ok = -1;
    if (fgets(line, sizeof line, pp)) {
        if (pkg_from_line(line, out, n) == 0) ok = 0;
    }
    pclose(pp);
    return ok;
}

static int fg_from_window(char *out, size_t n) {
    return fg_from_cmd(out, n,
        "dumpsys window 2>/dev/null | grep -m1 -E 'mCurrentFocus|mFocusedApp'");
}

static int fg_from_activity(char *out, size_t n) {
    return fg_from_cmd(out, n,
        "dumpsys activity activities 2>/dev/null | grep -m1 -E 'topResumedActivity|mResumedActivity'");
}

/* AsoulOpt 那套思路：盯 /proc 的新进程，发现新起来的进程命中了游戏名单就立刻重算，
 * 不用等用户切窗口/等缓存过期。只做 readdir + 少量 cmdline 读，1Hz 下开销可忽略。 */
static int new_game_process(void) {
    DIR *d = opendir(P_PROC);
    if (!d) return 0;
    long mx = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        long v = strtol(de->d_name, NULL, 10);
        if (v > mx) mx = v;
    }
    closedir(d);
    if (mx <= 0) return 0;
    if (g_max_pid <= 0) { g_max_pid = mx; return 0; }
    if (mx <= g_max_pid) return 0;
    long from = g_max_pid;
    g_max_pid = mx;
    if (mx - from > 256) return 1;      // pid 大跳变（重启/清理），直接重算一次
    for (long pid = from + 1; pid <= mx; pid++) {
        char cf[PATH_MAX], cl[256];
        snprintf(cf, sizeof cf, "%s/%ld/cmdline", P_PROC, pid);
        if (rdline(cf, cl, sizeof cl) != 0 || !cl[0]) continue;
        if (!strchr(cl, '.')) continue;
        if (is_game(cl)) return 1;
    }
    return 0;
}

static int foreground_pkg(char *out, size_t n) {
    static int inited;
    if (!inited) { inited = 1; load_ime(); }

    unsigned sig = 0;
    int sig_ok = (collect_topapp(&sig) == 0);
    long now = (long)time(NULL);

    int forced = 0;
    if (new_game_process()) forced = 1;
    if (sig_ok && g_topapp_sig && sig != g_topapp_sig) forced = 1;   // App 切换
    if (sig_ok) g_topapp_sig = sig;
    // 缓存里的那个包已经不在 top-app 了 → 说明它被切走了，必须重算
    if (!forced && sig_ok && g_fg_cache[0]) {
        int still = 0;
        for (int k = 0; k < g_fg_ncand; k++)
            if (!strcmp(g_fg_cands[k], g_fg_cache)) { still = 1; break; }
        if (!still) forced = 1;
    }

    if (!forced && g_fg_cache[0] && now - g_fg_ts < 10) {
        snprintf(out, n, "%s", g_fg_cache);
        return 0;
    }

    char res[256] = "";
    g_fg_source = 0;
    if (fg_from_window(res, sizeof res) == 0)      g_fg_source = 1;
    else if (fg_from_activity(res, sizeof res) == 0) g_fg_source = 2;
    else if (fg_from_cgroup(res, sizeof res) == 0)   g_fg_source = 3;

    if (g_fg_source && res[0]) {
        snprintf(g_fg_cache, sizeof g_fg_cache, "%s", res);
        g_fg_ts = now;
        snprintf(out, n, "%s", res);
        return 0;
    }
    // 全失败：保留上次结果，别让状态在空与有之间抖
    if (g_fg_cache[0]) {
        snprintf(out, n, "%s", g_fg_cache);
        return 0;
    }
    out[0] = 0;
    return -1;
}

/* ------------------------------------------------------------ 应用级策略 --
 * apps.conf 一行一个应用，用户可以在 WebUI 里给任意第三方应用指定策略，
 * 一旦设定就脱离「场景」，按包名直接生效（白名单语义）。
 *   <包名> <模式> <性能簇上限%> <超大核上限%> <地板%> <线程规则0|1>
 * 模式：follow=跟随场景 / game / battery / daily / perf / custom
 */
#define MAX_APPS 128
typedef struct {
    char pkg[128];
    char mode[16];
    int  perf, prime, floor, threads;
} apprule_t;
static apprule_t g_apps[MAX_APPS];
static int g_napps;
static struct timespec g_apps_mtime;
static const apprule_t *g_cur_app;      // 当前生效的应用规则（custom 时用它的百分比）

static const apprule_t *app_find(const char *pkg) {
    if (!pkg || !pkg[0]) return NULL;
    for (int i = 0; i < g_napps; i++)
        if (!strcmp(g_apps[i].pkg, pkg)) return &g_apps[i];
    return NULL;
}

static void apps_load(void) {
    struct stat st;
    struct timespec mt = {0, 0};
    if (stat(P_APPS, &st) == 0) {
        mt.tv_sec = st.st_mtime;
        mt.tv_nsec = st.st_mtim.tv_nsec;
        if (timespec_eq(mt, g_apps_mtime) && g_apps_mtime.tv_sec != 0) return;
    }
    g_apps_mtime = mt;
    g_napps = 0;
    FILE *f = fopen(P_APPS, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f) && g_napps < MAX_APPS) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char pkg[128], mode[16];
        int pp = 0, pr = 0, fl = 0, th = 0;
        if (sscanf(line, "%127s %15s %d %d %d %d", pkg, mode, &pp, &pr, &fl, &th) < 2) continue;
        if (!strcmp(mode, "follow")) continue;      // follow 等于没配置
        apprule_t *a = &g_apps[g_napps];
        snprintf(a->pkg, sizeof a->pkg, "%s", pkg);
        snprintf(a->mode, sizeof a->mode, "%s", mode);
        a->perf = pp > 0 ? pp : 100;
        a->prime = pr > 0 ? pr : 100;
        a->floor = fl;
        a->threads = th;
        g_napps++;
    }
    fclose(f);
    logv("apps: 载入 %d 条应用策略", g_napps);
}

/* ---------------------------------------------------------- 名称与图标 --
 * 名称：labels.conf（pkg=名称），WebUI 也会用同一份文件
 * 图标：从 APK 里直接抠 res/…ic_launcher*.png，缓存在 $DATA/icons/<pkg>.png
 *       —— 不依赖任何第三方工具，toybox 自带的 unzip 就够
 */
typedef struct { char pkg[128]; char name[96]; } label_t;
#define MAX_LABELS 512
static label_t g_labels[MAX_LABELS];
static int g_nlabels;
static struct timespec g_labels_mtime;

static void labels_load(void) {
    struct stat st;
    struct timespec mt = {0, 0};
    if (stat(P_LABELS, &st) == 0) {
        mt.tv_sec = st.st_mtime;
        mt.tv_nsec = st.st_mtim.tv_nsec;
        if (timespec_eq(mt, g_labels_mtime) && g_labels_mtime.tv_sec != 0) return;
    }
    g_labels_mtime = mt;
    g_nlabels = 0;
    FILE *f = fopen(P_LABELS, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f) && g_nlabels < MAX_LABELS) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        while (*v == ' ' || *v == '\t') v++;
        char *ke = k + strlen(k);
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        if (!*k || !*v) continue;
        snprintf(g_labels[g_nlabels].pkg, sizeof g_labels[0].pkg, "%s", k);
        snprintf(g_labels[g_nlabels].name, sizeof g_labels[0].name, "%s", v);
        g_nlabels++;
    }
    fclose(f);
}

static const char *label_of(const char *pkg) {
    for (int i = 0; i < g_nlabels; i++)
        if (!strcmp(g_labels[i].pkg, pkg)) return g_labels[i].name;
    return NULL;
}

static int pkg_ok(const char *pkg) {
    if (!pkg || !pkg[0] || strlen(pkg) > 120) return 0;
    for (const char *c = pkg; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-') return 0;
    return 1;
}

// 把 APK 里的图标抠出来；成功返回 0 并写 out 路径
//
// Android 上没装 unzip 的机器不少，所以这里按 unzip → busybox unzip → toybox unzip
// 依次降级（只探测一次）。选图标的规则也换掉了原来的「路径最长」——那是瞎猜，
// 改成：优先 ic_launcher 命中的、按未压缩大小取最大的那个（xxxhdpi 通常最大）。
static char g_unzip[64];        // "" = 一个都没有
static int  g_unzip_probed;

static void unzip_probe(void) {
    if (g_unzip_probed) return;
    g_unzip_probed = 1;
    if (system("command -v unzip >/dev/null 2>&1") == 0) snprintf(g_unzip, sizeof g_unzip, "unzip");
    else if (system("command -v busybox >/dev/null 2>&1") == 0) snprintf(g_unzip, sizeof g_unzip, "busybox unzip");
    else if (system("command -v toybox >/dev/null 2>&1") == 0) snprintf(g_unzip, sizeof g_unzip, "toybox unzip");
    else g_unzip[0] = 0;
    logv("icon: 解压工具 = %s", g_unzip[0] ? g_unzip : "（无）");
}

static int icon_missed(const char *pkg) {
    char mf[PATH_MAX];
    snprintf(mf, sizeof mf, "%s/.miss", P_ICONS);
    FILE *f = fopen(mf, "r");
    if (!f) return 0;
    char line[160];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!strcmp(line, pkg)) { found = 1; break; }
    }
    fclose(f);
    return found;
}

static void icon_mark_miss(const char *pkg) {
    char mf[PATH_MAX];
    snprintf(mf, sizeof mf, "%s/.miss", P_ICONS);
    FILE *f = fopen(mf, "a");
    if (!f) return;
    fprintf(f, "%s\n", pkg);
    fclose(f);
}

static int icon_path(const char *pkg, char *out, size_t n) {
    if (!pkg_ok(pkg)) return -1;
    snprintf(out, n, "%s/%s.png", P_ICONS, pkg);
    struct stat st;
    if (stat(out, &st) == 0 && st.st_size > 0) return 0;      // 已缓存
    mkdir(P_ICONS, 0755);
    if (icon_missed(pkg)) return -1;

    unzip_probe();
    if (!g_unzip[0]) return -1;

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/.i.png", P_DATA);
    char cmd[1024];
    // 1) 先按「ic_launcher 且体积最大」挑；2) 退而求其次取任意 mipmap png 里最大的
    snprintf(cmd, sizeof cmd,
        "apk=$(pm path %s 2>/dev/null | head -1 | sed 's/^package://'); "
        "[ -n \"$apk\" ] || exit 2; "
        "L=$(%s -l \"$apk\" 2>/dev/null); "
        "[ -n \"$L\" ] || exit 4; "
        "e=$(printf '%%s\\n' \"$L\" | awk 'NF>=4 && $4 ~ /^res\\/.*\\.png$/ && $4 ~ /ic_launcher|icon/ "
        "{print $1\" \"$4}' | sort -n | tail -1 | cut -d' ' -f2-); "
        "[ -n \"$e\" ] || e=$(printf '%%s\\n' \"$L\" | awk 'NF>=4 && $4 ~ /^res\\/mipmap.*\\.png$/ "
        "{print $1\" \"$4}' | sort -n | tail -1 | cut -d' ' -f2-); "
        "[ -n \"$e\" ] || exit 3; "
        "%s -p \"$apk\" \"$e\" > %s 2>/dev/null",
        pkg, g_unzip, g_unzip, tmp);

    int rc = system(cmd);
    if (rc != 0) {
        logv("icon: %s 抠取失败 (rc=%d, 工具=%s)", pkg, rc, g_unzip);
        icon_mark_miss(pkg);
        return -1;
    }
    if (stat(tmp, &st) != 0 || st.st_size < 64) {
        unlink(tmp);
        icon_mark_miss(pkg);
        return -1;
    }
    rename(tmp, out);
    return 0;
}

static char **g_games;
static int g_ngames, g_games_cap;
static struct timespec g_games_mtime;

static void games_load(void) {
    struct stat st;
    struct timespec mt = {0, 0};
    if (stat(P_GAMES, &st) == 0) {
        mt.tv_sec = st.st_mtime;
        mt.tv_nsec = st.st_mtim.tv_nsec;
        if (timespec_eq(mt, g_games_mtime) && g_games_mtime.tv_sec != 0) return;
    }
    g_games_mtime = mt;
    if (g_games) {
        for (int i = 0; i < g_ngames; i++) free(g_games[i]);
        free(g_games);
        g_games = NULL;
    }
    g_ngames = g_games_cap = 0;
    FILE *f = fopen(P_GAMES, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || !*s) continue;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!*s) continue;
        if (g_ngames == g_games_cap) {
            g_games_cap = g_games_cap ? g_games_cap * 2 : 128;
            g_games = realloc(g_games, sizeof(char *) * (size_t)g_games_cap);
        }
        g_games[g_ngames++] = strdup(s);
    }
    fclose(f);
}

static int is_game(const char *pkg) {
    if (!pkg || !*pkg) return 0;
    for (int i = 0; i < g_ngames; i++) if (!strcmp(g_games[i], pkg)) return 1;
    return 0;
}

// 机型/系统版本：从 build.prop 读一次（比每次 popen getprop 便宜）
static char g_model[64], g_android[16];

static void read_buildprops(void) {
    static const char *files[] = {
        "/system/build.prop", "/vendor/build.prop", "/odm/etc/build.prop",
        "/product/build.prop",
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        FILE *f = fopen(files[i], "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (!*s || *s == '#') continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            char *v = eq + 1;
            char *e = v + strlen(v);
            while (e > v && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            if (!strcmp(s, "ro.product.model") && !g_model[0])
                snprintf(g_model, sizeof g_model, "%s", v);
            else if (!strcmp(s, "ro.build.version.release") && !g_android[0])
                snprintf(g_android, sizeof g_android, "%s", v);
        }
        fclose(f);
        if (g_model[0] && g_android[0]) break;
    }
}

static long battery_cap(void) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/class/power_supply/battery/capacity", P_SYS);
    return rdnum(p);
}
static long battery_temp(void) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/class/power_supply/battery/temp", P_SYS);
    long v = rdnum(p);
    return v > 0 ? v / 10 : -1;
}
static void battery_status(char *out, size_t n) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/class/power_supply/battery/status", P_SYS);
    if (rdline(p, out, n) != 0) out[0] = 0;
}

// ------------------------------------------------------------------- rules --
typedef struct { char pkg[128]; char pat[192]; char tgt[48]; regex_t re; int valid; const char *target; } rule_t;
static rule_t g_rules[MAX_RULES];
static int g_nrules;
static struct timespec g_rules_mtime;

static const char *mask_of(const char *t) {
    if (!strcmp(t, "prime"))  return "0xC0";   // CPU6-7 超大核簇
    if (!strcmp(t, "prime1")) return "0x80";   // CPU7 单核
    if (!strcmp(t, "prime2")) return "0x40";   // CPU6 单核
    if (!strcmp(t, "perf"))   return "0x3F";   // CPU0-5
    if (!strcmp(t, "mid"))    return "0x30";   // CPU4-5（同簇里偏大的两颗）
    if (!strcmp(t, "low"))    return "0x0F";   // CPU0-3
    if (!strcmp(t, "bg"))     return "0x03";   // CPU0-1
    return NULL;
}

static int popcount_mask(unsigned long m) {
    int c = 0;
    while (m) { c += (int)(m & 1UL); m >>= 1; }
    return c;
}

static void rules_load(void) {
    struct stat st;
    struct timespec mt = {0, 0};
    if (stat(P_RULES, &st) == 0) {
        mt.tv_sec = st.st_mtime;
        mt.tv_nsec = st.st_mtim.tv_nsec;
        if (timespec_eq(mt, g_rules_mtime) && g_rules_mtime.tv_sec != 0) return;
    }
    g_rules_mtime = mt;
    for (int i = 0; i < g_nrules; i++) if (g_rules[i].valid) regfree(&g_rules[i].re);
    g_nrules = 0;
    FILE *f = fopen(P_RULES, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f) && g_nrules < MAX_RULES) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        char *st = line;
        while (*st == ' ' || *st == '\t') st++;
        if (!*st) continue;
        // 以「最后一个空白分隔字段」为目标，中间整段都是正则（允许正则含空格，
        // 例如内核 comm "Job.worker 0"），否则 Job.worker 这类规则会被静默丢掉。
        char *last = strrchr(st, ' ');
        char *lastt = strrchr(st, '\t');
        if (lastt && (!last || lastt > last)) last = lastt;
        if (!last) continue;
        char *tgt = last + 1;
        while (*tgt == ' ' || *tgt == '\t') tgt++;
        if (!*tgt) continue;
        *last = 0;
        char *sp2 = strpbrk(st, " \t");
        if (!sp2) continue;
        char *pat = sp2;
        while (*pat == ' ' || *pat == '\t') pat++;
        char *pe = st + strlen(st);
        while (pe > pat && (pe[-1] == ' ' || pe[-1] == '\t')) *--pe = 0;
        if (!*pat) continue;
        char pkg[128];
        snprintf(pkg, sizeof pkg, "%.*s", (int)(sp2 - st), st);
        // 目标支持用 '+' 组合：核掩码 + hmbird 调度属性，例如
        //   prime1+preempt     独占 CPU7，并标记为抢占优先
        //   perf+uclamp:512    性能簇内，但 uclamp_max 压到 50%，不把频率顶上去
        char tgt_buf[48], tgt_buf_orig[48];
        snprintf(tgt_buf, sizeof tgt_buf, "%s", tgt);
        snprintf(tgt_buf_orig, sizeof tgt_buf_orig, "%s", tgt);
        const char *mask = NULL;
        {
            char *plus = strchr(tgt_buf, '+');
            if (plus) { *plus = 0; mask = mask_of(tgt_buf); }
            else mask = mask_of(tgt_buf);
        }
        // 非核掩码目标：交给 hmbird 自己的调度属性（不进亲和性）
        if (!mask && (!strcmp(tgt, "preempt") || !strcmp(tgt, "idle"))) {
            rule_t *r0 = &g_rules[g_nrules];
            memset(r0, 0, sizeof *r0);
            snprintf(r0->pkg, sizeof r0->pkg, "%s", pkg);
            snprintf(r0->pat, sizeof r0->pat, "%s", pat);
            if (regcomp(&r0->re, pat, REG_EXTENDED | REG_NOSUB) != 0) continue;
            r0->valid = 1;
            snprintf(g_rules[g_nrules].pat, sizeof g_rules[g_nrules].pat, "%s", pat);
            r0->target = (tgt[0] == 'p') ? "preempt" : "idle";
            g_nrules++;
            continue;
        }
        if (!mask) {
            logmsg("rules: 跳过未知目标 '%s' 的行", tgt);
            continue;
        }
        rule_t *r = &g_rules[g_nrules];
        memset(r, 0, sizeof *r);
        snprintf(r->pkg, sizeof r->pkg, "%s", pkg);
        snprintf(r->pat, sizeof r->pat, "%s", pat);
        if (regcomp(&r->re, pat, REG_EXTENDED | REG_NOSUB) != 0) {
            logmsg("rules: 正则编译失败，跳过 '%s'", pat);
            continue;
        }
        r->valid = 1;
        snprintf(r->tgt, sizeof r->tgt, "%s", tgt_buf_orig);
        r->target = r->tgt;
        g_nrules++;
    }
    fclose(f);
}

// ------------------------------------------------------------- thread 亲和 --
static long g_seen[MAX_SEEN];
static int g_nseen;
static long g_seen_pid = -1;
static int g_threads_pinned;
static int g_schedprop_pinned;

static int seen_has(long tid) {
    for (int i = 0; i < g_nseen; i++) if (g_seen[i] == tid) return 1;
    return 0;
}
static void seen_add(long tid) {
    if (g_nseen < MAX_SEEN) g_seen[g_nseen++] = tid;
}

static int g_pin_kind;   // 1=prefer_cluster 2=prefer_cpu 3=单核亲和 4=多核亲和 0=失败

static const char *cluster_of(const char *mask) {
    unsigned long m = strtoul(mask, NULL, 16);
    return (m & 0xC0UL) ? "0x2" : "0x1";   // bit1=超大核簇, bit0=性能簇
}

static void pin_tid(long tid, const char *hexmask) {
    unsigned long mm = strtoul(hexmask, NULL, 16);
    if (g_hb_present) {
        char v[64], p[PATH_MAX];
        // 单核目标（0x80/0x40）必须给精确核：prefer_cluster 只表达「哪个簇」，
        // 会把串行链上的两个线程丢到同一个核上排队。
        if (popcount_mask(mm) == 1) {
            snprintf(v, sizeof v, "%ld %s", tid, hexmask);
            snprintf(p, sizeof p, "%s/prefer_cpu", P_HM);
            if (wrverify(p, v) == 0) { g_pin_kind = 2; return; }
            g_pm_errno = errno;
        } else {
            // 1) 只给簇，让 hmbird 自己挑核：最不容易被它的扩掩码打回
            snprintf(v, sizeof v, "%ld %s", tid, cluster_of(hexmask));
            snprintf(p, sizeof p, "%s/prefer_cluster", P_HM);
            if (wrverify(p, v) == 0) { g_pin_kind = 1; return; }
            g_pm_errno = errno;
            // 2) 精确核掩码
            snprintf(v, sizeof v, "%ld %s", tid, hexmask);
            snprintf(p, sizeof p, "%s/prefer_cpu", P_HM);
            if (wrverify(p, v) == 0) { g_pin_kind = 2; return; }
        }
    }
    unsigned long mask = strtoul(hexmask, NULL, 16);
    cpu_set_t set;
    // 3) 单核目标：单核掩码是 hmbird 唯一会尊重的形态，钉死它
    if (popcount_mask(mask) == 1) {
        int cpu = -1;
        for (int i = 0; i < 64; i++) if (mask & (1UL << i)) { cpu = i; break; }
        if (cpu >= 0) {
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            if (sched_setaffinity((pid_t)tid, sizeof set, &set) == 0) { g_pin_kind = 3; return; }
        }
    }
    // 4) 多核目标：整体给掩码（hmbird 可能会扩，但至少是个提示；
    //    绝不能退化成“只钉第一个核”，否则一堆 Job.worker 会挤到 CPU0）
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++) if (mask & (1UL << i)) CPU_SET(i, &set);
    sched_setaffinity((pid_t)tid, sizeof set, &set);
    g_pin_kind = 4;
}

static void apply_threads(long pid, const char *pkg) {
    if (pid != g_seen_pid) { g_seen_pid = pid; g_nseen = 0; g_threads_pinned = 0; }
    char tdir[PATH_MAX];
    snprintf(tdir, sizeof tdir, "%s/%ld/task", P_PROC, pid);
    DIR *d = opendir(tdir);
    if (!d) return;
    struct dirent *de;
    int added = 0;
    while ((de = readdir(d))) {
        long tid = strtol(de->d_name, NULL, 10);
        if (tid <= 0) continue;
        if (seen_has(tid)) continue;
        seen_add(tid);
        char cf[PATH_MAX], comm[64];
        snprintf(cf, sizeof cf, "%s/%s/comm", tdir, de->d_name);
        if (rdline(cf, comm, sizeof comm) != 0 || !comm[0]) continue;
        const char *target = NULL;
        for (int pass = 0; pass < 2 && !target; pass++) {
            for (int i = 0; i < g_nrules && !target; i++) {
                if (!g_rules[i].valid) continue;
                int exact = strcmp(g_rules[i].pkg, "*") != 0;
                if (pass == 0 && !exact) continue;
                if (pass == 1 && exact) continue;
                if (exact && strcmp(g_rules[i].pkg, pkg)) continue;
                if (regexec(&g_rules[i].re, comm, 0, NULL, 0) == 0) target = g_rules[i].target;
            }
        }
        if (target) {
            char parts[64];
            snprintf(parts, sizeof parts, "%s", target);
            int pinned_any = 0;
            char *save = NULL;
            for (char *t = strtok_r(parts, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
                if (!strncmp(t, "uclamp:", 7)) {
                    char node[PATH_MAX], v[48];
                    int val = atoi(t + 7);
                    if (val < 1) val = 1;
                    if (val > 1024) val = 1024;
                    snprintf(node, sizeof node, "%s/set_uclamp", P_HM);
                    snprintf(v, sizeof v, "%ld %d", tid, val);
                    if (wrp(node, v) == 0) g_schedprop_pinned++;
                    else if (g_log_level >= 2) logv("uclamp: %ld 写入失败 (%s)", tid, t);
                    continue;
                }
                if (!strcmp(t, "preempt") || !strcmp(t, "idle")) {
                    char node[PATH_MAX], v[32];
                    snprintf(node, sizeof node, "%s/prefer_%s", P_HM, t);
                    snprintf(v, sizeof v, "%ld 1", tid);
                    if (wrp(node, v) == 0) g_schedprop_pinned++;
                    continue;
                }
                const char *m = mask_of(t);
                if (!m && !strncmp(t, "0x", 2)) m = t;      // 规则里可能直接写掩码
                if (m) { pin_tid(tid, m); pinned_any = 1; }
            }
            if (pinned_any || target[0]) added++;
        }
    }
    closedir(d);
    if (added) {
        g_threads_pinned += added;
        logv("threads: +%d pinned (%s, total %d)", added, pkg, g_threads_pinned);
    }
}

// ------------------------------------------------------------------- apply --
static char g_prof[16] = "";
static long g_cap_perf, g_cap_prime;

static void apply_profile(const char *name) {
    int pp, pr;
    profile_pct(name, &pp, &pr);
    if (!strcmp(name, "custom") && g_cur_app) { pp = g_cur_app->perf; pr = g_cur_app->prime; }
    if (pp < 100 && idx_perf >= 0) g_cap_perf = set_cap(idx_perf, pp);
    else g_cap_perf = policy_cap_now(idx_perf);
    if (pr < 100 && idx_prime >= 0) g_cap_prime = set_cap(idx_prime, pr);
    else g_cap_prime = policy_cap_now(idx_prime);

    int bg, fg, top;
    profile_uclamp(name, &bg, &fg, &top);
    set_uclamp("background", bg);
    set_uclamp("foreground", fg);
    set_uclamp("top-app", top);
    set_uclamp("system-background", bg + 10);

    if (!strcmp(name, "custom") && g_cur_app && g_cur_app->floor > 0 && idx_perf >= 0) {
        long t = pol[idx_perf].max / 100 * g_cur_app->floor;
        set_policy_min(idx_perf, freq_ge(&pol[idx_perf], t));
        if (!g_game_extras) g_game_extras = 1;    // 借用同一套还原逻辑
    }
    if (!strcmp(name, "game")) {
        /* 实测教训：v1.2.7 的「抬地板 + 双大核各钉一颗 + Job 压 uclamp」跑下来
         * 比官方调度更差——频率更高、帧更不稳、温度功耗都涨。原因是官方 hmbird
         * 本来就是按帧预算自己算频率，我们再叠一层地板和绑核只会让它和我们对打。
         * 所以默认交还给官方，我们的干预改成 opt-in。
         *
         *   official   —— 什么都不写（默认，等同于装了个空模块）
         *   light      —— 只对齐 frame_per_sec，不碰频率/线程
         *   aggressive —— 旧行为：地板 + bg cpuset + hmbird 系数 + 线程归位
         */
        if (!strcmp(C.game_mode, "aggressive")) {
            hmbird_apply_game();
            apply_game_extras();
        } else {
            // 从 aggressive 切回官方时，要把之前写进去的地板/系数全部撤销
            hmbird_restore();
            restore_game_extras();
            if (!strcmp(C.game_mode, "light")) hmbird_apply_frame_only();
        }
    } else {
        hmbird_restore();
        restore_game_extras();
    }
    if (C.gpu_tune) gpu_tune();

    snprintf(g_prof, sizeof g_prof, "%s", name);
    logv("apply: %s perf=%d%%(%ld) prime=%d%%(%ld)", name, pp, g_cap_perf, pr, g_cap_prime);
}

static void uclamp_restore(void);
static void charge_restore(void);

static void restore_all(void) {
    uclamp_restore();
    charge_restore();
    logv("restore_all: npol=%d extras=%d", npol, g_game_extras);
    for (int i = 0; i < npol; i++) {
        long v = (g_base_ok && g_base_max[i] > 0) ? g_base_max[i] : pol[i].max;
        char f[PATH_MAX];
        snprintf(f, sizeof f, "%s/scaling_max_freq", pol[i].path);
        wrl(f, v);
    }
    set_uclamp("background", 100);
    set_uclamp("foreground", 100);
    set_uclamp("top-app", 100);
    set_uclamp("system-background", 100);
    hmbird_restore();
    restore_game_extras();
    boost_restore();
    g_hb_tuned = 0;
    logmsg("restore: 已还原频率上限与调度参数");
}

// 即时判定（不含迟滞），loop 与 apply/命令共用
static char g_applied[16];   // 命令触发的即时应用结果，供 loop 同步 cur

static void decide_now(char *want, size_t n, char *fg_out, size_t fgn) {
    if (fg_out) fg_out[0] = 0;
    if (!screen_on()) { snprintf(want, n, "sleep"); return; }
    char fg[256] = "";
    if (foreground_pkg(fg, sizeof fg) == 0 && fg[0] && fg_out) snprintf(fg_out, fgn, "%s", fg);
    // 应用级策略优先于场景：用户设了就是白名单，不受 auto/手动模式限制
    const apprule_t *ar = app_find(fg);
    g_cur_app = ar;
    if (ar) {
        snprintf(want, n, "%s", ar->mode);
        return;
    }
    if (strcmp(C.mode, "auto")) { snprintf(want, n, "%s", C.mode); return; }
    if (C.auto_game && is_game(fg)) { snprintf(want, n, "game"); return; }
    long b = battery_cap();
    char st[32];
    battery_status(st, sizeof st);
    if (b > 0 && b <= 20 && strcmp(st, "Charging") && strcmp(st, "Full")) {
        snprintf(want, n, "battery");
        return;
    }
    snprintf(want, n, "daily");
}

static void state_write(const char *cur, const char *fg, int game, long batt,
                        long temp, int screen, long up);

static void apply_now(char *out, size_t n) {
    char want[16], fg[256];
    decide_now(want, sizeof want, fg, sizeof fg);
    apply_profile(want);
    snprintf(g_applied, sizeof g_applied, "%s", want);
    // 命令路径立刻落盘：WebUI 点一下就要看到新状态，不等下一拍
    long b = battery_cap();
    state_write(want, fg, !strcmp(want, "game"), b > 0 ? b : 100,
                battery_temp(), screen_on(), 0);
    snprintf(out, n, "applied=%s perf=%ld prime=%ld\n", want, g_cap_perf, g_cap_prime);
}

// ------------------------------------------------------------------- state --
static void state_write(const char *cur, const char *fg, int game, long batt,
                        long temp, int screen, long up) {    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/.state.tmp", P_DATA);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "mode=%s\n", C.mode);
    fprintf(f, "profile=%s\n", cur ? cur : "");
    fprintf(f, "screen=%d\n", screen);
    fprintf(f, "fg=%s\n", fg ? fg : "");
    fprintf(f, "game=%d\n", game);
    fprintf(f, "battery=%ld\n", batt);
    fprintf(f, "temp=%ld\n", temp);
    fprintf(f, "cap_perf=%ld\n", g_cap_perf);
    fprintf(f, "cap_prime=%ld\n", g_cap_prime);
    fprintf(f, "max_perf=%ld\n", idx_perf >= 0 ? pol[idx_perf].max : 0);
    fprintf(f, "max_prime=%ld\n", idx_prime >= 0 ? pol[idx_prime].max : 0);
    fprintf(f, "gov_perf=%s\n", gov_of(idx_perf));
    fprintf(f, "gov_prime=%s\n", gov_of(idx_prime));
    fprintf(f, "hmbird=%d\n", g_hb_present);
    fprintf(f, "hmbird_live=%d\n", hmbird_live());
    fprintf(f, "hmbird_tuned=%d\n", g_hb_tuned);
    fprintf(f, "threads=%d\n", g_threads_pinned);
    fprintf(f, "boost_killed=%d\n", g_boost_killed);
    fprintf(f, "intensity=%d\n", C.intensity);
    fprintf(f, "uptime=%ld\n", up);
    fprintf(f, "updated=%ld\n", (long)time(NULL));
    fclose(f);
    rename(tmp, P_STATE);
}

// -------------------------------------------------------------- 命令实现 ----
// 所有命令都往 out 里追加 key=value 文本（与 shell 版协议一致）
static void cmd_status(char *out, size_t n) {
    char line[512];
    size_t off = 0;
#define APP(...) do { snprintf(line, sizeof line, __VA_ARGS__); \
    size_t l = strlen(line); if (off + l < n) { memcpy(out + off, line, l); off += l; out[off] = 0; } } while (0)
    out[0] = 0;
    FILE *f = fopen(P_STATE, "r");
    if (f) {
        char buf[2048];
        size_t r = fread(buf, 1, sizeof buf - 1, f);
        buf[r] = 0;
        fclose(f);
        if (r) { memcpy(out, buf, r); off = r; out[off] = 0; }
    } else {
        APP("profile=unknown\n");
    }

    int alive = 0;
    long pid = rdnum(P_PID);
    if (pid_alive(pid)) alive = 1;
    APP("runner=%s\n", alive ? "running" : "stopped");
    APP("pid=%ld\n", pid > 0 ? pid : 0);

    char b[256];
    if (rdline("/proc/sys/kernel/osrelease", b, sizeof b) == 0) APP("kernel=%s\n", b);
    if (rdline("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", b, sizeof b) == 0) APP("gpu_cur=%s\n", b);

    APP("perf_policy=%s:%ld\n", idx_perf >= 0 ? strrchr(pol[idx_perf].path, '/') + 1 : "-",
        idx_perf >= 0 ? pol[idx_perf].max : 0);
    APP("prime_policy=%s:%ld\n", idx_prime >= 0 ? strrchr(pol[idx_prime].path, '/') + 1 : "-",
        idx_prime >= 0 ? pol[idx_prime].max : 0);
    for (int i = 0; i < npol; i++) {
        APP("gov_%s=%s\n", strrchr(pol[i].path, '/') + 1, gov_of(i));
        APP("max_%s=%ld\n", strrchr(pol[i].path, '/') + 1, policy_cap_now(i));
    }
    APP("hmbird_present=%d\n", g_hb_present);
    APP("model=%s\n", g_model[0] ? g_model : "OnePlus 15");
    APP("android=%s\n", g_android);
    APP("cfg_enable=%d\n", C.enable);
    APP("cfg_mode=%s\n", C.mode);
    APP("cfg_intensity=%d\n", C.intensity);
    APP("cfg_auto_game=%d\n", C.auto_game);
    APP("cfg_thread_opt=%d\n", C.thread_opt);
    APP("cfg_screen_off=%d\n", C.screen_off_saver);
    APP("cfg_kill_boost=%d\n", C.kill_boost);
    APP("cfg_io=%d\n", C.io_tune);
    APP("cfg_gpu=%d\n", C.gpu_tune);
    APP("cfg_launch_boost=%d\n", C.launch_boost);
    APP("cfg_log_level=%d\n", C.log_level);
    APP("cfg_charge_ctrl=%d\n", C.charge_ctrl);
    APP("cfg_game_mode=%s\n", C.game_mode);
    APP("chg_last_pct=%d\n", g_chg_last_pct);
    APP("chg_suspend=%d\n", g_chg_suspended);
    APP("chg_gear=%d\n", g_chg_gear);
    APP("chg_node=%s\n", g_chg_cur.valid ? g_chg_cur.path : "-");
    {
        char cdv[32] = "-";
        if (g_chg_cd.valid) rdline(g_chg_cd.path, cdv, sizeof cdv);
        APP("chg_cool_down=%s\n", cdv);
    }
    APP("games=%d\n", g_ngames);
    APP("version=%s\n", APP_VER);
#undef APP
}

static void cmd_doctor(char *out, size_t n) {
    size_t off = 0;
    char line[512];
    out[0] = 0;
#define APP(...) do { snprintf(line, sizeof line, __VA_ARGS__); \
    size_t l = strlen(line); if (off + l + 1 < n) { memcpy(out + off, line, l); off += l; out[off] = 0; } } while (0)
    APP("== FeatherTune doctor %s ==\n", APP_VER);
    char b[256];
    APP("kernel=%s\n", rdline("/proc/sys/kernel/osrelease", b, sizeof b) == 0 ? b : "?");
    APP("policies=%d perf=%s(%ld) prime=%s(%ld)\n", npol,
        idx_perf >= 0 ? strrchr(pol[idx_perf].path, '/') + 1 : "-", idx_perf >= 0 ? pol[idx_perf].max : 0,
        idx_prime >= 0 ? strrchr(pol[idx_prime].path, '/') + 1 : "-", idx_prime >= 0 ? pol[idx_prime].max : 0);
    for (int i = 0; i < npol; i++) {
        APP("%s: gov=%s max=%ld/%ld min=%ld freqs=%d base_max=%ld\n",
            strrchr(pol[i].path, '/') + 1, gov_of(i), policy_cap_now(i), pol[i].max,
            pol[i].min, pol[i].nfreq,
            (g_base_ok && i < MAX_POL) ? g_base_max[i] : 0);
    }
    APP("hmbird present=%d live=%d tuned=%d sysctl=%s\n", g_hb_present, hmbird_live(),
        g_hb_tuned, P_HM);
    static const char *keys[] = {"coefficient", "perf_high_ratio", "cpus_reserved",
                                 "cpus_exclusive", "freq_policy", "frame_per_sec", "heavy_boost"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", P_HM, keys[i]);
        if (rdline(p, b, sizeof b) == 0) APP("  %s=%s\n", keys[i], b);
    }
    for (int g = 0; g < 4; g++) {
        const char *grp = g == 0 ? "top-app" : g == 1 ? "foreground" : g == 2 ? "background" : "system-background";
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s/cpu.uclamp.max", P_CGRP, grp);
        APP("%s uclamp_max=%s\n", grp, rdline(p, b, sizeof b) == 0 ? b : "n/a");
    }
    APP("brightness=%s\n", BRIGHT[0] ? (rdline(BRIGHT, b, sizeof b) == 0 ? b : "?") : "n/a");
    long cap = battery_cap(), tmp = battery_temp();
    char st[32];
    battery_status(st, sizeof st);
    APP("battery=%ld%% %s temp=%ld\n", cap, st, tmp);
    APP("boost_killed=%d\ntaskset=%s\n", g_boost_killed,
        access("/system/bin/taskset", X_OK) == 0 ? "/system/bin/taskset" : "none");
    APP("refresh=%dHz\n", g_refresh);
    if (idx_perf >= 0) {
        char mp[PATH_MAX];
        snprintf(mp, sizeof mp, "%s/scaling_min_freq", pol[idx_perf].path);
        APP("perf_min=%ld (baseline %ld)\n", rdnum(mp),
            g_base_ok ? g_base_min[idx_perf] : -1);
    }
    {
        char bp[PATH_MAX], bv[64];
        snprintf(bp, sizeof bp, "%s/background/cpuset.cpus", P_CGRP);
        APP("bg_cpuset=%s\n", rdline(bp, bv, sizeof bv) == 0 ? bv : "n/a");
    }
    {
        char fgp[256];
        int fr = foreground_pkg(fgp, sizeof fgp);
        APP("fg=%s (source=%d 1=window 2=activity 3=cgroup 0=none) cands=%d ime=%s\n",
            fr == 0 ? fgp : "-", g_fg_source, g_fg_ncand, g_ime_pkg[0] ? g_ime_pkg : "-");
        for (int k = 0; k < g_fg_ncand; k++)
            APP("  cand%d=%s%s\n", k, g_fg_cands[k], is_game(g_fg_cands[k]) ? " [game]" : "");
    }
    {
        static const char *cg_probe[] = {
            "/top-app/cgroup.procs", "/top-app/tasks",
            "/background/cpuset.cpus", "/cgroup.controllers",
        };
        APP("cgroup_root=%s (node=%s)\n", P_CGRP, g_cset_node);
        APP("cpuctl_root=%s uclamp_ok=%d\n", P_CPUCTL, g_uclamp_ok);
        for (size_t i = 0; i < sizeof cg_probe / sizeof cg_probe[0]; i++) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s%s", P_CGRP, cg_probe[i]);
            APP("  probe %s = %s\n", cg_probe[i], access(p, R_OK) == 0 ? "readable" : "MISSING");
        }
        {
            char p2[PATH_MAX];
            bg_cpus_path(p2, sizeof p2);
            APP("  bg_cpus %s = %s\n", p2, access(p2, W_OK) == 0 ? "writable" : "MISSING");
            snprintf(p2, sizeof p2, "%s/foreground/cpu.uclamp.max", P_CPUCTL);
            APP("  uclamp %s = %s\n", p2, access(p2, W_OK) == 0 ? "writable" : "MISSING");
        }
        FILE *pc = fopen("/proc/self/cgroup", "r");
        if (pc) {
            char cl[256];
            int k = 0;
            while (fgets(cl, sizeof cl, pc) && k < 4) {
                char *nl = strchr(cl, '\n'); if (nl) *nl = 0;
                APP("  self_cgroup: %s\n", cl);
                k++;
            }
            fclose(pc);
        }
    }
    APP("prefer_errno=%d (13=EACCES 22=EINVAL 2=ENOENT)\n", g_pm_errno);
    {
        APP("rules=%d\n", g_nrules);
        for (int i = 0; i < g_nrules && i < 10; i++)
            APP("  rule%d: %s | %s | %s\n", i, g_rules[i].pkg, g_rules[i].pat,
                g_rules[i].target ? g_rules[i].target : "?");
    }
    APP("pin_kind=%d (1=prefer_cluster 2=prefer_cpu 3=single-core 4=multi)\n", g_pin_kind);
    if (g_hb_present) {
        char v[64], pp[PATH_MAX];
        int me = (int)getpid();
        snprintf(pp, sizeof pp, "%s/prefer_cluster", P_HM);
        snprintf(v, sizeof v, "%d 0x2", me);
        // 读回是 "<leader>::<comm>::<hex>" 列表，不是写进去的原文，所以按值比对
        APP("selftest prefer_cluster=%s\n", hm_selftest("prefer_cluster", "0x2", me) ? "ok" : "FAIL");
        APP("selftest prefer_cpu=%s\n", hm_selftest("prefer_cpu", "0x40", me) ? "ok" : "FAIL");
        APP("selftest prefer_preempt=%s\n", hm_selftest("prefer_preempt", "1", me) ? "ok" : "FAIL");
        snprintf(pp, sizeof pp, "%s/frame_per_sec", P_HM);
        APP("frame_per_sec_now=%s\n", rdline(pp, v, sizeof v) == 0 ? v : "n/a");
    }
    long pid = rdnum(P_PID);
    APP("daemon pid=%ld alive=%d\n", pid > 0 ? pid : 0, pid_alive(pid));
#undef APP
}

// 运行一条命令（daemon 与本地 CLI 共用）
static void run_cmd(const char *line, char *out, size_t n) {
    char cmd[64] = "", a1[128] = "", a2[128] = "", a3[128] = "";
    char a4[64] = "", a5[64] = "", a6[64] = "", a7[64] = "";
    int got = sscanf(line, "%63s %127s %127s %127s %63s %63s %63s %63s",
                     cmd, a1, a2, a3, a4, a5, a6, a7);
    if (got <= 0) { snprintf(out, n, "err=empty\n"); return; }

    cfg_load();
    detect_topology();
    capture_syscaps();
    detect_refresh();
    detect_paths();
    hmbird_detect();
    games_load();
    rules_load();
    apps_load();
    labels_load();

    if (!strcmp(cmd, "status")) { cmd_status(out, n); return; }
    if (!strcmp(cmd, "doctor")) { cmd_doctor(out, n); return; }
    if (!strcmp(cmd, "apply"))  { apply_now(out, n); return; }
    if (!strcmp(cmd, "log")) {
        int want = got >= 2 ? atoi_safe(a1, 40) : 40;
        FILE *f = fopen(P_LOG, "r");
        if (!f) { snprintf(out, n, "(空)\n"); return; }
        char *buf = calloc(1, 65536);
        size_t r = buf ? fread(buf, 1, 65535, f) : 0;
        fclose(f);
        if (!buf) { snprintf(out, n, "err=oom\n"); return; }
        buf[r] = 0;
        int lines = 0;
        for (size_t i = 0; i < r; i++) if (buf[i] == '\n') lines++;
        int skip = lines - want;
        size_t i = 0;
        while (skip > 0 && i < r) { if (buf[i] == '\n') skip--; i++; }
        snprintf(out, n, "%s", buf + i);
        free(buf);
        return;
    }
    if (!strcmp(cmd, "games") || !strcmp(cmd, "pkg")) {
        if (got == 1 || (got >= 2 && !strcmp(a1, "list"))) {
            size_t off = 0;
            out[0] = 0;
            for (int i = 0; i < g_ngames && off + 256 < n; i++)
                off += (size_t)snprintf(out + off, n - off, "%s\n", g_games[i]);
            return;
        }
        if (!strcmp(a1, "add") && got >= 3) {
            FILE *f = fopen(P_GAMES, "a");
            if (f) { fprintf(f, "%s\n", a2); fclose(f); }
            g_games_mtime.tv_sec = 0;
            snprintf(out, n, "ok=added:%s\n", a2);
            return;
        }
        if (!strcmp(a1, "del") && got >= 3) {
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof tmp, "%s/.g.tmp", P_DATA);
            FILE *in = fopen(P_GAMES, "r"), *o = fopen(tmp, "w");
            if (in && o) {
                char b[256];
                while (fgets(b, sizeof b, in)) {
                    char c[256];
                    snprintf(c, sizeof c, "%s", b);
                    char *s = c;
                    while (*s == ' ' || *s == '\t') s++;
                    char *e = s + strlen(s);
                    while (e > s && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
                    if (strcmp(s, a2)) fputs(b, o);
                }
            }
            if (in) fclose(in);
            if (o) fclose(o);
            rename(tmp, P_GAMES);
            g_games_mtime.tv_sec = 0;
            snprintf(out, n, "ok=removed:%s\n", a2);
            return;
        }
        snprintf(out, n, "err=usage\n");
        return;
    }
    if (!strcmp(cmd, "set") && got >= 3) {
        if (cfg_set(a1, a2) != 0) { snprintf(out, n, "err=write\n"); return; }
        apply_now(out, n);
        return;
    }
    if (!strcmp(cmd, "mode") && got >= 2) {
        const char *ok[] = {"auto", "daily", "battery", "game", "perf", "sleep"};
        int valid = 0;
        for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) if (!strcmp(a1, ok[i])) valid = 1;
        if (!valid) { snprintf(out, n, "err=bad_mode\n"); return; }
        cfg_set("mode", a1);
        apply_now(out, n);
        return;
    }
    if (!strcmp(cmd, "apps") || !strcmp(cmd, "app")) {
        if (got >= 2 && !strcmp(a1, "set") && got >= 3) {
            // app set <pkg> <mode> [perf prime floor threads]
            const char *pkg = a2;
            const char *mode = got >= 4 ? a3 : "custom";
            int pp = got >= 5 ? atoi_safe(a4, 100) : 100;
            int pr = got >= 6 ? atoi_safe(a5, 100) : 100;
            int fl = got >= 7 ? atoi_safe(a6, 0) : 0;
            int th = got >= 8 ? atoi_safe(a7, 1) : 1;
            if (pp < 20) pp = 20;
            if (pp > 100) pp = 100;
            if (pr < 20) pr = 20;
            if (pr > 100) pr = 100;
            if (fl < 0) fl = 0;
            if (fl > 95) fl = 95;
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof tmp, "%s/.a.tmp", P_DATA);
            FILE *in = fopen(P_APPS, "r"), *o = fopen(tmp, "w");
            int wrote = 0;
            if (o) {
                if (in) {
                    char b[512];
                    while (fgets(b, sizeof b, in)) {
                        char c[512];
                        snprintf(c, sizeof c, "%s", b);
                        char *t = c;
                        while (*t == ' ' || *t == '\t') t++;
                        char *sp = strpbrk(t, " \t\r\n");
                        if (sp) *sp = 0;
                        if (!strcmp(t, pkg)) continue;      // 旧行丢掉，末尾重写
                        fputs(b, o);
                    }
                    fclose(in);
                }
                fprintf(o, "%s %s %d %d %d %d\n", pkg, mode, pp, pr, fl, th);
                fclose(o);
                wrote = 1;
            }
            if (wrote) {
                rename(tmp, P_APPS);
                g_apps_mtime.tv_sec = 0;
                g_apps_mtime.tv_nsec = 0;
                apps_load();
        labels_load();
                snprintf(out, n, "ok=app:%s mode=%s perf=%d prime=%d floor=%d thr=%d\n",
                         pkg, mode, pp, pr, fl, th);
            } else {
                snprintf(out, n, "err=write_failed\n");
            }
            return;
        }
        if (got >= 2 && !strcmp(a1, "del") && got >= 3) {
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof tmp, "%s/.a.tmp", P_DATA);
            FILE *in = fopen(P_APPS, "r"), *o = fopen(tmp, "w");
            if (in && o) {
                char b[512];
                while (fgets(b, sizeof b, in)) {
                    char c[512];
                    snprintf(c, sizeof c, "%s", b);
                    char *t = c;
                    while (*t == ' ' || *t == '\t') t++;
                    char *sp = strpbrk(t, " \t\r\n");
                    if (sp) *sp = 0;
                    if (!strcmp(t, a2)) continue;
                    fputs(b, o);
                }
                fclose(in);
                fclose(o);
                rename(tmp, P_APPS);
                g_apps_mtime.tv_sec = 0;
                apps_load();
                snprintf(out, n, "ok=deleted:%s\n", a2);
            } else {
                if (in) fclose(in);
                snprintf(out, n, "err=write_failed\n");
            }
            return;
        }
        // apps [list]：本机第三方应用 + 它们的策略（结果缓存 60s，pm 本身要 0.3~0.6s）
        static char cache[16384];
        static long cache_ts;
        long now2 = (long)time(NULL);
        if (!cache[0] || now2 - cache_ts > 60) {
            FILE *pp2 = popen("pm list packages -3 2>/dev/null | sed 's/^package://'", "r");
            size_t off = 0;
            cache[0] = 0;
            if (pp2) {
                char ln[256];
                while (fgets(ln, sizeof ln, pp2) && off + 200 < sizeof cache) {
                    char *nl = strchr(ln, '\n');
                    if (nl) *nl = 0;
                    if (!ln[0]) continue;
                    const apprule_t *ar = app_find(ln);
                    const char *lb = label_of(ln);
                off += (size_t)snprintf(cache + off, sizeof cache - off, "%s|%s|%d|%s\n",
                                            ln, ar ? ar->mode : "-", is_game(ln) ? 1 : 0,
                                            lb ? lb : "");
                }
                pclose(pp2);
                cache_ts = now2;
            }
            // pm 读不到（SELinux 限制 / 非 Android 环境）时，至少把已配置的应用列出来，
            // 否则 WebUI 会是一片空白，用户连自己配过什么都看不到
            if (!cache[0]) {
                size_t off2 = 0;
                for (int i = 0; i < g_napps && off2 + 200 < sizeof cache; i++)
                    { const char *lb2 = label_of(g_apps[i].pkg);
                      off2 += (size_t)snprintf(cache + off2, sizeof cache - off2, "%s|%s|%d|%s\n",
                                             g_apps[i].pkg, g_apps[i].mode,
                                             is_game(g_apps[i].pkg) ? 1 : 0, lb2 ? lb2 : ""); }
                for (int i = 0; i < g_ngames && off2 + 200 < sizeof cache; i++) {
                    if (app_find(g_games[i])) continue;
                    { const char *lb3 = label_of(g_games[i]);
                      off2 += (size_t)snprintf(cache + off2, sizeof cache - off2, "%s|%s|1|%s\n",
                                             g_games[i], "follow", lb3 ? lb3 : ""); }
                }
            }
        }
        snprintf(out, n, "%s", cache);
        return;
    }
    if (!strcmp(cmd, "labels")) {
        size_t off = 0;
        out[0] = 0;
        for (int i = 0; i < g_nlabels && off + 200 < n; i++)
            off += (size_t)snprintf(out + off, n - off, "%s=%s\n",
                                    g_labels[i].pkg, g_labels[i].name);
        return;
    }
    if (!strcmp(cmd, "icon")) {
        char ip[PATH_MAX];
        if (got < 2) { snprintf(out, n, "err=missing_pkg\n"); return; }
        if (icon_path(a1, ip, sizeof ip) != 0) { snprintf(out, n, "err=no_icon\n"); return; }
        snprintf(out, n, "ok=%s\n", ip);
        return;
    }
    if (!strcmp(cmd, "apprule")) {
        size_t off = 0;
        out[0] = 0;
        for (int i = 0; i < g_napps && off + 200 < n; i++)
            off += (size_t)snprintf(out + off, n - off, "%s|%s|%d|%d|%d|%d\n",
                                    g_apps[i].pkg, g_apps[i].mode, g_apps[i].perf,
                                    g_apps[i].prime, g_apps[i].floor, g_apps[i].threads);
        return;
    }
    if (!strcmp(cmd, "charge")) {
        // charge           -> 当前状态
        // charge probe     -> 逐个列出候选节点及可写性，用来确认这台机器开放了哪个
        size_t off = 0;
#define CAPP(...) do { if (off < n) off += (size_t)snprintf(out + off, n - off, __VA_ARGS__); } while (0)
        if (got >= 2 && !strcmp(a1, "probe")) {
            CAPP("soc=%ld temp=%ld\n", battery_cap(), battery_temp());
            char st[32] = "";
            battery_status(st, sizeof st);
            CAPP("status=%s\n", st);
            static const char *all[] = {
                "/class/power_supply/battery/constant_charge_current_max",
                "/class/power_supply/battery/constant_charge_current",
                "/class/power_supply/battery/current_max",
                "/class/power_supply/battery/input_current_limit",
                "/class/power_supply/battery/input_suspend",
                "/class/power_supply/battery/charge_disable",
                "/class/power_supply/battery/charging_enabled",
                "/class/power_supply/battery/voltage_max",
                "/class/oplus_chg/battery/current_max",
                "/class/oplus_chg/battery/chg_current_max",
                "/class/oplus_chg/battery/fastchg_current_max",
                "/class/oplus_chg/battery/input_suspend",
                "/class/oplus_chg/battery/cool_down",
                "/class/oplus_chg/battery/normal_cool_down",
                "/class/oplus_chg/battery/batt_limit",
                "/class/oplus_chg/battery/mmi_charging_enable",
                "/class/power_supply/battery/voltage_max",
            };
            for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
                char p2[PATH_MAX], v2[64];
                snprintf(p2, sizeof p2, "%s%s", P_SYS, all[i]);
                int w = access(p2, W_OK) == 0;
                int r = rdline(p2, v2, sizeof v2) == 0;
                CAPP("%s %s %s\n", all[i], r ? "read" : "----", w ? "WRITE" : "-");
                if (r) CAPP("    = %s\n", v2);
            }
            return;
        }
        charge_init();
        CAPP("enabled=%d active=%d suspended=%d last_pct=%d\n",
             C.charge_ctrl, g_chg_active, g_chg_suspended, g_chg_last_pct);
        CAPP("node=%s orig=%s\n", g_chg_cur.valid ? g_chg_cur.path : "-", g_chg_cur.orig);
        CAPP("suspend_node=%s\n", g_chg_suspend.valid ? g_chg_suspend.path : "-");
        CAPP("soc=%ld temp=%ld knee=%d wall=%d resume=%d curve=%s\n",
             battery_cap(), battery_temp(), C.charge_knee_soc, C.charge_temp_wall,
             C.charge_resume_temp, C.chg_curve);
#undef CAPP
        return;
    }
    if (!strcmp(cmd, "version")) { snprintf(out, n, "%s (%d)\n", APP_VER, APP_VC); return; }
    // 只读探测：只报告路径与可写性，不写任何系统节点、不落状态
    if (!strcmp(cmd, "probe")) {
        size_t off = 0;
#define PAPP(...) do { if (off < n) off += (size_t)snprintf(out + off, n - off, __VA_ARGS__); } while (0)
        PAPP("version=%s\n", APP_VER);
        PAPP("cgroup_root=%s\n", P_CGRP);
        PAPP("cset_node=%s\n", g_cset_node);
        PAPP("cpuctl_root=%s\n", P_CPUCTL);
        {
            char p2[PATH_MAX];
            bg_cpus_path(p2, sizeof p2);
            PAPP("bg_cpus=%s %s\n", p2, access(p2, W_OK) == 0 ? "W" : "-");
            snprintf(p2, sizeof p2, "%s/background/%s", P_CGRP, g_cset_node);
            char v2[64];
            PAPP("bg_cpus_val=%s\n", rdline(p2, v2, sizeof v2) == 0 ? v2 : "?");
            snprintf(p2, sizeof p2, "%s/top-app/cgroup.procs", P_CGRP);
            char buf2[2048];
            int got = read_all(p2, buf2, sizeof buf2);
            int lines = 0;
            if (got > 0) for (char *q = buf2; *q; q++) if (*q == '\n') lines++;
            PAPP("topapp_procs=%d read=%d\n", lines, got);
            static const char *ug[] = {"top-app", "foreground", "background"};
            for (int i = 0; i < 3; i++) {
                snprintf(p2, sizeof p2, "%s/%s/cpu.uclamp.max", P_CPUCTL, ug[i]);
                PAPP("uclamp_%s=%s\n", ug[i], access(p2, W_OK) == 0 ? "W" : "-");
            }
        }
        PAPP("refresh=%dHz\n", g_refresh);
        PAPP("hmbird_dir=%d\n", g_hb_present);
        {
            char p2[PATH_MAX], v2[64];
            snprintf(p2, sizeof p2, "%s/frame_per_sec", P_HM);
            PAPP("frame_per_sec=%s\n", rdline(p2, v2, sizeof v2) == 0 ? v2 : "?");
            snprintf(p2, sizeof p2, "%s/prefer_cpu", P_HM);
            char bb[1024];
            PAPP("prefer_cpu_entries=%s\n",
                 read_all(p2, bb, sizeof bb) > 0 ? "readable" : "unreadable");
        }
        PAPP("rules=%d\n", g_nrules);
        PAPP("fg=%s\n", ({ static char f[256]; foreground_pkg(f, sizeof f); f; }));
        PAPP("perf=%s cap=%ld/%ld\n", idx_perf >= 0 ? pol[idx_perf].path : "-",
             idx_perf >= 0 ? policy_cap_now(idx_perf) : 0,
             idx_perf >= 0 ? pol[idx_perf].max : 0);
#undef PAPP
        return;
    }
    snprintf(out, n, "err=unknown_cmd\n");
}

// ------------------------------------------------------------ 传输层：unix -
static int sock_listen(void) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", P_SOCK);
    unlink(P_SOCK);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    chmod(P_SOCK, 0666);
    return fd;
}

static void sock_handle(int fd, char *outbuf, size_t outsz) {
    char req[256] = {0};
    ssize_t r = read(fd, req, sizeof req - 1);
    if (r <= 0) { close(fd); return; }
    req[r] = 0;
    char *nl = strchr(req, '\n');
    if (nl) *nl = 0;
    if (!req[0]) { close(fd); return; }
    run_cmd(req, outbuf, outsz);
    if (!outbuf[0]) snprintf(outbuf, outsz, "ok\n");
    write(fd, outbuf, strlen(outbuf));
    close(fd);
}

// ------------------------------------------------------------ 传输层：http -
static void http_reply(int fd, const char *body, const char *ctype) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     ctype, strlen(body));
    if (write(fd, hdr, (size_t)n) < 0) { /* ignore */ }
    if (write(fd, body, strlen(body)) < 0) { /* ignore */ }
}

// 直接把磁盘上的任意文件吐回去（应用图标用）
static void http_serve_abs(int fd, const char *path, const char *ctype) {
    FILE *f = fopen(path, "rb");
    if (!f) { http_reply(fd, "err=notfound\n", "text/plain; charset=utf-8"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        http_reply(fd, "err=size\n", "text/plain; charset=utf-8");
        return;
    }
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                     "Access-Control-Allow-Origin: *\r\nCache-Control: max-age=86400\r\n"
                     "Connection: close\r\n\r\n", ctype, sz);
    if (write(fd, hdr, (size_t)n) < 0) { /* ignore */ }
    char buf[8192];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
        if (write(fd, buf, r) < 0) break;
    }
    fclose(f);
}

// 直接由 daemon 提供给浏览器打开的 WebUI（最强的兜底通道）
static void http_serve_file(int fd, const char *name) {
    if (strstr(name, "..") || strchr(name, '\\')) {
        http_reply(fd, "err=bad_path\n", "text/plain; charset=utf-8");
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", P_WEB, name);
    FILE *f = fopen(path, "rb");
    if (!f) { http_reply(fd, "err=no_webroot\n", "text/plain; charset=utf-8"); return; }
    char *buf = malloc(1 << 20);
    if (!buf) { fclose(f); http_reply(fd, "err=oom\n", "text/plain; charset=utf-8"); return; }
    size_t r = fread(buf, 1, 1 << 20, f);
    fclose(f);
    buf[r < (1 << 20) ? r : (1 << 20) - 1] = 0;

    const char *ctype = "application/octet-stream";
    if (strstr(name, ".html")) ctype = "text/html; charset=utf-8";
    else if (strstr(name, ".css")) ctype = "text/css; charset=utf-8";
    else if (strstr(name, ".js")) ctype = "application/javascript; charset=utf-8";
    else if (strstr(name, ".svg")) ctype = "image/svg+xml";

    char hdr[320];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n", ctype, r);
    if (write(fd, hdr, (size_t)n) < 0) { /* ignore */ }
    if (write(fd, buf, r) < 0) { /* ignore */ }
    free(buf);
}

static void url_decode(char *s) {
    char *d = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *d++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else if (*s == '+') {
            *d++ = ' ';
        } else {
            *d++ = *s;
        }
    }
    *d = 0;
}

static void http_handle(int fd, char *outbuf, size_t outsz) {
    char req[1024] = {0};
    ssize_t r = read(fd, req, sizeof req - 1);
    if (r <= 0) { close(fd); return; }
    req[r] = 0;
    char method[8] = "", path[512] = "";
    if (sscanf(req, "%7s %511s", method, path) != 2) { close(fd); return; }

    char *q = strchr(path, '?');
    char query[256] = "";
    if (q) { *q = 0; snprintf(query, sizeof query, "%s", q + 1); }

    char cmd[512] = "";
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) { http_serve_file(fd, "index.html"); close(fd); return; }
    if (!strcmp(path, "/style.css")) { http_serve_file(fd, "style.css"); close(fd); return; }
    if (!strcmp(path, "/app.js"))    { http_serve_file(fd, "app.js");    close(fd); return; }
    if (!strcmp(path, "/favicon.ico")) { http_reply(fd, "", "image/x-icon"); close(fd); return; }

    if (!strcmp(path, "/status"))       snprintf(cmd, sizeof cmd, "status");
    else if (!strcmp(path, "/doctor"))  snprintf(cmd, sizeof cmd, "doctor");
    else if (!strcmp(path, "/apply"))   snprintf(cmd, sizeof cmd, "apply");
    else if (!strcmp(path, "/version")) snprintf(cmd, sizeof cmd, "version");
    else if (!strcmp(path, "/log")) {
        char n[16] = "60";
        if (sscanf(query, "n=%15s", n) != 1) snprintf(n, sizeof n, "60");
        snprintf(cmd, sizeof cmd, "log %s", n);
    } else if (!strcmp(path, "/games")) {
        snprintf(cmd, sizeof cmd, "pkg list");
    } else if (!strcmp(path, "/games/add") || !strcmp(path, "/games/del")) {
        char p[128] = "";
        if (sscanf(query, "p=%127s", p) == 1) {
            url_decode(p);
            snprintf(cmd, sizeof cmd, "pkg %s %s",
                     path[7] == 'a' ? "add" : "del", p);
        } else snprintf(cmd, sizeof cmd, "err=missing_pkg");
    } else if (!strcmp(path, "/charge")) {
        snprintf(cmd, sizeof cmd, "charge%s", query && strstr(query, "probe") ? " probe" : "");
    } else if (!strcmp(path, "/labels")) {
        snprintf(cmd, sizeof cmd, "labels");
    } else if (!strcmp(path, "/icon")) {
        char pk[128] = "";
        if (sscanf(query, "p=%127s", pk) == 1) {
            url_decode(pk);
            char ip[PATH_MAX];
            if (icon_path(pk, ip, sizeof ip) == 0) {
                http_serve_abs(fd, ip, "image/png");
            } else {
                http_reply(fd, "err=no_icon\n", "text/plain; charset=utf-8");
            }
        } else {
            http_reply(fd, "err=missing_pkg\n", "text/plain; charset=utf-8");
        }
        close(fd);
        return;
    } else if (!strcmp(path, "/apps")) {
        snprintf(cmd, sizeof cmd, "apps");
    } else if (!strcmp(path, "/app")) {
        char op[16] = "", pk[128] = "", m[20] = "custom";
        int va = 100, vb = 100, vc = 0, vd = 1;
        char *q2 = query;
        while (q2 && *q2) {
            char *amp = strchr(q2, '&');
            char kv[256];
            size_t len = amp ? (size_t)(amp - q2) : strlen(q2);
            if (len >= sizeof kv) len = sizeof kv - 1;
            memcpy(kv, q2, len);
            kv[len] = 0;
            char *eq = strchr(kv, '=');
            if (eq) {
                *eq = 0;
                char *v = eq + 1;
                url_decode(v);
                if (!strcmp(kv, "op")) snprintf(op, sizeof op, "%s", v);
                else if (!strcmp(kv, "p")) snprintf(pk, sizeof pk, "%s", v);
                else if (!strcmp(kv, "m")) snprintf(m, sizeof m, "%s", v);
                else if (!strcmp(kv, "a")) va = atoi(v);
                else if (!strcmp(kv, "b")) vb = atoi(v);
                else if (!strcmp(kv, "c")) vc = atoi(v);
                else if (!strcmp(kv, "d")) vd = atoi(v);
            }
            q2 = amp ? amp + 1 : NULL;
        }
        if (!pk[0]) snprintf(cmd, sizeof cmd, "err=missing_pkg");
        else if (!strcmp(op, "del")) snprintf(cmd, sizeof cmd, "app del %s", pk);
        else snprintf(cmd, sizeof cmd, "app set %s %s %d %d %d %d", pk, m, va, vb, vc, vd);
    } else if (!strcmp(path, "/rules")) {
        FILE *f = fopen(P_RULES, "r");
        if (!f) { http_reply(fd, "", "text/plain; charset=utf-8"); close(fd); return; }
        char *b = malloc(65536);
        size_t r = b ? fread(b, 1, 65535, f) : 0;
        fclose(f);
        if (!b) { http_reply(fd, "err=oom\n", "text/plain; charset=utf-8"); close(fd); return; }
        b[r] = 0;
        http_reply(fd, b, "text/plain; charset=utf-8");
        free(b);
        close(fd);
        return;
    } else if (!strcmp(path, "/set")) {
        char k[64] = "", v[64] = "";
        if (sscanf(query, "k=%63[^&]&v=%63s", k, v) == 2) {
            url_decode(k);
            url_decode(v);
            snprintf(cmd, sizeof cmd, "set %s %s", k, v);
        } else snprintf(cmd, sizeof cmd, "err=bad_query");
    } else if (!strcmp(path, "/mode")) {
        char m[32] = "";
        if (sscanf(query, "m=%31s", m) == 1) {
            url_decode(m);
            snprintf(cmd, sizeof cmd, "mode %s", m);
        } else snprintf(cmd, sizeof cmd, "err=missing_mode");
    } else if (!strcmp(path, "/restore")) {
        restore_all();
        snprintf(cmd, sizeof cmd, "ok=restored");
    } else {
        http_reply(fd, "err=not_found\n", "text/plain; charset=utf-8");
        close(fd);
        return;
    }

    if (!strncmp(cmd, "err=", 4)) { http_reply(fd, cmd, "text/plain; charset=utf-8"); close(fd); return; }
    if (!strcmp(cmd, "apply")) {
        g_prof[0] = 0;
    }
    run_cmd(cmd, outbuf, outsz);
    http_reply(fd, outbuf, "text/plain; charset=utf-8");
    close(fd);
}

static int http_listen(void) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 只监听 127.0.0.1
    sa.sin_port = htons((uint16_t)g_http_port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// 找 top-app 里与前台包名一致的主进程 pid
static long find_game_pid(const char *pkg) {
    char path[PATH_MAX], buf[4096];
    char nl[2] = { 10, 0 };                 // 换行符，避免源码里写转义
    snprintf(path, sizeof path, "%s/top-app/cgroup.procs", P_CGRP);
    if (read_all(path, buf, sizeof buf) < 0) return -1;
    char *save = NULL;
    for (char *t = strtok_r(buf, nl, &save); t; t = strtok_r(NULL, nl, &save)) {
        long pp = strtol(t, NULL, 10);
        if (pp <= 0) continue;
        char cf[PATH_MAX], cl[256];
        snprintf(cf, sizeof cf, "%s/%ld/cmdline", P_PROC, pp);
        if (rdline(cf, cl, sizeof cl) == 0 && cl[0] && !strcmp(cl, pkg)) return pp;
    }
    return -1;
}

// --------------------------------------------------------------- daemon ----
static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static long now_s(void) { return (long)time(NULL); }

static int run_daemon(void) {
        // 单实例：多份守护进程会互相抢 socket/state，表现就是“有时连得上有时连不上”
        {
            char lockp[PATH_MAX];
            snprintf(lockp, sizeof lockp, "%s/.daemon.lock", P_DATA);
            mkdir(P_DATA, 0755);
            int lfd = open(lockp, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
            if (lfd >= 0) {
                if (flock(lfd, LOCK_EX | LOCK_NB) != 0) {
                    fprintf(stderr, "featherd-daemon: already running, exit\n");
                    return 0;
                }
                // 锁 fd 故意不关闭：进程活着锁才在
            }
        }
    mkdir(P_DATA, 0755);
    log_rotate();
    // 关键：WebUI 的 fetch 超时 / curl 提前退出都会让我们写一个已关闭的 socket，
    // 默认动作是 SIGPIPE 直接杀进程（真机上就是这个把 daemon 打成了僵尸）。
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGHUP, on_sig);

    int keep_oom = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (keep_oom >= 0) { write(keep_oom, "-1000", 5); close(keep_oom); }

    cfg_load();
    detect_paths();
    detect_topology();
    capture_syscaps();
    detect_refresh();
    hmbird_detect();
    games_load();
    rules_load();

    logmsg("daemon start %s: perf=%s(%ld) prime=%s(%ld) hmbird=%d pid=%d", APP_VER,
           idx_perf >= 0 ? strrchr(pol[idx_perf].path, '/') + 1 : "-",
           idx_perf >= 0 ? pol[idx_perf].max : 0,
           idx_prime >= 0 ? strrchr(pol[idx_prime].path, '/') + 1 : "-",
           idx_prime >= 0 ? pol[idx_prime].max : 0, g_hb_present, (int)getpid());

    if (C.kill_boost) boost_off();
    if (C.io_tune) io_tune();

    int lsock = sock_listen();
    int hsock = http_listen();
    if (hsock < 0) logmsg("http: 端口 %d 绑定失败，WebUI 只能走 ksu.exec", g_http_port);
    pid_t self = getpid();
    FILE *pf = fopen(P_PID, "w");
    if (pf) { fprintf(pf, "%d\n", (int)self); fclose(pf); }

    char outbuf[8192];
    char cur[16] = "", cand[16] = "";
    int cand_n = 0, tick = 0;
    char last_fg[256] = "";
    long burst_until = 0, last_batt = 100, start_ts = now_s();
    char last_stat[32] = "";
    const int interval_ms = 1000;

    while (!g_stop) {
        struct pollfd pfds[2];
        int nf = 0;
        if (lsock >= 0) { pfds[nf].fd = lsock; pfds[nf].events = POLLIN; pfds[nf].revents = 0; nf++; }
        if (hsock >= 0) { pfds[nf].fd = hsock; pfds[nf].events = POLLIN; pfds[nf].revents = 0; nf++; }
        int pr;
        if (nf) pr = poll(pfds, (nfds_t)nf, interval_ms);
        else { usleep(interval_ms * 1000); pr = 0; }
        if (pr > 0) {
            int served = 0;
            for (int i = 0; i < nf; i++) {
                if (!(pfds[i].revents & POLLIN)) continue;
                int c = accept(pfds[i].fd, NULL, NULL);
                if (c < 0) continue;
                if (pfds[i].fd == lsock) sock_handle(c, outbuf, sizeof outbuf);
                else http_handle(c, outbuf, sizeof outbuf);
                served = 1;
            }
            (void)served;   // 不 continue：让本拍继续走完同步与落盘
        }
        if (g_stop) break;

        // 命令里刚应用过 profile：立即同步状态，UI 不用等下一拍
        if (g_applied[0]) {
            snprintf(cur, sizeof cur, "%s", g_applied);
            snprintf(cand, sizeof cand, "%s", g_applied);
            cand_n = 99;
            g_applied[0] = 0;
            state_write(cur, last_fg, !strcmp(cur, "game"), last_batt,
                        battery_temp(), screen_on(), now_s() - start_ts);
        }

        bool changed = cfg_load();
        apps_load();
        if (!C.enable) {
            if (cur[0]) { restore_all(); cur[0] = 0; }
            continue;
        }

        long now = now_s();
        int scr = screen_on();
        char fg[256] = "";
        char want[16] = "";
        decide_now(want, sizeof want, fg, sizeof fg);
        if (scr && fg[0] && strcmp(fg, last_fg)) {
            logmsg("app: 检测到切换软件 %s -> %s", last_fg[0] ? last_fg : "(无)", fg);
            snprintf(last_fg, sizeof last_fg, "%s", fg);
            if (C.launch_boost) {
                burst_until = now + C.boost_ms / 1000;
                logmsg("app: 临时还原频率上限与调度参数 %dms（%ld%%/%ld%%，期间不压 uclamp）",
                       C.boost_ms, 100L, 92L);
            }
        }
        if (!scr) last_fg[0] = 0;
        if (tick % 5 == 0) {
            long b = battery_cap();
            last_batt = b > 0 ? b : last_batt;
            battery_status(last_stat, sizeof last_stat);
        }

        if (!strcmp(want, cand)) cand_n++;
        else { snprintf(cand, sizeof cand, "%s", want); cand_n = 1; }

        // 进游戏要快（用户已经在打了），出游戏要稳（避免一次探测抖动就把地板撤掉）
        int need = 2;
        if (!strcmp(cand, "game")) need = 1;
        else if (!strcmp(cur, "game")) need = 3;
        if (!cur[0]) need = 1;
        // 手动指定模式时不做防抖：WebUI 点一下要立刻生效，防抖只服务于 auto 识别
        if (strcmp(C.mode, "auto")) need = 1;

        if ((strcmp(cand, cur) && cand_n >= need) || (cur[0] && changed)) {
            const char *why = strcmp(cand, cur) ? "场景切换" : "配置变更";
            char prev[16];
            snprintf(prev, sizeof prev, "%s", cur[0] ? cur : "-");
            apply_profile(cand);
            logmsg("switch: %s -> %s  因%s (fg=%s, %d 拍确认, 上限 %ld/%ld)",
                   prev, cand, why, last_fg[0] ? last_fg : "-", cand_n, g_cap_perf, g_cap_prime);
            snprintf(cur, sizeof cur, "%s", cand);
            if (strcmp(cur, "game")) { g_seen_pid = -1; g_nseen = 0; }
        }

        if (C.launch_boost && cur[0] && strcmp(cur, "game") && strcmp(cur, "perf") && now < burst_until) {
            set_cap(idx_perf, 100);
            set_cap(idx_prime, 92);
        }

        if (tick % 10 == 0 && cur[0] && strcmp(cur, "game") && strcmp(cur, "perf") && g_cap_perf > 0) {
            long nowcap = policy_cap_now(idx_perf);
            if (nowcap > g_cap_perf * 105 / 100) {
                apply_profile(cur);
                logv("drift: 上限被外部改写(%ld)，已重新应用", nowcap);
            }
        }

        charge_tick();

        if (!strcmp(cur, "game") && C.thread_opt && !strcmp(C.game_mode, "aggressive")) {
            long pid = find_game_pid(last_fg);
            if (pid > 0) apply_threads(pid, last_fg);
        }

        if (tick % 2 == 0)
            state_write(cur, last_fg, !strcmp(cur, "game"), last_batt, battery_temp(), scr, now - start_ts);

        tick++;
        if (tick % 60 == 0) mkdir(P_DATA, 0755);   // 目录被清理时自愈
        if (!scr) { /* 熄屏时每 3 tick 才真正检查一次 */ }
    }

    restore_all();
    if (lsock >= 0) { close(lsock); unlink(P_SOCK); }
    if (hsock >= 0) close(hsock);
    unlink(P_PID);
    logmsg("daemon exit");
    return 0;
}

// ------------------------------------------------------------------- CLI ----
static int sock_client(const char *cmd, char *out, size_t n) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", P_SOCK);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    char line[512];
    snprintf(line, sizeof line, "%s\n", cmd);
    if (write(fd, line, strlen(line)) < 0) { close(fd); return -1; }
    size_t off = 0;
    ssize_t r;
    while (off + 1 < n && (r = read(fd, out + off, n - off - 1)) > 0) off += (size_t)r;
    out[off] = 0;
    close(fd);
    return (int)off;
}

static int cli_daemonize(void) {
    long pid = rdnum(P_PID);
    if (pid_alive(pid)) {
        printf("already running (%ld)\n", pid);
        return 0;
    }
    pid_t p = fork();
    if (p < 0) return 1;
    if (p > 0) {
        usleep(350 * 1000);
        long np = rdnum(P_PID);
        if (np > 0 && kill((pid_t)np, 0) == 0) { printf("started (%ld)\n", np); return 0; }
        printf("start failed\n");
        return 1;
    }
    setsid();
    if (chdir("/") != 0) { /* ignore */ }
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
    }
    _exit(run_daemon());
}

static int cli_main(int argc, char **argv, char *outbuf, size_t outsz) {
    if (argc < 2) { printf("usage: feather <run|start|stop|restart|status|set|mode|doctor|pkg|apps|log|restore|reset|version>\n"); return 2; }
    const char *c = argv[1];

    if (!strcmp(c, "run")) return run_daemon();
    if (!strcmp(c, "start")) return cli_daemonize();
    if (!strcmp(c, "stop")) {
        long pid = rdnum(P_PID);
        if (pid > 0) kill((pid_t)pid, SIGTERM);
        for (int i = 0; i < 20 && pid_alive(pid); i++) usleep(100 * 1000);
        if (pid > 0) kill((pid_t)pid, SIGKILL);
        unlink(P_PID);
        detect_topology(); capture_syscaps(); hmbird_detect(); restore_all();
        printf("stopped\n");
        return 0;
    }
    if (!strcmp(c, "restart")) {
        cli_main(2, (char *[]){"feather", "stop"}, outbuf, outsz);
        usleep(400 * 1000);
        return cli_main(2, (char *[]){"feather", "start"}, outbuf, outsz);
    }
    if (!strcmp(c, "restore")) {
        detect_topology(); capture_syscaps(); hmbird_detect(); restore_all();
        printf("ok=restored\n");
        return 0;
    }
    if (!strcmp(c, "reset")) {
        char src[PATH_MAX], dst[PATH_MAX];
        const char *moddir = getenv("FEATHER_MODDIR");
        if (!moddir) moddir = "/data/adb/modules/feather_tune";
        const char *files[] = {"config.ini", "games.conf", "rules.conf"};
        for (size_t i = 0; i < 3; i++) {
            snprintf(src, sizeof src, "%s/etc/%s", moddir, files[i]);
            snprintf(dst, sizeof dst, "%s/%s", P_DATA, files[i]);
            FILE *in = fopen(src, "r");
            if (!in) continue;
            FILE *o = fopen(dst, "w");
            if (o) {
                char b[4096];
                size_t r;
                while ((r = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, r, o);
                fclose(o);
            }
            fclose(in);
        }
        printf("ok=reset\n");
        return 0;
    }

    // 其余命令：拼成一行发给 daemon
    char line[512] = "";
    size_t off = 0;
    for (int i = 1; i < argc && off + 130 < sizeof line; i++)
        off += (size_t)snprintf(line + off, sizeof line - off, "%s%s", i > 1 ? " " : "", argv[i]);
    if (sock_client(line, outbuf, outsz) > 0) { fputs(outbuf, stdout); return 0; }

    // daemon 没起来：本地直接算（保证 WebUI 在守护进程挂掉时也能显示信息）
    run_cmd(line, outbuf, outsz);
    fputs(outbuf[0] ? outbuf : "err=daemon_down\n", stdout);
    return 0;
}

int main(int argc, char **argv) {
    // ps 里显示成 featherd-daemon，而不是含义模糊的 feather
    prctl(PR_SET_NAME, "featherd-daemon", 0, 0, 0);
    path_init();
    read_buildprops();
    static char outbuf[65536];
    return cli_main(argc, argv, outbuf, sizeof outbuf);
}
