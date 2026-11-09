# Dynamo-Inspired Key-Value Store

A replicated, fault-tolerant key-value store inspired by Amazon's Dynamo, with DynamoDB's service split: request router, metadata service, storage nodes, and an auto-admin that tracks node health. Distributed Systems course project (Team 18: Dhruv Rajput, Sohail Memon).

- Consistent hashing with virtual nodes; every key on 3 nodes; leaderless quorums (N=3, W=2, R=2)
- Vector-clock versions; durable append-only log with group commit and crash recovery
- Heartbeat failure detection with peer confirmation; Merkle-tree anti-entropy and read repair
- C++17, gRPC/protobuf, CMake; experiments run as SLURM jobs on the RCE cluster

Results (kill/restart on 4 machines): 100% availability with one storage node down, failure detected ~3.1 s after the last heartbeat, repair in 0.9–2.6 s, 0 of 123,000 acknowledged writes lost. Details: [results/README.md](results/README.md).

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

| Binary                      | Role                                                                                              |
| --------------------------- | ------------------------------------------------------------------------------------------------- |
| `dkv_node --id A`           | storage node: version log + index + Merkle trees, coordinator (quorums), heartbeats, anti-entropy |
| `dkv_router --id R1`        | request router: cached ring + health view, forwards to the first live replica, fails over         |
| `dkv_metadata`              | ring / membership source of truth                                                                 |
| `dkv_admin`                 | Auto-Admin: cluster health (peer-confirmed DOWN), repair tracking, CSV + event logs               |
| `dkv_cli`                   | put / get / status / fault injection                                                              |
| `dkv_loadgen`, `dkv_verify` | workload generator with ack log; acknowledged-write verifier                                      |
