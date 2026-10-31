# Beyond-5s · Resolver v2 engineering snapshot

**Measured resolver revision:** development `dc2802fc2f6d76035e9d97bc621263e8b4f59cec`.  
**Measured FDB integration:** `22a8ae9fba943b73cf149d21b05dfe6b07c6e013`, with the integrated benchmark at `3ba52abba955998757fa939b731fc3d2b8fa25b1`.  
**Evaluation campaigns:** 29–30 October 2026, on Intel, AMD and Apple M3 processors.

## Start here

Resolver v2 checks a batch's read-conflict ranges in groups: sorted read begins are partitioned among the children of each node, so nearby reads share a descent, and up to G interleaved tasks prefetch what their next step reads. Each leaf is scanned directly for the batch's pending reads before the task yields. The history representation, write path and reclamation model are v1's.

V2 checks reads faster than the sequential tree in the custom benchmarks, in skiplisttest and in FDB's conflictset point workload, and faster than FDB's skip list for ranges in the integrated custom benchmark and in skiplisttest; on conflictset ranges its read time is close to FDB's and above v1's. Its individual write application remains slower than both. Under the same 6 GiB counted-history budget it retains the same final minimum history as v1, 582.938 seconds. See the [evaluation](06-evaluation.md) for the workloads, machines and caveats.

## Choose a reading path

| Question | Document | Approximate effort |
|---|---|---|
| What does this work contribute? | [Engineering overview](01-engineering-overview.md) | 5 minutes |
| How does it fit into Beyond-5s? | [Project architecture](02-project-architecture.md) | 5 minutes |
| How does the implementation work? | [Resolver v2 design](03-resolver-v2-design.md) | 30–40 minutes |
| Why these choices, and what changed? | [Decisions and evolution](04-decisions-and-evolution.md) | Selective |
| How did the work develop? | [Engineering journal](05-engineering-journal.md) | Selective |
| What do the measurements show? | [Evaluation](06-evaluation.md) | 15 minutes |
| What evidence supports the claims? | [Evidence and validation](07-evidence-and-validation.md) | Reference |

## Version identity and scope

The measurements identify their sources by the development and integration commits above and by the binary hashes in the evaluation packages. The public source series adds one change after those measurements, the reservation of room for a write's longest keys; the [evidence register](07-evidence-and-validation.md#write-reservation-after-the-measurements) explains it and why it does not turn the results into measurements of the corrected code.

The [resolver v1 snapshot](../v1/README.md) is the sequential baseline this milestone builds on. The [Resolver design](../../design/02-resolver.md) under `design/` follows the current implementation.
