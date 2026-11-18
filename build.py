#!/usr/bin/env python3
"""打包 FeatherTune 为可刷入 zip。

v1.4.1 起这里不只负责压缩，还负责挡住两类「刷机失败」：
  1. CRLF：Windows 下检出/编辑过的 .sh 会被原样打进 zip，Android 的 sh 在
     `then\\r` 处直接语法错误，管理器只报「安装失败」。这里统一归一化成 LF，
     并把归一化过的文件打印出来，避免再靠肉眼发现。
  2. 权限位：Python 的 ZipInfo 默认 create_system=0（MS-DOS），此时 external_attr
     里的 Unix mode 会被 Info-ZIP 忽略，解压出来 bin/feather 是 0644、不可执行。
     现在显式写 create_system=3，权限位才生效。
"""
import os, stat, sys, zipfile, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "module")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dist")

# 必须是 LF 结尾的文本类型；其余一律按二进制原样打包
TEXT_EXT = {
    ".sh", ".prop", ".ini", ".conf", ".cfg", ".js", ".css", ".html", ".htm",
    ".md", ".txt", ".json", ".yml", ".yaml", ".xml", ".toml",
}

BOM = b"\xef\xbb\xbf"


def is_text(arc: str) -> bool:
    return os.path.splitext(arc)[1].lower() in TEXT_EXT


def version():
    v, c = "v0.0.0", "0"
    with open(os.path.join(ROOT, "module.prop"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("version="):
                v = line.split("=", 1)[1].strip()
            elif line.startswith("versionCode="):
                c = line.split("=", 1)[1].strip()
    return v, c


def collect():
    files = []
    for dirpath, _, names in os.walk(ROOT):
        for n in names:
            if n.startswith(".") or n.endswith("~") or n.endswith(".bak") or n.endswith(".orig"):
                print(f"  skip  {n}")
                continue
            p = os.path.join(dirpath, n)
            arc = os.path.relpath(p, ROOT).replace("\\", "/")
            files.append((arc, p))
    files.sort(key=lambda x: (x[0] != "module.prop", x[0]))
    return files


def load(arc: str, path: str):
    """返回 (bytes, 是否被归一化)。文本类型保证 LF、无 BOM。"""
    with open(path, "rb") as f:
        data = f.read()
    if not is_text(arc):
        return data, False
    changed = False
    if data.startswith(BOM):
        data = data[len(BOM):]
        changed = True
    if b"\r\n" in data:
        data = data.replace(b"\r\n", b"\n")
        changed = True
    if b"\r" in data:  # 老 Mac 换行，Android 上同样会坏
        data = data.replace(b"\r", b"\n")
        changed = True
    return data, changed


def validate(arc: str, data: bytes):
    if arc == "module.prop":
        text = data.decode("utf-8", "replace")
        for k in ("id=", "version=", "versionCode="):
            if not any(l.startswith(k) for l in text.splitlines()):
                raise SystemExit(f"! module.prop 缺少 {k}")
    if arc.endswith(".sh") and not data.startswith(b"#!/system/bin/sh"):
        print(f"  warn  {arc} 首行不是 #!/system/bin/sh")
    if is_text(arc) and b"\r" in data:
        raise SystemExit(f"! {arc} 归一化后仍有 CR，拒绝打包")


def main():
    v, c = version()
    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, f"FeatherTune_{v}.zip")
    if os.path.exists(out):
        os.remove(out)

    files = collect()
    now = time.localtime()[:6]
    fixed, total = [], 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for arc, p in files:
            data, changed = load(arc, p)
            validate(arc, data)
            if changed:
                fixed.append(arc)
            zi = zipfile.ZipInfo(arc, date_time=now)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.create_system = 3  # Unix：只有这个值，下面的 mode 才会被解压器采用
            mode = 0o755 if (arc.endswith(".sh") or arc.startswith("bin/")) else 0o644
            zi.external_attr = (stat.S_IFREG | mode) << 16
            z.writestr(zi, data)
            total += len(data)

    size = os.path.getsize(out)
    print(f"built {out}")
    print(f"  version={v} versionCode={c}  files={len(files)}  "
          f"raw={total/1024:.0f} KiB  zip={size/1024:.1f} KiB")
    if fixed:
        print("  normalized CRLF/BOM -> LF:")
        for a in fixed:
            print("   ", a)
    else:
        print("  text files already LF (no CR found)")

    # 自检：重新读回来确认真的是 LF + Unix 权限
    with zipfile.ZipFile(out) as z:
        bad = [i.filename for i in z.infolist()
               if is_text(i.filename) and b"\r" in z.read(i.filename)]
        if bad:
            raise SystemExit(f"! 自检失败，仍然含 CR: {bad}")
        modes = {i.filename: (i.external_attr >> 16) & 0o777 for i in z.infolist()}
    print(f"  self-check ok  bin/feather mode={oct(modes.get('bin/feather', 0))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
