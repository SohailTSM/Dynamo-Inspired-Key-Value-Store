# Dynamo-Inspired Key-Value Store

Distributed Systems course project. A replicated, fault-tolerant key-value store inspired by Amazon's Dynamo, with configurable consistency. It merges two course project options into one system:

1. **Fault-tolerant store:** consistent-hash partitioning, replication, failure detection, anti-entropy repair, durability.
2. **Configurable consistency:** read/write quorums, vector clocks, conflict resolution under partitions.

## Requirements

### Cluster and API
- 4–6 node processes, each serving a **GET/PUT** API.
- **Consistent hashing** partitions keys across the nodes.
- **At least 3 replicas per key.**

### Consistency
- **Configurable quorums:** clients set **R** (read quorum) and **W** (write quorum).
- **Vector clocks** track versions and tell apart concurrent writes.
- Conflicting versions are returned to the client for **explicit reconciliation**, using a declared conflict-resolution policy.

### Fault tolerance
- **Heartbeat-based failure detection** marks unavailable nodes.
- The store keeps serving requests within a stated failure tolerance.
- **Durable, versioned storage** on disk.
- **Anti-entropy:** replicas compare digests to find missing or stale updates and repair them when a node returns.
- **No acknowledged write is lost** under the chosen write policy.

## Experiments to run
| Experiment | What to measure |
|---|---|
| Kill a replica during writes, then restart it | Repair time, request availability during the failure, no lost acknowledged writes |
| Partition the cluster, update the same key on both sides, reconnect | Conflicting versions detected by vector clocks; the declared resolution rule holds |
| Sweep quorum settings (e.g. R=1/W=1, R=2/W=2, R=1/W=3) under concurrent updates | Latency, availability, returned versions, stale-read frequency |

## Resume checklist
The resume describes this project. Every item below must be true in the final build. When results exist, add the numbers (repair time, availability %, stale-read rate per quorum setting) to the resume.
- [ ] Consistent-hash partitioning across 4–6 nodes, 3 replicas per key, GET/PUT API
- [ ] Client-set R/W quorums, vector clocks, explicit conflict resolution
- [ ] Heartbeat failure detection, durable versioned storage, digest-based anti-entropy repair
- [ ] Replica-kill and network-partition experiments measuring repair time, availability, latency, stale reads

## Status

Phase 1 (course Project 1, `Proposal.pdf`) is implemented. Phase 2 (configurable consistency) hooks are in place but not enabled. Design: [docs/hld/HLD.md](docs/hld/HLD.md), [docs/lld/LLD.md](docs/lld/LLD.md) (index: [docs/README.md](docs/README.md)). Decisions and progress: [PROJECT_LOG.md](PROJECT_LOG.md).

## Build

Local (Ubuntu):
```bash
sudo apt install cmake libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc   # once
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
(cd build && tests/dkv_tests)                                                  # unit tests
```

RCE (builds on a compute node through SLURM; nothing runs on the login node):
```bash
scripts/rce/sync.sh                                             # copy sources to ~/dynamo-kv/src
scripts/rce/ssh.sh 'sbatch --wait' < scripts/rce/build.sbatch   # module load cmake/3.20.3 gRPC/1.74.1, build, unit tests
```
`scripts/rce/ssh.sh` reads the login and password from the gitignored `secrets/rce.json` (template: `secrets/rce.example.json`).

## Run a local cluster (4 storage nodes + router + metadata + auto-admin on 127.0.0.1)

```bash
scripts/local/cluster.sh start                 # config/cluster.local.json; run dir build/local-run/<timestamp>
build/src/dkv_cli --config config/cluster.local.json put hello world
build/src/dkv_cli --config config/cluster.local.json get hello
scripts/local/cluster.sh status                # health per node, last repairs
scripts/local/cluster.sh kill B                # kill -9 a storage node
scripts/local/cluster.sh restart B             # restart it: it recovers from its log, then repairs via anti-entropy
scripts/local/cluster.sh stop
```

## Kill/restart experiment

Local:
```bash
KEYS=2000 RATE=300 BASE_S=10 DOWN_S=15 POST_S=10 VICTIM=B scripts/local/experiment.sh
```
RCE (4-8 compute nodes; one SLURM job runs the whole sweep):
```bash
scripts/rce/ssh.sh 'sbatch --export=ALL,KEYS_LIST="1000 10000",RATES_LIST="200 1000",TWO_DOWN=1' < scripts/rce/experiment.sbatch
```
Every run produces `results.json`, `analysis.txt`, `availability.svg` and `latency_p99.svg`. The job also writes `summary.md` / `summary.csv` covering all runs. Metrics:
- **request availability** per window (before / down / repair / after)
- p50/p95/p99 latency
- **failure detection time** (kill → admin DOWN, same clock)
- **repair time** (node process start → REPAIR_DONE, node clock), plus keys and bytes repaired
- **lost acknowledged writes** (`dkv_verify`, must be 0)

## Processes

| Binary | Role |
|---|---|
| `dkv_node --id A` | storage node: version log + index + Merkle trees, coordinator (quorums), heartbeats, anti-entropy |
| `dkv_router --id R1` | request router: cached ring + health view, forwards to the first live replica, fails over |
| `dkv_metadata` | ring / membership source of truth |
| `dkv_admin` | Auto-Admin: cluster health (peer-confirmed DOWN), repair tracking, CSV + event logs |
| `dkv_cli` | put / get / status / fault injection |
| `dkv_loadgen`, `dkv_verify` | workload generator with ack log; acknowledged-write verifier |
