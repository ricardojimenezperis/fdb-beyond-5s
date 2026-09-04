# Design Overview

*Status: frozen for implementation. Every recollection about current FDB internals below is marked for verification against the code; log entry 001 does exactly that.*

## 1. Why the limit exists — the causal hierarchy

The five-second limit is written into two twin retention windows:

1. **Storage Servers (the binding constraint).** The `VersionedMap` holds *every* version newer than the durable one — the oldest version the storage engine has persisted — in RAM: the complete multi-version history within the read window, superseded versions included. The storage engine below it holds a single version. Retained memory ≈ α · write-rate · window, *values included* — the expensive equation. Extending the window multiplies pinned RAM; that is why the window must stay short.
2. **Resolvers (the derived twin).** In current FDB, conflict history covers every transaction still eligible to commit — and because read and commit horizons share the same short bound, the Resolver window mirrors the read window. This project deliberately separates those horizons. It stores only ranges + versions (no values) — far cheaper in memory, but its maintenance is per-entry and CPU-hot.

A long transaction usually dies first on the **read path** (first read past the window gets `transaction_too_old` from a Storage Server); a compute-then-commit transaction dies on the **commit path** (resolver history no longer covers its read version). The two phases lift these two guards respectively: the Resolver phase extends the commit path, the Storage phase extends the read path — which is why a long reader needs only the second, a compute-then-commit transaction only the first, and a long read-write transaction both.

This project attacks both, in the reverse order of causality: first make the twin cheap (Resolver phase — a performance play), then free the cause (Storage phase — a capability play).

## 2. Doctrine

Three principles recur in every chapter; they are the design's identity.

- **Costs are privatized to those who incur them.** Old readers pay for their own history hops and their own cold pages. The hot path never subsidizes the long tail. This extends all the way up to cluster policy: the window is governed by retention budgets charged to the oldest cohorts and relieved by prefix revocation, not by braking everyone.
- **Allocation is a bump; deallocation is the floor.** Pages are the allocator: allocating is advancing an offset; freeing does not exist per object — a page (or an epoch) dies whole when one integer (its max version) falls below a moving floor. Contents are trivially destructible with no owning pointers; refreshing an entry is a *new* record, never an in-place bump (a hot cell must not immortalize its page).
- **Make proven irrelevance cheap.** When a version provably cannot affect an operation — it is older than the reader's snapshot, it belongs to an epoch outside the read version's band, it lies below the retention floor — the data structure must let the operation skip it entirely: not examine it and discard it, but never visit it. Concretely: a skipped subtree, a skipped epoch, a page that is never read from disk. The saving must come from the structure, not from a fast path bolted on afterwards.

## 3. Scope and non-goals (v1)

- **Extended:** the *read* window. Long read-only transactions become cheap and safe.
- **Unchanged:** the *commit* window (resolver conflict history), the client API, and the error contract. `transaction_too_old` keeps its meaning; only its trigger becomes dynamic.
- **Unchanged:** TLogs. Extending the read window must not become pressure on commit-critical machinery.
- **Not introduced:** any new class of durable state. Historical versions are replicated soft state (see `02-storage.md` §6).
- **Out of scope, sequenced as Beyond:** long *writing* transactions — a deliberate separation with its design already written ([`04-read-reservations.md`](04-read-reservations.md): promotion + non-blocking read reservations) — plus online aggregates as deltas and storage-integrated validation.

> Long read-only transactions are implemented by paged historical storage. Long read-write transactions are intentionally deferred, with promotion and non-blocking read reservations specified separately.

The full sequence, from bounded contribution to vision: paged history → promotion + reservations → the colocated second-generation architecture ([`06-colocated-architecture.md`](06-colocated-architecture.md)), where these phases become the fast path of a redistributed transaction protocol — reachable through the deployable client-coordinated bridge ([`07-client-coordinated-bridge.md`](07-client-coordinated-bridge.md)), which keeps the existing Commit Proxy/TLog as orderer and durable decision authority.

## 4. Predicted numbers, and how they were produced

All cost models use: hot comparison ≈ 1.5 ns, cache miss ≈ 100 ns, and a 100 tps toy workload (5 write + 5–10 read ranges/txn, W = 5 s) whose per-entry results are scale-invariant. Headline results:

- **Resolver:** per entry lifecycle ~370 ns (81% of it per-entry GC on cold nodes) → ~32 ns. Structural capacity 0.57 M → ~6 M txn/s/core (10.5×). End-to-end by Amdahl with 1–2.5 μs of serialization/messaging overhead: **~2× (1.6–2.4×)**. The 10× also flips the resolver to overhead-bound, which is why wire compression is the follow-up stage of the same attack.
- **Storage:** ~8× structural, but Storage Server totals are dominated by disk engine and network, so end-to-end CPU gain is only **5–15%** — deliberately *not* the headline. The headline is the RAM-vs-window curve: at 500 K writes/s, ~96 B/node PTree pins hundreds of MB at 5 s and ~3 GB at 60 s (impossible — that is the limit), versus a bounded resident buffer with excess history on disk.
- **Both gains grow with the window** (Gustafson's reading of Amdahl): longer windows fatten per-entry costs in the traditional designs precisely in the regime this project enables. Today's ~2× is the floor of the 5-second world, not the ceiling of the proposed one.

Every number above is a parameterized prediction. The first measurements (perf split of structure vs overhead; cost of cold unlinks; conflict workload matrix) are specified in the benchmark plans and exist to correct these numbers in public.

## 5. Validation strategy

1. **Deterministic simulation first.** Seeded, reproducible random workloads against a naive reference model, plus invariant asserts written as executable specifications (e.g., the suffix-history invariant: `∀v ∈ [MRV, MAV]: history(v) available`). This is FoundationDB's native language of trust.
2. **The full FoundationDB simulation suite** — where the long tail of internal consumers (change feeds, watches, fetchKeys, byte sampling) will bite, by design.
3. **TSS pairing.** The modified Storage Server runs as a Testing Storage Server against a stock one, serving identical data and reads with client-side response comparison. "Runs TSS-paired against stock" is the strongest available evidence.

## 6. Strategy of entry

Evidence before proposal: first make FoundationDB itself produce the numbers that justify each next step.

1. **Conflict workload profiler** (standalone value: observability that does not exist today; near-zero overhead by default, detail behind a knob; measures work, not shape — including the commit−read version distribution that dimensions everything downstream).
2. **Conflict-range wire compression** (sorted-batch delta and/or zstd with a trained dictionary; no semantic change).
3. **Dynamic retention floor** on the existing resolver structure (minimal diff, immediate value).
4. Then the structural changes, gated by the published profiles.