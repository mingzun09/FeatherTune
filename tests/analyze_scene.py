import csv, statistics as st, sys, os

A = r"D:/Hermes Agent CN Desktop/data/hermes-home/attachments/scene hp调度王者荣耀 2026-10-14 13-05-22.csv"
B = r"D:/Hermes Agent CN Desktop/data/hermes-home/attachments/羽量调教 王者荣耀 2026-11-10 23-18-30.csv"

def load(p):
    with open(p, encoding='utf-8-sig') as f:
        rows = list(csv.DictReader(f))
    out = []
    for r in rows:
        d = {}
        for k, v in r.items():
            try:
                d[k] = float(v)
            except Exception:
                d[k] = v
        out.append(d)
    return out

def num(rows, key):
    return [r[key] for r in rows if isinstance(r.get(key), float)]

def pct(vals, q):
    if not vals: return 0
    s = sorted(vals)
    i = min(len(s)-1, int(q*(len(s)-1)))
    return s[i]

def report(name, rows):
    print(f"\n===== {name}  ({len(rows)} s) =====")
    fps = num(rows,'FPS')
    print(f"FPS   mean={st.mean(fps):.2f} min={min(fps):.0f} p1={pct(fps,0.01):.0f} p5={pct(fps,0.05):.0f} p50={pct(fps,0.5):.0f}")
    jank = sum(num(rows,'JANK')); bj = sum(num(rows,'BigJANK'))
    print(f"JANK 总数={jank:.0f}  BigJANK={bj:.0f}")
    ft = num(rows,'Max FrameTime(ms)')
    # 165Hz -> 6.06ms/帧；12ms=掉1帧，18ms=掉2帧
    drops = {'<=6':0,'7-12':0,'13-18':0,'19-30':0,'>30':0}
    for v in ft:
        if v <= 6.5: drops['<=6'] += 1
        elif v <= 12.5: drops['7-12'] += 1
        elif v <= 18.5: drops['13-18'] += 1
        elif v <= 30: drops['19-30'] += 1
        else: drops['>30'] += 1
    print("Max FrameTime 分布(每秒样本):", drops, f"  mean={st.mean(ft):.2f} p99={pct(ft,0.99):.1f}")
    # 估算掉帧：以 6.06ms 为预算，MaxFT 每超出 6.06 即掉 1 帧（粗略）
    lost = sum(max(0, round(v/6.06)-1) for v in ft)
    print(f"按秒算掉的帧 ≈ {lost:.0f}  (占总量 {lost/(sum(fps)):.3%})")

    for cl, cpus in (('perf 0-5', range(0,6)), ('prime 6-7', (6,7))):
        vals = []
        for c in cpus: vals += num(rows, f'CPU{c}(MHz)')
        u = []
        for c in cpus: u += num(rows, f'CPU{c}(%)')
        print(f"{cl}: freq mean={st.mean(vals):.0f} min={min(vals):.0f} p5={pct(vals,0.05):.0f} p50={pct(vals,0.5):.0f} p95={pct(vals,0.95):.0f} max={max(vals):.0f} | util mean={st.mean(u):.1f}%")
    cur = num(rows,'Current(mA)'); pw = num(rows,'Power(mW)')
    print(f"电流 mean={st.mean(cur):.0f}mA  p95={pct(cur,0.95):.0f}  | 功率 mean={st.mean(pw):.0f}mW")
    print(f"CPU温度 mean={st.mean(num(rows,'CPU(℃)')):.1f}℃  DDR={st.mean(num(rows,'DDR(Mbps)')):.0f}Mbps  GPU={st.mean(num(rows,'GPU(%)')):.1f}%")
    for th in ('UnityMain','UnityGfxDeviceW','GfxRenderWorker'):
        k = f'{th}(cpus)'
        if rows and k in rows[0]:
            from collections import Counter
            c = Counter(r[k] for r in rows)
            top = ', '.join(f"{a}×{b}" for a,b in c.most_common(3))
            print(f"{th:16s} 落核: {top}")

a = load(A); b = load(B)
report("基准  hp调度(未装模块)", a)
report("羽量调教 v1.1 游戏档", b)
