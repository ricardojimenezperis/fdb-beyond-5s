# Resolver design

This document defines how the Resolver will retain conflict history using
two reusable arenas and a retention floor supplied by the Master.

The retention protocol is specified in `01-floor-tracking.md`.

References to current FoundationDB internals refer to
`apple/foundationdb` main at `a443d3ee60`.

## 1. What the Resolver does

The Resolver checks whether a transaction can commit without violating
serializability. If it detects a conflict, the transaction is aborted.
Each transaction provides read and write conflict ranges. The Resolver
records write conflict ranges with their commit versions and checks
incoming read conflict ranges against that history. It stores no database
values (`ConflictSet.cpp:997–1050`).

For a transaction with read version `r`, the Resolver checks whether a write
after `r` overlaps one of its read conflict ranges. Writes accepted earlier
in the same batch are included when later transactions are checked.
Overlapping write ranges alone do not cause a conflict, so two transactions
that write the same key without reading it may both commit.

The current Resolver stores this history in a custom `SkipList`. It derives
the oldest retained version from `MAX_WRITE_TRANSACTION_LIFE_VERSIONS`
and removes obsolete entries individually (`Resolver.cpp:359`,
`ConflictSet.cpp:544–576`, `:986–991`).

For a transaction spanning several Resolvers, one Resolver may record its
write ranges even if another rejects the transaction. The Commit Proxy
combines their answers and aborts the transaction, but the recorded ranges
may later cause false conflicts. Longer retention may preserve these
entries for longer. This design will preserve the existing validation
semantics; changing how global commit outcomes reach Resolvers is outside
its scope.

The proposed design will replace individual deletion with whole-arena
reclamation. The retention floor will determine when an arena is obsolete,
and its storage will then be reused without visiting its individual nodes.

## 2. Conflict history in two reusable arenas

The Resolver will retain its existing `SkipList` representation and
conflict-validation algorithm. Each list will allocate its nodes from a
separate reusable arena.

At most two arenas will be active:

- `current` receives all new conflict ranges.
- `previous`, when present, is read-only.

Within each arena, insertion will maintain a canonical map from keys to
their latest write versions. When a range is overwritten, obsolete
interior boundaries will be unlinked but their nodes will not be freed
individually. Their memory will become reusable when the arena is reset.

### 2.1 Validation

Validation will check the relevant arenas and report a conflict if either
contains an overlapping write newer than the transaction's read version.

An arena whose highest version is at or below the read version can be
skipped with one comparison. Otherwise, validation will use the existing
SkipList range lookup.

A range rewritten in `current` will not require changes to `previous`.
Checking both lists preserves the required conflict information without
links between the arenas.

This design limits validation to at most two SkipList searches. It does
not imply that validation is one comparison per allocator page or that
searching retained history has no cost for short transactions.

### 2.2 Reclamation

Each arena will record its newest commit version. It will become eligible
for reclamation when:

```cpp
retentionFloor > newestVersionInArena
```

Every conflict version in that arena will then be older than the read
version of any transaction still admitted. The Resolver will reset and
reuse the arena in O(1), rather than destroy its nodes individually.

The reclamation unit will be the entire arena. Nodes may reference other
nodes within the same arena across allocator-page boundaries, so those
pages cannot be reclaimed independently.

A new or reset SkipList must be initialized with a version no greater
than the retention floor. A higher initial version would make untouched
keys appear to have been written recently and create false conflicts.

Resetting an empty arena must be cheap. The allocator must support reuse
without traversing all previously allocated nodes or pages; its actual
reset latency will be validated during implementation.

### 2.3 Arena formation and rotation

The Resolver will start with one writable arena, `current`. A second arena
will be formed only when required history has accumulated beyond
`arenaFormationThreshold`.

In single-arena mode, the following checks will run at serialized batch
boundaries:

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

The reclamation check takes precedence. An obsolete arena will be reset
even if it has not reached the formation threshold.

If the arena is still required and reaches the threshold, it will become
the read-only `previous`. The second arena will become the new `current`.

While both arenas exist, new writes will continue entering `current`.
When `previous` becomes obsolete:

- If `current` still contains required history, it will become the new
  `previous`, and the recycled arena will become the new `current`.
- If both arenas are obsolete, both will be reset and the Resolver will
  return to single-arena mode.

An obsolete `current` may also be reset independently while `previous`
remains required. This does not create another arena or rotate the pair.

After returning to single-arena mode, the same formation threshold will
apply again.

### 2.4 Memory accounting

The formation threshold will be measured in allocated bytes or allocator
pages. Reachable-entry counts are insufficient because unlinked nodes
will remain allocated until their arena is reset.

The threshold will avoid forming a second arena while the history is
small. It will govern formation only: reclamation will always depend on
the retention floor. One batch may exceed the threshold, so it will be
a soft target rather than a strict memory limit.

Two arenas bound the number of generations, not their total size. An old
transaction may prevent `previous` from being reclaimed while `current`
continues growing.

Backpressure must therefore use the total bytes owned by both arenas.
Counting only unreachable nodes would miss growth from new distinct keys;
counting only reachable nodes would miss memory retained after overwrites.

## 3. Implementation sequence

The work will proceed in two stages.

### Phase A: connect the retention floor

The Resolver will consume the floor authorized by the Master for both
transaction admission and conflict-history reclamation. It will not
maintain a separate registry of active transactions.

The Commit Proxy must retain the corresponding `keyResolvers` history.
Otherwise, retained conflicts could become unreachable because the proxy
no longer knows which Resolver holds them.

With legacy-client support enabled, retention will preserve at least the
ordinary validation window. Registered transactions may extend that window
but will not shorten it.

With legacy-client support disabled, retention will follow the versions
protected by the Master for live transactions and accepted commits still
being validated. The admission and activation rules are defined in
`01-floor-tracking.md`.

The applied floor will never retreat. A report cannot restore conflict
history that has already been reclaimed.

### Phase B: replace per-entry reclamation

The Resolver will move its existing SkipList representation into the two
reusable arenas described above.

This will replace the incremental floor sweep and individual freeing of
unlinked nodes with whole-arena reuse. Validation semantics and intra-batch
transaction ordering will remain unchanged.

## 4. What the measurements established

The original model predicted a substantial throughput improvement from
eliminating individual reclamation. The measurements did not support
that prediction.

In the measured workload, the floor sweep and interior deletion walk
accounted for approximately 8.4% of conflict-detection time. Conflict lookup
accounted for 61.5%. The workload sweeps and lookup-cost analysis did not
establish a throughput advantage for two SkipLists.

The reason for this design is therefore its reclamation behaviour. When
old history becomes obsolete, the existing structure must walk and delete
individual entries. The arena design will make that storage reusable as
a unit.

Implementation measurements will establish:

- The lookup cost of consulting up to two SkipLists.
- Memory retained by unlinked nodes.
- Arena reset and initialization latency.
- Peak memory while an old arena remains required and the current one grows.

No general Resolver throughput improvement is assumed. Detailed benchmark
methods and results will be published separately.

## 5. Future wire compression

Compressing conflict ranges sent from Commit Proxies to Resolvers is
independent of arena reclamation and is outside the initial implementation.

The usual client path sorts ranges within each transaction, but a complete
Commit Proxy batch is not globally sorted. Per-transaction prefix encoding
could avoid a global merge; other client paths would still require
normalization or a fallback.

Cross-transaction encoding and general-purpose compression may be evaluated
later. Their network savings must be measured against the CPU and latency
they add to the commit path. Stateful key interning is out of scope.

## 6. Alternatives considered

### Continue deleting entries incrementally

A controller could allocate cleanup work according to reclaimable memory
or elapsed time instead of subsequent write volume. This would retain
per-node traversal and destruction, while adding a policy that balances
memory recovery against commit latency.

Whole-arena reuse removes that per-node cleanup work.

### Reclaim individual pages behind a global index

A linked index may reference nodes across page boundaries. A node can also
remain necessary as the boundary terminating a live range, even when its
own version is old (`ConflictSet.cpp:560–563`).

Independent page reclamation would require repairing those dependencies.
Separate arenas avoid cross-generation links.

### Store immutable records and rebuild the index

Appending every update as an immutable range record would retain
overlapping records rather than a canonical map. The writable generation
would need an interval index, and sealed generations would require
compaction or rebuilding.

Mutable SkipLists inside arenas preserve the existing lookup algorithm
without that additional machinery.

### Keep redundant links and repair them after reclamation

Redundant links could support navigation while obsolete pages are removed,
followed by a repair sweep. That sweep would still visit and modify
individual nodes, moving rather than eliminating the cleanup work.

### Interlace the old and current generations

Cross-linking generations could combine their searches, but would
complicate insertion, rotation and reclamation. Its benefit depends on
how often reads search both generations. The existing analysis has not
established enough benefit to justify that complexity.

Independent SkipLists keep the arenas separable.

### Replace the SkipList with an ART or radix tree

A different index would require new implementation and validation work.
It would not change the whole-arena reclamation rule.

The initial implementation will therefore preserve the existing SkipList,
which already supports ordered conflict-range validation.

## 7. Status

The Resolver design is complete and ready for implementation.

The first stage will connect the Master-authorized retention floor to the
Resolver and the Commit Proxy's keyResolvers history. The second will
introduce the two-arena representation.

The target is predictable O(1) arena reuse. Lookup cost, memory consumption
and allocator behaviour will be validated during implementation; higher
general throughput is not assumed.
