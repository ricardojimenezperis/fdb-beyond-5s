# Resolver Phase — Generational Paged Conflict Set

*Status: frozen. Reopening criteria are explicit and measurable (§7). Statements about the current implementation are recollections to be verified in log entry 001.*

## 1. Context: what the resolver does

The resolver stores only **writes** — write-conflict ranges annotated with their commit versions — and validates incoming read sets against them. The conflict window is `(read version, commit]`: the past does not conflict, it is *read*; a write with version ≤ rv was visible to that snapshot. In backward validation, the last committer pays for its entire concurrency: open transactions are invisible (first-committer-wins), so N short transactions + one long one cost N cheap validations + 1 expensive one — the theoretical minimum.

Everything is ranges. `clearRange` makes stored values ranges; `getRange` read sets force **order** on the structure (phantom protection: a read range must conflict with a point write inside it — the reason a hash cannot serve this workload, and, extended to the wire, the reason keys must travel). The current structure (to verify): a custom batch-optimized skip list with a `removeBefore(V)` whose `V` is the window constant in disguise, and whose reclamation is per-entry — dead nodes scattered in key order, unlinked one by one with cold pointer-chasing.

**Gap statement:** *FDB already prunes conflict history by version — but the threshold is a constant and the reclamation is per-entry. This project makes the floor dynamic and the death wholesale.*

## 2. Final design: two independent epoch skip lists

Two skip lists — **current** and **previous** — each fully self-contained in its own append-only pages (towers and records both). No pointer ever crosses a death boundary. An epoch spans one window W of versions.

- **Insert:** into the current epoch only. Bump-allocate the record; splice the tower (O(log) walk + O(1) expected links).
- **Rotation:** at a batch boundary (single-threaded — no batch straddles the cut), when the current epoch reaches W: drop *all* pages of the previous epoch to the free pool, previous ← current, current ← fresh. Death is O(1), touches nothing.
- **Validation with the band filter:** the previous epoch holds only versions < t₁ (start of current). If `rv ≥ t₁`, nothing in it can conflict — *one integer comparison skips it entirely.* Typical (recent) read versions therefore pay ~1.0 searches; the second search is paid by the brief post-rotation window (~δ/W) and by old read versions — the causers. The filter is per-validation, never a global mode.
- **Duplicates across epochs** (a boundary rewritten in both) resolve by max semantics — zero maintenance.
- **Degenerate regime:** if a dynamic floor is pinned (long reader), sealed epochs accumulate; validation consults only epochs with `maxVersion > rv` (the same filter, generalized), with background flattening as a backstop.

### Memory doctrine

Pages are the allocator: bump append-only, contents trivially destructible, no owning pointers (in-page copies or `(pageID, offset)` only). Records are **immutable** — refreshing a boundary is a *new* record in the current page, never an in-place version bump (a hot cell must not immortalize its page). Because commit versions are monotone and batches are processed in order, pages fill in version order and die in creation order: the page queue *is* the store, and GC is popping its head while `head.max < floor`.

## 3. De-risked sequencing

- **Phase A — dynamic floor on the existing structure.** `removeBefore` driven by a negotiated floor instead of the constant (the floor's input signal is specified in [`03-floor-tracking.md`](03-floor-tracking.md)). Minimal diff, immediate value, zero structural risk.
- **Phase B — the generational structure above,** gated by published profiles. If B does not pay, A has shipped and the analysis of *why* B does not pay is itself a contribution.

## 4. Cost model (summary)

Toy parameters: 100 tps, 5+5 ranges/txn, W = 5 s → N = 2,500 entries/window; miss = 100 ns ≈ 70 comparisons. Per entry lifecycle (insert + validate + die):

| Design | ns / entry lifecycle | Structural capacity | Notes |
|---|---|---|---|
| Current (per-entry GC) | ~370 | ~0.57 M txn/s/core | 81% of cost = cold unlinks |
| Two independent epochs | ~32 | ~6 M txn/s/core | death = page pop |

Per transaction, the structural totals of the two epoch variants (different unit — μs/txn, all ranges of a transaction included):

| Epoch variant | Structural μs / txn | End-to-end |
|---|---|---|
| Two independent epochs | 0.17–0.25 | ~1.96× |
| Interlaced OLD/NEW (alt., §7) | 0.23–0.35 | ~1.9× |

— a 2–5% end-to-end difference, within noise; the decision falls to invariant simplicity and option value (§7).

Search costs barely separate designs (halving N saves one comparison — the log is merciless); **miss-class operations separate them** (cold unlinks vs. nothing). End-to-end by Amdahl (overhead 1–2.5 μs/txn): **~2×** (1.6–2.4×), plus a qualitative dividend: half the resolvers for the same load ⇒ fewer multi-resolver phantom conflicts ⇒ fewer spurious aborts. The 10× also makes the resolver overhead-bound, promoting wire compression to the follow-up stage of the same attack.

## 5. Wire ladder (follow-up stage)

Keys must travel (ranges force them — see §1), but cheaper:

1. **Sorted-batch delta/prefix encoding** proxy→resolver (tuple-encoded keyspaces share long prefixes; ~5–10× typical; zero semantic change; candidate first PR).
2. **zstd with a trained dictionary** (subspace prefixes are stable — the dictionary is a materialized prefix trie; stateless).
3. Rejected for v1: stateful interning (protocol fragility). Future work, community-gated: a hashed point×point lane with a coarse range lane (touches serializability precision).

In-structure: discriminator prefixes inline in index nodes (fence keys); full keys in page records.

## 6. Benchmark plan

Six numbers close every open comparison: NEW-only lookup · OLD lookup · OLD+NEW as two searches · OLD+NEW as one interleaved walk · insert (independent vs interleaved) · **q** = measured fraction of cross-generation validations. They yield I, S, and the break-even **q\* = I/S** directly. Plus: perf split of structure vs overhead on a loaded resolver (fixes the Amdahl prediction); the cost in misses of a real cold unlink (the 10.5×'s main sensitivity); and the conflict workload profiler's quadrant matrix weighted by cycles, with the commit−read version distribution (the observable q before any redesign, and the epoch-sizing input).

## 7. Alternatives considered

*A profiler-gated fast path over a learned compact key domain — tri-state, completeness-certified — is specified separately in [`05-compact-domain-accelerator.md`](05-compact-domain-accelerator.md).*

- **Global pointer-skeleton index over pages.** Killed by a theorem: *a linked ordered index cannot die by the floor* — navigation cannot jump through vanished nodes (generation checks certify death but not continuation; they work for data pointers, not transit pointers).
- **Generational index rebuild** (incremental, Redis-rehash style): same per-window budget as `removeBefore` but sequential and compacting. Superseded by epochs (simpler).
- **Redundant next-pointers + era heal-sweep:** in-place cousin of the rebuild; finer invariants for the same budget.
- **Asymmetric interlaced OLD/NEW generations** (only OLD carries cross-pointers toward NEW; NEW is self-sufficient, so the band filter still skips OLD entirely for recent rv; rotation leaves nothing dangling). Fully correct in its final form. Decision criterion, made falsifiable: **cross-linking is worthwhile iff the measured cross-generation fraction exceeds q\* = I/S** — where I (the permanent insert tax of positioning in OLD) and S (the saving of one coordinated walk) are both of the order of one OLD descent, so q\* lands near 1: nearly unsatisfiable outside old-snapshot-dominant regimes. Independent epochs also keep OLD truly immutable — option value (spill, mmap, sharing) that cross-writes destroy. *Principle: when throughput ties within noise, invariant simplicity and option value decide.*
- **Per-epoch ART/radix instead of skip lists:** structural prefix sharing and O(key-length) descents; must be epoch-local (a global trie reintroduces per-node lifetime via prefix refcounts). Index variant to benchmark against FDB's highly tuned SkipList.

## 8. Open questions (for the community)

- Exact ConflictSet/SkipList mechanics: does the resolver (or proxy) sort batch ranges today? What does the commit proxy send per resolver? How is `removeBefore` amortized, and over which allocator?
- Intra-batch validation invariants to inherit (accepted writes conflicting with later transactions in the same batch).
- Phantom writes of rejected multi-resolver transactions: their acceptability argument depends on the short window — what changes when the write window grows (Beyond)?
