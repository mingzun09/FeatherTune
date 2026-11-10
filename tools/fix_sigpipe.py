import io

p = 'src/feather.c'
s = io.open(p, encoding='utf-8').read()
def rep(old, new):
    global s
    assert old in s, 'NOT FOUND: ' + old[:70]
    s = s.replace(old, new, 1)

# 1) 忽略 SIGPIPE：客户端中途断开时 write() 只返回 EPIPE，不该把守护进程打死
rep('''    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGHUP, on_sig);''',
'''    // 关键：WebUI 的 fetch 超时 / curl 提前退出都会让我们写一个已关闭的 socket，
    // 默认动作是 SIGPIPE 直接杀进程（真机上就是这个把 daemon 打成了僵尸）。
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGHUP, on_sig);''')

# 2) 僵尸视为已死：kill(pid,0) 对僵尸也返回 0，会把僵尸当成存活进程
rep('''static int g_hb_present, g_hb_tuned;''',
'''// 判断 pid 是否真的活着：僵尸（Z）虽然 kill(pid,0) 成功，但其实已经死了
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

static int g_hb_present, g_hb_tuned;''')

s = s.replace('''    if (pid > 0 && kill((pid_t)pid, 0) == 0) alive = 1;''',
              '''    if (pid_alive(pid)) alive = 1;''')
s = s.replace('''    APP("daemon pid=%ld alive=%d\\n", pid > 0 ? pid : 0, pid > 0 && kill((pid_t)pid, 0) == 0);''',
              '''    APP("daemon pid=%ld alive=%d\\n", pid > 0 ? pid : 0, pid_alive(pid));''')
s = s.replace('''    if (pid > 0 && kill((pid_t)pid, 0) == 0) {''',
              '''    if (pid_alive(pid)) {''')
s = s.replace('''        for (int i = 0; i < 20 && pid > 0 && kill((pid_t)pid, 0) == 0; i++) usleep(100 * 1000);''',
              '''        for (int i = 0; i < 20 && pid_alive(pid); i++) usleep(100 * 1000);''')

io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('SIGPIPE 忽略 + 僵尸识别')
