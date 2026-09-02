# Benchmarks

No results yet — by design: the predictions were published first (see the README table and the design docs) so that these measurements can confirm or destroy them in public.

## Planned measurements

**Terrain (log entry 001, before any change):**
- perf split of a loaded resolver: cycles in conflict-set structure vs deserialization/batching/actor overhead — fixes the Amdahl prediction (structure at 70% ⇒ ~3× end-to-end; at 30% ⇒ ~1.4× and wire compression becomes the protagonist);
- measured cost (in cache misses) of a real cold `removeBefore` unlink — the main sensitivity of the 10.5× structural claim;
- what the commit proxy actually sends per resolver, and where batch sorting happens today.

**Conflict workload profiler (early PR):**
- the 2×2 quadrant matrix (point/range read × point/range write) weighted by *cycles*, not counts;
- entries examined per check, separated from logical range span; skip levels traversed; conflict/no-conflict;
- the commit−read version distribution — the observable q, and the epoch-sizing input.

**Resolver phase B (six numbers close every open comparison):**
1. NEW-only lookup · 2. OLD lookup · 3. OLD+NEW as two searches · 4. OLD+NEW as one interleaved walk · 5. insert, independent vs interleaved · 6. q (measured cross-generation fraction) — yielding I, S, and the break-even q\* = I/S.

**Storage phase:**
- α (physical nodes/bytes per mutation) for PTree vs paged design;
- the RAM-vs-retained-window curve (the project's opening figure);
- the R(v) redundancy staircase under failures and repair;
- a long-reader workload (minute-scale snapshot) that is impossible on stock FDB;
- TSS-paired correctness and latency comparison against a stock Storage Server.

## Workloads and harnesses — Apple's, not invented here

All load generation reuses FoundationDB's own machinery: the **workload framework** in `fdbserver/workloads/` (the same `.actor.cpp` workloads run unchanged in deterministic simulation and on a real cluster) and **Mako** as the standard load generator. New capabilities get new workloads *written inside that framework* — the long reader above, and adversarial spill/revocation loads — so each one both validates the feature and ships as a contributable artifact in the same PR. Numbers produced with upstream's own workloads and simulation are numbers a maintainer can reproduce with one command; no benchmarking methodology of ours needs defending.

For the stable-snapshot showcase, the SQL exercising layer is likewise Apple's: the **Record Layer and its relational/SQL interface** (and its own test-query corpus) running long read-only queries against a pinned read version — the query engine that today must terminate one transaction and open the next at a *different read version* to survive past the window, now resuming every continuation within the same transaction snapshot: incremental execution as always, transactionally unfractured over paged history. Simple long scans and aggregations, not TPC-H: the thesis under test is the snapshot, not analytics.
