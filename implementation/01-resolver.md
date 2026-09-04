# Resolver — implementation guide

*Tree: `apple/foundationdb` main @ `a443d3ee60`. Every site is cited `file:line` against
that commit; verify against `git show a443d3ee60:<path>`, never against a working tree
that may carry instrumentation.*

*Design: `../design/01-resolver.md` (what and why) and `../design/03-floor-tracking.md`
(where the floor comes from). This document is the how: edit sites, order, invariants,
tests, and the exit criteria for each step.*

---

## 0. The one-paragraph orientation

The resolver keeps a single `SkipList` of key → last-write-version (`ConflictSet.cpp:753–760`),
validates each batch's read ranges against it, inserts the batch's write ranges, and
sweeps a budgeted number of nodes below a floor. Today that floor is a constant offset
from the current commit version (`Resolver.cpp:359`). **Phase A replaces the constant with
a negotiated floor and makes the sweep survive a floor that stops moving. Phase B replaces
the allocator and the reclamation model with two arena-backed epochs.** Phase A ships on
its own; Phase B is gated on a prototype measurement (T3.1).

---

# Phase A — demand-driven retention floor

## A.1 What changes, in one line each

| Site | Today | After |
|---|---|---|
| `Resolver.cpp:359` | `newOldestVersion = req.version − MAX_WRITE_TRANSACTION_LIFE_VERSIONS` | `= max(negotiated floor, req.version − MAX_WRITE_TRANSACTION_LIFE_VERSIONS)` |
| `ConflictSet.cpp:986` | sweep runs only if the floor advanced | sweep runs while reclaimable debt remains |
| `ConflictSet.cpp:991` | budget `3·|write ranges| + 10` | budget from debt and a time slice |
| `ResolverInterface.h:122` | request carries `version`, `prevVersion` | plus the proxy's floor and its generation |
| `CommitProxyServer.cpp:2104` | `oldestVersion = prevVersion − MAX_WRITE_…` | the same negotiated floor |

Everything else — validation, the too-old rule, the reply — is untouched.

## A.2 Step 1: make the floor an input instead of a constant

**The single computation site is `Resolver.cpp:359`.** The value it produces is used
twice, and the two uses must not be separated:

* `addTransaction(tr, newOldestVersion)` (`:361`) decides `tooOld` at
  `ConflictSet.cpp:805` — `tr.read_snapshot < newOldestVersion && !read_conflict_ranges.empty()`
  — which becomes `TransactionTooOld` in the reply (`Resolver.cpp:381–384`) and
  `transaction_too_old` at the proxy (`CommitProxyServer.cpp:2043`).
* `detectConflicts(req.version, newOldestVersion, …)` (`:374`) drives the sweep
  (`ConflictSet.cpp:986–993`).

**The invariant that couples them: you may only admit what you can still validate.** A
transaction with `read_snapshot ≥ floor` must find every conflicting write that happened
after its snapshot still represented in the skip list. So the admission floor and the GC
floor must be the *same* value in a batch, and the floor must be **monotone** — a floor
that retreats would admit transactions whose history has already been swept.

Concretely:

1. Add to `ResolveTransactionBatchRequest` (`ResolverInterface.h:122`) two fields: the
   proxy's requested floor and the generation it was computed in. Serialize them in the
   existing `serialize()` of that struct.
2. In `Resolver`, keep `Version publishedFloor` alongside `version` (`Resolver.cpp:199` is
   the constructor where `conflictSet(newConflictSet())` is built). On each batch:
   ```
   demand = min over live proxies of their requested floor        // globalValidationDemand
   floor  = max(demand, req.version − MAX_WRITE_TRANSACTION_LIFE_VERSIONS)
   publishedFloor = max(publishedFloor, floor)                    // never retreats
   ```
   **Mind the direction of that `max`.** `req.version − MAX_WRITE_TRANSACTION_LIFE_VERSIONS`
   is a **lower bound on the floor**, not an upper one: the floor may rise above it —
   reclaiming *sooner* because demand proves the history is unneeded — but never fall below
   it. **The resolver consequently never retains more conflict history than it does today.**
   Storage is the opposite case, and the extended *read* window is where the capability
   lives.
3. Replace `:359` with that value.

**Generation fencing, not timing.** A proxy that was replaced must not lower the floor.
The resolver already has the machinery: it watches `ServerDBInfo` and dies on epoch change
(`Resolver.cpp:832`, `ClusterRecovery.cpp:461–466`). Carry the epoch in the request and
**ignore any request whose generation is older than the highest seen**; a floor from a
superseded proxy is not "late", it is void. See `../design/03-floor-tracking.md` §4.

**Where the proxy's number comes from** is the subject of `../design/03-floor-tracking.md`:
`globalOldestClientRV` (the oldest read version any live client may still commit against)
and `oldestInFlightCommitRV`, published by the GRV proxies. The resolver does not compute
it and must not: it only takes the minimum, clamps, and publishes monotonically.

**Do not forget the proxy's own copy.** `CommitProxyServer.cpp:2104` computes
`oldestVersion = prevVersion − MAX_WRITE_TRANSACTION_LIFE_VERSIONS` to coalesce
`keyResolvers` (`ProxyCommitData.h:293`) every `RESOLVER_COALESCE_TIME`
(`CommitProxyServer.cpp:2100–2110`). If the resolver retains more history than the proxy's
key-resolver map does, multi-resolver transactions lose the mapping for old read versions.
Feed it the same floor.

### Exit criteria for step 1

* `bin/fdbserver -r skiplisttest` unchanged in behaviour (it does not exercise the floor
  plumbing; it is the regression guard for the structure).
* Simulation green with the floor pinned to today's constant — i.e. prove the refactor is
  a no-op before making the value dynamic.
* A simulation workload that holds one read version open and observes the resolver's
  retained history grow and then release when the reader finishes.

## A.3 Step 2: make the sweep survive a stationary floor

This is the part T3.3 measured, and it is a prerequisite, not an optimization. Two
independent changes at `ConflictSet.cpp:986–993`:

**(a) Decouple the trigger from the floor advance.** Today:

```cpp
if (newOldestVersion > cs->oldestVersion) { … sweep … }
```

A demand-driven floor plateaus whenever an old reader holds it, and then the condition is
false and *nothing is reclaimed*. Measured: **8.3× retained population** under 50-batch
plateaus, still rising at the last sample. Keep the floor update gated on the advance, but
gate the *sweep* on outstanding debt:

```cpp
const bool advanced = newOldestVersion > cs->oldestVersion;
if (advanced) { cs->oldestVersion = newOldestVersion; cs->sweepPending = true; }
if (advanced || cs->sweepPending) { … sweep …
    if (removed == 0 && cs->removalKey.size() == 0) cs->sweepPending = false; }
```

The termination test is the cursor: `removeBefore` returns the number removed and leaves
`finger.getValue()` as the resume point (`:992`); a full lap that removes nothing means
the list is clean at this floor. Storing `sweepPending` in `ConflictSet` (`:753–760`) is
the whole state cost.

**(b) Decouple the budget from write volume.** `3·|combined write ranges| + 10` (`:991`)
ties reclamation to *current* writes, so a cluster that writes hard and then goes quiet
drains at ≤ 10 nodes per batch — measured extrapolation **~52 600 batches**. Replace with a
budget driven by debt and bounded by a time slice: keep the write-proportional term as a
floor, add a term proportional to estimated reclaimable nodes, and cap by elapsed
microseconds so a batch's latency stays bounded. Peak per-batch sweep cost measured
**≤ 0.43 ms** in every arm, which is the headroom you are spending.

**What is not yet established:** a bound on catch-up *time*. The experiment showed the
debt stops growing with (a), not that it is repaid within a bounded number of batches. The
missing metric is a "full lap" counter per floor generation — instrument the prototype to
report laps, not just removals.

### Exit criteria for step 2

* A `skiplisttest` variant with a monotone floor schedule that holds the floor for N
  batches and then jumps: retained population must not grow across plateaus. The harness
  used for T3.3 is described in `../benchmarks/measurement-results.md` §T3.3 — reuse its
  parameter names (`CS_FLOORMODE`, `CS_FLOOR_HOLD_BATCHES`, `CS_DRAIN_AT`).
* **Guard against the trap that invalidated the first two runs of that experiment:** the
  control arm must genuinely disable the fix. Verify by asserting that the control arm's
  sweep count equals the number of batches in which the floor advanced.
* Per-batch sweep time p99 within budget under the drain workload.

## A.4 Knobs to add

Follow the existing style in `ServerKnobs.cpp` (`init( NAME, value );`, `:152–166` for the
window knobs, `:925` for the resolver's own). Suggested:

* `RESOLVER_FLOOR_ENABLED` — kill switch; false restores `:359`'s constant exactly.
* `RESOLVER_MAX_RETENTION_VERSIONS` — hard ceiling on how far the floor may lag `req.version`,
  so a stuck client cannot pin unbounded memory.
* `RESOLVER_SWEEP_BUDGET_MIN` / `_TIME_SLICE_US` — the two terms of step 2(b).
* `RESOLVER_FLOOR_STALENESS_LIMIT` — how long a proxy's floor may go un-refreshed before
  the resolver falls back to the constant.

Remember `MAX_WRITE_TRANSACTION_LIFE_VERSIONS` exists **twice**: `ServerKnobs` and a client
copy (`ClientKnobs.cpp:235`, declared `Knobs.h:136`, used at `NativeAPI.cpp:4756`). The
client uses it to decide when to stop retrying. Extending the server-side window without
telling the client means clients give up on transactions the cluster would still accept —
that is a client-visible change and belongs in its own PR.

## A.5 Observability to add first

The resolver publishes no size metric at all (`Resolver.cpp:162–177`, `216–218`), and
`RESOLVER_STATE_MEMORY_LIMIT` bounds the *state-transaction* buffer, not the skip list.
Before changing behaviour, add counters — they are also the PR that stands alone:

* retained node count (`SkipList::count()` is O(n), `:406–414` — sample it, do not call it
  per batch) and, better, a maintained counter incremented in `insert` and decremented in
  both unlink paths;
* nodes examined vs removed per sweep, and sweeps skipped because the floor did not move;
* the published floor, its lag behind `req.version`, and which proxy is the binding
  constraint;
* the commit−read version distribution — the `q` that dimensions Phase B, and the input to
  epoch sizing.

---

# Phase B — two arena-backed epochs

**Do not start here, and do not start it early with a stand-in floor.** Phase B is justified
on reclamation grounds only, and gated on the prototype (T3.1) showing the search and memory
cost is acceptable; T2.2 found no throughput regime that justifies it by itself.

**The epochs are the last consumer of the floor, not a parallel track.** T3.1's question is
how the structure behaves under a *real* demand-driven floor — plateaus while a reader holds
it, jumps when one finishes, and the rate of sub-X retirements those produce. A synthetic
floor only reproduces the shapes you thought to inject, which is precisely what invalidated
the first two runs of T3.3. Build the floor for real first (`03-floor-tracking.md`), both
because the measurement needs it and because the floor is the part the upstream community has
to accept before any of this is adoptable.

## B.1 What the structure becomes

Two live epochs at most, backed by two reusable arena slots. Each epoch holds **the
existing canonical `SkipList` representation and lookup algorithm** — same `Node` layout
(`ConflictSet.cpp:241–311`), same search — with two changes:

* **Allocation** moves from `FastAllocated` `Node::create` (`:263–290`) to a bump pointer
  in the epoch's arena. `Node::destroy()` (`:291–303`) becomes a no-op for epoch-local
  nodes; the epoch's arena dies whole.
* **`remove(start,end)`** (`:579–597`) keeps the splice at every level (`:586–588`) and
  **drops the destroy loop** (`:590–596`): splice-and-abandon. The abandoned bytes are
  reclaimed when the epoch dies. Measured amplification on the tree's own workload:
  **0.2 pp** of `Detect` (overwrite ratio 0.052), up to 2.0 pp in hot-key regimes.

## B.2 The state machine (from `../design/01-resolver.md` §2.a — implement it exactly)

Two triggers, death first:

* **Floor advance** → death tests, both modes.
* **Allocation growth** → the X formation test, single mode only. A pair can form during a
  floor plateau.
* Both at one batch boundary → **death wins**.

```
single:  if floor > maxTS(current):  discard current; re-seed empty at <= floor; stay single
         elif ownedBytes(current) >= X:  seal current as previous; open the other arena; go dual

dual:    if floor <= maxTS(previous):  stay dual
         elif floor > maxTS(current):  free both; open one empty current <= floor; go single
         else:  free previous; seal current as previous; reuse the freed arena; stay dual
```

**Seeding is the sharp edge.** `SkipList(version)` already exists and does exactly the
right thing — it sets every header level's max version (`ConflictSet.cpp:416–422`), and
`clearConflictSet` uses it (`:765–767`). A new `current` must be seeded **at or below the
floor** and must **never inherit `maxTS(previous)`**: if it did, every untouched key would
report a version above older readers' `rv` and produce false conflicts at scale.

Two implementation notes that are not part of the machine: discarding an *empty* epoch
must be a cheap no-op (metadata or bump-pointer reset), and a `current` that dies while
`previous` lives may be reset in place — a memory optimization, not a rotation.

## B.3 Validation across two epochs

The band filter is one integer per epoch: if `rv ≥ maxTS(previous)` the previous epoch
cannot contain a conflicting write and is skipped entirely. Duplicated boundaries across
epochs resolve by max semantics, so no merge maintenance is needed. The cost to measure is
the fraction of validations that must consult both (`q`), and the per-validation cost of
the second descent — the six numbers in `benchmarks/README.md` under "Resolver phase B".

## B.4 What the prototype must report (T3.1)

Beyond the six numbers: bytes allocated per arena and total peak; amplification from
abandoned nodes; **unit cost *and* per-event-type rate** of discard/re-seed (single→single,
dual→dual, in-place reset) — the aggregate is rate × unit cost and neither factor can be
assumed; and behaviour across floor plateaus and jumps, including formation at X during a
plateau. `totalOwnedBytes` must trigger backpressure, a quantity distinct from X.

---

## Test and build reference

```bash
# build (the project's own image; clang 19.1.5, no host toolchain needed)
docker run --rm -v /data/fdb:/data/fdb -w /data/fdb/build \
  foundationdb/build:rockylinux9-latest ninja fdbserver flow_bench fdbclient_bench

bin/fdbserver -r skiplisttest -C /data/fdb/scratch/fdb.cluster   # the conflict-set benchmark
bin/fdbserver -r versionedmaptest -C /data/fdb/scratch/fdb.cluster
bin/fdbserver -r simulation -f tests/…                            # the real gate
```

`skiplisttest` (`ConflictSet.cpp:1121–1216`) builds batches and reports the
`PerfDoubleCounter` split — `Detect`, `D.CheckRead`, `D.MergeWrite`, `D.RemoveBefore`
(`:47–49`) — which is the instrument for every claim above. A cluster file is required
even for test roles; a dummy file suffices.

The real gate is the simulation suite, then TSS pairing for the storage phase. A resolver
change is testable in simulation because conflict outcomes are deterministic: the same
seed must produce the same commit/abort decisions before and after.
