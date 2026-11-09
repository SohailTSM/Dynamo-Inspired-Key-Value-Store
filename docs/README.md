# Design documents

Read in this order:

| Document | What it covers | Diagrams |
|---|---|---|
| [hld/HLD.md](hld/HLD.md) | High-level design: requirements, failure model, services (Router, Metadata, Storage nodes, Auto-Admin), partitioning, versioning, PUT/GET/failure/repair flows, deployment, experiment plan | [hld/diagrams/](hld/diagrams/): use case, component, node internals, deployment, PUT/GET/failure-detection/repair sequences, health state machines, experiment activity |
| [lld/LLD.md](lld/LLD.md) | Low-level design: repo layout, config, protobuf APIs, classes, on-disk format, algorithms, threading, error mapping, tests, milestones, implementation notes (§16) | [lld/diagrams/](lld/diagrams/): class diagrams, group commit sequence, node startup activity, Merkle layout |

Each `diagrams/` folder has `src/` (PlantUML sources) and `out/` (compiled SVG + PNG). All diagrams share [plantuml-style.iuml](plantuml-style.iuml).

Rebuild every diagram with:

```bash
docs/build_diagrams.sh   # needs only Java; downloads the pinned PlantUML jar into tools/ on first run
```

Related: experiment results are in [../results/](../results/README.md); decisions and progress are in [../PROJECT_LOG.md](../PROJECT_LOG.md).
