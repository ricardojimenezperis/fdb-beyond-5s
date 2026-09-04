# Storage Phase — Paged Version Storage

*Status: reviewed against `apple/foundationdb` main @ `a443d3ee60`. The design is sound and
most of its recollections about current internals check out; **one mechanism is misnamed**
(§1), **one figure contradicts `00-overview.md`** (§7), and **one client-compatibility
surface needed closing** (§6 — its logical contract is now specified, its encoding left open).
Claims about current FDB carry `file:line`; unmeasurable
claims are marked **[measure]**.*

## 1. Context *(corrected)*

The `VersionedMap` keeps the recent multi-version window in Storage Server RAM as a PTree —
a persistent treap with an auxiliary fat-node pointer (`Reference<PTree> pointer[3]`, the
third selected by `updated`/`replacedPointer`, `fdbclient/include/fdbclient/VersionedMap.h:94–109`),
refcounted and individually `FastAllocated` (`:94`).

**Correction — it is not "continuous structural compaction".** `forgetVersionsBefore`
(`VersionedMap.h:788–802`) drops roots from the deque and sets `oldestVersion`; the Storage
Server uses `forgetVersionsBeforeAsync` (`storageserver.cpp:10888`), which additionally hands
sole-owner roots to `deferredCleanupActor` (`VersionedMap.h:804–836`, `:67–77`), freeing ≤100
nodes per `yield` through an explicit worklist. There is **no rebalancing and no path
rewriting**. The real `compact()` (`VersionedMap.h:871–882`) is called **only** from
`fdbclient/bench/BenchVersionedMap.cpp:442` — the Storage Server never compacts.

The per-entry cost is therefore *refcount-driven destruction of scattered nodes*, not
compaction. This does not weaken the "per-entry work → page pops" argument of §7 — scattered
refcount frees are exactly the miss-class work a page pop eliminates — but the mechanism must
be named correctly, because a reader who goes looking for a compaction loop will not find one.

**Confirmed:** every read does pass through it. Point reads take
`VersionedData::lastLessOrEqualAt(data->data().getRoot(version), version, key)` and fall
through to `storage.readValue` (`storageserver.cpp:2431–2436`, `:2494`); range reads
materialize a view, `data->data().at(version)` (`:3174`), and merge with engine results.

**Confirmed:** Redwood is already a versioned B+ tree with its own pager —
`fdbserver/kvstore/VersionedBTree.cpp`, `PageToVersionedMapT` (`:1970`) mapping logical page
→ version → physical page, with `remappedPages` maintained at `:2341` and consumed at `:2769`.
**But precedent, not a reusable component.** Redwood demonstrates that FDB already contains
production implementations of logical-to-physical page remapping and version-aware pager
machinery — evidence that the required primitives are native to this codebase. It is *not* yet
evidence that the Redwood pager can be reused unchanged: PVS history is soft state with
different recovery semantics (§6), and must not accidentally become a second durable store.
"The machinery half-exists" is right as narrative and must not be read as a reuse claim.

## 2. Core design

Single-version main tree; version chains in shard-local append-only pages addressed
`(pageID, offset, length)`; spill-not-durability; page-granular reclamation against a moving
floor. Physical layout, preallocated circular extents, page directory and the two watermarks
`MRVᵢ` / `MRV_RAMᵢ` are unchanged.

**"Byte-for-byte today's behaviour" is a testable claim, and a binding constraint.** The current hot read path is synchronous and allocation-free: the floor test is
`tryGetReadyReadVersion` (`storageserver.cpp:2057–2076`, unit-tested at `:2078–2093`) and the
lookup goes straight to a root without materializing a `ViewAtVersion` (`:2431`, `:2494`) —
both introduced by the coroutine-overhead work (`b624489520`).

**Corrected in review — `rv ≥ MRV_RAMᵢ` does not license removing the Page Directory.**
`MRV_RAMᵢ` only says the needed history is *resident*. A read with
`MRV_RAMᵢ ≤ rv < currentVersion` may still have to walk the version chain, and since chains
are addressed `(pageID, offset)` and "never hold raw addresses" (§2), traversal must translate
`pageID` even for a resident page. The only reads that avoid the chain entirely are those whose
visible value is already materialized in the single-version main tree. The criterion is
therefore **three paths, not two**:

> **Hot-path acceptance criterion.** A latest-version read satisfied by the single-version main
> tree must not consult the history Page Directory or construct a historical view. For
> `MRV_RAMᵢ ≤ rv < currentVersion`, a read may traverse resident version-chain pages, but page
> resolution must remain **synchronous, allocation-free and free of storage I/O**. Only
> `rv < MRV_RAMᵢ` may enter the asynchronous spill path. Benchmarks must report these three
> paths separately rather than treating every `rv ≥ MRV_RAMᵢ` read as equivalent.

This maps onto what exists: the current point read already has exactly two outcomes — value
found in the versioned map (`path == 1`) or fall through to `storage.readValue`, which suspends
(`path == 2`, `storageserver.cpp:2432–2436`). The new design adds a third *between* them, and
that middle path is the one whose cost nobody has measured.

If the table is to be avoided for resident history too, that needs an explicit mechanism —
direct RAM pointers with a different representation on spill, say — not merely a requirement.

## 3. Versioned clearRange

Two-phase logical marks + push-down-on-write certificate. No code claims to verify; the
invariant ("no post-clear data below this node") is what licenses subtree unlinking without
inspection, and is the right thing to write as an executable assert.

## 4. Lazy GC

cts-ordered GC queue consumed as a prefix, generation IDs against ABA, per-node
"has tombstones" bit, in-place GC only on resident pages.

**The reclaimed-link invariant.** §2 justifies page-granular death as "one integer
against a moving floor; live readers protect their pages for free". That argues nobody *needs*
the content; it does not make surviving **inbound** links safe. A newer version points at an
older one by `(pageID, offset)`; when the older page dies because `maxCTS < floor`, those links
persist. And the generation tag is explicitly *not* leaned on for this — it is "an assert,
not a correctness dependency" (§2) — so the design has *no* stated correctness mechanism for
dangling chain links.

**That characterization must be retired.** Under the optimistic access protocol below, the
generation is not a debugging assert: it is *the* correctness mechanism, and its publication
order and counter width become load-bearing. §2's sentence should be rewritten accordingly.

> **Reclaimed-link invariant.** Page-granular reclamation does not require incoming references
> to disappear, but every incoming link to a reclaimable page must be **safely recognizable
> without dereferencing that page**. A chain link therefore carries enough version metadata to
> prove its target lies below the effective floor, or resolves through a directory tombstone
> that terminates the traversal. For every admitted `rv ≥ minimumRetainedVersion`, lookup must
> reach its visible value without resolving a reclaimed page. Page deletion is legal only once
> this holds for every incoming chain.

Four candidate mechanisms; the first is the most aligned with immutable records and O(1)
reclamation: (a) each link carries its target's version, checked before resolving `pageID`;
(b) the Page Directory keeps a `reclaimed` tombstone that terminates traversal; (c) the
boundary link is truncated or rewritten before the page is freed; (d) the page survives as a
skeleton until no retained chain references it.

**Why this is tractable here and was fatal in the Resolver.** `01-resolver.md` §7 kills a
global pointer-skeleton index with "a linked ordered index cannot die by the floor" — because
*navigation* must pass through vanished nodes. These chains are **directed and newest-first**:
traversal only ever needs to stop, never to continue past a dead target. That is why a version
stamp on the link suffices here and did not there.

**Simulation property:** `admitted rv ≥ MRV ⇒ lookup never dereferences a reclaimed page`.
Cases: a key unchanged across many epochs; a visible version sitting in the last retained page;
a link from a RAM page to a page that was spilled and then reclaimed; a `clearRange` whose
boundary or certificate lived in the deleted page; reclamation concurrent with an already-started
read; and ABA through circular reuse of the same `pageID`. The existing generation IDs help with
the last one but do not by themselves give correct chain termination.

### The concurrent case needs a second, different invariant

Safe *navigation* is not safe *lifetime*. The invariant above stops a reader from beginning to
follow a dead link; it says nothing about a reader that already resolved a `pageID` and holds
the page when GC retires or reuses it. Generation IDs detect ABA — you notice you were handed
the wrong thing — but detection is not prevention of use-after-free.

> **Resolved-page access invariant.** After resolving `(pageID, generation)`, a reader must use
> one of two protocols:
> **(1) guarded access** — retirement and reuse are prevented until the guard is released; or
> **(2) optimistic materialization** into operation-owned storage, followed by validation that
> the directory still maps `pageID` to the same `generation` **before any bytes are decoded,
> traversed or exposed**. Failed validation discards the buffer and either retries or returns
> the appropriate local-capability error.

It has to be a disjunction, not a single rule: the optimistic path deliberately *permits* reuse
while the I/O is outstanding, which the guarded formulation would forbid.

**Ordering for the guarded path:** (1) the floor declares the page unnecessary; (2) the
directory stops handing it to new readers; (3) already-started readers release their guards;
(4) only then is the storage reused and the generation changed.

**Ordering for the optimistic path** — and this is the part "own buffer" does *not* give you
by itself:

> Retirement publishes removal of the old directory generation **before** its extent may be
> overwritten or reassigned. Any I/O overlapping reuse therefore fails the post-read generation
> check. Generation reuse or wraparound within the lifetime of an outstanding operation is
> forbidden.

1. capture `(pageID, generation)`;
2. issue I/O into an operation-owned scratch buffer;
3. retirement publishes a different generation *before* reusing the extent;
4. on return, compare;
5. interpret the bytes only if it matches; otherwise discard.

An operation-owned buffer prevents use-after-free *in memory*; it does not by itself prevent
**consuming a read that overlapped the extent being overwritten**. Publishing the generation
change first, plus validating before decode, is what completes that proof.

**And the generation must be non-repeating by construction, not by timing.** "Wide enough for
the maximum outstanding-I/O window" is not a proof: a coroutine can stay suspended
indefinitely — cancellation may be deferred, a process may stall, state may survive a long
pause. Take time out of the condition:

> **Scope of non-repetition.** A page generation must not repeat while any directory entry,
> chain link, guard, or outstanding I/O from the previous use can still exist.

That scope is neither "the process" nor "the disk" — it is *the lifetime of the authority that
can still hold references*. Realize it as:

> `(directoryIncarnation, extentReuseCounter)`. The incarnation changes whenever the Page
> Directory is initialized or reset; the counter never wraps within an incarnation, and
> exhaustion must fail rather than wrap. An incarnation may be retired only after its
> outstanding operations are drained or fenced. **No persistence is required**, because restart
> discards the directory and the retained history rather than reconstructing them from residual
> extents.

Within an incarnation there is no ABA even if an I/O stays outstanding forever; 64 bits make
exhaustion practically irrelevant while the rule stays formally exact.

**Why not a per-process or on-disk scope.** A restart destroys every coroutine and outstanding
read, empties the Page Directory, and does not recover history (§6, and
`setInitialVersion` at `storageserver.cpp:1602–1613`). Residual extents survive as *bytes*, but
their logical identity does not: nothing maps a `pageID` to them any more. With no observer
left, repeating a generation after restart cannot produce ABA — the original A is unreachable.
The incarnation therefore exists for **directory resets that can overlap still-outstanding
operations**, not for process restarts.

One realization note that survives the correction:

> `thisServerID` cannot serve as the directory incarnation **by itself**: it persists across
> restore and does not distinguish consecutive directory lifetimes. A separate volatile,
> non-repeating directory epoch is required whenever old operations may overlap a reset.

It may still appear as a *component* of a composite identity — useful for traces and asserts —
but it supplies none of the discriminating power. (Persisted under `persistID` and re-read on
restore: `storageserver.cpp:11141`, `:11503`.)

**Caveat for a future implementation.** This argument depends on §6's soft-state model. If some
later version ever reconstructs history by scanning residual extents, identity would have to
become persistent — or the extents explicitly erased or logically formatted at startup — and
that would be a change to §6, not a local optimization.

**This is cheaper here than it looks, because the runtime is cooperative and single-threaded.**
"Concurrent" means *across a suspension point*, not across threads: a guard only has to survive
`co_await`. FDB already solves this exact shape twice in the Storage Server —
`durableVersionLock` exists so "no eager reads both begin before the commit was effective and
are applied after we change the durable version" (`storageserver.cpp:11091–11096`), and the
point-read path re-validates after suspending on disk rather than pinning:
`if (version < data->storageVersion()) throw transaction_too_old()`
(`:2438–2442`). Both mechanisms are available, but **check-after-use has a precondition**:

> Check-after-use is admissible only when the asynchronous I/O materializes the page into
> **operation-owned storage**, and the directory generation is revalidated *before* decoding,
> traversing links, or exposing any result. A path that retains direct access to reclaimable
> storage across a suspension must instead hold a guard — checking afterwards would detect the
> ABA but not prevent the invalid access.

The design already satisfies that precondition on the path that needs it: §2 specifies that
old-history I/O is "read-through, not cache-through (scratch buffer, discarded)". That policy
was introduced to stop a long scan evicting the hot window; it *also* makes check-after-use
legitimate for spilled reads. Two independent motivations converging is a good sign — but the
connection has to be stated, or a later optimization that shares page buffers would silently
break the lifetime argument.

Per path, then:

- **resident, no `co_await`** — cooperative single-threaded execution prevents interleaving; no
  guard needed;
- **spilled, into an operation-owned scratch buffer** — read, revalidate the generation, then
  consume;
- **any direct access to reclaimable storage that survives a `co_await`** — guard mandatory.

The full proof needs both invariants: *termination* before reclaimed links, and *stability*
after resolving a page.

## 5. Cluster control loop *(one confirmation, one gap)*

The oldest-active-read-version signal is produced by
[`03-floor-tracking.md`](03-floor-tracking.md) — cross-reference verified in both
directions: `03-floor-tracking.md` §6 cites this section for priced revocation, and this section's
"the aggregated watermark deliberately does not identify individual readers" matches `03-floor-tracking.md`'s
single `globalOldestClientRV` exactly.

**Confirmed:** "free space is already tracked" — Ratekeeper consumes
`ss.getSmoothFreeSpace()` against `SERVER_KNOBS->MIN_AVAILABLE_SPACE`, computing spring and
target bytes from it and raising `limitReason_t::storage_server_min_free_space`
(`fdbserver/ratekeeper/Ratekeeper.cpp:647`, `:684–696`). The disk backstop the design wants
to lean on exists.

**Confirmed:** `minimumRetainedVersion` does **not** exist in the code — it is introduced by
this design. (`03-floor-tracking.md` §2 depends on that fact and states it the same way.)

**Gap — revocation is specified only as lazy discovery.** §5 says a dormant client "discovers
`transaction_too_old` on its next touch". That is after-the-fact. `03-floor-tracking.md`
§7a additionally requires an **admission-time** rule: a reported floor older than the
permitted window is not silently accepted under a newer value; the proxy either rejects the
registration or returns the effective *granted* floor, and only the granted floor enters the
reduction.

Those two are compatible — registration-time clamp for *new* demand, lazy discovery for
*revoked* demand — but they are not the same rule, and this document does not contain the
first. **This is an extension of 02's contract by 03, not an inheritance of it**, and it
should be stated here so the pair does not drift: *a budget may deny a lease, but it may not
pretend to have granted one.*

## 6. History availability model *(supported by code; new-error contract specified, encoding open)*

**Confirmed — history is not recovered after restart.** `setInitialVersion`
(`storageserver.cpp:1602–1613`) sets `version = desiredOldestVersion = oldestVersion =
durableVersion = ver` and calls `forgetVersionsBefore(ver)`, so a restarted replica begins
with an empty window. "Rebuilds history organically" is consistent with today's behaviour.

**Confirmed — cluster recovery may invalidate all retained history.**
`recoveryTransactionVersion = lastEpochEnd + MAX_VERSIONS_IN_FLIGHT`
(`fdbserver/clustercontroller/ClusterRecovery.cpp:1387`) with
`MAX_VERSIONS_IN_FLIGHT = 100 × VERSIONS_PER_SECOND` (`fdbserver/core/ServerKnobs.cpp:156`)
already puts every pre-recovery read version out of range. The `transaction_too_old` contract
is preserved because it is today's contract.

**The new error is a client-visible protocol surface — contract specified below, encoding
open.** `future_version` exists
(`flow/include/flow/error_definitions.h:45`, code 1009); **`too_old_local` does not** — it is
introduced here. §6 says neither error invalidates the transaction "while a peer can serve",
but does not say what a client that has never heard of `too_old_local` does with it. Three
requirements follow, and they mirror `03-floor-tracking.md` §3 exactly:

1. The new code must be gated on the negotiated protocol version, using FDB's established
   idiom (`if (ar.protocolVersion().hasX())`, e.g.
   `fdbclient/include/fdbclient/CommitProxyInterface.h:151–153`).
2. A legacy client must never receive it — a replica-local capability failure must degrade to
   an error the old client already handles, or the read must be retried server-side.
3. Mixed-version simulation must cover a legacy client reading a shard whose replicas have
   divergent `MRVᵢ`.

Without (2) the "no flag day" property this project claims elsewhere does not hold for reads.

**And the logical contract should be closed now, even if the encoding stays open.** A Storage
Server that does not retain `rv` has not shown the transaction is too old — a peer may serve it:

`local replica lacks rv ≠ transaction_too_old`  ·  `all eligible replicas lack rv = transaction_too_old`

> **Endpoint, not transaction.** A replica-local history miss is a retryable
> *endpoint-capability* result. It never invalidates the transaction and never causes the client
> to acquire another read version: the read layer retries another replica preserving the original
> pinned RV and its lease. Only once the routing/capability layer establishes that **no eligible
> replica** can serve that RV may the client receive `transaction_too_old`.
>
> **Legacy:** a legacy client never receives `too_old_local`. A compatible server-side or
> client-proxy path retries another replica and, if none can serve, returns the existing
> `transaction_too_old`. New clients may additionally consume the local-capability result as a
> routing hint.

**FDB already has this exact shape**, which makes the contract cheap to state and to test:
`wrong_shard_server` is a replica-local capability failure that the client handles by
invalidating its location cache and retrying after `WRONG_SHARD_SERVER_DELAY`, **without
touching `trState->readVersionFuture`** (`fdbclient/NativeAPI.cpp:1786–1787`, `:1914–1915`,
`:1921`). `too_old_local` should behave the same way, differing only in which capability failed.

**The race is expected, not a bug.** Router observes `MRVᵢ ≤ rv`; the replica revokes history;
the read arrives. The replica returns the local error, the read reroutes with the *same* RV, and
if it was the last eligible replica then revocation has already moved the availability contract
and the transaction ends explicitly. Routing by interval is a hint (§6); retry-on-peer is the
correctness path.

## 7. Cost model — figure withdrawn, methodology kept

A predicted **~8× reduction in structure-only CPU cost** per transaction, end-to-end 5–15 % —
**[measure]**, and framed as a dividend rather than the headline.

**The RAM-vs-window figure is withdrawn pending measurement**, aligning this document with
`00-overview.md` §4. The "~96-byte nodes" and "hundreds of MB at 5 s, ~3 GB at 60 s" figures
quote a per-node size below the code's own accounting: the in-tree budget comment totals
**128 allocated bytes** (`storageserver.cpp:13276–13313`) — **stale: measured, the node is
88 bytes, allocated as 96** (`../benchmarks/measurement-results.md` T0.1), which vindicates the
per-node figure —
`overheadPerItem = nextFastAllocatedSize(sizeof(PTreeT)) * 4` (`VersionedMap.h:765–766`), and
the charge actually used for byte budgets is
`overheadPerItem*2 + (mutationBytes + MutationRef::OVERHEAD_BYTES)*2`
(`fdbclient/include/fdbclient/StorageServerInterface.h:1266–1272`) — doubled because a
mutation lives in both the versioned map and `mutationLog` (`storageserver.cpp:885`).

The *methodology* stands: `N_phys = α · λw · W` with **α a measured parameter per design**
is the correct framing, and the PTree's α does include
path-copy amplification and tombstones. What must not survive is quoting a number for α before
measuring it. `versionedMapTest()` already prints `sizeof(StorageServer::VersionedData::PTreeT)`
at runtime (`storageserver.cpp:13318`, via `fdbserver -r versionedmaptest`); comparing measured
RSS growth against `mvccStorageBytes` is ~2 hours of work and redraws the figure honestly.

## 8. Internal consumers *(two answered, one added)*

- **Change feeds — partially answered.** They exist (`storageserver.cpp`,
  `fdbclient/include/fdbclient/StorageServerInterface.h`) but do **not** appear among the
  terms of `proposedOldestVersion` (`storageserver.cpp:10453–10462`), which are only
  `version`, `cursor->getMinKnownCommittedVersion()`, `lastTLogVersion`, `oldestVersion`,
  `desiredOldestVersion` and `initialClusterVersion`. So a lagging feed does **not** pin the
  MVCC floor through that path today. The open question narrows to: where *does* feed
  retention bind, and does the paged design change it?
- **NEW — native CDC is a second consumer this list omits.** This tree carries
  `fdbserver/cdcproxy/CDCProxy.cpp` and `ClientDBInfo::nativeCdcEnabled`
  (`fdbclient/include/fdbclient/CommitProxyInterface.h:116`, serialized at `:146`). Its
  retention contract needs the same question asked of it.
- **fetchKeys — sharper than it first appears, and the current code sidesteps it.** Today the
  destination does **not** build history out of order: it picks
  `shard->transferredVersion = data->version.get() + 1` and calls `createNewVersion` on it
  (`storageserver.cpp:7921–7927`), i.e. it inserts at a *new highest* version. The in-tree
  FIXME at `:7922–7924` says the documented-correct alternative
  (`batch->changes[0].version`) "never introduces extra versions into the data structure, but
  violates some ASSERTs currently". So insert-below-head is a requirement the **new** design
  creates, and there is already in-tree evidence that the correct choice breaks current
  assertions. Ranking this as the most serious structural stress is, if anything, understated.
- **Watches, byte sampling, Ratekeeper knobs, Redwood page headers** — unchanged.

## 9. Validation

Deterministic simulation with a reference model and the suffix invariant as executable
specification; the full FDB simulation suite; TSS pairing. **Addition:** the TSS comparison
needs an explicit rule for the one class of divergence that *is* the feature — a modified SS
and a stock SS will legitimately disagree on old-version reads. Without that rule TSS evidence
is unusable here (the same caveat is recorded in `00-overview.md` §5).
