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
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_VER   "v1.2.2"
#define APP_VC    10004
#define MAX_FREQ  64
#define MAX_POL   4
#define MAX_RULES 64
#define MAX_SEEN  2048
static int g_http_port = 7891;   // 可由 FEATHER_HTTP_PORT 覆盖（测试隔离端口）

// ------------------------------------------------------------------ paths --
static char P_DATA[PATH_MAX], P_CPU[PATH_MAX], P_CGRP[PATH_MAX], P_SYS[PATH_MAX],
            P_PROC[PATH_MAX], P_HM[PATH_MAX], P_CONF[PATH_MAX], P_GAMES[PATH_MAX],
            P_RULES[PATH_MAX], P_STATE[PATH_MAX], P_LOG[PATH_MAX], P_PID[PATH_MAX],
            P_SOCK[PATH_MAX];

static void path_init(void) {
    const char *e;
    snprintf(P_DATA, sizeof P_DATA, "%s", (e = getenv("FEATHER_DATA")) ? e : "/data/adb/feather");
    snprintf(P_CPU, sizeof P_CPU, "%s", (e = getenv("FEATHER_CPU_ROOT")) ? e : "/sys/devices/system/cpu");
    snprintf(P_CGRP, sizeof P_CGRP, "%s", (e = getenv("FEATHER_CGROUP_ROOT")) ? e : "/sys/fs/cgroup");
    snprintf(P_SYS, sizeof P_SYS, "%s", (e = getenv("FEATHER_SYS_ROOT")) ? e : "/sys");
    snprintf(P_PROC, sizeof P_PROC, "%s", (e = getenv("FEATHER_PROC_ROOT")) ? e : "/proc");
    snprintf(P_HM, sizeof P_HM, "%s", (e = getenv("FEATHER_HMBIRD_SYS")) ? e : "/proc/sys/hmbird_II");
    snprintf(P_CONF, sizeof P_CONF, "%s/config.ini", P_DATA);
    snprintf(P_GAMES, sizeof P_GAMES, "%s/games.conf", P_DATA);
    snprintf(P_RULES, sizeof P_RULES, "%s/rules.conf", P_DATA);
    snprintf(P_STATE, sizeof P_STATE, "%s/state", P_DATA);
    snprintf(P_LOG, sizeof P_LOG, "%s/feather.log", P_DATA);
    snprintf(P_PID, sizeof P_PID, "%s/featherd.pid", P_DATA);
    snprintf(P_SOCK, sizeof P_SOCK, "%s/control.sock", P_DATA);
}

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
    char b[512];
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
}

static char g_bg_cpus_orig[64];
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
        snprintf(p, sizeof p, "%s/background/cpuset.cpus", P_CGRP);
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
        snprintf(p, sizeof p, "%s/background/cpuset.cpus", P_CGRP);
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

static void set_uclamp(const char *group, int maxpct) {
    char path[PATH_MAX], v[32];
    snprintf(path, sizeof path, "%s/%s/cpu.uclamp.max", P_CGRP, group);
    snprintf(v, sizeof v, "max %d%%", maxpct);
    wrp(path, v);
}

// ------------------------------------------------------------------ hmbird --
static int g_hb_present, g_hb_tuned;

static void hmbird_detect(void) {
    struct stat st;
    g_hb_present = (stat(P_HM, &st) == 0 && S_ISDIR(st.st_mode));
    if (!g_hb_present) {
        const char *g = gov_of(idx_prime);
        if (strstr(g, "hmbird") || strstr(g, "scx")) g_hb_present = 1;
    }
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

static void hmbird_apply_game(void) {
    if (!g_hb_present) return;
    // 帧率窗口对齐：跟面板刷新率一致，hmbird 的帧预算才算得准（纯收益，不受 hmbird_tune 影响）
    if (C.hmbird_frame >= 0) {
        int fps = C.hmbird_frame > 0 ? C.hmbird_frame : g_refresh;
        if (fps > 0) {
            char v[16];
            snprintf(v, sizeof v, "%d", fps);
            if (wrverify(({ static char _p[PATH_MAX]; snprintf(_p, sizeof _p, "%s/frame_per_sec", P_HM); _p; }), v) == 0)
                logv("hmbird: frame_per_sec=%d", fps);
        }
    }
    if (!C.hmbird_tune) return;
    char heavy[64];
    snprintf(heavy, sizeof heavy, "%s", C.hm_heavy);
    for (char *p = heavy; *p; p++) if (*p == ',') *p = ' ';
    hm_write("coefficient", C.hm_coeff);
    hm_write("perf_high_ratio", C.hm_high);
    char res[16];
    snprintf(res, sizeof res, "%d", C.hmbird_reserved);
    hm_write("cpus_reserved", res);
    hm_write("heavy_boost", heavy);
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
    if (!g_hb_present) return;
    hm_write("coefficient", "1024,1024");
    hm_write("perf_high_ratio", "60,60");
    hm_write("cpus_reserved", "240");
    hm_write("heavy_boost", "0 400 140");
    for (int i = 0; i < npol; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/hmbird/target_loads", pol[i].path);
        wrp(p, "90");
        snprintf(p, sizeof p, "%s/hmbird/apply_freq_immediately", pol[i].path);
        wrp(p, "1");
    }
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

/* 前台包名
 * 真机 top-app 里通常同时有 systemui / 输入法 / 前台 App 的多个 pid，
 * 所以不能“第一个像包名的就返回”——要按 1) 游戏名单 2) 非系统包 3) 兜底 挑。
 * 结果写进 g_fg_source 方便 doctor 排查（1=top-app 2=dumpsys 0=失败）。 */
static char g_fg_cands[16][128];
static int  g_fg_ncand;
static int  g_fg_source;

static int collect_topapp(char *out, size_t n) {
    static const char *paths[] = { "/top-app/cgroup.procs", "/top-app/tasks" };
    g_fg_ncand = 0;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        char p[PATH_MAX], buf[4096];
        snprintf(p, sizeof p, "%s%s", P_CGRP, paths[i]);
        if (read_all(p, buf, sizeof buf) < 0) continue;
        char *save = NULL;
        for (char *t = strtok_r(buf, "\n", &save); t && g_fg_ncand < 16; t = strtok_r(NULL, "\n", &save)) {
            long pid = strtol(t, NULL, 10);
            if (pid <= 0) continue;
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
    if (!g_fg_ncand) return -1;

    // 1) 命中游戏名单的优先（这也是我们最关心的判定）
    for (int k = 0; k < g_fg_ncand; k++)
        if (is_game(g_fg_cands[k])) { snprintf(out, n, "%s", g_fg_cands[k]); return 0; }
    // 2) 跳过系统包
    for (int k = 0; k < g_fg_ncand; k++)
        if (!is_system_pkg(g_fg_cands[k])) { snprintf(out, n, "%s", g_fg_cands[k]); return 0; }
    // 3) 兜底
    snprintf(out, n, "%s", g_fg_cands[0]);
    return 0;
}

// 兜底：dumpsys（重，只在廉价路径全失败时用，且 6 秒内不重复）
static int fg_from_dumpsys(char *out, size_t n) {
    static long last_try;
    long now = (long)time(NULL);
    if (now - last_try < 6) return -1;
    last_try = now;
    FILE *pp = popen("dumpsys activity activities 2>/dev/null | grep -m1 -E 'topResumedActivity|mResumedActivity'", "r");
    if (!pp) return -1;
    char line[512];
    int ok = -1;
    if (fgets(line, sizeof line, pp)) {
        // ActivityRecord{... u0 com.xxx/.MainActivity t123}
        char *u = strstr(line, "u0 ");
        char *tok = u ? u + 3 : line;
        char *sp = strpbrk(tok, " 	}\r\n");
        char *slash = strchr(tok, '/');
        if (slash && (!sp || slash < sp)) sp = slash;
        if (sp && sp > tok) {
            *sp = 0;
            if (strchr(tok, '.')) { snprintf(out, n, "%s", tok); ok = 0; }
        }
    }
    pclose(pp);
    return ok;
}

static int foreground_pkg(char *out, size_t n) {
    if (collect_topapp(out, n) == 0) { g_fg_source = 1; return 0; }
    if (fg_from_dumpsys(out, n) == 0) { g_fg_source = 2; return 0; }
    g_fg_source = 0;
    return -1;
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
typedef struct { char pkg[128]; char pat[192]; regex_t re; int valid; const char *target; } rule_t;
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
        const char *mask = mask_of(tgt);
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
        r->target = mask;
        g_nrules++;
    }
    fclose(f);
}

// ------------------------------------------------------------- thread 亲和 --
static long g_seen[MAX_SEEN];
static int g_nseen;
static long g_seen_pid = -1;
static int g_threads_pinned;

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
        if (target) { pin_tid(tid, target); added++; }
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

    if (!strcmp(name, "game")) {
        hmbird_apply_game();
        apply_game_extras();
    } else {
        hmbird_restore();
        restore_game_extras();
    }
    if (C.gpu_tune) gpu_tune();

    snprintf(g_prof, sizeof g_prof, "%s", name);
    logv("apply: %s perf=%d%%(%ld) prime=%d%%(%ld)", name, pp, g_cap_perf, pr, g_cap_prime);
}

static void restore_all(void) {
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
    if (pid > 0 && kill((pid_t)pid, 0) == 0) alive = 1;
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
        APP("fg=%s (source=%d 0=none 1=top-app 2=dumpsys) cands=%d\n",
            fr == 0 ? fgp : "-", g_fg_source, g_fg_ncand);
        for (int k = 0; k < g_fg_ncand; k++)
            APP("  cand%d=%s%s\n", k, g_fg_cands[k], is_game(g_fg_cands[k]) ? " [game]" : "");
    }
    {
        static const char *cg_probe[] = {
            "/top-app/cgroup.procs", "/top-app/tasks",
            "/background/cpuset.cpus", "/cgroup.controllers",
        };
        APP("cgroup_root=%s\n", P_CGRP);
        for (size_t i = 0; i < sizeof cg_probe / sizeof cg_probe[0]; i++) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s%s", P_CGRP, cg_probe[i]);
            APP("  probe %s = %s\n", cg_probe[i], access(p, R_OK) == 0 ? "readable" : "MISSING");
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
        APP("selftest prefer_cluster=%s\n", wrverify(pp, v) == 0 ? "ok" : "FAIL");
        snprintf(pp, sizeof pp, "%s/prefer_cpu", P_HM);
        snprintf(v, sizeof v, "%d 0x3", me);
        APP("selftest prefer_cpu=%s\n", wrverify(pp, v) == 0 ? "ok" : "FAIL");
        snprintf(pp, sizeof pp, "%s/frame_per_sec", P_HM);
        APP("frame_per_sec_now=%s\n", rdline(pp, v, sizeof v) == 0 ? v : "n/a");
    }
    long pid = rdnum(P_PID);
    APP("daemon pid=%ld alive=%d\n", pid > 0 ? pid : 0, pid > 0 && kill((pid_t)pid, 0) == 0);
#undef APP
}

// 运行一条命令（daemon 与本地 CLI 共用）
static void run_cmd(const char *line, char *out, size_t n) {
    char cmd[64] = "", a1[128] = "", a2[128] = "", a3[128] = "";
    int got = sscanf(line, "%63s %127s %127s %127s", cmd, a1, a2, a3);
    if (got <= 0) { snprintf(out, n, "err=empty\n"); return; }

    cfg_load();
    detect_topology();
    capture_syscaps();
    detect_refresh();
    detect_paths();
    hmbird_detect();
    games_load();
    rules_load();

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
    if (!strcmp(cmd, "apps")) {
        FILE *p = popen("pm list packages -3 2>/dev/null | sed 's/^package://'", "r");
        if (!p) { snprintf(out, n, "err=pm\n"); return; }
        size_t off = 0;
        out[0] = 0;
        while (off + 128 < n && fgets(out + off, (int)(n - off), p)) off = strlen(out);
        pclose(p);
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
    if (!strcmp(cmd, "version")) { snprintf(out, n, "%s (%d)\n", APP_VER, APP_VC); return; }
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
    } else if (!strcmp(path, "/apps")) {
        snprintf(cmd, sizeof cmd, "apps");
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
    mkdir(P_DATA, 0755);
    log_rotate();
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
            snprintf(last_fg, sizeof last_fg, "%s", fg);
            if (C.launch_boost) burst_until = now + C.boost_ms / 1000;
        }
        if (!scr) last_fg[0] = 0;
        if (tick % 5 == 0) {
            long b = battery_cap();
            last_batt = b > 0 ? b : last_batt;
            battery_status(last_stat, sizeof last_stat);
        }

        if (!strcmp(want, cand)) cand_n++;
        else { snprintf(cand, sizeof cand, "%s", want); cand_n = 1; }

        int need = 2;
        if (!strcmp(cur, "game")) need = 4;
        if (!cur[0]) need = 1;
        // 手动指定模式时不做防抖：WebUI 点一下要立刻生效，防抖只服务于 auto 识别
        if (strcmp(C.mode, "auto")) need = 1;

        if ((strcmp(cand, cur) && cand_n >= need) || (cur[0] && changed)) {
            apply_profile(cand);
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

        if (!strcmp(cur, "game") && C.thread_opt) {
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
    if (pid > 0 && kill((pid_t)pid, 0) == 0) {
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
        for (int i = 0; i < 20 && pid > 0 && kill((pid_t)pid, 0) == 0; i++) usleep(100 * 1000);
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
    path_init();
    read_buildprops();
    static char outbuf[65536];
    return cli_main(argc, argv, outbuf, sizeof outbuf);
}
