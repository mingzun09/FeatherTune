#!/usr/bin/env python3
"""打包 FeatherTune 为可刷入 zip（保留文件权限位）"""
import os, stat, sys, zipfile, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "module")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dist")

def version():
    v, c = "v0.0.0", "0"
    with open(os.path.join(ROOT, "module.prop"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("version="):
                v = line.split("=", 1)[1].strip()
            elif line.startswith("versionCode="):
                c = line.split("=", 1)[1].strip()
    return v, c

def main():
    v, c = version()
    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, f"FeatherTune_{v}.zip")
    if os.path.exists(out):
        os.remove(out)

    files = []
    for dirpath, _, names in os.walk(ROOT):
        for n in names:
            p = os.path.join(dirpath, n)
            arc = os.path.relpath(p, ROOT).replace("\\", "/")
            files.append((arc, p))
    files.sort(key=lambda x: (x[0] != "module.prop", x[0]))

    now = time.localtime()[:6]
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for arc, p in files:
            zi = zipfile.ZipInfo(arc, date_time=now)
            zi.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if (arc.endswith(".sh") or arc.startswith("bin/")) else 0o644
            zi.external_attr = (stat.S_IFREG | mode) << 16
            with open(p, "rb") as f:
                z.writestr(zi, f.read())

    size = os.path.getsize(out)
    print(f"built {out}")
    print(f"  version={v} versionCode={c}  files={len(files)}  size={size/1024:.1f} KiB")
    for arc, _ in files:
        print("   ", arc)

if __name__ == "__main__":
    sys.exit(main())
