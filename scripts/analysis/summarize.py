#!/usr/bin/env python3
"""Summarize all runs of one experiment job into a Markdown table + summary.csv.

Usage: summarize.py <results_dir>     (expects <results_dir>/run-*/results.json from analyze_run.py)
"""
import csv
import glob
import json
import os
import sys


def g(d, *path):
    for p in path:
        if not isinstance(d, dict) or p not in d:
            return None
        d = d[p]
    return d


def fmt(v, digits=2):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.{digits}f}"
    return str(v)


def main():
    root = sys.argv[1]
    rows = []
    for path in sorted(glob.glob(os.path.join(root, "run-*", "results.json"))):
        r = json.load(open(path))
        e = r.get("experiment", {})
        rows.append({
            "run": r["run"],
            "keys": e.get("keys"),
            "rate": e.get("rate"),
            "victims": e.get("victims") or r.get("victim"),
            "downtime_s": r.get("downtime_s"),
            "detect_kill_ms": r.get("detection_ms_kill_to_down"),
            "detect_hb_ms": r.get("detection_ms_last_heartbeat_to_down"),
            "repair_from_start_ms": g(r, "repair", "repair_ms_from_start"),
            "repair_pass_ms": g(r, "repair", "repair_ms_pass"),
            "repair_keys": g(r, "repair", "keys_fetched"),
            "repair_bytes": g(r, "repair", "bytes_fetched"),
            "avail_before": g(r, "windows", "before", "availability_pct"),
            "avail_down": g(r, "windows", "down", "availability_pct"),
            "avail_repair": g(r, "windows", "repair", "availability_pct"),
            "avail_after": g(r, "windows", "after", "availability_pct"),
            "p50_down_ms": g(r, "windows", "down", "latency_ms_p50"),
            "p99_down_ms": g(r, "windows", "down", "latency_ms_p99"),
            "p99_before_ms": g(r, "windows", "before", "latency_ms_p99"),
            "requests": g(r, "overall", "requests"),
            "acked_keys": g(r, "verify", "acked_keys"),
            "lost": g(r, "verify", "lost"),
        })
    if not rows:
        print("no results found")
        return 1
    with open(os.path.join(root, "summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"# Experiment summary: {os.path.basename(os.path.normpath(root))}\n")
    print("| run | keys | ops/s | victim(s) | down s | detect ms (kill→DOWN) | repair ms (start→done) | "
          "repair keys | avail before | avail down | avail repair | avail after | p99 before | p99 down | acked keys | lost |")
    print("|" + "---|" * 16)
    for r in rows:
        print(f"| {r['run']} | {fmt(r['keys'])} | {fmt(r['rate'])} | {fmt(r['victims'])} | {fmt(r['downtime_s'], 1)} | "
              f"{fmt(r['detect_kill_ms'])} | {fmt(r['repair_from_start_ms'])} | {fmt(r['repair_keys'])} | "
              f"{fmt(r['avail_before'], 3)}% | {fmt(r['avail_down'], 3)}% | {fmt(r['avail_repair'], 3)}% | "
              f"{fmt(r['avail_after'], 3)}% | {fmt(r['p99_before_ms'])} | {fmt(r['p99_down_ms'])} | "
              f"{fmt(r['acked_keys'])} | {fmt(r['lost'])} |")
    total_lost = sum((r["lost"] or 0) for r in rows)
    print(f"\nTotal lost acknowledged writes across all runs: **{total_lost}**")
    return 0


if __name__ == "__main__":
    sys.exit(main())
