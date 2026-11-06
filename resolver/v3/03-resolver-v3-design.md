# Temporal conflict B+ tree · Resolver v3.8 design

[Index](README.md) · [Current Resolver design](../../design/02-resolver.md) · [v2 snapshot](../v2/README.md)

**Selected source:** [resolver-v3](https://github.com/ricardojimenezperis/foundationdb/tree/resolver-v3), commit 55d5f98974b352f3a124439ae86beb0f8b7ca76d. The default write policy is two groups of eight tasks. This snapshot describes that tree, not the later experimental branches.

## Purpose and interface

The Resolver retains write-conflict history. Each retained half-open interval carries the commit version (CV) of the write that established it. A read [rb,re) at read version RV conflicts with a retained write [wb,we) at CV when rb < we, wb < re, and CV > RV. Overlaying [30,60)@120 on [10,90)@100 preserves [10,30)@100 and [60,90)@100 around the new interval.

TempConflictBTree exposes two batch phases:

1. checkReads<G> consumes sorted native batch endpoints, checks eligible reads, and locates the destination leaves of **both read and write begins** in a common descent. It records write destinations but neither applies writes nor advances the reclamation cutoff.
2. addWrites receives transaction admission results and a common CV. It filters rejected and too-old transactions, unions accepted write intervals, and applies the result through grouped write tasks in key order. It may reclaim history to reserve space for a write.

The caller keeps the input endpoints, transaction records and borrowed key bytes alive through both phases. CVs passed to addWrites must be nondecreasing. A failed per-write reserve may throw after earlier writes or reclamation have occurred; the operation is not transactionally rolled back.

![The two batch phases: checkReads checks reads and prepares write destinations; addWrites applies accepted writes after transaction decisions are complete.](figs/fig-v3-batch-phases.png)

*Figure 1. checkReads checks eligible reads and records write destinations without applying writes or advancing reclaimCV. Once transaction decisions are complete, addWrites filters and unions accepted intervals, validates their destinations, and applies them in begin order. Empty leaves queued during read traversal are released after its tasks finish.*

## Storage and retained history

PagePool supplies interchangeable blocks to BNodeStore and KeyStore. The default node/key block size is 2 KiB within a 64 KiB page. Leaves contain logical-order bytes, physical slots, CVs and RangeRef descriptors; internal nodes hold child pointers, separators and conservative maximum CVs. Stored keys can span linked key blocks. A range descriptor refers to key blocks and includes a first-block generation; internal separators reuse range references rather than owning duplicate bytes.

The tree stores disjoint intervals and maintains parent-child links and a doubly linked leaf chain. Insertion and deletion shift logical-order indices rather than every descriptor. Splits, root normalization, exterior preservation and age-based reclamation retain the v1/v2 representation. Empty nodes are removed before an operation returns, except that an empty root leaf may remain.

The pool budget bounds history blocks, **not** temporary batch vectors, scheduler slots or pending cleanup records. The separate Storage Server data-version retention and read-version protection protocol remain outside this component.

## One mixed descent for reads and write destinations

The input is sorted by key. checkReads builds a compact begin sequence, readWriteBegins, with a tagged index for read or write begins. nextBegin links begins of the same type and nextOther skips to the next begin of the other type. Empty reads, reads no longer required by transaction/reporting status and too-old transactions are filtered according to the native batch contract. A read with RV below reclaimCV_ marks its transaction too old; equality is admissible.

A task owns a consecutive slice [opBegin,opEnd) of this **mixed** sequence and a current tree node. At an internal node it binary partitions the slice against a separator, placing equal begins on the right. Each nonempty half becomes a task or reuses the current task; a slice with no remaining boundary descends to one child. The initial task covers the root and all retained begins. The root may already be a leaf.

Write begins participate in routing and in deciding which slices reach a leaf, but they do not acquire a read conflict result. At the leaf the task records the first write in its slice, the leaf, a possible split candidate and whether a right-neighbour check is required. It then handles only the slice's read begins: first-key search, live predecessor, and a forward scan across leaves. Read ends are not sorted, so a resolved read is unlinked from the task's pending subset without moving the begin-sorted records.

![Read and write begins are routed together through the B+ tree; reads are checked and write destinations are recorded at the leaves.](figs/fig-v3-mixed-descent.png)

*Figure 2. checkReads partitions a key-ordered sequence of read and write begins at each internal node, sending begins equal to a boundary to the right. At a leaf, it checks the reads and records a destination for the first write in the slice. Write begins receive no conflict result, and no write is applied during this phase.*

A buffered write-leaf result has a ticket. The write frontier emits only the ordered prefix whose destinations are known; a later task cannot reorder writes by completing first. Buffer slots are recycled after their ticket and all older tickets have been released. read-only slices can be skipped by the write frontier. The emitted destinations are batch-local hints, not durable pointers.

![The write frontier emits prepared destinations in begin order even when leaf tasks finish out of order.](figs/fig-v3-write-frontier.png)

*Figure 3. Leaf tasks may finish out of order, but the write frontier emits only the known prefix of destinations in begin order. It skips read-only slices and holds a later result until every earlier ticket is released. Buffer slots are then recycled; the emitted destinations remain batch-local hints.*

The grouped read scheduler from v2 remains: up to G resident tasks, new right siblings, and suspended tasks. A visit consumes an earlier hint and often emits the next hint. It scans a whole leaf in one visit, then hints the right leaf and yields. V3 routes more begins than v2, but preserves read-conflict semantics and the v2 scheduling model.

### Cleanup while reads are active

During checkReads there is no write insertion or key-block reclamation, though expired leaf entries may be compacted and expired separators refreshed before hinting or comparing their key bytes. A leaf is compacted before its saved rank is used; a batch stamp prevents repeat compaction. Empty leaves are queued as (node,generation) and structural release is deferred until traversal tasks retire. cleanPending processes that queue after the read scheduler completes. The generation is checked because duplicate queue entries are allowed.

## Accepted intervals and their prepared destinations

addWrites first prepares a representative for each write group that landed on a leaf. Multiple groups naming the same leaf use one representative. The read phase may have left write destinations requiring a right-neighbour check; the representative retains that requirement.

The accepted begins are streamed in sorted endpoint order. Rejected or too-old transactions contribute no write. Overlapping accepted intervals are unioned using endpoint indices, without copying keys or allocating one object per interval. Adjacent intervals may remain distinct. The first begin of a merged interval retains the relevant prepared destination. A per-representative preparation record stores its first accepted begin and a mutation counter.

![Accepted write intervals are unioned by endpoint index and reuse destinations prepared during checkReads.](figs/fig-v3-accepted-union.png)

*Figure 4. Rejected and too-old transactions contribute no writes. addWrites unions overlapping accepted intervals using endpoint indices; adjacent intervals may remain distinct. Write groups that reached the same leaf share a representative, and the first begin of each merged interval retains the destination prepared during checkReads. Every write in the call carries its common CV.*

## Two groups of eight write tasks

The default macros are RESOLVER1_WRITE_GROUPS=2 and RESOLVER1_WRITE_LG=8. The two groups hold at most 16 write tasks in total. The optional three-group policy exists in the source, but is not the selected configuration. A zero LG builds the scalar fallback.

A write task carries its accepted interval, destination cursor, leaf-search bounds, scan position, step, and the metadata needed to validate the destination. The scheduler is single-threaded and cooperative: one visit advances a task until it hints a dependency, parks, or completes. It does not create OS threads and it does not perform two tree mutations at once.

The oldest task of the oldest group is the **only writer**. Younger tasks may search and prefetch their own target leaves and stored keys, then park at SearchReady. A parked task is removed from the runnable bitmap, rather than polled on every turn; it is woken when it becomes oldest. The successor group is filled early, its destination leaves are hinted, and its runnable searches start when the oldest group's first index reaches the halfway point. When the oldest group completes, its slot is refilled and the group roles rotate. This timing gives new searches visits in which to mature while older writes continue.

![Two groups of cooperative write tasks overlap searches and prefetches while only the oldest task writes.](figs/fig-v3-write-groups.png)

*Figure 5. The diagram uses four tasks per group to show the scheduling policy; v3.8 defaults to two groups of eight. Visits run on one thread. Only the oldest task writes, while younger tasks search and prefetch before parking at SearchReady. The successor group's searches become runnable halfway through the oldest group; after that group completes, its slot is refilled and the roles rotate.*

The state machine distinguishes inspection and optional right routing, binary search and key comparison, SearchReady, predecessor inspection, insertion, forward scan, end comparison, erase and continuation through linked key blocks. A task can chain already-local steps within a visit; prefetch-dependent steps yield. The scan inserts the new interval, preserves the prior interval's left exterior and the first covered interval's right exterior when applicable, and erases covered entries. It does **not** retire fully covered interior leaves as one operation; that was an experimental v3.17 change.

![States of a cooperative write task, from destination search and parking through insertion and forward erasure.](figs/fig-v3-write-task-states.png)

*Figure 6. Younger tasks search and prefetch, then park at SearchReady. When a task becomes the oldest, it validates its saved destination before using it, reserves space before mutation, and continues through insertion and the forward scan. Steps with local data can run in one visit; a step needing prefetched data yields. An invalid destination or a reservation that changes the destination or reclamation cutoff restarts inspection.*

This is a non-preemptive task scheduler for latency overlap within one core. Its benefit comes from interleaving independent memory accesses while preserving serial write order; the number of tasks is a tunable latency/locality trade-off, not thread parallelism.

## Pointer, split and reclamation safety

A speculative destination is validated before its saved leaf/key pointers are consumed. The task checks its representative, destination anchor, leaf mutation counter and reclaim cutoff. If an older mutation invalidated its destination, a younger task parks. When it becomes oldest, it resets its search in the current destination leaf and reissues a leaf hint. If still valid, it reuses the searched position.

![A write task validates its speculative destination before reusing a saved search position or following a changed leaf boundary.](figs/fig-v3-destination-validation.png)

*Figure 7. If the destination remains valid, the oldest task reuses its searched position. An older split can invalidate that position, requiring a new search in the current leaf. If the split moves the insertion point beyond the leaf's new upper boundary, the right-neighbour check moves the search to the new right leaf. Saved pointers are checked before use.*

Before the oldest task mutates, reserveWrite checks space for key bytes, up to two leaf slots and any required split work. Reserving may reclaim blocks and change the destination or cutoff; the task then restarts inspection. Only after reservation does it prepare split state. Saved split candidates are reused while their epoch remains valid; a changed epoch triggers a parent walk. Splits and leaf removal redirect prepared write destinations and the active cursor. A right-neighbour check repairs a destination whose old leaf or partition boundary no longer identifies the correct insertion point.

![The oldest write task reserves pool space before mutation and restarts destination inspection if reservation reclaims history.](figs/fig-v3-reserve-before-mutate.png)

*Figure 8. After validating its destination, the oldest task reserves space for key bytes, leaf slots and any required split work. If reservation reclaims history, the destination or reclaim cutoff may have changed, so the task restarts inspection. Split state is prepared and the tree is mutated only after the reservation holds. The pool budget covers history blocks, while a failed reserve may leave earlier writes applied.*

The read phase does not reclaim key blocks. In write maintenance, expired CVs are checked before dereferencing possibly reclaimed key bytes. Reclamation advances reclaimCV_ and releases old node/key ages through the existing BNodeStore and KeyStore mechanisms. The v3.8 release does not introduce a separate batched cleanup-task queue or a new reclaimOldest index.

## Integration and validation boundary

BTreeV3.cpp compiles resolver1 inside namespace btree_v3, sharing ResolverBatchTypes.h and FDB StringRef through fdbcompat.h. ConflictSet chooses it through its native integration. BTREE_V3_TASKS defaults to 16 in CMake for checkReads; it is distinct from the write group's 2 × 8 capacity.

Correctness requires identical conflict bits, transaction decisions, too-old accounting and accepted write order under half-open boundaries, equal begins, expired separators, splits, removed leaves, long keys, memory pressure and optional per-read reporting. 

See [validation and evidence](05-validation-and-evidence.md) for the tests and source identities. Performance reports should identify both the measured original commit and its published tree-equivalent commit; this document makes no cross-session speedup claim.
