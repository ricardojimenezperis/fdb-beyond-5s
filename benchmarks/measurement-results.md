# Measurement results — Tier 0

Run 2026-09-04 against `apple/foundationdb` main @ `a443d3ee60`, built in the project's own
image `foundationdb/build:rockylinux9-latest` (clang 19.1.5, cmake 3.31.8, ninja 1.10.2,
Boost 1.78), `CMAKE_BUILD_TYPE=Release`, `USE_LD=LLD`, `USE_LIBCXX=1`. Host: 16 cores, 62 GB.
Build directory `/data/fdb/build`, outside the source tree.

> **CPU scaling was enabled on the host** (google-benchmark warns). Absolute times are
> indicative; ratios within one run are the trustworthy part.

---

## T0.1 — PTree node size and memory (`fdbserver -r versionedmaptest`)

```
SS Ptree node is 88 bytes
PTree node is 72 bytes, allocated as 96 bytes
198563 distinct after 1000000 insertions
Memory used: 287.894880 MB
```

**`sizeof(StorageServer::VersionedData::PTreeT) = 88` bytes**, and
`nextFastAllocatedSize(88) = 96` (`flow/include/flow/FastAlloc.h:254–255`) — so **96 bytes
allocated per node**.

**This reverses a correction made during the audit.** Design 1.0's "~96 B/node" figure was
**right**. The cross-check (`design-crosscheck.md` X4, `../design/00-overview.md` C3,
`../design/02-storage.md` S2) rejected it on the strength of the in-tree size budget at
`storageserver.cpp:13276–13313`, which totals 128 allocated bytes. **That comment is stale**;
the code it documents now produces 88 → 96. The audit was right that α had never been measured
against RSS, and wrong about the direction of the error.

**Allocation amplification.** 287.89 MB ÷ 96 B ≈ **3.0 M nodes for 10⁶ insertions** — i.e.
**PTree node-allocation amplification ≈ 3 allocated nodes per insertion in `versionedMapTest`**.
Use that full name: it is *not* the α of `RAM ≈ α · write-rate · window`, which additionally
carries key and value bytes, allocator slack and auxiliary structures, and which is a
steady-state residency figure rather than a cumulative allocation count. Compare the two in-tree estimates:
`overheadPerItem = nextFastAllocatedSize(sizeof(PTreeT)) * 4` assumes **4**
(`VersionedMap.h:765–766`), and `mvccStorageBytes` says *"1 insertion into version map costs 2
nodes in avg"* (`StorageServerInterface.h:1268`). Measured sits between them.

*Caveat, and it matters:* `versionedMapTest` never calls `forgetVersionsBefore` and retains all
1000 versions, so this is **allocation** amplification, not steady-state residency. α against
RSS still requires T1.3.

---

## T0.2 — Resolver phase split (`fdbserver -r skiplisttest`)

```
Detect only:      1.390 sec   0.899 Mtransactions/sec   3.597 Mkeys/sec
  D.CheckRead        0.536    38.6 %
  D.MergeWrite       0.510    36.7 %
  D.Sort             0.199    14.3 %
  D.RemoveBefore     0.125     9.0 %
  D.Combine          0.011     0.8 %
  D.CheckIntraBatch  0.007     0.5 %
430411 entries in version history
```

**The 81 %-in-cold-unlinks premise is not supported by this workload.** The floor sweep —
`removeBefore`, timed as `D.RemoveBefore` — is **9.0 %** of detect time. Attributing *all* of
`D.MergeWrite` to unlinking (it is not: it includes `find` and `insert`) still only reaches
45.7 %.

This matters because 81 % is the base of the chain: 370 ns → 32 ns per entry, hence ~10.5×
structural, hence ~2× end-to-end by Amdahl (`../design/01-resolver.md` §4, `../design/00-overview.md` §4).
If the sweep is 9 %, removing it entirely caps the structural gain near **1.1× on this
workload**.

Second data point in the same direction: the model assumes a baseline of **0.57 M txn/s/core**;
measured detect-only throughput is **0.899 M txn/s**. The starting point is already ~1.6× better
than the model's premise.

**Validation, not reclamation, is the largest phase** at 38.6 %. A design that leaves search
neutral (`../design/01-resolver.md` §2.d) and removes the sweep has limited headroom here.

*Caveats.* The harness models a **50-version window** (`ConflictSet.cpp:1190`), not
`MAX_WRITE_TRANSACTION_LIFE_VERSIONS`, and never produces a too-old transaction. 430 411 final
entries from ~2.5 M ranges written means substantial interior destruction *did* occur — and it
is inside `D.MergeWrite`, indistinguishable. **T1.1 is exactly the split that resolves this**,
and T2.2 is what puts the structure in the design's actual regime.

---

## T0.3 — Existing benchmark baselines

**`flow_bench --benchmark_filter=ConflictDetection`:**

| | Time | Items/s |
|---|---|---|
| `MiniConflictSet/realistic` (`std::vector<bool>`) | 11 552 ns | 51.9 M/s |
| `WordBitsetConflictSet/realistic` | 941 ns | 637.8 M/s |

**12.3×** — but this quantifies a win *already taken*: the tree's live `MiniConflictSet` is
already the word-bitset version (`ConflictSet.cpp:849–911`). It corroborates T0.2's
`D.CheckIntraBatch` at 0.5 %: intra-batch conflict detection is no longer a cost centre.

**`fdbclient_bench --benchmark_filter=VersionedMap/StringRef`** (10⁶ items):

| Operation | Time | Items/s |
|---|---|---|
| insert | 1536 ms | 651 k/s |
| find | 1434 ms | 697 k/s |
| lower_bound | 1491 ms | 671 k/s |
| upper_bound | 1709 ms | 585 k/s |
| last_less_or_equal | 1674 ms | 598 k/s |
| last_less | 403 ms | 2.48 M/s |
| find_sorted | 400 ms | 2.50 M/s |
| scan | 180 ms | 5.55 M/s |
| erase | 1129 ms | 885 k/s |

`VersionedMap/int/multiversion`: `create_version` 23.2 M/s, `historical_scan` 22.6 M/s,
`historical_find` 7.65 M/s, `historical_lower_bound` 5.34 M/s, `latest_insert` 5.03 M/s,
`latest_erase` 5.37 M/s, **`compact_forget` 1.22 k/s**.

The 3.6× gap between random `find` (697 k/s) and `find_sorted` (2.50 M/s) measures the
**locality advantage of sorted access in the storage-side `VersionedMap` benchmark**.
Attributing it specifically to cache misses requires hardware counters, and it is **not**
evidence about `ConflictSet` — it mixes locality with access order, on a different structure.

---

## T1.1 + T1.2 — `D.MergeWrite` decomposed, and the overwrite ratio

Instrumentation added to `fdbserver/resolver/ConflictSet.cpp` (timers around finger
construction, end-boundary insert, level splice, interior walk, begin-boundary insert; counters
for nodes and allocated bytes created / interior-destroyed / swept). Same workload as T0.2,
unchanged.

```
D.MergeWrite decomposition (0.598 s of 1.463 s Detect):
phase                   sec    %Detect  %MergeWrite
M.Find               0.3735      25.5%        62.5%
M.InsertEnd          0.0792       5.4%        13.3%
M.Splice             0.0023       0.2%         0.4%
M.Interior           0.0027       0.2%         0.4%
M.InsertStart        0.0863       5.9%        14.4%
(unattributed)       0.0537       3.7%         9.0%
D.RemoveBefore       0.1204       8.2%            -

Ceilings vs today (share of Detect that disappears):
  keep-freeing (sweep only)     : 8.2%
  arena (sweep + interior walk) : 8.4%

created 2 341 456 nodes / 188 194 008 alloc-bytes
interior-destroyed 121 581 / 9 757 256
swept 1 789 759 / 143 875 440
Overwrite ratio (interior-destroyed / created): 0.052
```

**The gross removable *reclamation* share is 8.4 % of Detect on this workload** — the floor
sweep plus the interior walk, holding everything else constant. Against a modelled ~10.5×
structural, that is the whole of what wholesale death removes.

**It is not a ceiling on the design's performance.** Two epochs also change the *lookup* cost,
in both directions: worse when a query must consult both; better when the band filter skips the
sealed epoch and the remaining `current` is smaller than today's full-window list; worse again if
the consulted epoch holds far more boundaries than a window's worth. Since search is **61.5 %**
of Detect, that term dominates whatever reclamation does — and it is unmeasured. There is no
1.092× ceiling on total performance until Δlookup is measured.

**The arena buys 0.2 percentage points over keep-freeing.** The interior walk is 0.2 % of
Detect, because the **overwrite ratio is 0.052**: only 121 581 of 2 341 456 created nodes are
ever destroyed by insertion. **Under the then-current throughput-only criterion this favoured
keep-freeing**, since the arena's entire advantage is the walk it skips and it would pay physical
amplification for it (`../design/01-resolver.md` §2.e). *T3.3 subsequently superseded that allocator
decision by making whole-arena retirement the objective; the ratio now prices the arena's memory
cost rather than choosing between the two.*

**Where the time actually goes.** Grouping the phases:

| Group | Share of Detect |
|---|---|
| Search (`D.CheckRead` 36.0 % + `M.Find` 25.5 %) | **61.5 %** |
| Sort (`D.Sort`) | 13.6 % |
| Node mutation (two inserts + splice) | 11.7 % |
| **Reclamation (sweep + interior walk)** | **8.4 %** |
| Combine + intra-batch + unattributed | 4.9 % |

`M.Find` alone is 62.5 % of `D.MergeWrite`: the insert path is mostly **search**, not mutation.
The resolver, on this workload, is search-and-sort bound — not reclamation bound.

**Memory side — and the attribution matters.** Of 188.2 MB allocated, 153.6 MB (82 %) is
eventually destroyed — 143.9 MB by the sweep, 9.8 MB by interior destruction — leaving ~34.6 MB
live (430 115 entries). **Those 153.6 MB are not the arena's differential price.** The
sweep-eligible bytes are retained until epoch death in *both* variants, because in both the
sweep is gone; only the interior-destroyed bytes distinguish them, since keep-freeing still frees
those immediately at insert time while the arena abandons them.

| Variant | Work removed from the hot path | Work actually eliminated | Cumulative bytes whose reclamation is deferred |
|---|---|---|---|
| keep-freeing | sweep, 8.2 pp | **unknown** — the nodes are still walked and destroyed later by `SkipList::destroy()` at rotation; this is deferral and batching, not elimination | 143.9 MB (sweep-eligible) |
| arena | sweep + interior walk, 8.4 pp | most of that walking, in exchange for retention | 153.6 MB (both categories) |

*"Deferred", not "additional residency":* these are cumulative over the run, not simultaneous
peak. Peak requires imposing a sealing policy and simulating rotations — it is not derivable from
this run.

So the ~143.9 MB is **the generational design's own deferral**, common to both allocators; only
the ~9.8 MB interior share distinguishes the arena, and it is small here precisely *because* the
overwrite ratio is small.

**Gross ceilings, and why they are only gross.**

`S_keep-freeing,max = 1/(1−0.082) = 1.089×` · `S_arena,max = 1/(1−0.084) = 1.092×`

These bound only the reclamation term. The full comparison is

`T_epoch − T_current = ΔT_lookup − T_sweep − T_interior + T_rotation + T_band`

where `ΔT_lookup` covers zero, one or two lists and their respective sizes. Net gain on this
workload may therefore be below 8–9 %, zero, negative — or, if the partition genuinely cheapens
the dominant search, modestly above it. Nothing here settles that sign.

**Methodological caveats.**
- Instrumented Detect is 1.463 s against T0.2's 1.390 s (+5.3 %); most of that lands in
  `D.MergeWrite` (0.510 → 0.598) and in the 3.7 % unattributed. Ratios are the trustworthy part.
- The two runs do **not** use the same random data — `-r skiplisttest` does not fix the seed, and
  final entry counts differ by 0.07 % (430 115 vs 430 411). Statistically equivalent, not
  identical. A seeded re-run (`-s`) would tighten this.
- **The overwrite ratio of 0.052 is a property of this harness**, not of FDB workloads: 2.5 M
  ranges of width 1–11 over a 20 M key space are almost disjoint. A rewrite-heavy workload would
  shift cost into the interior walk and change the arena's value. **This motivated T2.2**, which
  subsequently swept those regimes and found no crossover.

---

## T2.2 — crossover search: **none found**

Harness parameterized by environment (`CS_KEYSPACE`, `CS_WIDTH`, `CS_WINDOW`, `CS_LAGMODE`,
`CS_BATCHES`, `CS_PER_BATCH`), defaults reproducing the historical workload exactly — verified:
overwrite ratio 0.052, `M.Find` 62.0 % of `D.MergeWrite`. Window points scale `batches` inversely
with `per_batch` so total work is constant and ~10 windows elapse (otherwise a large window simply
disables the sweep instead of enlarging it).

```
case                        Detect ChkRead  M.Find   M.Int   Sweep     KF%  Arena%   entries      q      ovr
default                      1.536   0.553   25.2%    0.2%    9.0%    9.0%    9.2%    427598  1.000    0.052
keyspace 2e6                 1.099   0.462   22.0%    0.8%    3.8%    3.8%    4.6%    209002  1.000    0.280
keyspace 2e5                 0.761   0.394   10.7%    0.8%    1.2%    1.2%    2.0%     43749  1.000    0.644
keyspace 2e4 (hot)           0.536   0.317    3.0%    0.3%    0.3%    0.3%    0.6%      5519  1.000    0.868
width 100                    1.130   0.472   21.6%    0.7%    3.8%    3.8%    4.5%    224346  1.000    0.261
width 1000                   0.758   0.407   11.0%    0.7%    1.2%    1.2%    2.0%     50273  1.000    0.600
width 1000 + hot 2e5         0.468   0.245    0.5%    0.1%    0.1%    0.1%    0.1%       742  1.000    0.929
window 200 (10 wins)         1.584   0.539   28.9%    0.2%    8.3%    8.3%    8.4%    421341  1.000    0.051
window 500 (10 wins)         1.579   0.541   29.9%    0.2%    8.6%    8.6%    8.8%    416834  1.000    0.051
lag uniform                  1.469   0.477   26.9%    0.2%    9.5%    9.5%    9.7%    438689  0.746    0.053
lag recent-skewed            1.300   0.312   30.6%    0.2%   10.5%   10.5%   10.7%    445527  0.392    0.054
recent + hot 2e5             0.822   0.267   21.7%    2.0%    2.3%    2.3%    4.3%     56769  0.392    0.892
recent + hot + w1000         0.417   0.163    2.6%    0.4%    0.2%    0.2%    0.6%       529  0.392    0.996
```

### The reclamation share moves the wrong way

**Higher overwrite ratio gives a *smaller* removable share, not larger.** Keyspace 2·10⁷ → 2·10⁴
takes the overwrite ratio from 0.052 to 0.868 and the keep-freeing share from **9.0 % to 0.3 %**.
Wider ranges do the same (width 1000: 1.2 %). The mechanism is not subtle: heavy overwrite keeps
the *live set* tiny — 5 519 entries versus 427 598 — so there is almost nothing left to sweep.
The interior walk does grow in relative terms (0.2 % → 2.0 % in `recent + hot 2e5`, the one point
where the arena is meaningfully better than keep-freeing, 4.3 % vs 2.3 %), but the total collapses.

**No monotonic window effect was discernible** across 50 → 200 → 500 versions at constant total
work: 9.0 % → 8.3 % → 8.6 %. The differences are small relative to the known run-to-run and
instrumentation uncertainty — they are not the same kind of error, so no single figure bounds
them, but neither is large enough to read a trend from. That refutes `../design/00-overview.md` §4's *"both gains grow with the window"* **over the interval
tested**; it does not prove independence of W.

**The best case for reclamation is 10.5 %**, at recent-skewed reads on the *sparse* default
keyspace — the **sparsest, lowest-overwrite setting tested**. Whether that is the least *realistic*
setting is not established until T4.2 measures real workloads.

### The search side: the band filter does not pay in the comparison model

Recent-skewed reads are the one axis that helps, and they help where it matters: modelled
**q drops to 0.392**, so ~61 % of validations would consult a single epoch. But the design's own
`log N` model settles what that is worth. With N = 445 527 (`lag recent-skewed`), splitting into
two halves:

| q | expected search cost | vs today |
|---|---|---|
| 0.000 | 17.77 | **−5.3 %** |
| **0.392** (modelled, recent-skewed) | 24.73 | **+31.8 %** |
| 0.746 | 31.02 | +65.3 % |
| 1.000 | 35.53 | +89.3 % |

`log₂(N) = 18.77`, `log₂(N/2) = 17.77`. The **comparison-model break-even is `q ≤ 0.056`**, from
`(1+q)·log₂(N/2) ≤ log₂(N)` — the **lowest modelled `q`** is **seven times too high**, and even
`q = 0` saves only 5.3 % because halving N saves one comparison. This is the design's own "the log
is merciless" argument turned against it.

*Model assumptions, and they are not small:* two equal halves, cost proportional to `log₂ N`, and
no account of the one→two→one epoch cycle, unequal epoch sizes, range traversal or cache
behaviour. It is a discard signal, not an implementation break-even — always cite it as the
**comparison-model** break-even.

### Conclusion

**T2.2 finds no *throughput* regime that by itself justifies Tier 3.** (T3.3 subsequently justified
the prototype on reclamation grounds — `../design/01-resolver.md` §3. This section is unrevised by that.) Reclamation yields ≤ 10.5 % gross and
falls toward zero in the hot, wide-range regimes; and on the search side the two-epoch split costs
~32 % more at the **lowest modelled `q`**, needing a nominal comparison-model `q ≤ 0.056` to
break even. The `ΔT_lookup` term that was Phase B's remaining upside points the
wrong way.

**Caveats that keep this short of proof.** `q` is modelled from an assumed lag distribution
(`u³`), not from a measured `commitVersion − readVersion` distribution — that is still T4.2, the
profiler, and it is now the one measurement that could revive the question. The search model is
pure comparison count; real cost includes cache misses, the band check is one comparison, and the
sealed epoch may be much smaller than half. T3.1 remains the only thing that can *measure*
`ΔT_lookup`. But the margin here is a factor of seven, not a few percent.

---

## T3.3 — sweep catch-up under a forward floor jump

*Third revision. Two earlier runs were wrong in opposite directions: the first used a
**retreating** floor schedule, which the monotone contract forbids; the second fixed the schedule
but had a **broken control arm** — with the fix disabled it still swept every batch, so it was not
reproducing today's gated behaviour at all. What follows uses a monotone floor and a control that
reproduces `ConflictSet.cpp:986` faithfully.*

### Setup

A reader pins `globalValidationDemand = R`; the published floor is `max(R, now − W)`, clamped
monotone. While `R > now − W` the floor holds at **exactly the same value** — the plateau — and on
release it jumps forward to `now`. Seeded, constant total work. Two arms: today's gated sweep, and
a variant carrying explicit `sweepPending` debt cleared when a pass ends having freed nothing.

### Result 1 — debt grows throughout the plateau (writes continuing, hold 50 batches)

```
                 retained_peak  retained_final   dead_peak  dead_final   freed   budget  used
gated (today)         1910108         1906280     1903932     1903932   66879   243298  0.976
sweepPending           233277          230207      228493      227859 2064530  3380095  0.989
```

**8.3× retained population**, and **debt grew throughout the observed plateau and reached its
maximum at the final sample**: `dead_peak == dead_final` exactly (1 903 932). A finite run cannot
demonstrate unbounded growth, and the plateau is itself bounded in version distance by W. The gated arm was granted 243 k of
budget across the run against 3.38 M for the other — it simply never ran.

The mechanism is the one predicted: during a plateau `newOldestVersion == cs->oldestVersion`, so
the `>` test at `ConflictSet.cpp:986` is false. **Equality, not "below"** — the earlier write-up
named the wrong condition.

### Result 2 — draining is impractical once writes stop

Writes stop at batch 250, floor held, 1750 drain batches:

```
gated (today)   debt 297881 -> 297881   examined 0        no progress at all
sweepPending    debt 297881 -> 288291   examined 17500    5.5/batch => ~52 608 further batches
```

The gated arm makes **literally zero** progress. But the fix is **not sufficient either**: with no
writes the budget is `3 × 0 + 10 = 10` nodes per batch, so draining 298 k of debt extrapolates to
**~52 600 batches**. This is the concern recorded in `conflictset-map.md` §3.3 — reclamation rate
is tied to *current write volume*, not to accumulated debt — now measured.

### Against the Phase A exit criterion

| Condition | Status |
|---|---|
| no sustained growth of `deadRetained` | **fails** gated — debt grew throughout the observed plateau and peaked at the final sample; **holds** with `sweepPending` while writes continue |
| no unacceptable per-batch time | **holds** — peak per-batch sweep ≤ 0.43 ms in every arm |
| catch-up within a bounded number of batches | **not established** — the drain never completes in either arm |

**Phase A therefore needs two changes, not one.** Decoupling the sweep from the floor advance is
necessary but insufficient; the **budget must also be decoupled from write volume** — driven by
reclaimable debt or by a time slice. Otherwise a cluster that goes quiet after a write burst retains
the debt for an impractically large number of otherwise empty batches (~52 600 at budget 10); and if
no batches execute at all, reclamation simply waits until processing resumes, or requires a
background actor. *Neither is "indefinite retention" in the strict sense — the drain is finite
whenever batches run — but both are unacceptable operationally.*

**The trade-off this creates, stated without overreach.** Sweeping slowly protects latency but
retains a large reclaimable population and inflates memory occupancy; sweeping fast enough recovers
memory sooner but concentrates node traversal and individual `destroy()` into commit batches, and
may cost latency — especially the tail. **That latency penalty has not been measured here** (peak
per-batch sweep time was ≤ 0.43 ms in every arm). The rigorous conclusion is not that incremental
reclamation is unworkable, but that making it robust **requires designing a new controller** —
debt-aware, memory-pressure-aware or time-budgeted.

**This is why the generational structure is back on the table (`../design/01-resolver.md` §3).** Not
because T2.2 was wrong — it was not — but because arena-backed epochs remove that controller from
the problem entirely: reclamation cost stops depending on node count, reclaimable fraction,
subsequent write volume, a sweep cursor or a per-batch budget.

### Scope and caveats

- The hold/release schedule is a **model**. Real plateau durations and jump magnitudes come from
  T4.2.
- `sweepPending` is **a plausible defensive mechanism whose cost and completion semantics remain
  to be established**, not "cheap insurance": its termination test here is "a pass freed nothing
  and reached the end of the list", which is cheap in these runs but unproven in general.
- What is established is narrow: **no unbounded retained-population growth was observed with
  `sweepPending` during these finite runs**, and without it debt grew for the whole observed
  plateau, peaking at the last sample.
  Eventual catch-up is not established for either arm.
- A structural metric would be better than debt totals: stamp a generation on each floor advance,
  record the sweep cursor at publication, and measure until a **full lap** under that floor
  completes. A lap certifies every node that existed and was reclaimable under it was examined,
  and does not move as new writes arrive.

---

## What this changes

| Claim | Document | Status |
|---|---|---|
| "~96 B/node" | design 1.0 | **Confirmed** (88 → 96) |
| "the in-tree budget says 128, so ~96 is too low" | `design-crosscheck.md` X4, `../design/00-overview.md` C3, `../design/02-storage.md` S2 | **Wrong** — the comment is stale |
| "α remains unknown" | `../design/00-overview.md` §1.1 | **Still entirely unmeasured** against RSS. What T0.1 gives is *PTree node-allocation amplification* ≈ 3 nodes/insertion — a different quantity |
| "81 % of cost = cold unlinks" | `../design/01-resolver.md` §4, `../design/00-overview.md` §4 | **Not supported** here — 9.0 % |
| baseline 0.57 M txn/s/core | `../design/01-resolver.md` §4 | **Understates** — measured 0.899 M |
| "search costs barely separate designs" | `../design/01-resolver.md` §4, R42 | Search is **61.5 %** of Detect (T1.1) |
| ~10.5× structural, ~2× end-to-end | `../design/01-resolver.md` §4, `../design/00-overview.md` §4 | **Gross removable reclamation share is 8.4 % of Detect** on this workload (T1.1). Not a performance ceiling — Δlookup unmeasured |
| arena vs keep-freeing | `../design/01-resolver.md` §2.e | Arena buys **0.2 pp** at ovr 0.052, up to **2.0 pp** at ovr 0.89 — but the total collapses there too (T2.2) |
| "both gains grow with the window" | `../design/00-overview.md` §4 | **Not supported**: 9.0 → 8.3 → 8.6 % across 50/200/500 versions at constant work (T2.2) |
| Phase A sweep behaviour | `../design/01-resolver.md` §3, §6.10 | **Two defects, measured.** Gating the sweep on the floor advancing lets debt grow through a plateau — it grew for the whole observed plateau and peaked at the final sample (8.3× retained, `dead_peak == dead_final`); and the budget being tied to write volume makes draining impractical once writes stop (~52 600 batches). Phase A needs both decoupled (T3.3) |
| Phase B performance case | `../design/01-resolver.md` §4 | **No crossover found**, and this stands. Reclamation ≤ 10.5 % and falls with overwrite; search is +31.8 % at the **lowest modelled `q`**; nominal comparison-model break-even `q ≤ 0.056` (T2.2) |
| Phase B decision | `../design/01-resolver.md` §3 | **Reopened for prototyping by T3.3 — on reclamation grounds, not throughput.** Arena-backed epochs replace the incremental reclamation debt that T3.3 measured. T2.2's negative result is unrevised |

## Next

**T2.2 rejected the generational structure as a throughput optimization. T3.3 reopens it for an
independent reason: eliminating the latency-versus-memory trade-off created by per-node reclamation
under a demand-driven floor.** Both results stand; they answer different questions.

**Next is the Phase B prototype** (`../design/01-resolver.md` §3, §6): two arena-backed canonical
SkipLists with death-driven rotation. Its purpose is not to find a throughput win — T2.2 says there
is none — but to price the trade: *lookup and retained-memory cost* against *whole-epoch
reclamation*. **T3.1's exit criterion changes accordingly**: it is no longer gated on
`q ≤ 0.056`; it must measure whether two epochs' search and memory cost is acceptable in exchange
for removing the reclamation controller.

One caution the prototype must resolve: `totalOwnedBytes` must trigger backpressure, which is a
different quantity from the bootstrap threshold X. *(A second caution — unbounded rotation
frequency — named the wrong quantity. X bounds how often a *pair* forms: after returning to
single-epoch mode no second epoch is formed until a live `current` reaches X. Arena retirement is
death-driven and deliberately has no X-derived lower bound — a short burst leaves a small arena
that is freed whole once the floor proves it obsolete, which is the intended privatization of cost.
T3.1 must still count the rate — per event type — because the aggregate cost is rate × unit cost;
what it must not do is treat the rate as something to suppress;
`../design/01-resolver.md` §2.a.1 property 3.)*

**Still open on Phase A regardless:** bounded catch-up *time*, which needs a per-floor-generation
"full lap" metric rather than a debt total. And **T4.2** (the conflict workload profiler) retains
its independent value — real plateau durations and jump magnitudes come from it.

The storage-side items (`measurement-plan.md` T2.3, T4.4, T4.5) are untouched by any of this.
