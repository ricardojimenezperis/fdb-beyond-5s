# Resolver design

The baseline for FoundationDB internals is apple/foundationdb main at a443d3ee60. This document describes the **final Resolver v3**. Its source-specific [engineering snapshot](../resolver/v3/README.md) holds the detailed task machinery and evidence. The immutable [v2 snapshot](../resolver/v2/README.md) describes the preceding grouped-read implementation.

## Objective and conflict semantics

A resolver retains write-conflict history under a fixed history-pool budget. It answers read/write conflict queries, rather than storing database values. A half-open read [rb,re) at RV conflicts with a retained half-open write [wb,we) at CV when rb < we, wb < re and CV > RV. Overlaying newer writes preserves the old versions on the uncovered left and right exteriors. A read version below reclaimCV_ is too old for local history; equality is admissible.

This component is one part of the Beyond-5s protocol. Storage Servers must retain the requested database versions, and read-version protection/admission must agree with the resolver's history cutoff. The [project overview](00-overview.md), [Commit Proxy design](01-commitproxy.md) and [Storage design](03-storage.md) cover those separate responsibilities.

## Implementation boundary

BTreeV3.cpp compiles the standalone TempConflictBTree component into FDB. ConflictSet provides sorted endpoints, transaction/read records and the candidate CV. The component draws 2 KiB node and key blocks by default from a shared PagePool; BNodeStore tracks nodes and age, and KeyStore owns stored range bytes. Logical-order indices map sorted ranks to physical node slots. Internal separator ranges reuse stored key references. The history pool budget excludes temporary batch vectors and scheduler records.

The batch has two ordered phases:

1. checkReads<G> routes a mixed sequence of **read and write begins** through the tree once. It marks conflicts and too-old transactions, and records each write slice's leaf destination. It may compact expired entries but cannot add writes or reclaim key blocks while read tasks retain pointers.
2. addWrites filters rejected/too-old transactions, unions accepted writes by sorted endpoint indices, and applies them at the supplied CV. It may reclaim and split the tree. The default scheduler interleaves two groups of eight write tasks, but only the oldest task mutates the tree.

The input batch and its key bytes remain alive through both calls. The read task capacity G defaults to 16 in FDB and is distinct from the write group's 2×8 capacity.

## Shared read/write routing

The mixed readWriteBegins vector tags each begin by type. Per-type and opposite-type links allow tasks to find the next begin without copying keys into task records. Each task owns a sorted slice and recursively partitions it around internal separators. Equal begins route right. A nonempty slice with one possible child descends directly; partitions can progress independently within a single-thread scheduler.

At a leaf, a task buffers the first write and its destination, then checks the read subset with a first-key search, predecessor check and forward scan. Completed reads are unlinked from the pending subset. Results for write-bearing slices are emitted through an ordered prefix frontier, so a task that reaches its leaf early cannot reorder subsequent writes. Empty leaves are structurally removed after all read traversal tasks finish.

## Grouped write scheduling

The default source parameters are RESOLVER1_WRITE_GROUPS=2 and RESOLVER1_WRITE_LG=8. Write tasks interleave independent leaf searches and key prefetches. The oldest task is the only writer; younger searches park when they reach SearchReady and are woken as they become oldest. The successor group's searches start when the older group is halfway through, allowing memory requests to overlap older writes. This is a cooperative, non-preemptive scheduler within one thread, not parallel tree mutation.

Before using a saved destination, a task checks representative, anchor, reclamation cutoff and the leaf generation and mutation epoch saved with that write rank. Older writes can split or remove leaves; reclamation can free and reuse their blocks; and another write can change ranks in a surviving leaf. Any mismatch causes a renewed search. Node and key-block generation values occupy disjoint spaces, so a recycled key block cannot validate a stale leaf reference. Destinations and the active cursor are redirected, and a right-neighbour check repairs uncertain placement. Split candidates are reused only while their separate split epoch remains valid.

The overlay inserts the new interval, preserves prior-version exteriors and removes covered entries. Final v3 still scans and erases covered entries individually. A whole-interior-leaf removal experiment exists on the separate v3.17 branch, not in the selected release.

## Cleanup and scope

Read checks may lazily refresh expired separators before referencing stored key bytes. They compact each leaf once before using stable ranks, queue empty nodes by pointer/generation and drain the queue after all tasks retire. Space pressure advances reclaimCV_ through the existing node/key allocation-age reclamation. Final v3 has no separate list of cleanup tasks.

The final production source is `f866c98d8585a77280adbf44c72650f16e975f86`, published on branch [resolver-v3](https://github.com/ricardojimenezperis/foundationdb/tree/resolver-v3) and at tag [`resolver-v3-final`](https://github.com/ricardojimenezperis/foundationdb/releases/tag/resolver-v3-final). The older tag `resolver-v3` records the v3.8 milestone at `55d5f98974`; the v3.9 and v3.10–v3.17 branches remain experiments based on that milestone. The [v3 design snapshot](../resolver/v3/03-resolver-v3-design.md) gives the state machine and invariants; the [final evaluation](../resolver/v3/06-evaluation.md) reports the consolidated correctness and HugeTLB performance evidence.

## Alternatives and trade-offs

Applying each accepted interval with the scalar cursor would avoid write-task scheduling, but serialize leaf/key-cache misses. The two-group scheduler overlaps those misses without changing the mutation order. Increasing group capacity can improve prefetch lead while also increasing live state and cache pressure; the selected default is two groups of eight.

Allowing writes in different leaves to mutate before the oldest write would require a dependency proof that covers split and leaf-removal effects as well as current destinations. The selected design retains one writer and avoids that extra coordination. The v3.9 and v3.10–v3.17 branches preserve those experiments outside the selected release.

## Testing, observability and integration

Validate conflict decisions, too-old accounting and optional per-read reporting against the native reference; exercise equal endpoints, a root leaf, expired keys, predecessor/right-neighbour routing, split and leaf redirection, long keys, tight budgets and multi-leaf overlays. A phase-specific benchmark must distinguish read checking, write application, cleanup and the whole batch. Build flags, HugeTLB coverage, source tree and code alignment matter when comparing performance.

Deployment still requires FDB integration and simulation checks beyond component tests. The [validation record](../resolver/v3/05-validation-and-evidence.md) identifies checks completed for the published source without inferring behavior from performance alone.
