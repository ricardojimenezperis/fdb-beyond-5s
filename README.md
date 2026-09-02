# FDB Beyond 5s

**Extending FoundationDB beyond its 5-second read-version window by decoupling version retention from memory.**

Paged, spillable version history · generational conflict storage · public design, engineering log, and benchmarks.

**Target:** turn the five-second read-version wall into an operator-controlled storage budget, while making Resolver conflict-history management substantially cheaper.

**Status:** design phase complete · implementation starts September 2026 · developed in public, including the reasoning.

---

## The problem

FoundationDB keeps its MVCC window in the memory of the Storage Servers: the multi-version structure (`VersionedMap` / PTree) retains roughly the last five seconds of versions in RAM, and the Resolver retains conflict history over the same read-version horizon to validate commits. The read window is bounded by Storage Server RAM — that equation *is* the five-second limit; the Resolver horizon simply follows it.

The familiar consequence: `transaction_too_old` after five seconds. Long analytical reads, large scans, consistent exports, and long-running read-only workflows — all constrained by the same short read-version window, regardless of available memory or disk capacity.

## The approach

Two contained phases, one doctrine.

**Phase 1 — Resolver: remove the CPU bottleneck.**
Replace per-entry conflict-history management with a *generational paged* design: two independent skip lists (current and previous epoch), each self-contained in append-only pages. Allocation is a pointer bump; reclamation is dropping an entire epoch when the version floor passes it. A one-integer band filter skips the previous epoch for recent read versions. No per-entry unlinking exists anywhere.

**Phase 2 — Storage: remove the RAM–window coupling.**
A single-version main tree with per-key version chains living in append-only, immutable, *spillable* pages addressed as `(pageID, offset)` through the pager. Old readers pay for their own history; the hot path never subsidizes them. Historical versions are replicated **soft state**: a restarted replica re-accumulates history organically rather than recovering it, while reads are routed according to each replica's retained-version capability — an availability policy, not new durable machinery.

**The doctrine** (it recurs in every chapter): costs are privatized to those who incur them; allocation is a bump and reclamation follows the version floor; and structures are chosen so that proven irrelevance becomes work *not done*.

## Predictions — before benchmarks

These are parameterized predictions, published before implementation so the benchmarks can confirm or destroy them:

| Metric | Current | Pre-implementation prediction | Notes |
|---|---|---|---|
| Resolver conflict-history cost per entry lifecycle | ~370 ns | ~32 ns | dominated today by per-entry GC on cold nodes |
| Resolver structure-only throughput ceiling | ~0.57 M txn/s/core | ~6 M txn/s/core | 10.5×; sensitive to the measured cost of cold unlinks |
| Resolver end-to-end throughput (Amdahl) | ~300 K txn/s/core | ~600 K txn/s/core | 1.6–2.4× depending on the real structure/overhead split |
| Storage: historical-version RAM | ∝ window × write rate (pinned) | bounded, independent of window | excess history spills to disk |
| Read-version window | 5 seconds (wall) | operator-budgeted (knob) | write/commit window unchanged in v1 |

The first log entries will profile the actual FoundationDB code and replace every assumption above with a measurement.

## Roadmap

- [x] Resolver phase design — frozen, with alternatives considered and falsifiable reopening criteria
- [x] Storage phase design — frozen, including clearRange, GC, admission control, revocation, and history availability model
- [ ] Terrain: local build + simulation; **map of the real ConflictSet/SkipList** (log entry 001)
- [ ] Early PRs: conflict workload profiler · conflict-range wire compression
- [ ] Resolver A: dynamic retention floor on the existing structure (minimal diff)
- [ ] Resolver B: generational paged conflict set (gated by profiles)
- [ ] Storage core: paged version chains, TSS-paired against stock
- [ ] Versioned clearRange + lazy GC
- [ ] Spill, revocation watermark, capability-aware routing
- [ ] Full simulation suite green · benchmark report
- [ ] Upstream RFC and sliced PRs

Milestones are capability-gated, not date-gated. Each closed milestone ships with a log entry and results.

### Beyond v1

*These directions are capability consequences and possible consumers of PVS, not committed implementation scope.*

- **Long write transactions:** promotion and non-blocking read reservations (`design/04`).
- **Stable-snapshot query execution:** query layers commonly paginate or resume long executions through continuation-driven FDB transactions to remain inside the five-second window. Without retained history, those transactions normally acquire *different read versions*, so the combined execution need not correspond to one committed database state. Paged history extends FoundationDB's existing read-only snapshot semantics beyond five seconds: several physical read-only transactions pinned to one retained RV are *observationally equivalent* to one uninterrupted snapshot. Cursors, batching, suspension, spilling, and backpressure remain executor concerns; the snapshot remains whole. The configured on-disk history capacity determines how long that snapshot can be retained. Long read-only executions need no conflict footprint; the structural problem begins when a long transaction also *writes*, motivating promotion to server-side read reservations (`design/04`) and ultimately shard-local registration (`design/06`).
- **Relational consumers of stable snapshots (exploratory):** Paged Version Storage is independently useful and requires no query-layer changes. A natural downstream demonstration is **one long SQL statement resumed across multiple physical FDB transactions at a single retained RV**. Candidate consumers include the Record Layer's relational/SQL interface and an existing engine connected through an FDB foreign-data path. PVS supplies only the consistency substrate; physical operators, serialized execution state, spill placement, worker recovery, and optimizer integration remain the executor's responsibility. The first target would be one end-to-end long-query demonstrator; broader merge/hash joins, external sorts, aggregations, and cost-based optimizer integration remain exploratory directions.
- **Second-generation architecture:** online aggregates as commutative version-chain deltas and colocated conflict resolution with commit proportional to participants (`design/06`, via the deployable bridge of `design/07`).

## Repository map

| Path | Content |
|---|---|
| `design/00-overview.md` | Motivation, doctrine, scope, validation strategy |
| `design/01-resolver.md` | Resolver phase: full design, alternatives, cost model, benchmark plan |
| `design/02-storage.md` | Storage phase: full design, history availability model, cost model |
| `design/03-floor-tracking.md` | The oldest-active-read-version protocol: the sensor behind every retention decision |
| `design/04-read-reservations.md` | Post-v1: extending the *commit* window via promotion and non-blocking read reservations |
| `design/05-compact-domain-accelerator.md` | Profiler-gated: a partial conflict index with temporal completeness certificates |
| `design/06-colocated-architecture.md` | Vision/roadmap: colocated conflict resolution, local read sets, commit proportional to participants |
| `design/07-client-coordinated-bridge.md` | The deployable bridge to 06: client-coordinated protocol keeping Commit Proxy/TLog as orderer and durable decision authority |
| [`log/`](log/) | Engineering log — decisions, corrections, and attribution, as they happen |
| [`benchmarks/`](benchmarks/) | Measurement plans and (later) results |

*The design documents are complete and will be published in installments as implementation begins — each release announced in the [log](log/).*

## Prior work

Before this project I built a working FoundationDB fork exploring **isolation levels and O(1) MVCC garbage collection on the Resolvers** — snapshot isolation with constant-time conflict-history reclamation. That work is what pulled me into FDB internals; this project turns those experiments into a broader attack on the two structures that enforce the short version window: Resolver conflict history and Storage Server MVCC history. The fork and a short design note will be linked here.

## Methodology and AI transparency

I write the core C++ myself — architecture, data structures, algorithms, and the debugging that matters. I use Claude Code as an accelerator for the surrounding engineering: codebase exploration, scaffolding, tests, adversarial workloads, benchmark harnesses, profiling assistance, and code review. The engineering log documents **who did what, in both directions**: when the AI found a problem in my work, when I found a problem in its output, and how each design decision was actually reached. The goal is to demonstrate technical judgment *and* honest AI leverage, not to pretend either away.

## Engage

Feedback is not just welcome — it is the point. The most valuable outcome of publishing this design is the list of requirements it does not yet know about. Open questions live at the end of each design doc; issues and discussions are open.

— **Ricardo Jiménez-Peris** · database systems engineer — MVCC, distributed transactions, storage engines. Three decades across research and industry; founder/CTO of a distributed HTAP SQL database.

*License: Apache-2.0 (matching FoundationDB).*
