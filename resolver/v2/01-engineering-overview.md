# Resolver v2 · Engineering overview

[Index](README.md)

V2 improves the read path of the temporal conflict B+ tree. It groups nearby reads during navigation, issues prefetch hints before later task visits, and processes pending read/write pairs directly within a leaf. Development v2.6 adds safe lazy refresh of expired separators before prefetching or comparing their keys, including wholly empty internal subtrees.

The history representation remains disjoint write intervals carrying commit versions. The pool and reclamation machinery retain v1's longer-history objective. Write materialization still performs an individual `addWrite` per accepted interval; grouping writes is the next milestone.

| Area | Final v2 behavior |
|---|---|
| Input | Eligible begins extracted from the batch's existing sorted endpoints |
| Internal search | Binary partition of read slices around central separators |
| Parallelism within one thread | Up to G consuming-state tasks; new and suspended tasks supply more work |
| Scheduler | One dispatch per task visit; ordered admission at round end |
| Leaf scan | Fused scan with a linked subset of unresolved reads and a reached-begin prefix |
| Cleanup | First-visit leaf compaction; deferred empty-node removal |
| Separator safety | Refresh the selected expired separator before issuing its key hint |
| Writes | Existing scalar overlay, exteriors, proactive split and reclamation path |

The same-session Intel campaign reports v2.6 at 0.891× FDB throughput on conflictset points and 0.967× on ranges. In the custom benchmark through FDB, range reads reach 1.138× FDB, while point and range writes reach 0.522× and 0.651× respectively. These are different workloads and phases; they must not be collapsed into one speedup.

On the long trace at a 6 GiB counted-history budget, reported minimum retained age is 582.938 s, matching v1 within a batch. Retention is still drifting downward, so this is not a steady-state claim. Writes consume about 72.5% of v2.6's execution time there. Historical FDB/v107 timing references used different conditions and are not controlled speed comparisons.

The next performance work therefore targets writes and then independently evaluates pool backing. A common read/write descent, stable task ownership, leaf write-destination arrays and HugeTLB support are not implemented by this v2 design.
