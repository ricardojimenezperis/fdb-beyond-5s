# Resolver v2 · Engineering journal

[Index](README.md)

## 22 October 2026 — Node layout and block size

The first changes separated the common node header from the storage used by each node type. Leaves and internal nodes now keep their own logical-order array and free-slot bitmap. The bitmap is sized from the node capacity, rather than assuming that every node fits in one machine word. Logical order still uses one byte per entry; moving those indices avoids shifting whole descriptors on ordinary insertion and deletion.

Block size became a compile-time choice through `RESOLVER1_BLOCK_SIZE`, with 2 KiB as the default. Tests that assumed particular capacities or byte counts were changed to derive them from the selected layout. This made it possible to compare block sizes without changing the implementation for each build.

## 23–25 October 2026 — Shared batch records and range references

Grouped reads needed the resolver's existing transaction records, read ranges and sorted endpoints. Their definitions moved into `ResolverBatchTypes.h`, shared by the FDB integration and the standalone component. This avoided translating the whole batch into a second set of records before checking conflicts.

`RangeRef` replaced the separate key-reference representation. A leaf range and an internal separator can refer to the same stored record; a separator compares only its begin. Point ranges keep one copy of the endpoint bytes. The change removed the old begin-key and end-key accessors and made the shared ownership of key bytes explicit.

Deferred cleanup also needed a way to recognize a node released after it was queued. Cleanup records retain the node's generation, and release clears that field. The field was placed beyond the bytes overwritten by the pool's free-list link. Key-block generations remain a separate mechanism: allocation-order reclamation lets a range reference be validated against one generation boundary.

## 28–29 October 2026 — Partitioning reads into tasks

The grouped traversal starts with the sorted read begins and partitions them around internal-node boundaries. A task retains a read slice and the search bounds still to resolve. When both sides of a partition have reads, the current task keeps the left side and a new task receives the right. A task descends as soon as its slice belongs to one child, even while another task is still searching boundaries in the parent.

The scheduler uses G resident slots and a separate visitation order. New right tasks wait until the end of the round. Admission interleaves their indices after their left halves, retains the earliest read slices, and suspends the remaining suffix in reverse order. Selected resident tasks stay in their slots; suspension and admission copy task records.

Early scans returned to the scheduler for individual read–write comparisons. Profiling attributed substantial cost to dispatch, prefetch issuance and task bookkeeping. Scanning an entire leaf in one handler removed those per-pair transitions. A task now yields at a leaf boundary and prefetches the next leaf before resuming there.

A linked subset keeps only unresolved reads. Read ends are not sorted, so a read that has finished may need to be removed from the middle of the sequence. The scan also remembers the prefix of begins already reached by a write end. Since live writes are disjoint and ordered, those begins do not need to be compared again against later write ends.

The separate prefetch and work dispatches were then combined. Each task visit consumes the access hinted earlier and normally issues the hint for its next visit. This leaves one scheduler dispatch per resident task per round. Local continuations, such as starting the scan after its initialization, can run directly within that visit.

## 29–30 October 2026 — Expiration and cursor safety

Several tasks can reach the same leaf through descent, forward scanning or predecessor lookup. Removing expired entries changes logical ranks. Each leaf therefore carries a read-batch stamp: its first traversal visit cleans it before saving a position, and later visits preserve that rank mapping. A leaf left empty stays in the chain until traversal ends, so other tasks can still follow its links. Generation-checked cleanup releases it afterwards and skips duplicate records for nodes already released.

Expired separators exposed a different problem. Their references can survive after the key blocks they name have been reclaimed and reused. The partition search now checks the selected separator's CV before hinting its key. If necessary, it refreshes the reference from live history in the child. That descent can stall, but it avoids reading reclaimed bytes and leaves saved internal-node bounds intact.

A wholly expired subtree cannot simply discard its read task: a long read may extend into live history to the right. Empty-child markers route conservatively left, and the forward leaf scan reaches later writes. The [design](03-resolver-v2-design.md) describes the routing and cleanup rules in detail.

The component suite reached 217 tests, including 16 grouped-read tests. The public-series preparation also corrected two test assumptions that failed at other block sizes: a fixed fanout of 40 and a fixed 4 MiB pool. The revised suite passed at 1, 2, 4 and 8 KiB. Reclamation was also exercised through replay, comparing decisions under memory pressure; unit-test coverage and replay evidence are distinguished in the [validation report](07-evidence-and-validation.md).

The implementation was regrouped into ten buildable commits on the public v1 base. Each implementation state was compiled and tested, including the FDB build and unit tests. The regrouping preserves the development history separately from the public series, so the measured source identities remain traceable.

## 30 October 2026 — Comparable performance measurements

The measurements showed why the benchmark paths needed to stay separate. FDB's conflictset range workload accepts only 16.5% of transactions; skipping the remaining reads of an already-conflicted transaction matters much more there than in the point workload. Results from that workload cannot stand in for the shorter intervals used by skiplisttest.

The standalone reference also needed correction. Its original baseline came from a later source state than the sequential tree used in the integrated benchmarks. Repeating the Intel standalone campaign with the fork's sequential implementation and one common harness gave read-throughput ratios of 2.11× on points and 1.46× on ranges. Write throughput was 0.88× and 0.91× respectively. The earlier runs remain historical records rather than being mixed into the new samples. Endpoint preparation and sorting are outside the grouped-read timer in this harness.

On the long trace, v2 retained the same final minimum history as v1: 582.938 seconds under the same counted-history budget. Retention was still declining at the end of the measured segment. Writes accounted for 72.5% of v2's native time in the first round, while read checking accounted for 23.0%.

The [evaluation](06-evaluation.md) keeps the workloads and machine sessions separate and links the records behind the tables and figures. The [design history](04-decisions-and-evolution.md) follows the implementation choices in more detail.
