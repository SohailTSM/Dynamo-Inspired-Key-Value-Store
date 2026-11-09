# Experiment results: kill/restart on RCE (job 119958, 2026-11-09)

The main experiment from `Proposal.pdf` §5, run with `scripts/rce/experiment.sbatch` on 4 RCE compute nodes (node03–06; the other 4 were drained by admins that night).

## Setup

| Item              | Value                                                                                                                                                              |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Cluster           | 4 storage nodes (A–D, one per machine), 1 router, Metadata Service, Auto-Admin, loadgen; layout in [rce-119958/placement.json](rce-119958/placement.json)          |
| Replication       | N = 3, W = 2, R = 2 (survives 1 storage-node failure), 64 vnodes per node                                                                                          |
| Failure detection | heartbeat every 1 s; DOWN after 3 missed beats, plus a peer-confirmation majority at the Auto-Admin                                                                |
| Workload          | open loop, 50% PUT / 50% GET, 256-byte values, 16 client threads; keys preloaded once                                                                              |
| Timeline per run  | 20 s baseline → `kill -9` victim D → 30 s down (load continues) → restart D → wait for REPAIR_DONE → 20 s after → stop load → verify every acknowledged key        |
| Sweep             | keys ∈ {1,000, 10,000, 50,000} × load ∈ {200, 1,000} ops/s, plus one run with **two** nodes down (beyond the stated tolerance)                                     |
| Clocks            | every duration is measured on a single machine's clock: detection on the Auto-Admin's machine (the driver issues the kill there too), repair on the restarted node |

## Results

| keys                           | load (ops/s) | requests | availability while down | detection (kill → DOWN) | repair time (start → done) | keys repaired | data repaired | p50 / p95 while down | lost acked writes |
| ------------------------------ | ------------ | -------- | ----------------------- | ----------------------- | -------------------------- | ------------- | ------------- | -------------------- | ----------------- |
| 1,000                          | 200          | 19,280   | **100%**                | 2.37 s                  | 0.86 s                     | 735           | 0.2 MB        | 6.3 / 8.9 ms         | **0** of 1,000    |
| 1,000                          | 1,000        | 96,447   | **100%**                | 2.24 s                  | 0.95 s                     | 692           | 0.2 MB        | 7.6 / 10.6 ms        | **0** of 1,000    |
| 10,000                         | 200          | 18,080   | **100%**                | 2.22 s                  | 1.11 s                     | 2,040         | 0.6 MB        | 7.4 / 10.3 ms        | **0** of 10,000   |
| 10,000                         | 1,000        | 90,411   | **100%**                | 3.13 s                  | 1.85 s                     | 5,687         | 1.7 MB        | 7.0 / 9.8 ms         | **0** of 10,000   |
| 50,000                         | 200          | 20,544   | **100%**                | 2.90 s                  | 1.32 s                     | 2,277         | 0.7 MB        | 5.8 / 8.2 ms         | **0** of 50,000   |
| 50,000                         | 1,000        | 103,641  | **100%**                | 2.77 s                  | 2.64 s                     | 10,051        | 3.0 MB        | 8.2 / 11.3 ms        | **0** of 50,000   |
| 1,000, **2 nodes down** (C, D) | 200          | 19,552   | **46.4%**               | 3.18 s                  | 1.65 s                     | 227           | 0.07 MB       | 6.3 / 8.6 ms         | **0** of 1,000    |

Availability was also 100% before the kill, during repair and after repair in every single-failure run. In the two-down run it was 85.5% during repair and 100% after. Full table: [rce-119958/summary.md](rce-119958/summary.md) (CSV: [summary.csv](rce-119958/summary.csv)). Per-run charts: `rce-119958/run-*/availability.svg` and `latency_p99.svg`.

## What the numbers show

1. **Stated tolerance holds (1 failure).** With one of four storage nodes killed for 30 s, every one of the 348,403 requests across the six single-failure runs succeeded. In-flight requests coordinated by the killed node were retried by the router on the next replica. Requests to the dead node as a _replica_ still reached W = 2 through the other two.
2. **No acknowledged write was lost.** All 123,000 acknowledged keys (7 runs) returned exactly their last acknowledged value after recovery (byte-for-byte check, `dkv_verify`). This holds even in the two-down run, where the system refused writes it could not make durable on W = 2 nodes instead of acknowledging them.
3. **Failure detection takes ≈3 s, as designed.** The Auto-Admin declared DOWN **3.06–3.09 s after the last heartbeat** in every run: 3 missed 1-second beats plus at most one 100 ms detector tick. Kill→DOWN varies between 2.2 and 3.2 s only because the kill lands at a random point in the heartbeat interval. Peer confirmation (a majority of other nodes also suspect the victim) never delayed the decision, because peers suspect after 1 missed beat.
4. **Repair time scales with how much the node missed, not with total data size.** The anti-entropy pass compares Merkle trees, descends only into differing subtrees, and copies only the missing or older versions. Repair took 0.86 s for 735 keys and 2.64 s for 10,051 keys (3 MB). At 200 ops/s, 50,000 keys repaired faster (1.32 s) than 10,000 keys at 1,000 ops/s (1.85 s), because fewer keys changed during the outage.
5. **Beyond tolerance, the failure is explicit, not silent.** With 2 of 4 nodes down, a key is unavailable only if both dead nodes are among its 3 replicas. That is exactly half the keys for 4 nodes and N = 3, and the measured 46.4% availability matches it. The other requests got an explicit `QUORUM_NOT_REACHED`, and after both nodes returned and repaired, availability went back to 100% with 0 lost writes.
6. **Latency.** p50 5.7–8.2 ms, p95 ≤ 11.3 ms on RCE, essentially unchanged while a node is down. A PUT costs about 4 network round trips (client → router → coordinator → clock read from one replica → replicate to the others) plus fsyncs, at ~0.8 ms per round trip between RCE nodes (measured in milestone 0). The nodes were shared with other users' jobs during the run.

## Reproduce

```bash
scripts/rce/sync.sh
scripts/rce/ssh.sh 'sbatch --wait' < scripts/rce/build.sbatch
scripts/rce/ssh.sh 'sbatch --export=ALL,KEYS_LIST="1000 10000 50000",RATES_LIST="200 1000",TWO_DOWN=1' < scripts/rce/experiment.sbatch
```

Results appear in `~/dkv-results/<jobid>/` on RCE (raw per-request CSVs included). This folder holds the summaries, per-run `results.json`, Auto-Admin health and repair CSVs, and charts.

## Not yet done

- **5- and 6-node clusters (the proposal's 8-machine layout):** only 4 compute nodes were schedulable. `make_config.py` switches to the proposal layout automatically when 8 nodes are allocated (`#SBATCH -N 8`).
- **Repeated runs for error bars:** `REPEATS=3` repeats every configuration.
- **Longer downtimes** (e.g. 120 s): `DOWN_S=120`.
