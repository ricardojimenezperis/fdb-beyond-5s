# Resolver v1 · Decisions and evolution

[Documentation index](README.md) · [Design](03-resolver-v1-design.md)

This record explains the decisions behind the sequential resolver v1: the problem each decision addresses, the mechanism chosen, and its benefits and costs. The design document contains the detailed algorithms and examples. Decision identifiers group the rationale by mechanism; they do not imply a chronology.

The snapshot uses 2 KiB blocks and is associated with campaign commit `aff127286e68ea7933dc9ef502a7dbd760e1e43d`. Subsequent read algorithms are distinguished from the v1 implementation.

## Architectural intent

The resolver records write-conflict history, not database values. Accepted write ranges modify that history; read ranges query it for overlapping writes newer than the transaction's read version. The objective is to retain more usable history within a fixed memory budget while providing a foundation for efficient batch processing.

The representation combines a B+ tree, compact node descriptors, separately stored key bytes, and one shared storage pool. Its memory savings come from several decisions working together: amortizing node metadata, sharing point endpoints, referencing existing separator keys, and avoiding unnecessary entry movement. The cost is additional indirection and maintenance of temporal and structural invariants.

## Data representation and storage

### D1 — Use a B+ tree to amortize index overhead

**Decision:** represent write-conflict history in a temporal B+ tree. Leaves hold ranges and versions; internal nodes hold navigation references. V1's 2 KiB layout supports 40 children per internal node and 59 entries per leaf in the reported build.

**Expected benefit:** grouping many entries in each block amortizes headers and links, increases fanout, and creates opportunities to reuse index data across nearby searches. More compact history can extend retention under the same budget.

**Cost:** in-node searches, splits, separator maintenance and workload-dependent occupancy replace the skip list's navigation and update mechanisms. External key access can still stall comparisons. A shallower structure does not automatically mean lower latency, and retention gains do not establish a speedup.

**Design:** “Overall approach” and sections 3 and 14. See [evaluation](06-evaluation.md) for measured outcomes.

### D2 — Reference existing separator keys instead of copying inline prefixes

**Decision:** each separator uses a `KeyRef` to the minimum key of its child. Neither a complete key copy nor a reserved inline prefix is stored alongside it. References at multiple levels can reuse the same key bytes.

**Benefit:** avoids duplicated key or prefix payload in internal nodes. It also supports the later plan to prefetch only the external keys that active searches need to compare.

**Cost:** comparisons generally require external-key access. An inline prefix could resolve some comparisons inside the node; omitting it trades that opportunity for space. Reference size and access latency must be evaluated together. This decision does not imply that `KeyRef` is smaller than every alternative representation.

Separator refresh follows the referenced minimum's CV and lifetime, not the date on which a split promoted it.

**Design:** “Principal design decisions” and sections 4–7.

### D3 — Share one physical pool between nodes and keys

**Decision:** allocate both node blocks and key-record blocks from one `PagePool`, with no fixed quota for either role. `KeyStore` and `BNodeStore` are specialized managers over that pool, not separate physical stores. `TempConflictBTree` coordinates them.

V1 uses 64 KiB pages containing 32 blocks of 2 KiB. Allocation pops a block from the free list; release returns it. A released node block can subsequently hold keys, and vice versa.

**Benefit:** memory is not stranded in a fixed partition when the balance between index structure and key payload changes. Allocation and release are constant-time local free-list operations.

**Cost:** node size and key-block granularity are coupled. Changing block size changes fanout, header overhead, key chaining and reclamation granularity together. The pool budget covers page storage, not all process memory: transient search and append vectors also require accounting.

**Design:** sections 2–4.

### D4 — Move slot indices instead of complete entries

**Decision:** retain descriptors in physical slots and express sorted order through a permutation array, `order`. Each v1 order element is a one-byte slot index. A bitmap records which physical slots are free.

Insertion chooses the lowest free slot and shifts only order indices. Deletion removes the corresponding index and marks its slot free without compacting surviving descriptors. Parent links identify physical slots through `parentSlot`, so changing logical ranks does not invalidate those links.

For example, inserting slot `1` at rank `1` changes `order` from `[2, 0, 4]` to `[2, 1, 0, 4]`; descriptors in slots `2`, `0` and `4` stay in place. Deleting slot `0` then leaves `[2, 1, 4]` and makes slot `0` reusable.

**Benefit:** ordinary insertions and deletions move bytes rather than complete range or child descriptors. The bitmap avoids descriptor movement during deletion and supports slot reuse.

**Cost:** searches add an indirection; `parentRank()` scans the parent's order array to recover a logical rank. Splits still move descriptors between nodes and repair moved children's parent links.

**Design:** sections 3 and 11.

### D5 — Separate key payloads and avoid duplicating point endpoints

**Decision:** keep fixed-size descriptors in nodes and append variable-length key records into separate, linked blocks from the same pool. General ranges store begin bytes followed by end bytes. A point interval `[k,k\0)` stores only `k\0`: the begin key is the prefix of the same record, with `endStart = 0`.

**Benefit:** key length does not directly change node capacity, and node splits move descriptors without copying all referenced key bytes. Sequential record placement is independent of index order.

Point efficiency is an explicit goal. An isolated point in FDB's skip-list history uses separate begin and end boundary nodes containing `k` and `k\0`. Sharing the endpoint payload avoids repeating `k`. For a key of length `L`, the point record needs `L + 1` endpoint bytes instead of `2L + 1`, saving `L` bytes. A 16-byte key requires 17 endpoint bytes instead of 33; the interval occupies one leaf descriptor.

For `["abc","abc\0")`, the stored bytes are `61 62 63 00`: the begin reads three bytes and the end reads four from the same starting position.

**Cost:** comparisons access external payloads and may traverse block boundaries. Appending allocates the required new blocks before modifying existing storage and can fail. The allocation reserve is sized for the key copies an update can require. Endpoint savings are not total-memory savings: descriptors, block headers, occupancy and allocation slack also count. Existing boundaries and merged intervals affect FDB's node count.

**Design:** sections 3–4 and 9.

## Traversal and updates

### D6 — Descend once, then scan linked leaves for range reads

**Decision:** locate the read's begin key, check the possible predecessor, and scan entries forward through linked leaves until a conflict is found or the read's end is reached. Test versions before accessing potentially expired key bytes.

**Benefit:** a read can cover regions associated with several root-to-leaf paths without restarting from the root for each leaf. Checking the predecessor catches writes that begin before the read but overlap it. Early termination avoids further work once a newer overlapping write establishes a conflict.

**Cost:** the scan depends on the number of candidate entries and may cross many leaves. V1 handles reads individually; it does not implement grouped traversal or constant-time conflict resolution. Already-conflicting transactions are skipped without clearing their status.

**Design:** “Checking reads” and sections 7–8.

### D7 — Materialize write overlays and preserve exterior versions

**Decision:** store ordered, disjoint half-open intervals representing the latest relevant write version over covered key regions. Accepted writes replace covered history; fully covered entries disappear, and surviving exteriors retain their previous CVs. This preserves the skip-list resolver's conflict semantics while changing the representation.

For example, overlaying `[c,g)@20` on `[a,z)@10` produces `[a,c)@10`, `[c,g)@20`, `[g,z)@10`. New coverage also fills gaps; the update is not limited to intersections with existing entries.

**Benefit:** retains the information needed for read-conflict detection without storing every overlapping historical write as a separate record. Keeping the old CV on exteriors avoids false conflicts; updating the interior avoids missed conflicts.

**Cost:** writes can copy exteriors, remove many entries, and update minima across leaves. Their cost depends on affected coverage. Accepted ranges must satisfy the sorted, disjoint and nondecreasing-version input contract. The prepared insertion leaf must remain the actual insertion destination.

**Design:** “Materializing writes” and sections 1 and 9.

### D8 — Avoid split cascades through proactive capacity reservations

**Decision:** prepare capacity before insertion rather than allowing overflow to trigger successive ancestor splits. `needsSplit` is computed from node state:

- A leaf needs splitting when at most one slot is free, because a write can insert both a new interval and a surviving right exterior.
- An internal node counts its immediate children needing a split in `childrenNeedingSplit`. It needs splitting when that count equals its free-slot count.

Let `F` be an internal node's free slots and `P` its pending child splits. Maintain `P <= F`: each pending child split has a reserved parent slot. Equality is the proactive threshold, even if the node is not physically full.

During descent, `findKey` records pending internal candidates. After cleanup, preparation chooses the leaf if pending, otherwise the deepest internal candidate that still needs splitting. Only one existing node is split per `addWrite` preparation; splitting the root additionally creates a root.

**Why it avoids a cascade:** replacing one pending child with two nonpending halves consumes one free slot and removes one pending contribution:

```text
F' = F - 1
P' = P - 1
F' - P' = F - P
```

The parent's split state is unchanged. Later insertion can propagate a split-state transition as metadata without immediately splitting that ancestor.

Internal splits balance weight: a pending child has weight two, another child weight one. Total weight is `N + P`, where `N` is child count. At the threshold it equals node capacity. Splitting approximately by half the weight distributes existing occupancy and reservations, leaving both halves nonpending.

For example, an illustrative capacity-eight node with six children and two pending children has `F = P = 2`. Splitting a pending leaf changes it to seven children with `F = P = 1`; no parent split is required. A later write into a nonpending leaf can select that internal node for its own split. Design section 10.6 follows both operations numerically.

**Benefit:** distributes structural split work across writes instead of concentrating a chain of splits in one insertion.

**Cost:** counters and transitions are correctness-critical. Cleanup can change candidates, so they must be rechecked. Entry movement, metadata propagation, key copying, overlap removal and reclamation still cost work; this is not a constant-time write or a fixed latency guarantee.

**Design:** sections 7 and 10–11.

### D9 — Combine upward metadata maintenance in one ascent

**Decision:** `propagateUpdates` handles split-state changes, child-minimum changes and conservative maximum-CV increases together. Each effect continues only as far as needed: split deltas while ancestor split states change, minimum updates along a first-child chain, and maximum updates where bounds increase.

**Benefit:** reuses the existing `parent` and `parentSlot` links and the minimum key reference obtained for propagation. No extra reverse pointer in key records is required for this ascent.

**Cost:** structural identity, minimum-key lifetime and temporal bounds must remain consistent. Reordering logical ranks does not alter physical parent slots, but moving children during a split requires repairing them.

**Design:** sections 3, 6 and 11.

## Reclamation and occupancy

### D10 — Maintain conservative ages instead of recomputing exact maxima

**Decision:** node and subtree maximum CVs are conservative upper bounds. They may increase, but deletion does not recompute them downward. Nodes belong to an intrusive age list; raising a node's maximum moves it to the tail when necessary.

**Benefit:** avoids scanning descendants to recover an exact maximum after each deletion. Conservative bounds permit safe identification of old subtrees, and a nondecreasing update stream supports tail insertion into the age list.

**Cost:** a node can retain an old maximum after its newest entries disappear, delaying reclamation. Retention and reclamation efficiency therefore depend on the workload and require measurement.

A separator's CV has a different meaning: it belongs to the referenced child minimum. Promoting that key during a split does not assign the splitting write's CV to it. Likewise, copying an exterior preserves its original CV even though its bytes are newly allocated.

**Design:** sections 5–6, 9, 11 and 13.

### D11 — Reclaim storage in bulk and clean stale descriptors safely

**Decision:** maintain a monotone `reclaimCV_`, compare the oldest eligible key block and node, and reclaim blocks and subtrees under memory pressure. Clean remaining expired descriptors during traversal, refreshing separators before comparing their referenced keys.

Logical age, physical lifetime and key order remain separate concerns. An entry may be expired while its bytes are still allocated. A reference's generation identifies whether its first block has been reclaimed, without dereferencing that potentially reused block. Ordered key-block reclamation ensures continuation blocks do not disappear ahead of a live record's first block.

**Benefit:** avoids a root traversal for every expired interval. Reclamation operates on blocks and subtrees, while version checks and separator refresh preserve safe navigation.

**Cost:** cleanup can change structure and invalidate cursors. Reads may clean expired entries and remove empty nodes, although they add no logical write history and do not invoke the pressure-reclamation loop. Physically releasing a detached subtree still visits its nodes. Stored key access assumes no concurrent reclamation; this design does not supply a concurrent-access protocol.

The caller must reject reads older than the retained-history boundary. Local pressure reclamation is separate from the future distributed protocol that protects active transactions through minRV.

**Design:** sections 5–8 and 12–13.

### D12 — Let temporal reclamation and new insertions regulate node occupancy

**Decision:** unlink and release empty nonroot leaves, remove empty ancestors, and normalize the root. An empty root is released; an internal root with one child is replaced by that child. Partially filled siblings are neither redistributed nor merged.

**Rationale:** a node that stops receiving writes will eventually become old enough to be reclaimed as the retention boundary advances. A node that continues receiving writes can reuse the slots freed by expired entries. Its occupancy evolves as insertions fill holes and temporal cleanup creates new ones.

The working hypothesis is that this turnover provides enough natural occupancy regulation that actively redistributing or merging partially filled nodes would cost more than it saves. The design therefore relies on temporal reclamation and new insertions rather than performing additional work to keep occupancy evenly distributed across siblings.

**Benefit:** avoids entry movement and sibling rebalancing during deletion. Free slots remain available for subsequent writes, while inactive regions eventually leave the retained history. Parent and physical-slot links locate the descriptors that must be removed or updated.

**Cost:** this is a workload-dependent hypothesis, not a minimum-occupancy guarantee. Sparse writes can keep partially filled nodes alive, and new insertions need not target the nodes with the most free space. Occupancy, fanout and retention must therefore be measured under sustained insertion and temporal reclamation to determine whether explicit rebalancing would justify its cost.

**Design:** sections 3, 12 and 13.

## Preparation for future optimizations

### D13 — Prepare the representation for grouped reads and selective prefetch

**Decision:** keep nodes compact and key bytes external so a later traversal can keep several searches in flight, identify their upcoming key comparisons, and prefetch those bytes while advancing other searches.

**Expected benefit:** overlap external-key latency with useful work without reserving inline prefixes for every separator. Prefetch can target the keys actually needed rather than every key in a visited node.

**Cost and version boundary:** v1 is sequential. Group scheduling and selective prefetch belong to subsequent versions; their benefit depends on dependencies, prefetch distance, cache behavior and bandwidth. This rationale is not a claim that v1 already hides those latencies.

**Design:** “Principal design decisions” and sections 8 and 14.

## Decision record maintenance

For each substantive change, record the triggering observation, alternatives considered, chosen implementation, affected invariants, evidence and remaining costs. Link the accepted decision to the corresponding component and version design. Keep superseded decisions visible with their replacements. The journal records when work and experiments happened; this document explains the technical rationale.
