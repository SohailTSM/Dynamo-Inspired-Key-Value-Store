#!/usr/bin/env python3
"""Analyze one kill/restart experiment run (HLD §11 metrics).

Usage: analyze_run.py <run_dir>

Reads (all produced by the experiment scripts):
  load/requests_*.csv        per-request log from dkv_loadgen
  driver_events.jsonl        driver.kill / driver.start (experiment driver clock)
  admin/health_events.csv    admin health transitions (admin clock)
  admin/repairs.csv          REPAIR_DONE rows (node clock: process start -> done)
  load/verify_summary.json   dkv_verify result
Writes results.json, availability.svg and latency_p99.svg (pure-Python SVG, no matplotlib needed).

Windows: before = [load start, kill), down = [kill, restart), repair = [restart, repair done),
after = [repair done, load end]. A request "succeeds" if PUT returned OK or GET returned OK/NOT_FOUND.
"""
import csv
import glob
import json
import os
import statistics
import sys


def load_requests(run):
    rows = []
    for path in glob.glob(os.path.join(run, "load", "requests_*.csv")):
        with open(path) as f:
            for r in csv.DictReader(f):
                ok = r["code"] == "OK" or (r["op"] == "GET" and r["code"] == "NOT_FOUND")
                rows.append((int(r["t_start_wall_ms"]), int(r["latency_us"]), r["op"], ok, r["code"]))
    rows.sort()
    return rows


def pct(values, p):
    if not values:
        return None
    s = sorted(values)
    k = min(len(s) - 1, max(0, int(round(p / 100.0 * (len(s) - 1)))))
    return s[k]


def window_stats(rows):
    n = len(rows)
    ok = sum(1 for r in rows if r[3])
    puts = [r for r in rows if r[2] == "PUT"]
    gets = [r for r in rows if r[2] == "GET"]
    lat_ok = [r[1] / 1000.0 for r in rows if r[3]]
    codes = {}
    for r in rows:
        codes[r[4]] = codes.get(r[4], 0) + 1
    return {
        "requests": n,
        "availability_pct": round(100.0 * ok / n, 3) if n else None,
        "put_availability_pct": round(100.0 * sum(1 for r in puts if r[3]) / len(puts), 3) if puts else None,
        "get_availability_pct": round(100.0 * sum(1 for r in gets if r[3]) / len(gets), 3) if gets else None,
        "latency_ms_p50": pct(lat_ok, 50),
        "latency_ms_p95": pct(lat_ok, 95),
        "latency_ms_p99": pct(lat_ok, 99),
        "codes": codes,
    }


def svg_line_chart(path, xs, ys, marks, xlabel, ylabel, ymin=None, ymax=None, title=""):
    """Dependency-free SVG line chart (used when matplotlib is not installed, e.g. on RCE)."""
    W, H, L, R, T, B = 760, 300, 64, 20, 34, 46
    if not xs:
        return
    x0, x1 = min(xs), max(xs) if max(xs) > min(xs) else min(xs) + 1
    lo = min(ys) if ymin is None else ymin
    hi = max(ys) if ymax is None else ymax
    if hi <= lo:
        hi = lo + 1
    sx = lambda x: L + (x - x0) / (x1 - x0) * (W - L - R)
    sy = lambda y: T + (1 - (y - lo) / (hi - lo)) * (H - T - B)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" font-family="sans-serif" font-size="11">',
           f'<rect width="{W}" height="{H}" fill="white"/>',
           f'<text x="{W/2}" y="18" text-anchor="middle" font-size="13" font-weight="bold">{title}</text>']
    for i in range(6):  # y grid + labels
        y = lo + (hi - lo) * i / 5
        out.append(f'<line x1="{L}" x2="{W-R}" y1="{sy(y):.1f}" y2="{sy(y):.1f}" stroke="#e5e5e5"/>')
        out.append(f'<text x="{L-6}" y="{sy(y)+4:.1f}" text-anchor="end" fill="#555">{y:.4g}</text>')
    for i in range(7):  # x labels
        x = x0 + (x1 - x0) * i / 6
        out.append(f'<text x="{sx(x):.1f}" y="{H-B+16}" text-anchor="middle" fill="#555">{x:.0f}</text>')
    out.append(f'<line x1="{L}" x2="{W-R}" y1="{H-B}" y2="{H-B}" stroke="#999"/>')
    out.append(f'<line x1="{L}" x2="{L}" y1="{T}" y2="{H-B}" stroke="#999"/>')
    for name, t in marks:
        if t is None:
            continue
        out.append(f'<line x1="{sx(t):.1f}" x2="{sx(t):.1f}" y1="{T}" y2="{H-B}" stroke="#888" stroke-dasharray="4,3"/>')
        out.append(f'<text x="{sx(t)+3:.1f}" y="{T+10}" fill="#444">{name}</text>')
    pts = " ".join(f"{sx(x):.1f},{sy(min(max(y, lo), hi)):.1f}" for x, y in zip(xs, ys))
    out.append(f'<polyline points="{pts}" fill="none" stroke="#2a6fdb" stroke-width="1.6"/>')
    out.append(f'<text x="{(L+W-R)/2}" y="{H-8}" text-anchor="middle">{xlabel}</text>')
    out.append(f'<text x="14" y="{(T+H-B)/2}" text-anchor="middle" transform="rotate(-90 14 {(T+H-B)/2})">{ylabel}</text>')
    out.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(out))


def main():
    run = sys.argv[1]
    exp = json.load(open(os.path.join(run, "experiment.json"))) if os.path.exists(os.path.join(run, "experiment.json")) else {}
    victim = exp.get("victim")
    rows = load_requests(run)
    if not rows:
        print("no requests found")
        return 1

    kill = start = None
    for line in open(os.path.join(run, "driver_events.jsonl")):
        e = json.loads(line)
        if e["event"] == "driver.kill" and kill is None:
            kill, victim = e["t_wall_ms"], e["node"]
        elif e["event"] == "driver.start" and start is None:
            start = e["t_wall_ms"]

    down_at = detect_since_hb = None
    hp = os.path.join(run, "admin", "health_events.csv")
    if os.path.exists(hp):
        for r in csv.DictReader(open(hp)):
            if r["node"] == victim and r["to"] == "DOWN" and kill and int(r["t_wall_ms"]) >= kill and down_at is None:
                down_at = int(r["t_wall_ms"])
                detect_since_hb = down_at - int(r["last_hb_wall_ms"])

    repair = None
    rp = os.path.join(run, "admin", "repairs.csv")
    if os.path.exists(rp):
        for r in csv.DictReader(open(rp)):
            if r["node"] == victim and int(r["incarnation"]) >= 2:
                repair = {k: int(r[k]) for k in ("process_start_wall_ms", "repair_start_wall_ms", "done_wall_ms",
                                                 "repair_ms_from_start", "repair_ms_pass", "keys_fetched", "bytes_fetched")}
                break

    t0, t_end = rows[0][0], rows[-1][0]
    done = repair["done_wall_ms"] if repair else None
    bounds = [("before", t0, kill or t_end + 1), ("down", kill, start), ("repair", start, done), ("after", done, t_end + 1)]
    windows = {}
    for name, a, b in bounds:
        if a is None or b is None:
            continue
        windows[name] = window_stats([r for r in rows if a <= r[0] < b])

    verify = {}
    vp = os.path.join(run, "load", "verify_summary.json")
    if os.path.exists(vp):
        verify = json.load(open(vp))

    result = {
        "run": os.path.basename(os.path.normpath(run)),
        "experiment": exp,
        "victim": victim,
        "downtime_s": round((start - kill) / 1000.0, 3) if kill and start else None,
        "detection_ms_kill_to_down": (down_at - kill) if down_at and kill else None,
        "detection_ms_last_heartbeat_to_down": detect_since_hb,
        "repair": repair,
        "windows": windows,
        "overall": window_stats(rows),
        "verify": verify,
    }
    with open(os.path.join(run, "results.json"), "w") as f:
        json.dump(result, f, indent=2)

    print(f"victim {victim}: down {result['downtime_s']} s; detection {result['detection_ms_kill_to_down']} ms after kill "
          f"({detect_since_hb} ms after last heartbeat)")
    if repair:
        print(f"repair: {repair['repair_ms_from_start']} ms from process start ({repair['repair_ms_pass']} ms anti-entropy pass), "
              f"{repair['keys_fetched']} keys, {repair['bytes_fetched']} bytes")
    print(f"{'window':8} {'requests':>9} {'avail%':>8} {'put%':>8} {'get%':>8} {'p50ms':>7} {'p95ms':>7} {'p99ms':>7}")
    for name in ("before", "down", "repair", "after"):
        w = windows.get(name)
        if not w:
            continue
        fmt = lambda v: "-" if v is None else (f"{v:.2f}" if isinstance(v, float) else str(v))
        print(f"{name:8} {w['requests']:>9} {fmt(w['availability_pct']):>8} {fmt(w['put_availability_pct']):>8} "
              f"{fmt(w['get_availability_pct']):>8} {fmt(w['latency_ms_p50']):>7} {fmt(w['latency_ms_p95']):>7} "
              f"{fmt(w['latency_ms_p99']):>7}")
    if verify:
        print(f"verify: {verify.get('acked_keys')} acked keys, lost = {verify.get('lost')}, errors = {verify.get('errors')}")

    # Per-second availability and p99 latency charts.
    buckets = {}
    for r in rows:
        buckets.setdefault((r[0] - t0) // 1000, []).append(r)
    xs = sorted(buckets)
    avail = [100.0 * sum(1 for r in buckets[x] if r[3]) / len(buckets[x]) for x in xs]
    p99 = [pct([r[1] / 1000.0 for r in buckets[x] if r[3]], 99) or 0 for x in xs]
    rel = lambda t: None if t is None else (t - t0) / 1000.0
    marks = [("kill", rel(kill)), ("restart", rel(start)), ("repair done", rel(done))]
    title = f"{result['run']}: victim {victim}"
    svg_line_chart(os.path.join(run, "availability.svg"), xs, avail, marks, "time since load start (s)",
                   "requests succeeding (%)", ymin=min(90.0, min(avail)), ymax=100.0, title=title + " - availability")
    svg_line_chart(os.path.join(run, "latency_p99.svg"), xs, p99, marks, "time since load start (s)",
                   "p99 latency (ms)", ymin=0, title=title + " - p99 latency")
    print(f"charts: {run}/availability.svg, {run}/latency_p99.svg")
    return 0

if __name__ == "__main__":
    sys.exit(main())
