# Measurement plan — ordered by difficulty

Every `[measure]` and benchmark item across `../design/00-overview.md`, `../design/01-resolver.md`,
`../design/02-storage.md`, `../design/03-floor-tracking.md` and `questions.md`, collected and ordered
easiest-first. Tree: `apple/foundationdb` main @ `a443d3ee60`.

**Precondition, and it is not free.** There is no build directory in this checkout and no
`cmake`, `ninja` or `clang++` on this machine (only `g++`). Everything below assumes a working
build first: `mkdir build && cd build && cmake -G Ninja <src> && ninja`. Budget that once; it
gates even Tier 0.

## Instruments that already exist

| Instrument | Invocation | Gives |
|---|---|---|
| `skipListTest()` (`ConflictSet.cpp:1121–1216`) | `bin/fdbserver -r skiplisttest` | Phase split via `PerfDoubleCounter`s (`:47–49`), throughput, final node count (`:1215`) |
| `versionedMapTest()` (`storageserver.cpp:13315–13346`) | `bin/fdbserver -r versionedmaptest` | `sizeof(PTreeT)`, allocated size class, distinct entries, **MB used** (`:13345`) |
| `flow_bench` | `ninja flow_bench; bin/flow_bench --benchmark_filter=ConflictSet` | `MiniConflictSet` variants only — *not* the SkipList |
| `fdbclient_bench` | `ninja fdbclient_bench; bin/fdbclient_bench --benchmark_filter=versioned_map` | PTree insert/find/bounds/scan/erase, `int` and `StringRef` |
| Resolver counters/histograms (`Resolver.cpp:162–191`, `:200–218`) | trace logs, `WORKER_LOGGING_INTERVAL` | ranges, txn outcomes, latency, queue depth, compute time |
| `fdbserver_resolver_test`, `fdbserver_storageserver_test` | `ninja <target>; bin/<target> -f <prefix>` | unit tests incl. `/fdbserver/storageserver/tryGetReadyReadVersion` |

Note `fdbserver_bench` exists but links **only `flow`** (`fdbserver/bench/CMakeLists.txt`), so a
ConflictSet benchmark needs either a link change there or a new target under
`fdbserver/resolver/`.

---

## Tier 0 — three runs, zero code

**T0.1 · `sizeof(PTreeT)` and PTree memory.** `bin/fdbserver -r versionedmaptest`. Prints the
node size, the allocator size class, and **MB used** for 10⁶ insertions. Compare against
`mvccStorageBytes` (`StorageServerInterface.h:1266–1272`) for the same mutation count.
*Settles:* the withdrawn RAM figure — `../design/00-overview.md` §4 C3, `../design/02-storage.md` §7, `questions.md` Q10 first half.
The in-tree comment claims 128 allocated bytes (`storageserver.cpp:13276–13313`); this says
whether that is still true. **~15 min after a build.**

**T0.2 · Existing resolver phase split.** `bin/fdbserver -r skiplisttest`. Prints
`D.Sort / D.CheckRead / D.CheckIntraBatch / D.Combine / D.MergeWrite / D.RemoveBefore`.
*Settles:* a first cut at the 81 %-in-cold-unlinks claim (`../design/01-resolver.md` §4 caveat 2) — with the known
limitation that the insert-time interior destroy is inside `D.MergeWrite`, which T1.1 fixes.
*Caveat:* the workload models a **50-version window** (`:1190`) and never produces a too-old
transaction. **~10 min.**

**T0.3 · Existing benchmark baselines.** `flow_bench --benchmark_filter=ConflictSet` and
`fdbclient_bench --benchmark_filter=versioned_map`. Records where the tuned code sits today —
the bar any replacement must clear. **~20 min.**

---

## Tier 1 — hours, a handful of lines, no new infrastructure

**T1.1 · Split the two deletion paths.** Add a `PerfDoubleCounter` alongside `g_merge` and time
the interior-destroy loop of `remove()` (`ConflictSet.cpp:590–596`) separately from the splice
and from `addConflictRanges`. Re-run T0.2. *Settles:* `../design/01-resolver.md` §4 caveat 2 and item (7) of §6;
it is what decides how much the arena variant is worth (`§2.e`). **~3 h including the run.**

**T1.2 · Overwrite ratio.** Count nodes destroyed in `remove()` versus nodes created in
`insert()` (`:602`, `:590–596`). Same file, same run as T1.1. *Settles:* `../design/01-resolver.md` §6.9 — the
input to what was then the arena-vs-keep-freeing decision. *That decision is now closed in favour
of the arena (`../design/01-resolver.md` §2.e): keeping per-node freeing keeps the per-node reclamation
debt T3.3 measured.* **~1 h, folded into T1.1's run.** **DONE** — ratio 0.052, swept to 0.93.

**T1.3 · α against RSS.** T0.1 gives allocator bytes; add process RSS sampling around the same
loop, and compare both to `mvccStorageBytes`'s predicted charge. *Settles:* the second half of
`questions.md` Q10 and the α that `../design/00-overview.md` §1.1 declares unknown. **~2 h.**

---

## Tier 2 — half a day to a day each

**T2.1 · Conflict-set footprint counter (`questions.md` Q1).** Thread a byte counter through
`insert` / `remove` / `removeBefore` (`:599–619`, `:579–597`, `:544–576`) and publish via
`specialCounter` next to the existing ones (`Resolver.cpp:216–218`). **On today's code only
`reachableBytes` is meaningful** — `ownedBytes` and `unreachableOwnedBytes` need the arena to
exist (`../design/01-resolver.md` §6.8), so ship the first now and the other two with the prototype. *Settles:* the
baseline every later claim is measured against; today the conflict set has **no** memory metric
at all. **~half a day + a simulation run.**

**T2.2 · Crossover search, not a realistic harness.** *Promoted to the decisive measurement by
T1.1/T1.2.* `skipListTest` hard-codes a 50-version window, never ages a transaction out
(`:1136–1150`, `:1190`), and has an overwrite ratio of 0.052 — on it, Phase B's gross ceiling is
1.09×. T2.2 must therefore **find the crossover**, if one exists, rather than re-run scenarios.

Vary systematically: overwrite ratio · range width and overlap · hot-domain size · window in
versions · the `commitVersion − readVersion` distribution, from which a **modelled** `q` follows
under an assumed sealing policy. Re-run the T1.1/T1.2 instrumentation unchanged for each point.
The *real* frequency of zero/one/two searches is T3.1's, not T2.2's.

**What it can measure:** sweep cost, interior-walk cost, overwrite ratio, list growth.
**What it cannot:** the frequency of zero/one/two searches, cost over `N_A` and `N_B`, the band
filter, rotation, end-of-epoch destruction — all of which need the prototype. So **T2.2 decides
whether there is enough reclaimable work to justify building it**; the net is decided by
T3.1–T3.3. Any net published here is a *modelled* net over assumed or separately
microbenchmarked lookup costs, and must be labelled as such:

`T_epoch − T_current = ΔT_lookup − T_sweep − T_interior + T_rotation + T_band`

**It does not start from neutrality:** on the tree's own workload there is no demonstrated case
based on reclamation. *Unblocks:* T3.1–T3.3. **~2 days**, and it decides whether they are worth
running at all.

**T2.3 · Storage brake crossover.** Which binds first under write pressure — the `MAX_READ…`
version window (`storageserver.cpp:10444–10447`) or `STORAGE_HARD_LIMIT_BYTES` with
`durableVersion < desiredOldestVersion` (`:10057–10068`)? Both already emit trace detail; sweep
write rate and value size in simulation. *Settles:* `../design/00-overview.md` §1.1's "binding constraint" premise
and `questions.md` Q10. **~2–3 days** (listed here because it needs no new code).

---

## Tier 3 — days, and a new benchmark target

All of Tier 3 needs a benchmark linking `fdbserver_resolver` — extend `fdbserver/bench` or add a
target under `fdbserver/resolver/` (~half a day of CMake), plus T2.2's harness.

**T3.1 · The six numbers (`../design/01-resolver.md` §6.1–6) — purpose restated.** NEW-only lookup · OLD lookup ·
OLD+NEW as two searches · OLD+NEW interleaved · insert · **q**. **No longer gated on
`q ≤ 0.056`**: T2.2 settled that there is no throughput win, and T3.3 reopened the design on
reclamation grounds instead (`../design/01-resolver.md` §3). T3.1's exit criterion is therefore whether
the **search and memory cost of two epochs is acceptable in exchange for whole-epoch
reclamation** — not whether it is faster. Requires the two-epoch prototype with death-driven
rotation **and a real demand-driven floor, not an injected schedule**: the behaviour under
test is what a live floor does — plateaus while a reader holds it, jumps when one finishes,
and the rate of sub-X retirements those produce — and a synthetic schedule only reproduces
the shapes chosen in advance, which is what invalidated the first two runs of T3.3. **T3.1
therefore comes after the floor protocol is implemented**, not alongside it. **~3 days**
once that prerequisite exists.

**T3.2 · Lookup neutrality (`../design/01-resolver.md` R42).** Model **both modes** (`../design/01-resolver.md` §2.a):
single-epoch accumulation to X — **triggered by allocation, so a pair can form during a floor
plateau** — **and its death-in-place path** (`floor > maxTS(current)` discards and re-seeds without
ever forming a pair — a sub-X retirement that X deliberately does not bound in frequency, so the
model needs both its unit cost and its rate), dual-epoch rotation driven by death, and the return
to single mode when both epochs are obsolete. `N_A + N_B` is *not*
bounded by a window's writes, so "search costs barely separate designs" is now a hypothesis.
Measure `log N_A + log N_B` against today's single map across workload shapes. **~2 days**, same
harness as T3.1.

**T3.3 · GC catch-up after a forward floor jump (`questions.md` Q2, `../design/01-resolver.md` §6.10).** Phase A
never lowers the floor; the burst is what must be absorbed when the oldest lease closes.
Measure reclaimable nodes exposed, catch-up time under the `3 × |write ranges| + 10` budget
(`:991`), and peak dead-state retention. **Phase A prerequisite.** **~2 days after T2.2.**

**T3.4 · Keep-freeing envelope — cancelled/superseded.** The `~32 ns` target belongs to the arena
variant. Keep-freeing is retained only as historical comparison data (T1.2, T2.2); it is no longer
a prototype candidate (`../design/01-resolver.md` §2.e, R66), so this task carries **no effort estimate and
no place in the recommended order**.

---

## Tier 4 — needs a loaded system, a cluster, or a prototype that does not exist yet

**T4.1 · Structure-versus-overhead split on a loaded resolver.** `perf record`, attributing
cycles and cache misses to `CheckMax::advance`, `SkipList::find`, `remove`, `removeBefore`. This
is what fixes the Amdahl prediction of `../design/00-overview.md` §4. The vtune annotations (`:467`, `:494`,
`:528`, `:585`) say where the original authors found the cost; the question is whether it moved.
**~2 days plus a loaded environment.**

**T4.2 · Conflict workload profiler (`../design/00-overview.md` §6.3).** The **commit−read version distribution**
and range-width/locality distributions. This is `q` for T3.1 and the epoch-sizing input, and it
is the one genuinely missing observability — counts and latencies already exist
(`Resolver.cpp:162–191`). **A project, not a measurement.**

**T4.3 · Handoff linearization cost (`../design/03-floor-tracking.md` §9, §8.4).** Added latency, message count and
Cluster Controller load for the commit pin. Needs the mechanism chosen first. **Days, after a
design decision.**

**T4.4 · Storage three-path read cost (`../design/02-storage.md` §2, S11).** Latest-value · historical-resident ·
historical-spilled, measured separately. Needs the paged design to exist. Today the point read
has only two outcomes (`storageserver.cpp:2432–2436`); the middle path is the unmeasured one.

**T4.5 · Storage structural CPU and the RAM-vs-window curve (`../design/02-storage.md` §7).** The ~8 % / 5–15 %
figures and the redrawn curve. Needs the prototype.

---

## Separate track — correctness simulation, not measurement

Listed so it is not confused with the above; none of it produces a number.

- **`../design/03-floor-tracking.md` §4a five failure-injection cases** — property
  `HANDOFF_ACCEPTED ⇒ pin ∨ fenced ∨ terminal`. Gates Phase A.
- **`../design/02-storage.md` §4 invariants** — `admitted rv ≥ MRV ⇒ lookup never dereferences a reclaimed page`,
  and the resolved-page access invariant.
- **`../design/02-storage.md` §6 / `../design/03-floor-tracking.md` §3** — mixed-version simulation for `too_old_local` and for the
  protocol-gated fields.
- **`../design/02-storage.md` §9** — the TSS divergence rule; without it TSS evidence is unusable, because the
  modified and stock servers *will* legitimately disagree on old-version reads.
- **`questions.md` Q5** — swap `skfastrand` for `deterministicRandom()` behind a build flag and
  run Joshua; and per `../design/00-overview.md` §5.1, two `SkipList` instances split that LCG sequence, so Phase B
  must test multiple level-assignment sequences explicitly.
- **`questions.md` Q6** — set `MAX_READ…` and `MAX_WRITE…` apart in simulation and run the fast
  suite plus `TxnTimeout`.

---

## Recommended order, and why

1. **T0.1** — 15 minutes, and it either confirms or breaks the storage phase's withdrawn
   headline.
2. **T0.2 + T0.3** — the existing baselines, before changing anything.
3. **T1.1 + T1.2** — one small diff, one run, and it settles the 81 % claim *and* the allocator
   input. Highest value per line of code in the whole list.
4. **T1.3** — closes α.
5. **T2.1** — the footprint baseline; ship `reachableBytes` now, the other two with the arena.
6. **T2.2** — the harness. Nothing in Tier 3 is meaningful without it.
7. **T3.3** — Phase A prerequisite, so it precedes the Phase B numbers.
8. **T3.1, T3.2** — the Phase B comparison. (T3.4 is cancelled, not deferred.)
9. **T2.3, T4.x** — the storage phase, which is the later project anyway.

The correctness track runs in parallel and gates shipping, not measuring: `../design/03-floor-tracking.md` §8.4's
failure-injection cases must pass before Phase A ships regardless of any number above.
