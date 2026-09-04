# Design Overview

*Status: frozen. Every claim about current FDB internals has been checked against
`apple/foundationdb` main @ `a443d3ee60`;
the audit is `design-crosscheck.md` and the terrain maps are
`conflictset-map.md` and `versionedmap-notes.md`. Claims below carry
`file:line`. Statements still unverified are marked **[measure]** and have a plan in
`questions.md`.*

## 1. Why the limit exists — the causal hierarchy

The familiar five-second transaction wall is enforced principally by **two independent
retention horizons**, plus commit-path age gates that *consume* those horizons rather than
constituting a third one (`CommitProxyServer.cpp:854–856`, `:1741–1746`). The two principal
knobs happen to share the same default; they are **not one bound serving both subsystems**,
which is what makes the two horizons separable at all.

1. **Storage Servers (the binding constraint — but see [measure] below).** The
   `VersionedMap` (`fdbclient/include/fdbclient/VersionedMap.h:738`) is a persistent-treap
   read index over `[oldestVersion, version]`. Precisely:
   - The retained floor is `oldestVersion`, **not** `durableVersion`
     (`storageserver.cpp:871–873`, asserted at `:1952`). `oldestVersion` is published
     before `durableVersion` advances (`:10889` vs `:6537`), so the **in-memory read index**
     may already have forgotten versions not yet persisted by the local storage engine.
     Replayability is retained a layer down: `mutationLog` still covers
     `(durableVersion, version]` (`:885`) and is erased only as versions become durable
     (`:6531–6532`), and the TLog is popped only up to the previous durable version
     (`:11097`). The two floors coincide at the end of an `updateStorage` pass.
   - It holds one root per version *at which mutations were applied*
     (`VersionedMap.h:839–848`; SS sites `storageserver.cpp:10417`, `:6499`), not per
     version.
   - Its **latest** view is deliberately the not-yet-durable delta: `changeDurableVersion`
     erases entries whose `insertVersion` equals the version just made durable
     (`:6504–6520`, invariant `:880–882`). The complete history lives in the *older* roots,
     shared structurally rather than copied (`VersionedMap.h:79–84`, `:94–109`).
   - **Values are in RAM**: `ValueOrClearToRef` holds the value or a clear-to
     key (`VersionedMap.h:712–733`).
   - The window is set by `MAX_READ_TRANSACTION_LIFE_VERSIONS`
     (`storageserver.cpp:10444–10447` → `:10480` → `:10889`).

   Retained memory is ≈ α · write-rate · window, values included. **The 96 B
   allocated-node size is confirmed by measurement** (`../benchmarks/measurement-results.md` T0.1: 88 bytes,
   `nextFastAllocatedSize` → 96). What remains unmeasured is the workload-dependent
   amplification from mutations to *live* nodes, retained values and RSS. T0.1 measures
   **PTree node-allocation amplification ≈ 3 allocated nodes per insertion in
   `versionedMapTest`**, without reclamation; that is not steady-state α, and must not be
   conflated with the α of this equation, which also carries keys, values, allocator slack and
   auxiliary structures.

   For context, the code's **conservative accounting charge per mutation** is
   `overheadPerItem*2 + (bytes + MutationRef::OVERHEAD_BYTES)*2`
   (`fdbclient/include/fdbclient/StorageServerInterface.h:1266–1272`), where
   `overheadPerItem = nextFastAllocatedSize(sizeof(PTreeT)) * 4` (`VersionedMap.h:765–766`),
   doubled again because the mutation is stored in both structures; the in-tree node budget
   is 128 allocated bytes (`storageserver.cpp:13276–13313`) — **but that comment is stale:
   measured, the node is 88 bytes and is allocated as 96** (`../benchmarks/measurement-results.md` T0.1).

That charge is not evidence against 96 B/node: it is a deliberately conservative per-mutation
   charge spanning two structures, not a per-node size, and `mvccStorageBytes` is not measured
   RSS. **[measure]** steady-state α remains **unknown** — §4 quotes no number until it is
   measured against RSS (`questions.md` Q10) — but the *structural* per-node factor is
   confirmed.

2. **Resolvers (the cheap twin).** Conflict history covers every transaction still
   eligible to commit. It stores only boundary keys + versions, **no values** — confirmed:
   a `Node` is a key plus one `Version` per level (`ConflictSet.cpp:241–311`), and only
   write ranges ever enter it (`:1035–1050`, `:442`). Maintenance is per-entry and
   CPU-hot — also confirmed, and the code testifies against itself: `removeBefore` unlinks
   node by node in key order with two explicit prefetches and the comment *"double
   prefetch gives +25% speed"* (`:544–576`, `:554–558`).

   **The Resolver window does not mirror the read window, and they do not share a bound.** The resolver floor uses only `MAX_WRITE_TRANSACTION_LIFE_VERSIONS`
   (`Resolver.cpp:359`); the SS floor uses only `MAX_READ_TRANSACTION_LIFE_VERSIONS`
   (`storageserver.cpp:10444–10447`). They are declared and documented as distinct knobs
   (`fdbserver/core/include/fdbserver/core/Knobs.h:38–51`) and initialized independently
   (`ServerKnobs.cpp:152–153`); they mirror because both default to
   `5 * VERSIONS_PER_SECOND`. **The horizon separation this project wants is, at the level
   of these two floors, already a knob change.** What is genuinely coupled is listed in
   `conflictset-map.md` §5 — chiefly `MAX_COMMIT_BATCH_INTERVAL`, clamped by *both*
   (`ServerKnobs.cpp:164–166`), and the proxy's MVCC-window gate, which applies `MAX_READ`
   on the *commit* path (`CommitProxyServer.cpp:1741–1746`).

   This reframes the resolver phase — twice. It is not a horizon-separation play (the knobs are
   already separate). And after T1.1/T1.2/T2.2 it is **not a performance play either**: there is no
   measured throughput case. What it now pursues is **reclamation compatible with a demand-driven
   floor** (T3.3, `01-resolver.md` §3); its performance cost remains to be measured. The separation is available today and its cost is
   protocol work, not structure (§3, §6).

**Death modes** *(refined)*. A long transaction usually dies first on the **read path**
(`transaction_too_old` from a Storage Server, `storageserver.cpp:2100–2102`, `:2131–2132`);
a compute-then-commit transaction dies on the **commit path** (the resolver marks it
`TransactionTooOld`, `ConflictSet.cpp:805`, and the proxy converts it,
`CommitProxyServer.cpp:2043–2044`). There is a third mode: a batch that queues longer
than `MAX_READ…/VERSIONS_PER_SECOND` is rejected wholesale at the proxy
(`CommitProxyServer.cpp:854–856`).

The **Storage phase** lifts the read-path guard. The **Resolver phase** makes
conflict-history retention cheaper and changes how its floor is maintained, but v1 does
**not** lengthen the commit window; it prepares the ground for doing so later. The proxy's
age gate is lifted by neither: it consumes whichever horizon is in force, and must be made
consistent with any new read contract rather than being removed by the new structure.

Order of attack is unchanged: first make the twin's reclamation compatible with a dynamic
floor (Resolver phase — no longer a throughput argument), then free the cause (Storage phase).

## 2. Doctrine

- **Costs are privatized to those who incur them.**
- **Allocation is a bump; deallocation is the floor.** Read as a doctrine of *wholesale
  death* — the epoch, not the object, is the unit of reclamation — this stands and is what
  Phase B implements. Read as a doctrine of *immutable records* it does not: the current
  structure depends on in-place mutation of surviving nodes to maintain its version index
  (`calcVersionForLevel`, `ConflictSet.cpp:283–289`; upward propagation in `insert`,
  `:609–618`; `removeBefore` folding maxima into predecessors, `:568–569`), and an
  immutable-record design would have to replace that mechanism rather than merely drop it.
  *That is now a lesson from a withdrawn alternative, not a pending cost:* Phase B keeps the
  map mutable inside each epoch and dies wholesale anyway
  (`01-resolver.md` §2.1, §2.2, R28).
- **Make proven irrelevance cheap.** Confirmed as sound: the current structure already
  does this at finer grain, via per-level max versions consumed as an early-out in
  validation (`ConflictSet.cpp:252–254`, `:680–681`, `:711–712`). The band filter
  generalizes an idea the code already relies on.

## 3. Scope and non-goals (v1 of the implementation)

- **Extended:** the *read* window.
- **Unchanged in length, dynamic in mechanism:** the *commit* window. Phase A
  (`01-resolver.md` §3) changes **how** the floor is computed, not how long the window is.
  Lengthening it is a separate, later decision.
- **Amended — the client *implementation* and the client/server retention contract are not
  untouched.** The client holds its own copy of
  `MAX_WRITE_TRANSACTION_LIFE_VERSIONS` (`ClientKnobs.cpp:235`,
  `fdbclient/include/fdbclient/Knobs.h:136`, "Copy of SERVER_KNOBS, as we can't link with
  it") and uses it to bound the idempotency-id search window (`NativeAPI.cpp:4756`). A
  server-side floor that moves without a client-visible story silently mis-sizes that
  window. The **public API** and the meaning of `transaction_too_old` can remain unchanged
  — this is not necessarily an API break — but the client-side idempotency search horizon
  must be made consistent with the server-side write-retention policy.
- **Unchanged:** TLogs. **Not introduced:** any new class of durable state.
- **Out of scope, sequenced as Beyond:** long *writing* transactions
  ([`04-read-reservations.md`](04-read-reservations.md)), online aggregates as deltas,
  storage-integrated validation.

## 4. Cost models and measured status

Cost model inputs unchanged: hot comparison ≈ 1.5 ns, cache miss ≈ 100 ns, 100 tps toy
workload (5 write + 5–10 read ranges/txn, W = 5 s).

- **Resolver:** ~370 ns → ~32 ns per entry lifecycle; 0.57 M → ~6 M txn/s/core; ~2×
  end-to-end (1.6–2.4×) — **historical target, suspended.** The representation is settled
  (today's skip list, twice — `01-resolver.md` §2.1), but T0.2 found *model* uncertainty,
  not measurement dispersion: the floor sweep is **9.0 %** of detect time, not 81 %; validation
  is the largest phase at 38.6 %; and the measured baseline is **0.899 M txn/s**, ~1.6× the
  0.57 M the chain assumes. Two limits keep that from being decisive — a 50-version window in
  the harness, and the insert-time interior destroy still folded into `D.MergeWrite` (36.7 %).
  **T1.1/T1.2 then measured the reclamation term:** the two things wholesale death removes — the
  floor sweep and the interior walk — total **8.4 % of detect time**. That is not a performance
  ceiling: two epochs also change lookup cost, and **search is 61.5 %** of Detect, so that term
  dominates and is unmeasured.
  T2.2 then found no crossover, and that stands — **there is no throughput case**. The design was
  nonetheless reopened for prototyping by T3.3 on *reclamation* grounds
  (`01-resolver.md` §3); these figures are not the reason and are not reinstated by it
  (`../benchmarks/measurement-results.md` T0.2, T1.1/T1.2, T2.2, T3.3).
  **Two caveats.**
  (a) *N depends on an allocator choice, not on the design.* Interior deletion is retained in
  both epoch variants — `remove(startF, endF)` runs unconditionally and with no version check
  (`ConflictSet.cpp:441`, `:579–597`) — so logical N stays at *live boundaries*. Only the
  per-epoch-arena variant, which stops freeing on unlink, grows *physical* N with ranges
  written. See `01-resolver.md` §2.1 and §2.e; an earlier reading of this as an unavoidable
  consequence of the epoch design was withdrawn.
  (b) *The 81 %-in-cold-unlinks split is measurable today and is not the split the model
  uses.* `g_removeBefore` covers only the floor sweep; the insert-time interior deletion is
  charged to `g_merge` (`ConflictSet.cpp:47–49`, `:952–994`, printed `:1210–1213`).
  Splitting them is a few lines (`questions.md` Q8).
- **Storage:** ~8× structural; end-to-end CPU gain 5–15 %. **The RAM-vs-window headline stays
  withdrawn pending measurement** — but *not* because 96 B/node was wrong; T0.1 confirms it
  (§1, *Storage Servers*). What is still missing is resident nodes per mutation in steady state,
  key and value bytes, allocator slack and auxiliary structures, and the contrast against RSS.
  The *shape* of the argument survives (RAM grows linearly with the window, and a bounded
  resident buffer with history on disk breaks that line); the constant is unquantified.
- **Both gains grow with the window** — **not supported for the resolver.** T2.2 swept 50 / 200 /
  500 versions at constant total work and the removable share was 9.0 / 8.3 / 8.6 % — flat. The
  storage side is untested.

**Storage figures remain parameterized predictions; resolver figures are historical provenance,
suspended pending T1.1/T1.2/T2.2** (`../benchmarks/measurement-results.md`, `../benchmarks/measurement-plan.md`). Mixing the
two states was itself an error this section previously made. The first three measurements have
concrete plans and, in two cases, instruments that already existed in the tree — Tier 0 has now
been run.

## 5. Validation strategy

1. **Deterministic simulation first.** Note that the resolver's level
   randomness comes from a `thread_local` LCG, not `deterministicRandom()`
   (`ConflictSet.cpp:40–45`), so **the level sequence is fixed independently of the
   simulation seed**. Operation ordering still varies across seeds and can therefore produce
   different resulting shapes; what sits outside FoundationDB's deterministic seeded schedule
   is the level randomness itself, so simulation does not systematically explore the
   level-assignment distribution. There is no *replacement* structure to wire up — Phase B
   reuses the existing `SkipList` — but two instances now **split that LCG sequence between
   them**, which can yield a shape distribution different from today's single list while the
   simulation still explores only one. Phase B should preserve deterministic execution while
   explicitly testing multiple level-assignment sequences; instantiating the existing structure
   twice does not make its seed-independent distribution adequately explored
   (`questions.md` Q5).
2. **The full FoundationDB simulation suite.**
3. **TSS pairing** — feasible (`StorageServer::isTss()`, `storageserver.cpp:1615`), with a
   one caveat: a TSS whose retention window *differs* from its pair will disagree
   with it precisely on old-version reads, which is the feature under test. The comparison
   harness needs an explicit rule for that class of divergence before TSS evidence means
   anything.

## 6. Strategy of entry *(reordered)*

Evidence before proposal, starting with the two measurements that now cost hours rather
than projects.

1. **Resolver node/byte counter.** `SkipList` already tracks every population change
   (`ConflictSet.cpp:599–619`, `:579–597`, `removedCount` at `:547`/`:565`/`:575`);
   publishing it needs a `specialCounter` alongside the existing
   `Version`/`NeededVersion`/`TotalStateBytes` (`Resolver.cpp:216–218`). ~50 lines. This is
   the genuinely-missing observability and the baseline every later claim needs.
2. **Storage RAM ground truth.** `sizeof(PTreeT)` is already printed by
   `fdbserver -r versionedmaptest` (`storageserver.cpp:13318`); compare measured RSS growth
   against `mvccStorageBytes`. ~2 hours, and it settles §4's withdrawn headline.
3. **Conflict workload profiler** — pitched correctly. Counts and latencies already exist
   (`Resolver.cpp:162–177`, `:181–191`, `:200–214`); what does **not** exist is the
   conflict-set footprint (item 1) and the **commit−read version distribution**, which is
   the input to epoch sizing and to `q`. Pitch the profiler on those two.
4. **Dynamic retention floor** — still the minimal-diff win, but scoped honestly as a
   three-component change (`01-resolver.md` §3).
5. **Conflict-range wire compression** — split into two variants rather than demoted
   wholesale. The batch is *not* sorted when it leaves the proxy; only per-transaction runs
   are (`ReadYourWrites.cpp:1388–1393`, `:1911–1990`), the proxy concatenates them
   (`CommitProxyServer.cpp:154–174`), and the raw NativeAPI path does not merge at all
   (`NativeAPI.cpp:3961`, comment at `:4701`).
   - *Cross-transaction* delta encoding is no longer a free first PR: it requires sorting
     the concatenated batch or merging its per-transaction sorted runs on the
     commit-critical path.
   - *Per-transaction* encoding reuses the already-sorted runs and resets the delta at each
     run boundary — no global merge, lower compression, and it needs a fallback or a local
     normalization pass for raw NativeAPI transactions.
   Measure both before choosing the wire format.
6. Then the structural changes, gated by the published profiles.
