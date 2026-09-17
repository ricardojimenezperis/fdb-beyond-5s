# Resolver Phase — Generational Paged Conflict Set

## 1. What the Resolver does

The Resolver checks whether a transaction can commit without violating
serializability. If it detects a conflict, the transaction is aborted. Each transaction
provides read and write conflict ranges. The Resolver keeps the write conflict
ranges of committed transactions, together with their commit versions. Read
conflict ranges are used only to query that history; the Resolver stores no
database values (`ConflictSet.cpp:997–1050`).

For a transaction with read version `r`, the Resolver checks whether a write
committed after `r` overlaps one of its read conflict ranges. Writes accepted
earlier in the same batch are included when later transactions are checked.
Overlapping write ranges alone do not cause a conflict, so two transactions
that write the same key without reading it may both commit. FoundationDB's rule
is therefore not simply first-committer-wins.

Conflict ranges have ordered endpoints, and exact overlap checks require that
order to be preserved. The current Resolver stores them in a custom skip list.
It derives the oldest retained version from
`MAX_WRITE_TRANSACTION_LIFE_VERSIONS` and removes obsolete entries individually
(`Resolver.cpp:359`, `ConflictSet.cpp:544–576`, `:986–991`).

For a transaction spanning several Resolvers, one Resolver may record its write
ranges even if another Resolver rejects the transaction. The Commit Proxy
combines their answers and aborts the transaction, but the recorded ranges may
later cause conservative false conflicts.

This is existing FoundationDB behaviour. Retaining conflict history for longer
may preserve those false-positive entries for longer, but it cannot allow an
invalid transaction to commit. The arena-backed representation must preserve
this behaviour; changing it is outside the scope of this design.


This project will preserve the same validation semantics while storing conflict
history in reusable pages. The retention protocol will determine the oldest
version still needed. Garbage collection will compare that version with the
youngest timestamp in the oldest retained page and, when the complete page is
obsolete, reuse it as a unit. Each garbage-collection step will therefore take
O(1) time instead of deleting conflict entries one by one.

## 2. Resolver conflict history in two reusable arenas

The Resolver will continue using its existing `SkipList` and the same conflict-validation rules. 
The change is how its memory is allocated and reclaimed.

Conflict history will be stored in at most two reusable arenas:

* `current` receives all new conflict ranges.
* `previous`, when present, is read-only and contains the preceding history.

Validation will search both arenas and report a conflict if either contains a write newer than the transaction's read version. A range rewritten in `current` does not need to be removed from `previous`: the newer version found across the two arenas takes precedence.

Within an arena, insertion will preserve the existing canonical `SkipList` representation. Replaced interior nodes will be unlinked but not freed individually. Their memory will be recovered when the entire arena is reused.

Garbage collection will be controlled by the retention floor. An arena is obsolete when:

```cpp
retentionFloor > newestVersionInArena
```

At that point every conflict version in the arena is older than every transaction still allowed to commit. The Resolver can therefore reset and reuse the whole arena in O(1), instead of deleting conflict entries one by one.

The Resolver will operate as follows:

* With only `current`, an obsolete arena is reset immediately.
* If a live `current` reaches the configured allocation threshold, it becomes `previous` and the second arena becomes the new `current`.
* With both arenas present, `previous` remains available until it becomes obsolete. It is then reset and reused as the next `current`.
* If both arenas are obsolete, both are reset and the Resolver returns to using a single arena.

The obsolescence check takes precedence over forming a second arena. History that is already obsolete is reclaimed rather than sealed.

Two conditions are required for correctness:

1. A new or reused arena must be initialized with a version no greater than the retention floor. A higher initial version would make untouched keys appear to have been written recently and would create false conflicts.
2. The allocation threshold must be measured in allocated bytes or pages, not in reachable entries. Unlinked nodes remain in their arena until it is reset.

The two arenas place an upper bound on the number of live generations, but not by themselves on total memory. A long-running transaction can prevent an old arena from being reclaimed while `current` continues growing. Memory usage across both arenas must therefore be accounted for and protected by backpressure.

This design preserves the Resolver's existing validation semantics while replacing entry-by-entry garbage collection with whole-arena reuse.

Conflict entries will remain mutable within each arena. When a range is
overwritten, the Resolver will continue removing the obsolete boundaries from
the SkipList so that it remains a canonical map from keys to their latest write
versions.

Backpressure must be based on the total bytes owned by both live arenas. A
reachable-entry count is insufficient because unlinked nodes remain allocated,
while counting only unreachable bytes would miss growth caused by continually
inserting distinct keys.

### Arena formation and rotation

The Resolver starts with a single writable arena, `current`. Maintaining a second arena is unnecessary while little conflict history has accumulated, because it would add another `SkipList` lookup without providing useful separation.

At each serialized batch boundary, the Resolver evaluates the following conditions in order:

```cpp
if (retentionFloor > current.newestVersion) {
    reset(current);
    remainInSingleArenaMode();
} else if (current.ownedBytes >= arenaFormationThreshold) {
    previous = seal(current);
    current = openEmptyArena(/* initialVersion <= retentionFloor */);
    enterTwoArenaMode();
}
```

Obsolescence is checked first. If all history in `current` is already older than the retention floor, the arena is reset and the Resolver remains in single-arena mode. Obsolete history must not be sealed merely because it has reached the formation threshold.

Otherwise, the second arena is opened when the memory owned by `current` reaches `arenaFormationThreshold`. The existing arena becomes the read-only `previous` arena, and the second arena becomes the new writable `current`.

The threshold is measured in allocated bytes or allocator pages, not in versions or reachable entries. Unlinked nodes remain allocated until their arena is reset, so an entry count would not represent the arena's physical size.

The threshold has two purposes:

1. Avoid the additional lookup cost of two `SkipList` instances while the conflict history is small.
2. Accumulate enough history in an arena to amortize sealing, resetting and reusing it.

The threshold only decides when the second arena is formed. It does not decide when history is reclaimed. Reclamation is always controlled by the retention floor.

While two arenas exist, all new writes go to `current` and validation searches both. When `previous` becomes obsolete, its arena is recycled. If `current` still contains required history, it becomes the new `previous` and the reset arena becomes the new `current`. If both arenas are obsolete, both are reset and the Resolver returns to single-arena mode.

After returning to single-arena mode, the same formation rule applies again: a second arena is not opened until the live `current` arena reaches `arenaFormationThreshold`.

The threshold is a soft target because one batch may take the arena beyond it. It is also distinct from the memory limit: backpressure must use the total bytes owned by both live arenas.


### Reclamation unit

The unit of reclamation is an entire arena, not an individual allocator page.
Nodes in one arena may reference other nodes from the same arena across page
boundaries, so an individual page cannot be reclaimed independently.

Each arena records the newest commit version written into it. When the
retention floor is greater than that version, all conflict history in the arena
is obsolete and the whole arena can be reset in O(1).

Boundary nodes may contain versions older than the arena itself. This is
expected: those values preserve the canonical map when a written range splits
an existing region. Reclamation depends only on the newest version in the
arena, so these older values do not require special handling.


## 3. Implementation sequence

The work will be implemented in two stages.

### Phase A: connect the retention floor

The Master will compute the oldest read version that must remain valid from the values reported by GRV Proxies and Commit Proxies. The Resolver will use the installed floor for both transaction admission and conflict-history reclamation.

The Commit Proxy must retain its `keyResolvers` history to the same floor. Otherwise, a transaction could be sent to the wrong Resolver even though the required conflict history still exists.

The effective floor depends on the compatibility mode:

```cpp
if (legacyClientsSupported) {
    effectiveFloor = installedFloor.present()
        ? std::min(ordinaryFiveSecondFloor, installedFloor.get())
        : ordinaryFiveSecondFloor;
} else {
    effectiveFloor = installedFloor.present()
        ? installedFloor.get()
        : currentVersion;
}
```

When legacy clients are supported, reported transactions may extend retention beyond five seconds but may not shorten the existing window. When legacy clients are disabled, only reported live transactions and accepted commits still being validated retain history.

The floor-tracking protocol is specified separately. The Resolver only consumes the floor installed by the Master; it does not maintain another source of transaction information.

### Phase B: replace per-entry reclamation

The existing Resolver deletes obsolete conflict entries incrementally. After a long transaction releases an old floor, many entries can become reclaimable at once. The current cleanup budget is tied to subsequent write traffic, so reclaiming that accumulated history may take a long time or require concentrating substantial work in later commit batches.

The two-arena design replaces that process. Conflict history remains in the existing `SkipList` representation, but each generation is allocated from a reusable arena. Once the retention floor passes the newest version in an arena, the Resolver resets the whole arena in O(1).

Phase A can be implemented and tested first, but the longer transaction window must not be activated until Phase B is available. Otherwise, releasing history retained by a long transaction could still create a large entry-by-entry cleanup backlog.


## 4. What the measurements established

The original model predicted that replacing entry-by-entry reclamation would substantially increase Resolver throughput. The measurements do not support that prediction.

In the measured workload, the floor sweep and the interior deletion walk together accounted for approximately 8.4% of conflict-detection time. Conflict lookup accounted for 61.5%. Additional experiments found no workload in which the reclamation savings compensated for the lookup cost of consulting two `SkipList` instances.

The two-arena design is therefore not proposed as a throughput optimization. Its purpose is to make reclamation predictable when a long transaction releases a large amount of retained conflict history.

With the current structure, reclaiming that history requires walking and deleting individual entries. Progress is also tied to later commit batches and their write volume. A large cleanup backlog can therefore remain for many batches or require more work to be placed on the commit path.

With the proposed structure, reclamation resets an entire obsolete arena. Its cost no longer depends on the number of conflict entries stored in that arena. The prototype must verify that the allocator provides this operation with effectively O(1) latency.

The prototype must measure four costs:

1. The lookup cost of consulting up to two `SkipList` instances.
2. The memory retained by nodes that have been unlinked but remain allocated until their arena is reset.
3. The latency of resetting and reinitializing an arena.
4. Peak memory while an old arena is retained and the current arena continues growing.

The design is acceptable only if these costs remain bounded and predictable. No Resolver throughput improvement is assumed.

Detailed measurements and the withdrawn historical model are kept in `../benchmarks/measurement-results.md`.


## 5. Future wire compression

Conflict ranges must continue travelling from Commit Proxies to Resolvers. Compressing them is independent of the arena-backed reclamation design and is not part of its first implementation.

Ranges are normally sorted within each transaction, but a complete Commit Proxy batch is not globally sorted. Delta encoding could therefore either restart for every transaction or require an additional merge on the commit path. Both options must be measured before selecting a wire format.

General-purpose compression may also be evaluated later. Stateful key interning is out of scope for the initial implementation.


## 6. Alternatives considered

### Continue deleting entries incrementally

The existing Resolver removes obsolete nodes individually. A more sophisticated controller could make the cleanup budget depend on reclaimable memory or elapsed time instead of subsequent write traffic.

This would preserve the current representation, but reclamation would still require traversing and destroying every obsolete node. It would also introduce a controller that must balance memory recovery against commit latency. The arena design avoids that trade-off by reclaiming a complete generation at once.

### Reclaim individual pages behind a global index

One option was to divide the existing ordered structure into pages and keep a global linked index over them.

This does not permit independent page reclamation. A node in one page may be needed as the predecessor, range boundary or navigation link for a node in another page. The current `removeBefore` implementation already reflects this dependency by retaining a node when either it or its predecessor is still needed (`ConflictSet.cpp:560–563`).

Reclaiming a page would therefore require repairing the global index, making reclamation proportional to the affected structure instead of O(1).

### Store immutable records and rebuild the index

Another option was to append every conflict-range update as a new immutable record and periodically rebuild a canonical index.

Without removing superseded boundaries, ranges overlap and validation becomes an interval-overlap query rather than the existing `SkipList` lookup. The writable generation would need an additional interval index even before rebuilding.

The existing insertion algorithm already maintains a canonical map efficiently. Keeping it and reclaiming its arena as a unit avoids both the additional index and the rebuild.

### Keep redundant links and repair them after reclamation

Redundant pointers could allow navigation to continue after some pages were removed, followed by a sweep that repaired the remaining links.

The repair sweep would again traverse and modify individual nodes after reclamation. This moves the entry-by-entry work rather than eliminating it, so it does not provide O(1) garbage collection.

### Interlace the old and current generations

The two generations could be cross-linked so that validation traversed them as one structure instead of performing two independent searches.

This would make insertion, rotation and reclamation more complex. It is beneficial only when almost every query needs both generations; the measurements did not show that pattern. Two independent lookups are simpler and keep the arenas completely separable.

### Replace the SkipList with an ART or radix tree

A different ordered index could be created inside each arena.

This would require implementing and validating a new conflict-index structure without improving the reclamation rule: the arena would still be the unit that becomes obsolete. The existing `SkipList` is already optimized for Resolver range validation, so the initial implementation will retain it.



## 8. Status

The Resolver design is complete and ready for implementation.

Implementation will proceed in two stages:

1. Connect the retention floor installed by the Master to the Resolver and to
   the Commit Proxy's `keyResolvers` history.
2. Replace entry-by-entry conflict-history reclamation with the two-arena
   design.

The two-arena design is intended to provide predictable, effectively O(1)
reclamation rather than higher general throughput. Measurements performed
during implementation will validate its lookup cost, memory amplification,
arena-reset latency and peak memory usage.

