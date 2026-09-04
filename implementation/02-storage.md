# Storage server — implementation guide

*Tree: `apple/foundationdb` main @ `a443d3ee60`; every site cited `file:line` against that
commit. Design: `../design/02-storage.md`, floor protocol in `../design/03-floor-tracking.md`.*

**Sequencing: this comes after the resolver.** The two phases lift different guards — the
resolver extends the *commit* path, storage extends the *read* path (`../design/00-overview.md` §1) —
and storage is the harder one: it holds values, not just ranges, and its retention is what
actually makes the five-second window binding.

---

## 0. Orientation

Every version newer than the durable one lives in RAM in a `VersionedMap` of persistent
treaps (`storageserver.cpp:854`, `VersionedMap.h:94–109`), with **88 bytes per node, 96
allocated** (measured, T0.1 — confirming the design and refuting the stale in-tree comment
at `storageserver.cpp:13276–13313`). The retention window is a constant:
`MAX_READ_TRANSACTION_LIFE_VERSIONS` (`ServerKnobs.cpp:152–153`), applied at
`storageserver.cpp:10444–10462`. Extending it multiplies pinned RAM linearly — hundreds of
MB at 5 s and ~3 GB at 60 s at 500 K writes/s, which is the wall.

The plan is therefore not "raise the knob": it is **make retention demand-driven, then make
the history spillable** so that a long snapshot costs disk, not RAM.

---

## Step S1 — demand-driven retention floor (the mirror of Phase A)

**The site is `storageserver.cpp:10453–10462`**, which computes `proposedOldestVersion`
from `version.get()`, the cursor's min known committed version and `lastTLogVersion`, minus
`maxVersionsInMemory` (`:10444–10447`), then floors it by several monotonicity clamps and
publishes it as `desiredOldestVersion` (`:10480`). `updateStorage` later turns that into
`oldestVersion` and calls `forgetVersionsBeforeAsync` (`:10887–10890`).

What changes: `maxVersionsInMemory` stops being a constant and becomes
`version.get() − storageReadFloor`, where the floor is the oldest read version any live
client may still read at, published by the same protocol the resolver consumes. Keep every
existing clamp — in particular `proposedOldestVersion = max(…, oldestVersion.get())` at
`:10460`, which is the monotonicity guarantee the rest of the server relies on.

**The two floors are different quantities and must not be merged.** The resolver's floor
covers transactions that may still *commit*; the storage floor covers snapshots that may
still be *read*. A long read-only transaction needs only the second; a compute-then-commit
transaction only the first. Naming them `resolverCommitFloor` and `storageReadFloor`
avoids a whole class of confusion.

**What enforces the current limit on the read path:** `storageserver.cpp:2100–2102` —
`if (version < data->oldestVersion.get() || version <= 0) throw transaction_too_old()`.
That contract does not change; only its trigger becomes dynamic.

**Back-pressure is mandatory here, unlike the resolver.** The resolver holds ranges; the
storage server holds values, so an unbounded floor is an OOM. `RESOLVER_STATE_MEMORY_LIMIT`
has no storage analogue for the versioned map — the closest existing lever is the
ratekeeper's view (`Ratekeeper.cpp:512`, `:647`). Add an explicit byte budget for retained
history and a policy for what happens when it is exhausted: refuse to hold the floor (the
oldest reader gets `transaction_too_old`, as today) rather than degrade the whole cluster.

### Exit criteria

* Simulation green with the floor pinned to today's constant (prove the refactor is a no-op).
* A long-reader workload holds a snapshot past 5 s and reads successfully; the retained
  byte counter rises and falls with the reader.
* The byte budget triggers and the oldest reader — not an arbitrary victim — is the one
  that fails.

---

## Step S2 — paged, spillable version history

This is the capability step and the bulk of the work. The existing structures to work with:

* `forgetVersionsBefore` / `forgetVersionsBeforeAsync` (`VersionedMap.h:788–802`, `:804–…`)
  — note the async form does the visible forgetting immediately and frees asynchronously
  (`storageserver.cpp:10884–10890`), which is the pattern any replacement must preserve:
  **visibility of the new oldest version must be atomic with respect to waiting actors**,
  even if the memory is returned later.
* `compact(Version)` (`VersionedMap.h:871–…`) — the only in-tree caller is a benchmark
  (`BenchVersionedMap.cpp:442`). Nothing in the server compacts.
* `mutationLog` (`storageserver.cpp:885`) — the durable-side companion; understand its
  lifetime before changing the in-memory side.
* The read path: `getRoot(v)` (`VersionedMap.h:759–763`) and
  `lastLessOrEqualAt` (`storageserver.cpp:2431–2432`) — every historical read resolves
  through a root snapshot, which is the seam a paged design must preserve.

The design's doctrine applies here as it does in the resolver: **allocation is a bump,
deallocation is the floor**, and a page dies whole when its maximum version falls below
the retention floor. The difference from the resolver is that a page may have to be
*written out and read back*, which introduces the only genuinely new machinery: a page
cache and an admission policy for historical reads.

**Non-goal, stated in the design and worth repeating in code review:** no new class of
durable state. Historical versions are replicated soft state — a storage server that loses
them can refetch or fail the read, exactly as today.

### What to measure before building (T2.3, T4.4, T4.5)

* α — physical nodes and bytes per mutation, for PTree vs the paged design.
* The RAM-vs-retained-window curve, which is the project's opening figure.
* The cost of a historical read that misses the resident buffer.

---

## Step S3 — validation strategy

Three levels, in this order (`../design/00-overview.md` §5):

1. **Deterministic simulation with a reference model.** Invariant asserts written as
   executable specifications — the suffix-history invariant
   (`∀v ∈ [MRV, MAV]: history(v) available`) is the one that catches retention bugs.
2. **The full FoundationDB simulation suite**, where the long tail of internal consumers —
   change feeds, watches, `fetchKeys`, byte sampling — will bite. This is by design: those
   consumers read historical versions and every one of them is a constraint on the
   retention change.
3. **TSS pairing.** Run the modified storage server as a Testing Storage Server against a
   stock one, serving identical data and reads with client-side comparison
   (`storageserver.cpp:1615` for the pairing flag). "Runs TSS-paired against stock" is the
   strongest evidence available and the thing a maintainer will ask for.

---

## Test and build reference

```bash
bin/fdbserver -r versionedmaptest -C /data/fdb/scratch/fdb.cluster
ninja fdbclient_bench && bin/fdbclient_bench --benchmark_filter=versioned_map
bin/fdbserver_storageserver_test                      # unit tests, CMakeLists.txt:8
```

`versionedMapTest()` (`storageserver.cpp:13315–13346`) prints the PTree node size and does
1000 versions × 1000 erase-range + insert, reporting distinct entries and the
`FastAllocator` delta. **It never calls `forgetVersionsBefore`** — it measures growth, not
reclamation, so any reclamation work needs a new harness. Likewise nothing in the tree
benchmarks `forgetVersionsBeforeAsync` or `deferredCleanupActor`; that gap is the first
thing to close on this side.
