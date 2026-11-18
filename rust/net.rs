// ============================================================================
//  net.rs —— UNIX socket 控制口 + 127.0.0.1 HTTP 桥
// ============================================================================
#![allow(static_mut_refs)]

use std::fs;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};

use crate::cmd::{icon_get, run_cmd};
use crate::cpu::restore_all;
use crate::state::st;
use crate::util::*;

// ------------------------------------------------------------------ poll ----
#[repr(C)]
pub struct PollFd {
    pub fd: i32,
    pub events: i16,
    pub revents: i16,
}

extern "C" {
    fn poll(fds: *mut PollFd, nfds: u64, timeout: i32) -> i32;
}

pub const POLLIN: i16 = 1;

pub fn sock_listen() -> Option<UnixListener> {
    let p = st().p.sock.clone();
    let _ = fs::remove_file(&p);
    let l = UnixListener::bind(&p).ok()?;
    let _ = fs::set_permissions(&p, fs::Permissions::from_mode(0o666));
    Some(l)
}

pub fn http_listen() -> Option<TcpListener> {
    let port = st().http_port;
    match TcpListener::bind(("127.0.0.1", port as u16)) {
        Ok(l) => Some(l),
        Err(e) => {
            st().http_errno = e.raw_os_error().unwrap_or(-1);
            None
        }
    }
}

/// 等可读事件；返回 (unix 可读, http 可读)
pub fn wait_readable(unix: Option<i32>, http: Option<i32>, timeout_ms: i32) -> (bool, bool) {
    let mut fds: Vec<PollFd> = Vec::new();
    if let Some(fd) = unix {
        fds.push(PollFd {
            fd,
            events: POLLIN,
            revents: 0,
        });
    }
    if let Some(fd) = http {
        fds.push(PollFd {
            fd,
            events: POLLIN,
            revents: 0,
        });
    }
    if fds.is_empty() {
        std::thread::sleep(std::time::Duration::from_millis(timeout_ms as u64));
        return (false, false);
    }
    let n = unsafe { poll(fds.as_mut_ptr(), fds.len() as u64, timeout_ms) };
    if n <= 0 {
        return (false, false);
    }
    let mut u = false;
    let mut h = false;
    for f in fds.iter() {
        if f.revents & POLLIN == 0 {
            continue;
        }
        if Some(f.fd) == unix {
            u = true;
        } else {
            h = true;
        }
    }
    (u, h)
}

// ------------------------------------------------------------ unix 协议 ----
pub fn sock_handle(s: UnixStream) {
    let mut s = s;
    let mut req = [0u8; 256];
    let n = match s.read(&mut req) {
        Ok(n) if n > 0 => n,
        _ => return,
    };
    let line = String::from_utf8_lossy(&req[..n]).to_string();
    let cmd = line.lines().next().unwrap_or("").trim().to_string();
    if cmd.is_empty() {
        return;
    }
    let mut out = run_cmd(&cmd);
    if out.is_empty() {
        out = "ok\n".to_string();
    }
    let _ = s.write_all(out.as_bytes());
}

// ------------------------------------------------------------------ http ----
fn http_reply(s: &mut TcpStream, body: &str, ctype: &str) {
    let hdr = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\n\
         Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        ctype,
        body.len()
    );
    let _ = s.write_all(hdr.as_bytes());
    let _ = s.write_all(body.as_bytes());
}

/// 图标缓存文件没有扩展名（PNG / WebP / JPEG 都可能有），按魔数决定 Content-Type。
/// 现代 APK 大量用 WebP 替代 PNG，写死 image/png 会让一部分浏览器/WebView 显示空白。
fn img_ctype(b: &[u8]) -> &'static str {
    if b.len() > 8 && &b[0..4] == b"\x89PNG" {
        "image/png"
    } else if b.len() > 12 && &b[0..4] == b"RIFF" && &b[8..12] == b"WEBP" {
        "image/webp"
    } else if b.len() > 3 && b[0] == 0xFF && b[1] == 0xD8 {
        "image/jpeg"
    } else if b.len() > 6 && &b[0..3] == b"GIF" {
        "image/gif"
    } else {
        "application/octet-stream"
    }
}

fn http_serve_abs(s: &mut TcpStream, path: &str, ctype: &str) {
    match fs::read(path) {
        Ok(b) if !b.is_empty() && b.len() <= 4 * 1024 * 1024 => {
            let ct = if ctype == "image/auto" {
                img_ctype(&b)
            } else {
                ctype
            };
            let hdr = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\n\
                 Access-Control-Allow-Origin: *\r\nCache-Control: max-age=3600\r\nConnection: close\r\n\r\n",
                ct,
                b.len()
            );
            let _ = s.write_all(hdr.as_bytes());
            let _ = s.write_all(&b);
        }
        _ => http_reply(s, "err=notfound\n", "text/plain; charset=utf-8"),
    }
}

fn http_serve_file(s: &mut TcpStream, name: &str) {
    if name.contains("..") || name.contains('\\') {
        http_reply(s, "err=bad_path\n", "text/plain; charset=utf-8");
        return;
    }
    let path = format!("{}/{}", st().p.web, name);
    let b = match fs::read(&path) {
        Ok(b) => b,
        Err(_) => {
            http_reply(s, "err=no_webroot\n", "text/plain; charset=utf-8");
            return;
        }
    };
    let ctype = if name.contains(".html") {
        "text/html; charset=utf-8"
    } else if name.contains(".css") {
        "text/css; charset=utf-8"
    } else if name.contains(".js") {
        "application/javascript; charset=utf-8"
    } else if name.contains(".svg") {
        "image/svg+xml"
    } else {
        "application/octet-stream"
    };
    let hdr = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\n\
         Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        ctype,
        b.len()
    );
    let _ = s.write_all(hdr.as_bytes());
    let _ = s.write_all(&b);
}

fn url_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out: Vec<u8> = Vec::new();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() && b[i + 1].is_ascii_hexdigit() && b[i + 2].is_ascii_hexdigit()
        {
            let h = std::str::from_utf8(&b[i + 1..i + 3]).unwrap_or("00");
            out.push(u8::from_str_radix(h, 16).unwrap_or(0));
            i += 3;
        } else if b[i] == b'+' {
            out.push(b' ');
            i += 1;
        } else {
            out.push(b[i]);
            i += 1;
        }
    }
    String::from_utf8_lossy(&out).to_string()
}

fn query_get(query: &str, key: &str) -> Option<String> {
    for kv in query.split('&') {
        if let Some(eq) = kv.find('=') {
            if &kv[..eq] == key {
                return Some(url_decode(&kv[eq + 1..]));
            }
        }
    }
    None
}

pub fn http_handle(mut s: std::net::TcpStream) {
    let mut req = [0u8; 1024];
    let n = match s.read(&mut req) {
        Ok(n) if n > 0 => n,
        _ => return,
    };
    let text = String::from_utf8_lossy(&req[..n]).to_string();
    let first = text.lines().next().unwrap_or("");
    let mut it = first.split_whitespace();
    let _method = it.next().unwrap_or("");
    let rawpath = match it.next() {
        Some(p) => p.to_string(),
        None => return,
    };
    let (path, query) = match rawpath.find('?') {
        Some(i) => (rawpath[..i].to_string(), rawpath[i + 1..].to_string()),
        None => (rawpath.clone(), String::new()),
    };

    match path.as_str() {
        "/" | "/index.html" => return http_serve_file(&mut s, "index.html"),
        "/style.css" => return http_serve_file(&mut s, "style.css"),
        "/app.js" => return http_serve_file(&mut s, "app.js"),
        "/favicon.ico" => return http_reply(&mut s, "", "image/x-icon"),
        _ => {}
    }

    let cmd: String;
    match path.as_str() {
        "/status" => cmd = "status".into(),
        "/doctor" => cmd = "doctor".into(),
        "/apply" => cmd = "apply".into(),
        "/version" => cmd = "version".into(),
        "/log" => {
            let n = query_get(&query, "n").unwrap_or_else(|| "60".into());
            cmd = format!("log {}", n);
        }
        "/games" => cmd = "pkg list".into(),
        "/games/add" | "/games/del" => {
            cmd = match query_get(&query, "p") {
                Some(p) => format!(
                    "pkg {} {}",
                    if path.ends_with("add") { "add" } else { "del" },
                    p
                ),
                None => "err=missing_pkg".into(),
            }
        }
        "/charge" => {
            cmd = if query.contains("probe") {
                "charge probe".into()
            } else {
                "charge".into()
            }
        }
        "/labels" => cmd = "labels".into(),
        "/icon" => {
            return match query_get(&query, "p") {
                Some(pk) => {
                    let force = query_get(&query, "r")
                        .map(|v| v == "1" || v == "force")
                        .unwrap_or(false);
                    match icon_get(&pk, force) {
                        Some(ip) => http_serve_abs(&mut s, &ip, "image/auto"),
                        None => http_reply(&mut s, "err=no_icon\n", "text/plain; charset=utf-8"),
                    }
                }
                None => http_reply(&mut s, "err=missing_pkg\n", "text/plain; charset=utf-8"),
            };
        }
        "/apps" => cmd = "apps".into(),
        "/app" => {
            let pk = query_get(&query, "p").unwrap_or_default();
            let op = query_get(&query, "op").unwrap_or_default();
            if pk.is_empty() {
                cmd = "err=missing_pkg".into();
            } else if op == "del" {
                cmd = format!("app del {}", pk);
            } else {
                let m = query_get(&query, "m").unwrap_or_else(|| "custom".into());
                let va = query_get(&query, "a")
                    .map(|v| atoi_safe(&v, 100))
                    .unwrap_or(100);
                let vb = query_get(&query, "b")
                    .map(|v| atoi_safe(&v, 100))
                    .unwrap_or(100);
                let vc = query_get(&query, "c").map(|v| atoi_safe(&v, 0)).unwrap_or(0);
                let vd = query_get(&query, "d").map(|v| atoi_safe(&v, 1)).unwrap_or(1);
                cmd = format!("app set {} {} {} {} {} {}", pk, m, va, vb, vc, vd);
            }
        }
        "/rules" => {
            let text = fs::read_to_string(&st().p.rules).unwrap_or_default();
            return http_reply(&mut s, &text, "text/plain; charset=utf-8");
        }
        "/set" => {
            let k = query_get(&query, "k").unwrap_or_default();
            let v = query_get(&query, "v").unwrap_or_default();
            cmd = if k.is_empty() || v.is_empty() {
                "err=bad_query".into()
            } else {
                format!("set {} {}", k, v)
            };
        }
        "/mode" => {
            cmd = match query_get(&query, "m") {
                Some(m) => format!("mode {}", m),
                None => "err=missing_mode".into(),
            };
        }
        "/restore" => {
            restore_all();
            cmd = "ok=restored".into();
        }
        _ => {
            http_reply(&mut s, "err=not_found\n", "text/plain; charset=utf-8");
            return;
        }
    }

    if cmd.starts_with("err=") {
        http_reply(&mut s, &cmd, "text/plain; charset=utf-8");
        return;
    }
    let out = run_cmd(&cmd);
    http_reply(&mut s, &out, "text/plain; charset=utf-8");
}
