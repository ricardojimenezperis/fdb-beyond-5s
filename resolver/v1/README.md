# Beyond-5s · Resolver v1 engineering snapshot

**Resolver revision:** `aff127286e68ea7933dc9ef502a7dbd760e1e43d`.  
**FDB integration revision:** `5a07ab1936aaa6c01b5f36191c5446dee0a3d674`.  
**Evaluation campaign:** 23 October 2026.  
**Documentation updated:** 24 October 2026.

## Start here

Resolver v1 is a temporal B+ tree for write-conflict history under a shared memory budget. It combines compact nodes, separately stored variable-length keys, point endpoints sharing the same bytes, proactive split preparation, and version-aware reclamation.

Under a **6 GiB counted-history budget**, v1 retains a minimum of **582.938 seconds** after its first discard, compared with FDB's calibrated **265-second window**: **2.200× as much history**. Its native full-trace execution time is **3,375.50 seconds versus 1,240.68 seconds**, with a reported time ratio of **2.721×**. The sequential implementation establishes the representation and reclamation policy; grouped traversal and selective prefetch remain planned optimizations for this milestone.

Both implementations produce matching recorded decisions and reject no reads for age or memory. At the end of the measured interval, v1's retention still decreases slowly and FDB's memory use still increases slowly. The final interval is treated as practically stable, without claiming an infinite-duration limit. See the [evaluation](06-evaluation.md) for the measurement scope and residual trends.

This is one component milestone within Beyond-5s. Longer resolver history must be coordinated with Storage Server version retention and transactional minRV tracking to support older transactions end to end.

## Choose a reading path

| Question | Document | Approximate effort |
|---|---|---|
| What does this work contribute? | [Engineering overview](01-engineering-overview.md) | 5 minutes |
| How does it fit into Beyond-5s? | [Project architecture](02-project-architecture.md) | 5 minutes |
| How does the implementation work? | [Resolver v1 design](03-resolver-v1-design.md) | 25–35 minutes |
| Why these choices, and what changed? | [Decisions and evolution](04-decisions-and-evolution.md) | Selective |
| How did the work develop? | [Engineering journal](05-engineering-journal.md) | Selective |
| What do the measurements show? | [Evaluation](06-evaluation.md) | 10 minutes |
| What evidence supports the claims? | [Evidence and validation](07-evidence-and-validation.md) | Reference |

## Version identity and scope

`btree-v1` is the campaign identifier for this sequential implementation, not a Git tag. The first campaign revision, `1ec5d3d4d8907e6883568a168bfaab5d1fcca044`, was superseded by the prepared-leaf correction in the resolver revision above.

Each component can evolve through its own milestone snapshots. An integrated release records the resolver, Storage Server and transaction-tracking revisions it combines. This snapshot describes v1 and compares its measured behavior with FDB; subsequent implementations are outside its scope.

Source code and raw campaign artifacts are not bundled here; their identities, locations and validation limits are recorded in the evidence register.
