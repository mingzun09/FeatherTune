// ============================================================================
//  util.rs —— 底层 IO / 日志 / 迷你正则 / libc 绑定
//
//  C 版里 rdline/wrp/logmsg 这些是散在顶部的 static 函数；这里原样搬过来，
//  行为必须逐字对齐（测试断言直接比对输出文本和写入内容）。
// ============================================================================
#![allow(static_mut_refs)]

use std::fs;
use std::io::Write;
use std::os::unix::fs::MetadataExt;

use crate::state::st;

// ------------------------------------------------------------------ libc ----
pub type CInt = i32;
pub type CLong = i64;

#[repr(C)]
pub struct Tm {
    pub tm_sec: CInt,
    pub tm_min: CInt,
    pub tm_hour: CInt,
    pub tm_mday: CInt,
    pub tm_mon: CInt,
    pub tm_year: CInt,
    pub tm_wday: CInt,
    pub tm_yday: CInt,
    pub tm_isdst: CInt,
    pub tm_gmtoff: CLong,
    pub tm_zone: *const u8,
}

extern "C" {
    pub fn localtime_r(t: *const i64, out: *mut Tm) -> *mut Tm;
    pub fn tzset();
    pub fn setenv(k: *const u8, v: *const u8, over: CInt) -> CInt;
    pub fn getenv(k: *const u8) -> *const u8;
    pub fn prctl(option: CInt, a2: u64, a3: u64, a4: u64, a5: u64) -> CInt;
    pub fn flock(fd: CInt, op: CInt) -> CInt;
    pub fn kill(pid: CInt, sig: CInt) -> CInt;
    pub fn getpid() -> CInt;
    pub fn sched_setaffinity(pid: CInt, len: usize, mask: *const u8) -> CInt;
    pub fn fork() -> CInt;
    pub fn setsid() -> CInt;
    pub fn _exit(code: CInt) -> !;
    pub fn dup2(a: CInt, b: CInt) -> CInt;
    pub fn chdir(p: *const u8) -> CInt;
}

pub const PR_SET_NAME: CInt = 15;
pub const LOCK_EX: CInt = 2;
pub const LOCK_NB: CInt = 4;
pub const SIGTERM: CInt = 15;
pub const SIGKILL: CInt = 9;
pub const SIGINT: CInt = 2;
pub const SIGHUP: CInt = 1;
pub const SIGPIPE: CInt = 13;

pub fn cstr(s: &str) -> Vec<u8> {
    let mut v = s.as_bytes().to_vec();
    v.push(0);
    v
}

// -------------------------------------------------------------------- io ----
/// 读一行：去掉尾部换行与首尾空白。失败返回 None。
pub fn rdline(path: &str) -> Option<String> {
    match fs::read(path) {
        Ok(b) => {
            if b.is_empty() {
                return None;
            }
            let end = b.iter().position(|&c| c == b'\n').unwrap_or(b.len());
            let s = String::from_utf8_lossy(&b[..end]).to_string();
            let t = s.trim_matches(|c| c == ' ' || c == '\t' || c == '\r').to_string();
            if t.is_empty() && end == 0 {
                return None;
            }
            Some(t)
        }
        Err(_) => None,
    }
}

/// 整文件读（不按行截断）——cgroup.procs 是多行的
pub fn read_all(path: &str) -> Option<String> {
    match fs::read(path) {
        Ok(b) => {
            if b.is_empty() {
                return None;
            }
            Some(String::from_utf8_lossy(&b).to_string())
        }
        Err(_) => None,
    }
}

pub fn rdnum(path: &str) -> i64 {
    match rdline(path) {
        Some(s) => parse_leading_num(&s),
        None => -1,
    }
}

/// 等价 C 的 strtol(...,&end,10)：解析开头数字，没有数字返回 -1
pub fn parse_leading_num(s: &str) -> i64 {
    let b = s.trim_start().as_bytes();
    let mut i = 0;
    let mut neg = false;
    if i < b.len() && (b[i] == b'-' || b[i] == b'+') {
        neg = b[i] == b'-';
        i += 1;
    }
    let start = i;
    let mut v: i64 = 0;
    while i < b.len() && b[i].is_ascii_digit() {
        v = v.saturating_mul(10).saturating_add((b[i] - b'0') as i64);
        i += 1;
    }
    if i == start {
        return -1;
    }
    if neg {
        -v
    } else {
        v
    }
}

pub fn wrp(path: &str, value: &str) -> bool {
    // 必须 truncate：C 版用的是 O_WRONLY|O_TRUNC，少了它「170 → 0」会变成「070」
    let mut f = match fs::OpenOptions::new()
        .write(true)
        .truncate(true)
        .open(path)
    {
        Ok(f) => f,
        Err(_) => return false,
    };
    if f.write_all(value.as_bytes()).is_err() {
        return false;
    }
    let _ = f.flush();
    if let Ok(trace) = std::env::var("FEATHER_WR_TRACE") {
        if !trace.is_empty() {
            if let Ok(mut tf) = fs::OpenOptions::new().create(true).append(true).open(&trace) {
                let _ = writeln!(tf, "{} <- {}", path, value);
            }
        }
    }
    true
}

pub fn wrl(path: &str, v: i64) -> bool {
    wrp(path, &v.to_string())
}

pub fn wrverify(path: &str, value: &str) -> bool {
    if !wrp(path, value) {
        return false;
    }
    match rdline(path) {
        Some(b) => b.contains(value),
        None => false,
    }
}

pub fn exists(path: &str) -> bool {
    fs::metadata(path).is_ok()
}

pub fn writable(path: &str) -> bool {
    // access(W_OK) 的语义：文件存在且当前用户可写
    if let Ok(md) = fs::metadata(path) {
        md.is_file() && is_writable_fd(path)
    } else {
        false
    }
}

fn is_writable_fd(path: &str) -> bool {
    use std::os::unix::fs::OpenOptionsExt;
    fs::OpenOptions::new()
        .write(true)
        .custom_flags(0)
        .open(path)
        .is_ok()
}

pub fn readable(path: &str) -> bool {
    fs::read(path).is_ok()
}

pub fn is_dir(path: &str) -> bool {
    fs::metadata(path).map(|m| m.is_dir()).unwrap_or(false)
}

pub fn mtime(path: &str) -> (i64, i64) {
    match fs::metadata(path) {
        Ok(m) => (m.mtime(), m.mtime_nsec()),
        Err(_) => (0, 0),
    }
}

// ------------------------------------------------------------------- log ----
pub fn now_s() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

pub fn ts_string() -> String {
    let t = now_s();
    let mut tm = Tm {
        tm_sec: 0,
        tm_min: 0,
        tm_hour: 0,
        tm_mday: 0,
        tm_mon: 0,
        tm_year: 0,
        tm_wday: 0,
        tm_yday: 0,
        tm_isdst: 0,
        tm_gmtoff: 0,
        tm_zone: std::ptr::null(),
    };
    unsafe {
        localtime_r(&t, &mut tm);
    }
    format!(
        "{:02}-{:02} {:02}:{:02}:{:02}",
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec
    )
}

pub fn logmsg(msg: &str) {
    let line = format!("{} {}\n", ts_string(), msg);
    if let Ok(mut f) = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&st().p.log)
    {
        let _ = f.write_all(line.as_bytes());
    }
}

pub fn logv(msg: &str) {
    if st().log_level < 2 {
        return;
    }
    logmsg(msg);
}

pub fn log_rotate() {
    let p = st().p.log.clone();
    if let Ok(md) = fs::metadata(&p) {
        if md.len() > 256 * 1024 {
            let _ = fs::rename(&p, format!("{}.1", p));
        }
    }
}

/// 日志时间戳：/data/adb 里的进程没有 TZ，localtime 会按 UTC 打，比手机时间差 8 小时
pub fn init_tz() {
    unsafe {
        if !getenv(cstr("TZ").as_ptr()).is_null() {
            return;
        }
    }
    if let Some(tz) = rdline("/data/property/persist.sys.timezone") {
        if !tz.is_empty() && tz.contains('/') {
            unsafe {
                setenv(cstr("TZ").as_ptr(), cstr(&tz).as_ptr(), 1);
                tzset();
            }
        }
    }
}

// ------------------------------------------------------------- 进程/线程 ----
/// 判断 pid 是否真的活着：僵尸（Z）虽然 kill(pid,0) 成功，但其实已经死了
pub fn pid_alive(pid: i64) -> bool {
    if pid <= 0 {
        return false;
    }
    let p = format!("/proc/{}/stat", pid);
    match fs::read(&p) {
        Ok(b) => {
            let s = String::from_utf8_lossy(&b).to_string();
            if let Some(pos) = s.rfind(')') {
                let rest = &s[pos + 1..];
                if let Some(ch) = rest.trim_start().chars().next() {
                    return ch != 'Z' && ch != 'X';
                }
            }
            false
        }
        Err(_) => unsafe { kill(pid as CInt, 0) == 0 },
    }
}

/// 设线程亲和：掩码按位 0..63
pub fn set_affinity(tid: i64, mask: u64) -> bool {
    let mut set = [0u8; 128];
    for i in 0..64u32 {
        if mask & (1u64 << i) != 0 {
            set[(i / 8) as usize] |= 1 << (i % 8);
        }
    }
    unsafe { sched_setaffinity(tid as CInt, 128, set.as_ptr()) == 0 }
}

pub fn popcount(m: u64) -> i32 {
    m.count_ones() as i32
}

pub fn parse_hex(s: &str) -> u64 {
    let t = s.trim();
    let t = if let Some(r) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        r
    } else {
        t
    };
    u64::from_str_radix(t, 16).unwrap_or(0)
}

pub fn atoi_safe(s: &str, def: i32) -> i32 {
    let t = s.trim();
    if t.is_empty() {
        return def;
    }
    let v = parse_leading_num(t);
    if v == -1 && !t.starts_with('-') {
        return def;
    }
    v as i32
}

// --------------------------------------------------------------- 迷你正则 ----
// rules.conf 用的是 POSIX ERE 的子集：^ $ . * + ? [...] | \x 和字面空格。
// 标准库没有正则，这里实现一个够用的回溯匹配器（不引入外部 crate，
// 这样 Android 端交叉编译完全离线、二进制也更小）。
#[derive(Clone, Debug)]
pub enum Node {
    Lit(u8),
    Any,
    Class { neg: bool, items: Vec<(u8, u8)> },
    Start,
    End,
    Quant(Box<Node>, u8), // 1='*' 2='+' 3='?'
}

#[derive(Clone, Debug)]
pub struct Seq {
    pub nodes: Vec<Node>,
}

#[derive(Clone, Debug, Default)]
pub struct Regex {
    pub alts: Vec<Seq>,
    pub valid: bool,
}

impl Regex {
    pub fn new(pat: &str) -> Regex {
        let mut re = Regex {
            alts: Vec::new(),
            valid: false,
        };
        // 顶层按未转义、不在 [] 内的 '|' 切分支
        let b = pat.as_bytes();
        let mut branches: Vec<&[u8]> = Vec::new();
        let mut i = 0;
        let mut start = 0;
        let mut in_class = false;
        while i < b.len() {
            match b[i] {
                b'\\' => i += 1,
                b'[' => in_class = true,
                b']' => in_class = false,
                b'|' if !in_class => {
                    branches.push(&b[start..i]);
                    start = i + 1;
                }
                _ => {}
            }
            i += 1;
        }
        branches.push(&b[start..]);

        for br in branches {
            if let Some(seq) = parse_seq(br) {
                re.alts.push(seq);
            }
        }
        re.valid = !re.alts.is_empty();
        re
    }

    pub fn is_match(&self, text: &str) -> bool {
        if !self.valid {
            return false;
        }
        let t = text.as_bytes();
        for seq in &self.alts {
            let anchored_start = seq
                .nodes
                .first()
                .map(|n| matches!(n, Node::Start))
                .unwrap_or(false);
            if anchored_start {
                if match_nodes(&seq.nodes[1..], t, 0) {
                    return true;
                }
            } else {
                for s in 0..=t.len() {
                    if match_nodes(&seq.nodes, t, s) {
                        return true;
                    }
                }
            }
        }
        false
    }
}

fn parse_seq(b: &[u8]) -> Option<Seq> {
    let mut nodes: Vec<Node> = Vec::new();
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        let atom: Node = match c {
            b'\\' => {
                i += 1;
                if i >= b.len() {
                    Node::Lit(b'\\')
                } else {
                    let n = Node::Lit(b[i]);
                    i += 1;
                    nodes_push(&mut nodes, n, 0);
                    continue;
                }
            }
            b'.' => {
                i += 1;
                Node::Any
            }
            b'^' if nodes.is_empty() => {
                i += 1;
                Node::Start
            }
            b'$' if i + 1 == b.len() => {
                i += 1;
                Node::End
            }
            b'[' => {
                let (node, ni) = parse_class(b, i)?;
                i = ni;
                node
            }
            _ => {
                i += 1;
                Node::Lit(c)
            }
        };
        // 量词
        let mut q = 0;
        if i < b.len() {
            match b[i] {
                b'*' => {
                    q = 1;
                    i += 1;
                }
                b'+' => {
                    q = 2;
                    i += 1;
                }
                b'?' => {
                    q = 3;
                    i += 1;
                }
                _ => {}
            }
        }
        nodes_push(&mut nodes, atom, q);
    }
    Some(Seq { nodes })
}

fn nodes_push(nodes: &mut Vec<Node>, n: Node, q: u8) {
    if q == 0 {
        nodes.push(n);
    } else {
        nodes.push(Node::Quant(Box::new(n), q));
    }
}

fn parse_class(b: &[u8], mut i: usize) -> Option<(Node, usize)> {
    // b[i] == '['
    i += 1;
    let mut neg = false;
    if i < b.len() && (b[i] == b'^') {
        neg = true;
        i += 1;
    }
    let mut items: Vec<(u8, u8)> = Vec::new();
    while i < b.len() && b[i] != b']' {
        let lo = if b[i] == b'\\' && i + 1 < b.len() {
            i += 1;
            b[i]
        } else {
            b[i]
        };
        i += 1;
        if i + 1 < b.len() && b[i] == b'-' && b[i + 1] != b']' {
            let hi = b[i + 1];
            i += 2;
            items.push((lo, hi));
        } else {
            items.push((lo, lo));
        }
    }
    if i < b.len() {
        i += 1; // 跳过 ']'
    }
    Some((Node::Class { neg, items }, i))
}

fn one(n: &Node, t: &[u8], pos: usize) -> Option<usize> {
    match n {
        Node::Lit(c) => {
            if pos < t.len() && t[pos] == *c {
                Some(pos + 1)
            } else {
                None
            }
        }
        Node::Any => {
            if pos < t.len() {
                Some(pos + 1)
            } else {
                None
            }
        }
        Node::Class { neg, items } => {
            if pos >= t.len() {
                return None;
            }
            let c = t[pos];
            let hit = items.iter().any(|&(lo, hi)| c >= lo && c <= hi);
            if hit != *neg {
                Some(pos + 1)
            } else {
                None
            }
        }
        Node::Start => {
            if pos == 0 {
                Some(pos)
            } else {
                None
            }
        }
        Node::End => {
            if pos == t.len() {
                Some(pos)
            } else {
                None
            }
        }
        Node::Quant(..) => None, // 不会出现在这里
    }
}

fn match_nodes(nodes: &[Node], t: &[u8], pos: usize) -> bool {
    if nodes.is_empty() {
        return true;
    }
    let n = &nodes[0];
    let rest = &nodes[1..];
    match n {
        Node::Quant(inner, q) => {
            // 贪心收集所有可达位置（positions[k] = 吃掉 k 个之后的位置）
            let mut positions = vec![pos];
            let mut cur = pos;
            let max = if *q == 3 { 1 } else { usize::MAX };
            while positions.len() <= max {
                match one(inner, t, cur) {
                    Some(np) if np != cur => {
                        positions.push(np);
                        cur = np;
                    }
                    _ => break,
                }
            }
            let min = if *q == 2 { 1 } else { 0 };
            let mut k = positions.len();
            while k > min {
                k -= 1;
                if match_nodes(rest, t, positions[k]) {
                    return true;
                }
            }
            false
        }
        _ => match one(n, t, pos) {
            Some(np) => match_nodes(rest, t, np),
            None => false,
        },
    }
}

// --------------------------------------------------------------- 其它杂项 ----
pub fn mkdir_p(path: &str) {
    let _ = fs::create_dir_all(path);
}

/// 把掩码串写成 "0x.." 之外的原样输出（C 版直接透传）
// ------------------------------------------------------- 外部命令（popen） ----
// 为什么不用 std::process::Command：它走 posix_spawn（clone + CLONE_VFORK），
// 在 qemu-user 下子进程根本不会 exec（arm64 回归会整片假失败）；popen 走 fork+exec，
// 真机与 qemu 都稳。C 版原本就是 popen，这里也保持一致。
#[repr(C)]
pub struct CFile {
    _priv: [u8; 0],
}

extern "C" {
    fn popen(cmd: *const u8, mode: *const u8) -> *mut CFile;
    fn pclose(f: *mut CFile) -> CInt;
    fn fread(ptr: *mut u8, size: usize, n: usize, stream: *mut CFile) -> usize;
}

/// 跑一条 shell 命令，返回 (stdout 全部, 退出码)；退出码 -1 表示 popen/pclose 失败
pub fn popen_capture(cmd: &str) -> Option<(String, i32)> {
    let c = cstr(cmd);
    let mode = cstr("r");
    let fp = unsafe { popen(c.as_ptr(), mode.as_ptr()) };
    if fp.is_null() {
        return None;
    }
    let mut buf: Vec<u8> = Vec::new();
    let mut chunk = [0u8; 8192];
    loop {
        let n = unsafe { fread(chunk.as_mut_ptr(), 1, chunk.len(), fp) };
        if n == 0 {
            break;
        }
        buf.extend_from_slice(&chunk[..n]);
        if buf.len() >= 1024 * 1024 {
            break; // 防御：异常输出不要把内存吃光
        }
    }
    let st = unsafe { pclose(fp) };
    // pclose 返回的是 wait status：低 7 位是信号，高 8 位才是退出码。
    // 被 timeout 杀掉时低 7 位非 0，不能当成「成功 0」。
    let code = if st < 0 {
        -1
    } else if st & 0x7f != 0 {
        128 + (st & 0x7f)
    } else {
        (st >> 8) & 0xff
    };
    Some((String::from_utf8_lossy(&buf).to_string(), code))
}

/// 跑外部命令并取第一行（带 timeout 包装，避免卡死守护进程）
pub fn popen_first_line(cmd: &str, secs: i32) -> Option<String> {
    let full = if secs > 0 {
        format!("timeout {} {}", secs, cmd)
    } else {
        cmd.to_string()
    };
    let (s, _) = popen_capture(&full)?;
    let first = s.lines().next().unwrap_or("").to_string();
    if first.is_empty() {
        None
    } else {
        Some(first)
    }
}

/// 整段输出（多行）
pub fn popen_all(cmd: &str, secs: i32) -> Option<String> {
    let full = if secs > 0 {
        format!("timeout {} {}", secs, cmd)
    } else {
        cmd.to_string()
    };
    popen_capture(&full).map(|(s, _)| s)
}

#[allow(dead_code)]
pub fn run_status(cmd: &str) -> i32 {
    match popen_capture(cmd) {
        Some((_, code)) => code,
        None => -1,
    }
}

#[allow(dead_code)]
pub fn chmod(path: &str, mode: u32) {
    use std::os::unix::fs::PermissionsExt;
    let _ = fs::set_permissions(path, fs::Permissions::from_mode(mode));
}

pub fn is_alnum(c: u8) -> bool {
    c.is_ascii_alphanumeric()
}
