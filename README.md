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
