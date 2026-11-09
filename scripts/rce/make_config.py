#!/usr/bin/env python3
"""Generates the cluster config + process placement for the hosts SLURM gave us (HLD §10, D10).

Usage: make_config.py --hosts "node03 node04 ..." --out cluster.json --placement placement.json
                      --data-dir /tmp/$USER/dkv-<job>/${DKV_RUN_TAG} [--storage N]

Rule: hosts[0] always runs metadata + auto-admin + the experiment driver, so kill timestamps and the
admin's DOWN timestamps come from the same clock (D9).
  >= 8 hosts: hosts[1..5] storage A-E, hosts[6] router, hosts[7] loadgen   (proposal layout)
  4..7 hosts: one storage node per host (A on hosts[0] too), router on hosts[1], loadgen on hosts[2]
"""
import argparse
import json


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hosts", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--placement", required=True)
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--storage", type=int, default=0, help="number of storage nodes (default: by layout)")
    ap.add_argument("--stamping", default="READ_BEFORE_WRITE")
    a = ap.parse_args()
    hosts = a.hosts.split()
    if len(hosts) < 4:
        raise SystemExit("need at least 4 hosts")
    ids = "ABCDEFGH"
    if len(hosts) >= 8:
        n = a.storage or 5
        storage_hosts = hosts[1:1 + n]
        router_host, loadgen_host = hosts[6], hosts[7]
    else:
        n = a.storage or len(hosts)
        storage_hosts = hosts[:n]
        router_host, loadgen_host = hosts[1], hosts[2]
    control = hosts[0]
    cfg = {
        "clusterName": "dkv-rce",
        "storageNodes": [{"id": ids[i], "host": h, "port": 17001 + i} for i, h in enumerate(storage_hosts)],
        "routers": [{"id": "R1", "host": router_host, "port": 17100}],
        "metadata": {"id": "M", "host": control, "port": 17200},
        "admin": {"id": "ADM", "host": control, "port": 17300},
        "quorum": {"n": 3, "r": 2, "w": 2},
        "vnodesPerNode": 64,
        "timing": {"heartbeatIntervalMs": 1000, "suspectAfterMissed": 1, "downAfterMissed": 3,
                   "detectorTickMs": 100, "coordTimeoutMs": 500, "routerTimeoutMs": 1000,
                   "antiEntropyIntervalS": 30, "ringPollIntervalS": 5},
        "storage": {"dataDir": a.data_dir, "segmentBytes": 67108864},
        "policy": {"resolution": "LWW_ON_CONCURRENT", "stamping": a.stamping},
    }
    placement = {
        "control": control,
        "router": {"R1": router_host},
        "loadgen": loadgen_host,
        "storage": {ids[i]: h for i, h in enumerate(storage_hosts)},
    }
    json.dump(cfg, open(a.out, "w"), indent=2)
    json.dump(placement, open(a.placement, "w"), indent=2)
    print(json.dumps(placement))


if __name__ == "__main__":
    main()
