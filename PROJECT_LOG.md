# Project Log — Dynamo-Inspired Fault-Tolerant Key-Value Store

Team 18: Dhruv Rajput (2025201094), Sohail Memon (2025201075)

This file records the working sessions on the project: what was done, what was decided and why, and the questions asked with their answers. Newest session at the bottom. Each session lists **Actions**, **Decisions**, **Q&A**, and **Next steps**.

---

## Session 1 — 2026-11-09 — Kickoff and planning

### Context given by the user
- Course offers two Dynamo projects:
  1. **Fault-tolerant store**: consistent hashing, ≥3 replicas, heartbeat failure detection, durable versions, digest-based anti-entropy, kill/restart experiment (repair time, availability, no lost acknowledged writes).
  2. **Configurable consistency**: client-set R/W, vector clocks, conflicting versions returned for reconciliation, network-partition experiment, stale-read frequency per quorum setting.
- Team chose **Project 1** and wrote `Proposal.pdf`. **Project 2 will be added after the coursework**, so the Project 1 design must leave room for it.
- `Transcript.txt`: Arpit Bhaiya's walkthrough of the DynamoDB (2022) paper. The user wants a **similar service-based architecture** (request router, metadata service, storage nodes, auto-admin, and so on).
- Follow software-engineering process: **HLD → LLD → implementation**.

### Actions
1. Read `Proposal.pdf` (2 pages). Key commitments:
   - 4–6 server processes, GET/PUT API, clear success/error for every request.
   - Consistent hashing with virtual nodes, clockwise walk for the preference list.
   - N=3, W=2, R=2, so the system survives 1 node failure. Behaviour with more failures must be explained.
   - Heartbeats every ~1 s; a node is down after ~3 missed beats; it is marked up again when beats return; detection time is measured.
   - Every version goes to an append-only log with fsync **before** the ack; all data is reloaded from disk on restart.
   - Anti-entropy: digests compared on return; only missing or stale keys are copied from a healthy replica.
   - The client records acknowledged writes and verifies them after recovery.
   - Tools: C++ or Go, gRPC + Protobuf, MD5/SHA-1, optional RocksDB/LevelDB.
   - Run on RCE accounts (8 machines, local disks, NTP).
   - Deliverables: code + README, kill/restart scripts, graphs and tables.
2. Read `Transcript.txt`. The architecture pieces it describes:
   - **Request Router**: front door. Caches routing info locally (99.75% hit rate in the paper).
   - **Metadata Service**: partition → storage-node mapping. Backed by **MemDS**, which is kept warm with asynchronous calls so cache misses never cascade.
   - **Storage Service / nodes**: partitions with a B-tree and a WAL. The leader writes the WAL, replicates, and acks after a quorum. Followers apply to the B-tree asynchronously. **Log replicas** (WAL only) allow fast recovery.
   - **Auto-Admin**: the control plane / "central nervous system". Watches fleet health, replication factor, hot partitions, splits and rebalancing, and failure recovery.
   - **Admission control**: per-partition token buckets, bursting, adaptive capacity, and Global Admission Control at the router.
   - **Durability**: WAL archived to S3, checksums end to end, continuous verification.
   - **Availability**: Multi-Paxos per partition. A follower confirms with its peers before starting an election (gray failures). Canary apps. Cached dependencies.
   - **Deployments**: canary rollout, automatic rollback, read-then-write deployments.
3. Checked the existing `README.md`. It already describes the merged Project 1 + 2 goals and a resume checklist.
4. Checked the local toolchain: Python 3.14, Java, and Node are present. **Go is not installed.** g++/protoc not checked yet.

### Observations / design tensions identified
- **DynamoDB (transcript) vs Dynamo (course projects).** DynamoDB uses a single **leader per partition with Multi-Paxos**. The course projects, especially Project 2 (concurrent versions, vector clocks, partitions with updates on both sides), assume **Dynamo-style leaderless quorums**. A strict leader design would make Project 2 impossible without a rewrite.
  - Proposed resolution: take DynamoDB's **service decomposition** (Router / Metadata / Storage / Auto-Admin) but keep Dynamo's **leaderless quorum replication** for the data path.
- **The "4–6 processes" requirement** is read as 4–6 **storage nodes**. Router, metadata, and admin are extra processes. With 8 RCE machines: 5 storage + 1 router + 1 metadata/admin + 1 client/load generator.
- **Future-proofing for Project 2**: the versioning format, the quorum parameters (R/W per request), and network fault injection should be in the design from day one, even if Project 1 only uses part of them.

### Q&A
**Round 1 (2026-11-09)**

| # | Question | Options offered | Answer |
|---|---|---|---|
| 1 | Implementation language? (Go not installed; g++ 15.2 present; protoc, cmake, gRPC-C++ absent) | Go (recommended) / C++ | **C++** |
| 2 | Write path: router → coordinator storage node (leaderless quorum), router does the quorum fan-out, or leader + Multi-Paxos (literal DynamoDB)? | three options | **Deferred.** User asked for an explanation of "Phase 2" first, then to be asked again. |
| 3 | Failure detection layout? | Peer + Auto-Admin (recommended) / peer-only / central-only | **Peer + Auto-Admin** |
| 4 | Versioning in Phase 1? | Vector clocks now (recommended) / simple counter now | **Vector clocks now** |

Decisions from round 1:
- **D1 — Language: C++.** Consequence: we must install protoc, gRPC-C++, and a build system (CMake) locally and on RCE (or link statically and copy binaries). Build setup goes into the LLD.
- **D2 — Failure detection:** storage nodes heartbeat each other (local view, used to skip dead replicas). They also report to an **Auto-Admin** service, which keeps the cluster-wide health view, informs routers, and starts and times anti-entropy repair.
- **D3 — Versioning:** every stored version carries a **vector clock** from day one. Phase 1 resolves concurrent versions with a declared deterministic rule. Phase 2 will return siblings instead of collapsing them.

Clarification given for Q2 ("what is Phase 2?"):
- In this chat, "Phase 2" means the second course option (Configurable Consistency) that the team plans to add after the coursework: client-set R/W, vector clocks, conflicting versions returned to the client, partition the cluster and write the same key on both sides, then measure stale reads.
- Why the write path matters: with **one leader per partition + Paxos**, a partitioned minority cannot accept writes. Updates "on both sides" then never happen, vector clocks never see concurrent versions, and R/W are fixed by the Paxos majority. Phase 2 would need Paxos removed. With **leaderless quorums**, any healthy replica can coordinate a write. With a low W, both sides accept writes, the writes become concurrent siblings, and vector clocks detect them. For Phase 1's N=3/W=2/R=2, both designs behave the same.

**Round 2 (2026-11-09)**

| # | Question | Answer |
|---|---|---|
| 2 (re-asked) | Write path, with the Phase 2 explanation | **Router → coordinator node (leaderless quorum)** |

- **D4 — Write path:** the router forwards to the first UP node in the key's preference list. That coordinator stamps the vector clock, fsyncs, replicates to the other N−1, and acks after W. GET reads R replicas, resolves, and does read repair. Strict quorum (the coordinator is always in the preference list). Reason: keeps DynamoDB's service split while making Phase 2 possible without a rewrite.

### HLD written
- Created `docs/HLD.md` (Draft v1). Contents: scope/phases, requirements table mapped to proposal goals, failure model and stated tolerance (f=1, R+W>N overlap argument, behaviour with 2 down), transcript → component mapping table, component and node-internals diagrams, partitioning (MD5 64-bit ring, 64 vnodes, static membership), versioning (vector clocks + declared LWW-on-concurrent rule), PUT/GET/heartbeat/anti-entropy flows, durability (CRC records, fsync before ack, group commit, torn-tail recovery), control-plane availability, RCE deployment (5 storage + router + metadata/admin + client), experiments and metrics, Phase 2 extension points, tech choices, risks, milestones, open items with defaults.
- Defaults chosen without asking (listed in HLD §16; the team can override): 5 storage nodes, 64 vnodes, 500 ms / 1 s timeouts, 30 s periodic anti-entropy, 1 router (2 supported), custom log instead of RocksDB, Auto-Admin declares DOWN only with peer confirmation (gray-failure rule from the transcript), MemDS not built (ring fits in every cache).

### Next steps
- Team reviews `docs/HLD.md` and confirms or changes the §16 defaults.
- Write `docs/LLD.md` (proto definitions, module/package layout, data structures, algorithms, storage file format, configuration, test plan).
- Then implement in milestones.

---

## Session 1 (cont.) — 2026-11-09 — Terminology + UML diagrams

### Q&A
- **Q (user):** "Are we calling Phase 2 the part we will do after the project?"
  **A:** Yes. **Phase 1** = course Project 1 (Fault-Tolerant store, `Proposal.pdf`), built for the coursework. **Phase 2** = course Project 2 (Configurable Consistency), added after the coursework as an upgrade. This is now written as a terminology note in HLD §1.
- **Request (user):** "I want proper UML compiled diagrams."

### Actions
- Chose **PlantUML** (UML-standard diagram types, text sources that diff well in git). It is compiled with PlantUML's built-in **Smetana** layout, so only Java is needed (Java 25 is installed; Graphviz is not).
- Wrote 11 diagram sources in `docs/diagrams/src/`, with a shared style in `_style.iuml`:
  - Use case: `use-case`
  - Component: `component` (system), `node-internals` (storage node)
  - Deployment: `deployment` (8 RCE machines)
  - Sequence: `seq-put`, `seq-get`, `seq-failure-detection`, `seq-repair`
  - State machine: `state-peer-local`, `state-admin-health`
  - Activity: `activity-experiment`
- Wrote `docs/diagrams/build.sh`. It downloads the pinned PlantUML jar (v1.2026.9) into `tools/` (gitignored) if missing, then renders SVG + PNG into `docs/diagrams/out/`.
- Added `.gitignore` (`tools/`, `build/`).
- Updated `docs/HLD.md`:
  - terminology note
  - diagram index
  - ASCII diagrams replaced with compiled-diagram links
  - diagrams embedded in §2, §4.2, §4.3, §7.1–7.4, §10, §11
- **Design refinement (D5):** added a **RECOVERING** health state in the Auto-Admin (DOWN → RECOVERING on re-registration → UP on `REPAIR_DONE`). A RECOVERING node is routable. This gives the repair timer a clear start and end.

### Blocker
- The auto-mode safety check **blocked downloading and running the PlantUML jar** (external code), so the diagrams are **not compiled yet** and their syntax is unverified. Waiting for the user to allow it, or to run `docs/diagrams/build.sh` themselves.

### Next steps
- Compile the diagrams and fix any syntax errors.
- Team reviews the HLD + diagrams. Then write `docs/LLD.md` (with UML class diagrams).

### Blocker resolved — diagrams compiled (user said "run it")
- Ran `docs/diagrams/build.sh`. It downloaded PlantUML 1.2026.9 into `tools/` and rendered all 11 diagrams to SVG + PNG in `docs/diagrams/out/` (22 files) with **no syntax errors**.
- Checked every rendered PNG visually and fixed what was wrong:
  - Removed the deprecated `ParticipantPadding` / `BoxPadding` skinparams. They added a yellow warning box to every diagram.
  - `node-internals`: restructured as a left-to-right flow (callers → API layer → Logic → StorageEngine / networking → external systems). Peers are split into "callers" and "callees", and minor edges are dotted. It was unreadably wide before.
  - `deployment`: two tier groups (storage tier; routing/control/client tier) joined by one "RCE LAN" link. Machines are declared in reverse order because Smetana lays out siblings reversed.
- Known limitation: Smetana (the Java-only layout engine) routes edges less neatly than Graphviz. If Graphviz is installed later (`sudo apt install graphviz`), removing `!pragma layout smetana` gives cleaner layouts.

### Next steps
- Team reviews `docs/HLD.md` + diagrams and confirms or changes the §16 defaults.
- Then write `docs/LLD.md` with UML class diagrams, `.proto` definitions, log record format, Merkle layout, threading model, config format, and test plan.

---

## Session 1 (cont.) — 2026-11-09 — Secrets folder

- **Request (user):** create a secrets folder with a file for the RCE cluster login and password.
- **Actions:**
  - Created `secrets/` (mode 700).
  - Created `secrets/rce.json` (mode 600, gitignored) for the real values, and `secrets/rce.example.json` (tracked template).
  - Fields: `username`, `password`, `ssh_port`, `jump_host` (optional gateway), `hosts.m1..m8`, `remote_base_dir`.
  - `.gitignore` now ignores `secrets/*` except the example. Verified with `git check-ignore`.
- **Decision (D6):** deployment/experiment scripts will read RCE access details from `secrets/rce.json`. Claude will not open or print this file unless a script needs it.
- **Recommendation given:** prefer SSH keys (`ssh-copy-id`) over a stored password for the scripts. The password field is kept because the user asked for it.

---

## Session 1 (cont.) — 2026-11-09 — RCE environment probe

- **User:** added credentials to `secrets/rce.json`. Login is `ssh <username>` with a password.
- **How Claude connects:** `sshpass`/`expect` are not installed locally, so `scripts/rce/ssh.sh` uses OpenSSH's `SSH_ASKPASS` (+ `SSH_ASKPASS_REQUIRE=force`). The helper `scripts/rce/askpass.sh` reads the password from the gitignored secrets file at connect time. The password never appears in commands or output. stdin is forwarded, so job scripts can be piped straight to `sbatch`.
- **Findings (read-only probes on the login node `rce.iiit.ac.in`):**
  - CentOS Stream 8, kernel 4.18. Login node: 64 CPUs, 93 GB RAM.
  - **SLURM cluster.** Partition `debug`, no time limit, DefMemPerCPU=3000 MB, account `cs3401`. Compute nodes node01–08:
    - node01, node02: **drained** ("Kill task failed")
    - node07: **draining** (same reason)
    - node08: **drained\*** ("upgrade", not responding)
    - **node03–06: usable** but shared (`mixed`). CPUs 96/64/48/48, RAM 192–514 GB.
  - SSH to a compute node is allowed **only while you hold a job there** (`pam_slurm_adopt`).
  - **gRPC/1.74.1 environment module** (`/home/apps/grpc-1.74.1`): protoc 31.1, grpc_cpp_plugin, static libs, CMake configs (gRPC, protobuf, absl, re2, c-ares, utf8_range). Built with **GCC 8.5**.
  - System g++ 8.5.0. `gcc-toolset-11/12/13` (GCC 13.2) also available. cmake 3.26.5. java, python3, git present. No go.
  - `/home`: ext4, 10 GB quota per user (1.8 GB used). No /scratch.
  - Login node clock: "System clock synchronized: no" (NTP service active).
- **Q&A:**
  - Q (Claude): deployment layout for 4 usable nodes? **User asked what "drained" means.** Explained: an admin/SLURM state where no new jobs can start; the reasons are shown above; only admins can resume those nodes. **Deployment question to be re-asked.**
  - Q (Claude): run milestone-0 toolchain check now? **User: "Do not run anything on the login node; use SLURM batch to run directly on compute nodes."**
- **Decisions:**
  - **D7 — No workloads on the login node.** All building and running goes through SLURM (`sbatch`/`srun`) on compute nodes. The login node is used only to submit jobs and read their output files. Job scripts are piped over ssh (`ssh.sh 'sbatch' < file`), so no files are copied onto the login node first.
  - **D8 — C++17, not C++20.** RCE's gRPC module was compiled with GCC 8.5 in C++17 mode. Building our code with a newer standard or compiler risks Abseil/libstdc++ ABI mismatches. Our code must also build locally (Ubuntu, g++ 15) against whatever gRPC version is installed there.
- **Action:** wrote `scripts/rce/m0_toolchain.sbatch` (2 nodes, 20 min limit). It writes the sources into `~/dynamo-kv-m0` and builds with CMake + the gRPC module **on a compute node**. It checks the per-node environment (home FS type, local /tmp, clock sync, fsync latency) and runs a 2000-RPC cross-node gRPC ping. Submitted with `sbatch --wait`.
- **Milestone 0 results:**
  - Job 119939 failed: compute nodes have **no system cmake** (only the login node has one). Fixed by `module load cmake/3.20.3 gRPC/1.74.1`.
  - Job 119940 on node03 + node06 **passed**:
    - Build: C++17 / GCC 8.5 / CMake 3.20 / gRPC 1.74.1, 32 s, ~68 MB statically linked binaries (only standard system .so deps).
    - Cross-node gRPC: 2000/2000 calls OK, p50 0.77 ms, p99 0.87 ms.
    - `/home` on compute nodes is **nfs4** (shared, so binaries are built once).
    - `/tmp` is **local xfs** (150–240 GB free), fsync of 4 KiB p50 0.07–0.16 ms.
    - Clocks: **node03 unsynced, node06 synced**.
  - The SLURM "forcing job termination / Killed" lines at the end are the script stopping the test server on purpose.
- **D9 — Single-clock timing:**
  - Repair time is measured on the restarted node's own clock.
  - Detection time is measured on the Auto-Admin's clock (last heartbeat received → DOWN). The experiment driver issues kills from the Auto-Admin's machine.
- **HLD updated:** §13 (C++17, cmake module), §7.3/§7.4 (single-clock measurement), §14 risks (gRPC risk resolved, NFS confirmed for /home, clock skew observed, only 4 schedulable nodes, no-login-node rule).
- **Leftover on RCE:** `~/dynamo-kv-m0` (sources + build, ~140 MB of the 10 GB quota) and `~/kv-m0-*.out`. Left in place for the user to inspect.
- **Still open:** deployment layout for the 4 usable nodes (to be re-asked now that "drained" is explained).
- **Q (Claude, re-asked with the "drained" explanation):** deployment layout for 4 usable nodes? **User: "They will come back when we need them."**
- **D10 — Deployment:** keep the proposal's **8-machine layout** for the experiments (m1–m5 storage, m6 router, m7 metadata + Auto-Admin + experiment driver, m8 loadgen).
  - Run as one SLURM job (`-N 8`), with processes started via `srun -w` or ssh into the job's nodes.
  - Placement is config-driven, so development runs use 4 machines (4 storage nodes, one per machine) until the drained nodes return.
  - Node data lives in local `/tmp`. Logs are copied to `/home` after each run.
  - HLD §10 updated.

---

## Session 1 (cont.) — 2026-11-09 — Storage engine Q&A + LLD

### Q&A
- **Q (user):** "What are we using for the actual KV storage?"
  **A:** Our own Bitcask-style engine, no RocksDB/LevelDB:
  - an append-only, segmented **version log** on local `/tmp` (CRC-framed records, fsync before ack, group commit)
  - an **in-memory index** `key → current versions + disk location` (values are read back with `pread`)
  - **Merkle trees** per ring range
  - recovery replays the log and truncates a torn tail.

  Why: matches the proposal's "append-only log + fsync", makes the durability mechanism visible for the report, keeps every version naturally, and needs nothing installed on RCE. The engine sits behind a `StorageEngine` interface, so RocksDB could be swapped in later.
- **User:** "Okay proceed to the LLD."

### Actions
- Wrote **`docs/LLD.md`** (Draft v1):
  - conventions (C++17, `dkv` namespace, Status/StatusOr, no exceptions across modules)
  - repository layout and executables
  - config schema (JSON loaded as a protobuf message, so no JSON library)
  - **complete `.proto` definitions** (common, kv, node, admin, metadata, config)
  - class-level design for every module
  - byte-level segment/record format and recovery rules
  - Merkle layout
  - algorithms in pseudocode: PUT, GET, store rule, group commit, failure detection, anti-entropy, startup, verification rule, router
  - error/status mapping, threading and lock order, observability events
  - unit/integration test plan, build instructions, milestones M1–M10 with acceptance tests, traceability matrix (proposal goal → design → test), Phase 2 hooks
- **8 new UML diagrams:**
  - class diagrams: `class-core`, `class-storage`, `class-node`, `class-control`, `class-client`
  - `seq-group-commit`, `activity-node-startup`, `merkle-layout` (object diagram)
- All 19 diagrams compiled. Every new one was checked visually. `class-node` was simplified: StorageNode's 13 ownership edges were replaced by a note.

### Design changes made during the LLD (also applied to the HLD + diagrams)
- **D11 — `READ_BEFORE_WRITE` stamping.** Problem found: HLD stamping used only the coordinator's *local* clocks. After a coordinator failover, the new coordinator could create a version *concurrent* with the last acknowledged one. The LWW tie-break would then use wall clocks, which are unsynced on RCE (node03), so an older write could win and an acknowledged write would look lost. Fix: the coordinator reads clocks (metadata only) from R−1 peers before stamping. Since R+W>N, the new version always descends from every acknowledged one. Cost: ~0.8 ms per PUT. `LOCAL_ONLY` is kept as a config option for a comparison experiment. `seq-put` and HLD §6/§7.1 updated.
- **D12 — Precise "repair done".** Under live load, Merkle roots never match exactly because writes are always in flight. New definition: the node first waits until its peers have acknowledged a heartbeat from its new incarnation (so every new write includes it), then makes one full pull pass over every healthy peer. Done = every version stored before the pass is local. `seq-repair` and HLD §7.4 updated.
- **D13 — Smaller choices:**
  - Merkle trees: fan-out 16, depth 2 (256 leaves) per ring range, leaf chosen by `SHA1(key)[0]`, lazy recomputation, batched `GetMerkleNodes` across ranges.
  - Per-key striped locks (1024) cover stamp + durable local append.
  - App errors travel in `Result` with gRPC OK; transport errors use gRPC status.
  - Own MD5/SHA-1/CRC32C implementations (no OpenSSL dependency); vendored `doctest` for unit tests.
  - Records = `[len][crc32c][protobuf LogRecord]`.

### Next steps
- Team reviews the LLD.
- Local build prerequisites need sudo: `sudo apt install cmake libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc`.
- Start implementation at **M1** (CMake skeleton, protos, `common/`, doctest).

---

## Session 1 (cont.) — 2026-11-09 night — Autonomous implementation

**User (before sleeping):** "Looks good to me and I have ran the command [apt install of gRPC/protobuf/cmake]. I will go to sleep now… You can keep working on it." Later: "5 hours limit resets on 6:40 something. So if limit expires mid run before that set a timer or something to continue after this reset."

**Working rules for the night:**
- Don't block on questions; choose defaults and log them here.
- No git commits or staging (global rule).
- RCE only through short, small `sbatch` jobs.

**Resume timer:** one-shot cron job `2f8d9723` fires **06:47 on 2026-11-09**. It tells Claude to read this section and continue from the last completed milestone. It is session-only and dies if this Claude session is closed.

**Toolchains:**
- local: Ubuntu, g++ 15, cmake 4.2, gRPC 1.51.1, protobuf 3.21.12 (no protobuf CMake config; `FindProtobuf` is the fallback)
- RCE: gRPC 1.74.1, protobuf 31.1, GCC 8.5

The code must build on both, so it sticks to stable APIs.

### Implementation decisions (night)
- **D14:** proto package is `dkv.pb` (C++ `dkv::pb::...`), so generated types don't clash with our own `dkv::` classes such as `VectorClock`.
- **D15:** `LogRecord` lives in `common.proto`.
- **D16:** extra proto fields beyond LLD §4:
  - `HeartbeatAck.seen_sender_incarnation`: tells a restarted node that a peer has seen its new incarnation (LLD §7.6).
  - `from` on ReadLocal / Merkle / Bucket / Fetch requests: needed for the incoming FaultInjector filter.
  - `NodeStatus.address`
  - `RepairReport.repair_start_wall_ms`
- **D17:** unit tests use a tiny in-repo harness (`tests/test_framework.h`, `TEST_CASE` / `CHECK` / `REQUIRE`) instead of downloading doctest. Same API style, no external code, works offline on RCE.
- **D18:** no `std::filesystem`, because GCC 8.5 needs `-lstdc++fs`. `common/fs.{h,cc}` uses POSIX instead.
- **D19:** CLI flags: a boolean flag must be written `--flag=true` or come last; otherwise the next token is taken as its value.

### Implementation progress
| Milestone | Status | Notes |
|---|---|---|
| M1 CMake + protos + common + tests | **done (local)** | 7/7 unit tests pass: MD5/SHA-1/CRC32C test vectors, flags, fs, config (defaults; rejects R+W≤N, duplicate ids, unknown fields), thread pool |
| M2 version + ring | **done (local)** | vector clocks, version digest, resolver (prune + declared tie rule), consistent-hash ring with vnodes. Tests include load balance (5 nodes × 64 vnodes: each node primary for 15–25% of keys) and RingInfo round-trip. |
| M3 storage engine | **done (local)** | record codec, segments, VersionLog with group commit, sharded Index, LogStorageEngine. Tests: persistence across reopen; 1600 concurrent writes with fewer fsyncs than records; rollover; torn tail truncated; sealed-segment corruption aborts; change listener. |
| M4 Merkle | **done (local)** | fixed 16×16 tree per range, lazy hashes, MerkleForest. Tests: one changed key → exactly one inner + one leaf differ. 29/29 unit tests pass. |

- **D20:** `Index::apply` is **order-independent**: it keeps the set of maximal versions, with the declared tie rule for equal clocks. Replicas that receive the same versions in different orders, live or during log replay, end with identical current sets, so their Merkle roots match. The change callback runs under the shard lock, so the Merkle tree sees updates in order.
- **D21:** test temp dirs live under `$DKV_TEST_TMPDIR` (default `./test-tmp/<pid>`, i.e. inside `build/`) and are removed when `dkv_tests` exits. The auto-mode safety check blocked a wildcard `rm -rf /tmp/dkv-test-*`, which was the right call. **Leftover for the user:** the first few runs of the fs test created small `/tmp/dkv-test-<pid>/` directories (a few bytes each). Safe to delete by hand.
- Fixed a test-harness bug: `CHECK_EQ` bound `auto&&` to references into temporaries (dangling). It now copies its operands.
| M5 node + router + metadata + cli | **done (local)** | rpc layer (channels with short reconnect backoff, fault injector, peer pool, ring cache), Coordinator (READ_BEFORE_WRITE PUT, R-quorum GET, read repair, request dedupe), replica services (Replicate re-checks the version digest end to end), dkv_metadata, dkv_router (health view + failover), dkv_cli. Builds with zero warnings. |
| M6 failure detection + Auto-Admin | **done (local)** | peer heartbeats (UP/SUSPECT/DOWN), AdminReporter, HealthTracker with peer confirmation, UNREACHABLE_FROM_ADMIN, RECOVERING, health_events.csv and repairs.csv |
| M7 anti-entropy | **done (local)** | startup pass waits until peers have acked the new incarnation, then batched Merkle descent → bucket digests → FetchVersions; periodic pass every 30 s; RepairReport to admin |

**First local end-to-end test (2026-11-09 03:09, 4 nodes on localhost, `scripts/local/cluster.sh`):**
- PUT/GET OK, with W=2–3 acks and vector clocks such as `{A:1}`. A missing key returns NOT_FOUND.
- `kill -9` node A, then 61 PUTs while it was down: **all succeeded**.
- Admin: A went SUSPECT at 1.07 s and **DOWN at 3.07 s** after its last heartbeat, "confirmed by 3/3 peers".
- Restart A (incarnation 2): DOWN → RECOVERING → UP. The startup repair **fetched 44 keys; REPAIR_DONE 635 ms after process start**.
- Read-back of all 61 keys: **0 bad**.

**Small deviations from the LLD made during implementation (D22):**
- READ_BEFORE_WRITE reads the remote clocks *before* taking the per-key lock, then merges the fresh local clocks under the lock. This is equally correct and keeps no lock across a network call (LLD §9).
- PUT fails fast with QUORUM_NOT_REACHED when fewer than W replicas are live, so no unackable local write is created.
- Read repair uses only the replies that arrived before the quorum wait returned. Late replies are not used; the periodic anti-entropy covers them.
- Admin RECOVERING is entered on Register, on a heartbeat with a new incarnation, or on RepairReport STARTED. UP is entered on RepairReport DONE.
- `scripts/local/cluster.sh` gives each run its own directory, `build/local-run/<timestamp>/`, so cleanup never needs deletes.
| M8 loadgen + verifier + experiment | **done (local)** | `dkv_loadgen` (open loop; each worker owns a disjoint key set so PUTs per key are sequential; values derived from (key, seq) so the verifier checks exact bytes; `--seq-base` keeps preload and main-run seqs distinct), `dkv_verify` (LLD §7.8 rule; reads several dirs in order), `scripts/local/experiment.sh`, `scripts/analysis/analyze_run.py` (per-window availability and latency, detection time, repair time → results.json; plots if matplotlib is present) |

**First local kill/restart experiment (03:13, 4 nodes on localhost, 2000 keys preloaded, 300 ops/s at 50% PUT, node B killed for 12 s):**

| window | requests | availability | p50 | p95 | p99 |
|---|---|---|---|---|---|
| before | 2416 | 100% | 3.34 ms | 7.26 ms | 8.82 ms |
| down | 3600 | 100% | 2.82 ms | 7.32 ms | 8.68 ms |
| repair | 496 | 100% | 2.59 ms | 7.42 ms | 8.55 ms |
| after | 2576 | 100% | 3.61 ms | 7.29 ms | 8.55 ms |

- Detection: DOWN **2839 ms after the kill** (3062 ms after the last heartbeat).
- Repair: **1640 ms from process start** (1074 ms anti-entropy pass), **812 keys / 245 KB** fetched.
- Verifier: **2000 acknowledged keys, 0 lost**.
- Local machine: matplotlib is **not** installed, so plots were skipped. (Installing it would be `pip install --user matplotlib`; left for the user to decide.)

### RCE build issues found and fixed (night)
- **rsync exclude bug:** `--exclude tools/` also skipped `src/tools/`. Excludes are now anchored to the repo root (`/tools/`, `/build/`, …).
- **D14 revised: proto package `dkv.pb` → `dkv.wire`** (C++ `dkv::wire::…`). protobuf 31 on RCE declares its own global `::pb` namespace. In files with `using namespace dkv;`, `pb::` was ambiguous and failed to compile there, while the local protobuf 3.21 has no `::pb` and built fine. 409 mechanical replacements in 46 files. Local tests still 30/30.
- The RCE build script now retries serially and prints the log tail when the parallel build fails, so errors are visible in the job output.
- matplotlib is not available on RCE or locally, so `analyze_run.py` draws its charts as **pure-Python SVG** (`availability.svg`, `latency_p99.svg`). No dependency is needed.
- **libsystemd link problem:** RCE compute nodes are not identical. gRPC's exported CMake targets link `/usr/lib64/libsystemd.so` (and `-lsystemd`). That dev symlink exists on node03 but not on node06, which only has `libsystemd.so.0`. `cmake/Dependencies.cmake` now reads the target names from `gRPCTargets.cmake` (CMake 3.20 has no IMPORTED_TARGETS property) and relinks them to `libsystemd.so.0`. **RCE build OK (27 s), 30/30 unit tests pass on a compute node.** The two "warnings" were make clock-skew messages (NFS + unsynced clocks), not compiler warnings.

### First RCE run (job 119956, node03–06, 1000 keys, 200 ops/s, victim D down 15 s)
- Availability **100%** in every window. Detection **2559 ms** after the kill. Repair **853 ms** from process start (577 keys). Verifier **0 lost**. The full pipeline (config from the SLURM node list → srun steps → kill/restart → verify → analysis → summary.md) works.
- **Bugs it exposed (fixed):**
  1. **Shutdown race → core dump.** In every server `main`, a signal thread called `stop()` while the main thread returned from `server->Wait()` and destroyed the object. Fix: `main` blocks in `WaitForTerminationSignal()` and runs shutdown itself. Node stop order is now: traffic sources (anti-entropy, admin reporter, failure detector, ring poller) → server drain (500 ms) → fan-out pool → log close. All 7 processes now exit within ~60 ms of SIGTERM; nodes report "stopped in 2–3 ms".
  2. **p50 latency 10 ms on RCE vs 3 ms locally.** Every process was started with `srun -c1`, so it was pinned to one core, shared by a 128-thread gRPC server and a 64-thread fan-out pool. Now storage nodes get 4 CPUs, loadgen 4, router 2, admin and metadata 1 (fits 8 CPUs per node).
  3. NFS delayed the driver seeing the loadgen pidfile (harmless). Wait raised to 60 s.

### Full sweep submitted
`KEYS_LIST="1000 10000 50000" RATES_LIST="200 1000" TWO_DOWN=1`, 20 s baseline / 30 s down / 20 s after, 4 nodes. Results land in `~/dkv-results/<jobid>/` on RCE.

### Local integration tests (LLD §11.2): `scripts/local/integration_tests.sh`: 7/7 pass
| test | result |
|---|---|
| smoke | 200 PUT/GET, 0 failures |
| durability | `kill -9` **all 4** storage nodes, restart: 0 of 300 keys lost |
| two_down | B and C down: 103/200 keys readable, 97 explicit QUORUM_NOT_REACHED (theory: keys whose 3 replicas include both B and C = 1/2); after restart 0 lost |
| control_plane | Metadata + Auto-Admin stopped: PUT/GET unaffected (HLD §9) |
| load_kill | kill coordinator A under 400 ops/s: 100% success, 0 lost |
| faults | A→B traffic dropped by the fault injector: writes still reach W |
| torn | 5 kill/restart cycles of C under 800 writes/s: always recovers, 0 lost (no torn tail was caught in these runs; the torn-tail path is covered by unit tests) |
| M9 RCE scripts + experiment + analysis + README | **done** | `scripts/rce/{sync.sh, build.sbatch, make_config.py, experiment.sbatch}`, `scripts/analysis/{analyze_run.py, summarize.py}`, README build/run/experiment sections, `results/README.md` |

### Main RCE sweep (job 119958, node03–06): see `results/README.md` and `results/rce-119958/`
| keys | ops/s | availability while down | detection kill→DOWN | repair start→done | keys repaired | lost |
|---|---|---|---|---|---|---|
| 1k | 200 | 100% | 2.37 s | 0.86 s | 735 | 0 |
| 1k | 1000 | 100% | 2.24 s | 0.95 s | 692 | 0 |
| 10k | 200 | 100% | 2.22 s | 1.11 s | 2,040 | 0 |
| 10k | 1000 | 100% | 3.13 s | 1.85 s | 5,687 | 0 |
| 50k | 200 | 100% | 2.90 s | 1.32 s | 2,277 | 0 |
| 50k | 1000 | 100% | 2.77 s | 2.64 s | 10,051 | 0 |
| 1k, 2 down | 200 | 46.4% (theory ~50%) | 3.18 s | 1.65 s | 227 | 0 |

- 348,403 requests in the single-failure runs, all succeeded. 123,000 acknowledged keys verified byte-for-byte, **0 lost**.
- Detection is always **3.06–3.09 s after the last heartbeat**.
- p50 5.7–8.2 ms, p95 ≤ 11.3 ms on RCE.
- Results copied to the repo: summaries, charts and per-run JSON only (524 KB). Raw CSVs stay in RCE `~/dkv-results/119958/`.
- Cosmetic fix: the restarted victim reused the pidfile name, so NFS on the driver node served the stale PID and cleanup had to SIGKILL it. Restarts now use `node-X.r`.
- Added **§16 Implementation notes** to `docs/LLD.md`, listing every deviation from the design.
- Validation run (job 119961) after the pidfile fix: clean shutdown (the only "Killed" is the intentional `kill -9`), 0 lost, repair 802 ms.

### Status at end of night session (~03:55, 2026-11-09)
**Phase 1 is complete.** Milestones M0–M9 are done and every proposal goal is implemented and measured (traceability: LLD §14).
- Unit tests: 30/30, locally and on RCE.
- Integration tests: 7/7.
- RCE sweep: 100% availability with 1 node down, ≈3.08 s detection, 0.8–2.6 s repair, **0 lost acknowledged writes**.

**Nothing was committed** (no `git add` / commit / push, per the global rule). All work is in the working tree.

**Resume timer cancelled:** the 06:47 cron job was there to resume unfinished work if the usage limit hit mid-run. All planned milestones finished before that, so it would only have started unrequested stretch work.

**Open items for the user / team:**
1. Review the code and the results (`results/README.md`).
2. Decide on M10 stretch goals (router token-bucket admission control, log compaction, second router). Optional; not in the proposal.
3. When node01/02/07/08 are back: rerun with `#SBATCH -N 8` (proposal layout, 5 storage nodes) and `REPEATS=3` for error bars.
4. Optional local cleanup: the small `/tmp/dkv-test-<pid>/` dirs from the first test runs; `build/local-run/` (local cluster runs); on RCE `~/dynamo-kv-m0`, `~/kv-m0-*.out`, `~/dkv-build-*.out`, `~/dkv-exp-*.out`.
5. Optional: `pip install --user matplotlib` is not needed (charts are SVG).

## Session 2026-11-09 (morning): node check

User: "We will do 1 and 2 later. First check the nodes." (1 = review, 2 = M10 stretch goals; both deferred.)

`sinfo` on RCE:

| node | state | CPUs alloc/idle/total | note |
|---|---|---|---|
| node01 | drained | 0/0/64 | "Kill task failed" since 2026-11-07 |
| node02 | drained | 0/0/96 | "Kill task failed" since 2026-10-27 |
| node03 | mixed | 28/68/96 | usable |
| node04 | mixed | 36/28/64 | usable |
| node05 | mixed | 36/12/48 | usable |
| node06 | mixed | 46/2/48 | usable but nearly full |
| node07 | draining | 2/46/48 | "Kill task failed" since 2026-09-29; one long interactive job left on it |
| node08 | drained* | 0/0/40 | "upgrade" since 2026-04-30, not responding |

Still only node03–06 schedulable, so the 8-node rerun is not possible yet. node06 has 2 idle CPUs (our experiment needs 8 per node), and ~35 other jobs are pending on priority, so even a 4-node job would queue right now.

User mentioned a possible limit of 4 nodes × 2 CPUs, then clarified it was a guess from a previous course and said to forget it. For reference only: a `cs3401` QOS exists on RCE (cpu=8 per user, 10 min, 1 job), but our association uses the `normal` QOS, so it does not apply to our jobs. Scripts unchanged.

## Session 2026-11-09 (morning, cont.): git ignore fix + docs restructure

User: "Check git and git ignore things. The tools folder inside src is not getting tracked. Also the structuring of docs folder is very bad, things for HLD and LLD are together in the diagrams folder."

- **Bug:** `.gitignore` had `tools/` (meant for the downloaded PlantUML jar), which also matched `src/tools/` (cli, loadgen, verifier). All patterns are now anchored to the repo root (`/tools/`, `/build/`, `/secrets/*`). Added `/test-tmp/`, `__pycache__/`, `*.pyc`. Now ignored: only `build/`, `tools/plantuml-*.jar`, `secrets/rce.json`, and `.claude/scheduled_tasks.lock` (from `.git/info/exclude`). Checked that no file git would track contains the real RCE password. Untracked set: 253 files, ~2.8 MB.
- **Docs restructure:**
  - `docs/hld/HLD.md` + `docs/hld/diagrams/{src,out}` (11 HLD diagrams)
  - `docs/lld/LLD.md` + `docs/lld/diagrams/{src,out}` (8 LLD diagrams)
  - `docs/plantuml-style.iuml` (shared style, was `diagrams/src/_style.iuml`), `docs/build_diagrams.sh` (replaces `docs/diagrams/build.sh`, renders both), `docs/README.md` (index).
  - Re-rendered all 19 diagrams: identical to before apart from embedded metadata.
  - Fixed links in HLD, LLD, README.md, and the rsync exclude in `scripts/rce/sync.sh`. All relative links checked: 0 broken.
  - LLD §2 "Repository layout" rewritten to the as-built tree (it still showed the design-time file names, doctest, etc.).
- Older entries in this log keep the old paths (`docs/HLD.md`, `docs/diagrams/...`) as history.
- Nothing staged or committed.

## Session 2026-11-09 (morning, cont.): first commits

User asked for 6 sequential commits, planned together. Answers: no Claude co-author, no author/co-author lines at all; commit `Proposal.pdf` but not `Transcript.txt` (added to `.gitignore`); split approved; push only after review.

| # | Commit | Contents | Checked before commit |
|---|---|---|---|
| 1 | c8dd51d Add project proposal, HLD, LLD and UML diagrams | .gitignore, Proposal.pdf, README (original part), docs/ (LLD without §16), secrets template | – |
| 2 | 63b5e00 Set up CMake build, protobuf APIs and common utilities | CMake, proto/, src/common, test harness, local config, RCE ssh/sync/m0/build scripts | build 0 warnings, 7/7 unit tests |
| 3 | 7886ca4 Add versioning, consistent-hash ring, log-structured storage and Merkle trees | src/version, ring, storage, merkle + tests | 29/29 |
| 4 | cb809f8 Add storage node: quorum coordinator, replication, failure detection and repair | src/rpc, src/node, signals | 29/29, dkv_node builds |
| 5 | 873227f Add metadata service, auto-admin, request router, client library and tools | metadata, admin, router, client, tools, local cluster + integration tests | 30/30 unit, 7/7 integration |
| 6 | c04deff Add RCE experiment pipeline, analysis scripts and kill/restart results | experiment.sbatch, make_config, analysis, results/, README final, LLD §16, this log | – (same code as 5) |

Commits 2–5 used trimmed `src/CMakeLists.txt` / `tests/CMakeLists.txt` (only the modules committed so far) and `common_test.cc` without the workload test until commit 5. Each snapshot was built from the index in a scratch dir. Not pushed yet.

Post-commit check (user: "Is everything working as expected after the commits?"), on a fresh `git archive HEAD` copy:
- Clean build, 0 warnings, all 7 binaries; unit tests 30/30; integration tests 7/7.
- Local experiment (2000 keys, 300 ops/s, victim B): 100% availability, detection 3077 ms after last heartbeat, repair 617 ms (946 keys), 0 lost.
- 59 relative links in all committed .md files: 0 broken. No leftover processes.
- RCE build not run: `rce.iiit.ac.in` did not resolve from this network (other sites resolve fine; probably off campus network / VPN).
- Later, on campus network: sync to RCE succeeded, build job 120147 (16 CPUs) sat PENDING (Priority) because the cluster was busy; the user stopped the wait and asked to cancel it. Cancelled before it started; no jobs of ours left in the queue. RCE build still unchecked.

## Session 2026-11-09 (cont.): pushed to GitHub

- User: "Push to github. We will try to run on RCE later when we can use 6-7 nodes."
- A single `git push` failed with HTTP 408 (upload timeout) on the campus network; pushing the 6 commits one at a time worked (2–4 s each). `origin/main` = c04deff; `main` tracks it.
- Next RCE work (RCE build check, experiment rerun) waits until 6–7 nodes are usable.
- User asked to remove the README's original content (written before the project started). Removed the intro that described both course projects merged, plus the Requirements, Experiments to run, and Resume checklist sections. Replaced them with a short accurate intro (architecture, key techniques, headline results). The Status/Build/Run/Experiment/Processes sections are unchanged. Not committed.
