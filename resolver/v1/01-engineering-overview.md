# Resolver v1 · Engineering overview

[Documentation index](README.md) · [Detailed design](03-resolver-v1-design.md) · [Measurements](06-evaluation.md)

## Problem

A transaction resolver must determine whether a read range intersects a write committed after the transaction's read version. A finite conflict-history window limits how far back this check can be performed safely. Retaining more history consumes memory, so extending the usable window requires attention both to representation and to the cost of discarding old information.

Resolver v1 addresses the local data-structure part of that problem. It stores a disjoint range representation of retained writes, uses a shared memory budget for nodes and key bytes, and advances an explicit reclamation cutoff when space is needed. Correct admission of old reads remains a caller responsibility.

## Contribution

The design is more than a B+ tree replacing a skip list. Several mechanisms work together:

1. **Shared allocation budget.** Fixed-size blocks can hold either a node or key records. Memory is not permanently partitioned between an index and its payload.
2. **Physical slots with logical ordering.** Insertions and deletions move compact rank-to-slot mappings rather than shifting every range or child descriptor.
3. **Separated versions with different meanings.** Entry CV determines conflict semantics; separator CV identifies the age of its referenced minimum; conservative node maxima support reclamation.
4. **Split preparation.** A leaf reserves capacity for both an inserted write and a surviving right exterior. Internal nodes track children needing splits so a child split can be absorbed by its parent.
5. **Reclamation-safe references.** Key generations identify reclaimed records without dereferencing a reused block. Search and cleanup check age before touching potentially invalid key bytes.
6. **Subtree removal.** An expired subtree is detached as a unit and its nodes released recursively, rather than deleting each interval via a separate search.


## One example of the reasoning

Writing `[c,g)` at CV 20 over an existing `[a,z)` at CV 10 must produce `[a,c)@10`, `[c,g)@20`, and `[g,z)@10`. The old version survives on both exteriors. This explains both the need for two insertion slots and why a split must preserve the insertion position in the leaf prepared for it.


## Evidence available at this snapshot

| Dimension | Evidence | Limit |
|---|---|---|
| Memory | Reported v1 accounting allocation of 6,442,188,800 B under 6 GiB | Not a process RSS claim; temporary allocations require separate accounting |
| Retention | Minimum 582.938 s after first discard; median 584.72 s over the last 600 s of trace time | Slow residual decline; final interval treated as practically stable, not an infinite-duration guarantee |
| Baseline | FDB 265 s is the largest tested window fitting the budget; 270 s exceeds it | Calibrated interval is [265,270) s |
| Behavioral equivalence | Matching full-run offered-work and decision hashes; no reads rejected | Workload-specific evidence; full log checks are documented in the campaign report |
| Native execution time | V1 3,375.50 s versus FDB 1,240.68 s; reported ratio 2.721× | Three isolated native rounds; same workload and memory budget, different retained-history lengths |
| Regression discipline | Prepared-leaf failure, corrective revision and named regression | See the evidence register for validation scope and artifacts |

## What this snapshot demonstrates—and what remains open

The design combines memory layout, interval semantics, allocation lifetime, and B+ tree structural invariants. Under the measured 6 GiB counted-history budget, v1 retains 2.200× as much history as FDB's calibrated window and takes 2.721× as long to resolve the same offered workload. This establishes the retention/time trade-off of the sequential implementation.

Remaining work includes profiling execution cost, measuring occupancy under sustained churn, validating write-time allocation reserves for boundary key lengths, and coordinating resolver retention with storage-version retention and distributed minRV tracking. Grouped traversal and selective prefetch are planned optimizations.

For a technical review, start with [invariants and write processing](03-resolver-v1-design.md), then the [decision record](04-decisions-and-evolution.md), and finally the [measurement boundaries](06-evaluation.md).
