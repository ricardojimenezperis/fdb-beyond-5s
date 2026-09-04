# Storage Phase — Paged Version Storage

*Status: frozen. Headline: remove the RAM–window coupling. Statements about current internals are recollections to be verified against the code.*

## 1. Context

The `VersionedMap` keeps the recent multi-version window in Storage Server RAM as a PTree — a persistent treap with auxiliary fat-node pointers (historically ~96-byte, refcounted, individually allocated nodes) that needs continuous structural compaction (`forgetVersionsBefore`) to recycle version slots. Every read passes through it (recent-version check before the disk engine; range reads merge PTree and engine results). Notably, **Redwood is already a versioned B+ tree** with its own pager — the machinery for versioned pages half-exists; the bridge between the in-memory window and paged version storage is the system's unfinished business.

## 2. Core design

- **Single-version main tree.** Thin nodes, latest version only. Hot-path reads descend a clean tree and check one version at the head.
- **Version chains in pages.** Older versions hang newest-first off each entry. Records are immutable, written once into append-only version pages, addressed `(pageID, offset)` through the pager — the same pointer whether the page is resident or spilled. Chain hops are paid only by readers of old versions. Version pages are **shard-local** (no global version log) — for locality, spill, and historical scans.
- **Spill, not durability.** Version pages carry no WAL and no durability promise: persistence is write-if-cold eviction under a local memory threshold. (Recovery: §6 — nothing is replayed.)
- **Reclamation.** A page carries the max cts of its records and dies whole when it falls below the oldest retained version — one integer against a moving floor; live readers protect their pages for free.

### Physical layout and residency

- **Locality is temporal, not spatial.** History pages fill in commit-version order, so co-located records were written together in time — there is no key locality to exploit. Consequently pages are the **allocation and reclamation unit, not the read unit**: `VersionRef = (pageID, offset, length)` allows a single directed read of exactly one record; carrying `length` in the reference keeps both options (record-granular vs whole-page I/O) open without changing the chain format.
- **Preallocated circular extent store.** History lives in large preallocated extents recycled at extent granularity: a background actor keeps reserve space ahead of the writer (no filesystem allocation on the hot path), writes are bump appends, and — because appends arrive in version order — extents are approximately temporal, so `floor > extent.maxCTS` returns whole extents to the free pool. After warm-up the history file stops growing: capacity is recycled, not extended. A cheap generation tag guards recycled pages against stale references (an assert, not a correctness dependency).
- **Residency via a page directory.** Chains never hold raw addresses; `PageDirectory[pageID] → RAM(ptr) | DISK(extent, offset)` makes spill a *residency change, not a structure transformation*.
- **Two watermarks, per replica.** `MRVᵢ` = oldest version retained by replica *i*; `MRV_RAMᵢ` = oldest version guaranteed resident on replica *i* (shard capability is the union of replica intervals, §6). Residency policy is deliberately **temporal, not access-based**: a transaction with `rv ≥ MRV_RAMᵢ` never touches disk for MVCC history on that replica; an older one pays its own reads. Old-history I/O is **read-through, not cache-through** (scratch buffer, discarded): a ten-minute scan cannot evict the hot window that keeps short transactions fast. With `W_RAM` defaulting to today's five seconds, the hot path is byte-for-byte today's behavior — the extension is pure opt-in.
- The property this buys, and the caption of the figure below: **extending the retained window does not enlarge the hot MVCC working set.**

## 3. Versioned clearRange

`clearRange` must keep a cost independent of the number of keys cleared — O(log n) structural metadata — while being versioned (older snapshots still see the range). Two-phase:

- **Logical (commit time), O(log n):** lazy-propagation marks on the canonical subtrees covering the range, each carrying the clear's cts; the visibility check rides the ordinary descent for free. Boundary (partially covered) leaves take ordinary per-key tombstones. Baseline alternative (kept in the doc): an auxiliary fragmented interval structure, RocksDB-DeleteRange style.
- **Strong invariant (write path):** a write entering a marked subtree first **pushes the mark down** along its path (copying it to the children of each marked node it passes) and materializes tombstones *with the clear's cts* at the leaf before applying itself. The mark thereby stops being merely a visibility filter and becomes a **certificate**: "no post-clear data below this node" — which licenses the deferred physical deletion to **unlink whole subtrees without inspection** once the floor passes the cts. Reads never push down. Structural operations (split/merge/redistribution) push marks down before moving pre-existing keys.
- Cost: O(log n) marks + O(two boundary leaves), independent of range population. The resurrection cost is paid by whoever writes into cleared territory.

## 4. Lazy GC

Garbage is registered at birth in a cts-ordered **GC queue** of `(pageID, death-cts, generation)`; the floor consumes it as a prefix. Generation IDs void stale entries (page already reclaimed wholesale — anti-ABA). A per-node "has tombstones" flag makes visit-driven cleanup one bit-test on clean nodes. **Residency rule:** in-place GC only on resident pages — cleaning a cold page fetched for a read would silently turn a read into future write I/O; cold garbage is the background sweeper's job, or dies whole with its page. All three garbage species (subtree marks, tombstones, chains) are reclaimed by the same floor.

## 5. Cluster control loop

*The oldest-active-read-version signal that this loop consumes is produced by the protocol in [`03-floor-tracking.md`](03-floor-tracking.md).*

- **Spill is a Storage-Server-local decision** (a version buffer pool with a memory threshold, evicting coldest pages) — fast local loop for a fast local resource, exactly as the SS already owns its durability flushing.
- **Ratekeeper signals are split.** (a) Undurable *new*-version bytes: unchanged physics, admission braking stays correct. (b) *Retained-window* bytes (resident + spilled): cluster braking is only the disk backstop (free space is already tracked); the precise lever is cohort retention accounting. Spill-path I/O saturation surfaces through the existing durability-lag sensor.
- **Accounting.** Old readers do not create versions — writes do; a reader only pins the floor (retained = write rate × pinned window). For any policy-selected candidate cutoff, the GC queue directly computes the bytes that advancing the floor to it would release — the marginal cost of retaining the oldest cohort. The aggregated watermark (`03-floor-tracking.md`) deliberately does not identify individual readers, so v1 charges retention to *cohorts*: *retention is charged to the oldest retained-version cohort and relieved by prefix revocation; short readers never generate control traffic or pin history beyond their lifetime.* (Genuinely per-reader attribution would require long-reader identity — exactly the per-TID state reserved for promoted transactions, `04-read-reservations.md`.)
- **Revocation under pressure.** The cluster revokes *read versions*, not transactions: a `minimumRetainedVersion` watermark; Storage Servers GC immediately; a dormant client discovers `transaction_too_old` on its next touch — the exact error contract applications already handle. *The five-second limit was always a static revocation; this design makes revocation dynamic and priced.* Prefix-only by choice (O(1), no transaction identification); the cutoff is the cheapest prefix that resolves the pressure (the GC queue doubles as the freed-bytes-per-cutoff function); revocation may be replica-local: the replica raises its MRVᵢ, answers `too_old_local`, and R(v) drops; the read retries on a peer with the *same* read version (no torn snapshot — every retry uses the same rv), and only when no usable replica retains v, or policy raises the shard floor, does it surface as `transaction_too_old` (§6).

## 6. History availability model (soft state)

> *Historical versions are replicated soft state. They are not recovered after Storage Server restart. A restarted replica rebuilds history organically; old reads are routed to replicas that still retain the requested version. Cluster recovery may invalidate all retained history, preserving FoundationDB's existing `transaction_too_old` semantics.*

Three layers (model ≠ representation):

1. **Abstract model:** per replica *i*, a capability set `Cᵢ ⊆ Versions`.
2. **Semantics:** shard capability `C_shard = ⋃ Cᵢ`; a read at v is servable iff `v ∈ C_shard`; effective historical redundancy `R(v) = |{i : v ∈ Cᵢ}|` — a step function with ≤ 2n breakpoints, whose deficit `D(v) = max(0, H − R(v))` *is* the repair work order.
3. **Representation invariant ("no holes"):** each replica always keeps a contiguous temporal *suffix*, `Cᵢ = [MRVᵢ, MAVᵢ]` — so two integers describe the full capability and H-membership is *emergent* from the published watermarks (no catalog).

Consequences: `future_version` and a new `too_old_local` become replica-local capability failures in opposite temporal directions (neither invalidates the transaction while a peer can serve); `transaction_too_old` remains the shard-level failure. Routing by interval is a hint; retry-on-peer with the same read version is the correctness path. Retention capability piggybacks on existing SS↔client communication — no additional round trip.

**H — historical redundancy as policy** (1 ≤ H ≤ n, over the existing team of n replicas): *durability of current data is a strong invariant; availability of old versions is a budget.* Repair is a race against the GC — an extent publishes only if still adjacent to the replica's current MRVᵢ (backward, contiguous, atomically lowering the watermark; in the single-threaded actor the "CAS" is an ordinary comparison). The three permitted operations — append at now, GC from MRVᵢ, repair backwards — each preserve the suffix by construction: **any hole is direct evidence of a bug**, which makes both implementation and the simulation assert small. Repair debt shrinks by itself as pages age out: **history redundancy is self-expiring redundancy** — the rare repair task that becomes *less* urgent with time, hence arbitrarily rate-limitable. TLogs are untouched throughout.

![Effective historical redundancy R(v) — illustrative](figures/rv-staircase.svg)

*R(v) is a step function with at most 2n breakpoints; where it dips below H, the shaded deficit D(v) is the repair work order — and it expires on its own.*

## 7. Cost model and the real headline

Structural CPU: ~8× per transaction (per-entry `forgetVersionsBefore` work → page pops; fat-node read descents → thin ones). End-to-end, Storage Servers are dominated by disk engine and network: **5–15% CPU** — a dividend, never the headline. The headline is the curve:

![RAM vs retained window — illustrative, pre-implementation](figures/ram-vs-window.svg)

*Illustrative, pre-implementation: parameters from §7; the benchmark exists to redraw this figure with measurements.*

Physical population is `N_phys = α · λw · W` with α (nodes/bytes per mutation) a *measured* parameter per design — the PTree's α includes path-copy amplification and tombstones; the paged design's α ≈ 1 record/mutation. At 500 K writes/s: hundreds of MB pinned at 5 s, ~3 GB at 60 s for the current design — that equation **is** the five-second limit; here it becomes an operator disk budget (∝ H·W).

## 8. Internal consumers — open questions

- **Change feeds:** version-ordered streaming across ranges; lagging feeds impose retention (floor = min(readers, feeds, durable)). What is their exact contract with the VersionedMap vs their own buffers?
- **Watches:** mutation-path hook trivially preserved; simulation loves watch + clearRange + restart corners.
- **fetchKeys / data movement:** the destination builds history out of order — chains must support insert-below-head (the most serious structural stress in the list). The source's long snapshot read pins the floor like any reader — and is billed like one.
- **Byte sampling:** must follow *logical* size (clears drop it instantly while physical bytes linger) or the data distributor decides on phantom bytes.
- **Ratekeeper knobs** for the split signals; **Redwood page headers** as a possible home for the subtree marks.

## 9. Validation

Deterministic simulation with a reference model and the suffix-invariant assert as executable specification; the full FDB simulation suite; **TSS pairing** against a stock Storage Server under identical workload. The star benchmarks: the RAM-vs-window curve, the R(v) staircase, and a long-reader workload that is impossible today.
