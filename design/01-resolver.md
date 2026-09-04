# Resolver Phase — Generational Paged Conflict Set

*Status: **correctness closed; implementation prototype justified by reclamation behaviour.**
T1.1/T1.2/T2.2 found no throughput case for two generational SkipLists, and **those results
stand**. T3.3 supplies an independent reason to prototype them: a demand-driven floor exposes
reclaimable state in bursts, while the current per-node sweep is coupled both to floor advancement
and to subsequent write volume. Making that mechanism robust would require a debt- or
time-controlled GC whose reclamation rate trades commit latency against retained memory.
Arena-backed epochs replace that incremental debt with whole-epoch retirement. Every claim
about current FDB is verified against `apple/foundationdb` main @ `a443d3ee60` and carries
`file:line`. Reopening criteria remain explicit and measurable (§7). Remaining unknowns are
marked **[measure]** with plans in `questions.md`.*

## 1. Context: what the resolver does — verified

The resolver stores only **writes**: write-conflict ranges annotated with their commit
versions, validated against incoming read sets. Confirmed —
`combineWriteConflictRanges` emits the union of write ranges of the transactions not
already known to conflict (`ConflictSet.cpp:1035–1050`), and only those are inserted, at
the batch commit version (`:1028–1033`, `:442`). Read ranges are query-only
(`:997–1003`). No values enter the resolver conflict structure or the Commit
Proxy→Resolver wire path: an ordinary transaction sends only conflict ranges, and
mutations travel that path solely for metadata transactions (`CommitProxyServer.cpp:191–197`,
`fdbserver/core/include/fdbserver/core/ResolverInterface.h:122–140`).

The conflict window is `(read version, commit]` — confirmed literally: validation
short-circuits on `getMaxVersion(l) <= version` (`ConflictSet.cpp:680–681`, `:711–712`)
and conflicts on `> version` (`:689`), so a write at exactly `rv` does not conflict.
The **backward-validation** argument stands: each transaction's read-conflict ranges are
checked against write-conflict ranges committed after its read version, with accepted
earlier transactions in the same batch contributing writes visible to later validations
(`ConflictSet.cpp:913–938`). The *"first-committer-wins"* label is dropped as too broad for
FDB: write-write overlap alone is **not** a conflict. Intra-batch conflict is tested only
over `tr.readRanges` (`:924–925`) while write ranges merely mark the bitset (`:934–936`),
and `tooOld` itself requires a non-empty read conflict set (`:805`) — two blind writers to
the same key both commit.

Everything is ranges, and `getRange` read sets force **order** — confirmed: validation
walks the ordered node sequence between a start and an end finger (`:663–727`), and batch
points are sorted by a custom MSD radix sort with the begin/end and read/write
discriminators fused into the comparison (`:93–137`, `:168–222`). A hash **alone** cannot
implement exact arbitrary-range overlap validation; ordered range endpoints must remain
available and therefore must travel under the current protocol. This does not exclude
partial hashing accelerators (§7).

**The current structure, verified.** A custom
batch-optimized skip list: 16-way interleaved finger construction (`:490–542`) and
validation (`:446–488`), insertion in stripes of 16 (`:1015–1025`), `_mm_prefetch` in the
finger walk (`:355–361`), vtune annotations still in the source (`:467`, `:494`, `:528`).
`removeBefore(V)`'s `V` really is the window constant in disguise:
`newOldestVersion = req.version − MAX_WRITE_TRANSACTION_LIFE_VERSIONS`
(`Resolver.cpp:359` → `ConflictSet.cpp:986–991`) — a *sliding* floor with a constant lag,
and `cs->oldestVersion` is already a variable (`:754`), which makes Phase A a small
structural diff (§3).

**There are two per-entry unlink paths, not one** — it is easy to count only the floor
sweep. *An earlier draft guessed the commit-path one might be larger; T1.1 measured it at
**0.2 %** of Detect on the tree's benchmark, against the sweep's 8.2 % — so on that workload it
is the smaller of the two by far, and both are small.* The two paths are:

* **Insert-time interior deletion.** `addConflictRanges` calls `remove(startF, endF)`
  unconditionally (`:441`), and `remove` walks level 0 destroying every node between the
  fingers **with no version check** (`:579–597`).
* **Floor sweep.** `removeBefore` (`:544–576`), budgeted at
  `3 × |combined write ranges| + 10` nodes *examined* per batch (`:991`, `:549`), resumed
  from a persisted key cursor (`:990`, `:992`).

The existing counters isolate only the **sweep** — `g_removeBefore` — while the insert-time
deletion sits inside `g_merge` alongside `find`, both boundary inserts and the level splice
(`:47–49`, `:952–994`). That is exactly why T1.1 was needed: its instrumentation added the
internal split of `D.MergeWrite`, and only then did the 81 % claim in §4 become measurable
(`questions.md` Q8, `../benchmarks/measurement-results.md` T1.1).

**Gap statement:** *FDB already prunes conflict history by a sliding, floor-driven
threshold, and the floor is already a variable. What is constant is its **lag**, and what
remains per-entry is **both** deletion paths. This project makes the lag negotiable and
the death wholesale — and must pay for wholesale death with a larger live set (§2).*

## 2. Phase B: two arena-backed epochs of the existing skip list

**Closed.** **At most two live epochs, backed by two reusable arena slots**, each holding the
**existing canonical `SkipList` representation and lookup algorithm, adapted to an epoch-local
arena** — the same piecewise map from key to last-write-version that ships today, with the
allocator replaced and `remove()` reduced to splice-and-abandon (§2.e). In single mode only
*current* is live; in dual mode *current* receives inserts and *previous* is sealed. A sealed
epoch's arena is discarded whole when the floor passes its maximum version, and reused immediately
as the next write target. No new data structure
is required.

> **The "open indexed representation" obligation is withdrawn (§2.1).** It came from
> assuming *record-level immutability* — "refreshing a boundary is a new record" —
> which forbids the interior deletion that keeps the map canonical, and therefore turns
> validation into an overlap-max query over accumulated intervals. **Two epochs never caused
> it.** Two canonical maps are two cheap step-function queries. Dropping the immutability
> requirement dissolves the obligation entirely; §2.1 records what it cost to discover that.

- **Insert:** into the current epoch only, using today's `addConflictRanges` unchanged —
  find fingers, insert the end node preserving the prior value (`ConflictSet.cpp:438–439`),
  remove the interior, insert the begin node at the commit version (`:441–442`). Each epoch
  stays canonical, so N tracks **live boundaries**, not ranges written.
- **Each epoch is a complete map** whose default is "nothing written in this epoch". That
  falls out of the existing end-node rule: in a fresh epoch, "what was there" is the epoch's
  seed version.
- **Rotation is death-driven, with a threshold only for forming generations (§2.a):** a floor
  advance triggers the **death** tests, allocation growth triggers the **X** test in single mode,
  and where both coincide at a batch boundary death wins. In single mode `floor > maxTS(current)`
  discards the lone arena and re-seeds it empty, staying single — a sub-X epoch is retired as often
  as the workload dictates. Only a *live* `current` reaching **X** is sealed into dual mode; there,
  `floor > maxTS(previous)` drives rotation, and if `current` is obsolete too the system returns to
  single mode. **X forms generations; the floor kills epochs.** No batch count, no timer, and no
  size threshold on steady-state rotation.
- **Why freeing is safe:** every admitted transaction has `rv ≥ floor`, so
  everything in the sealed epoch is `≤ maxTS < rv` and can never conflict — it is provably
  useless. Free it and reuse its storage as the new current. Ping-pong between two buffers.
- **Both operations at a batch boundary.** Confirmed feasible: batches are strictly serialized
  by version (`Resolver.cpp:226–249`, `:313`, `:324`, `:537`), the compute section `:324–555`
  contains **no** `co_await`, and duplicate re-deliveries skip it wholesale (`:556–559`).
- **Duplicates across epochs** resolve by max semantics, with zero maintenance: a range
  rewritten in *current* does not need its older boundaries removed from *sealed*, because
  the max across epochs dominates them.

### 2.a Two modes: a threshold forms generations, death rotates them

*Size does not drive steady-state dual-epoch rotation, but a **bootstrap/re-entry threshold
X remains necessary** — it decides whether a *live*
single epoch has earned a second generation. It bounds how often a **generation forms**, never how
often an **arena is retired**: §2.a.1 property 3. The separation in one line: **X forms
generations; the floor kills epochs.***

**Two distinct triggers, with death taking precedence.** The death tests and the formation test do
not fire on the same event:

- **Floor advancement triggers the death checks**, in both modes.
- **Allocation growth triggers the X formation check**, in single mode only — so during a *floor
  plateau* `current` can still reach X and must form a pair, even though no death test can fire.
- **When both are observed at the same serialized batch boundary, death takes precedence over
  formation** — an epoch the floor has already made obsolete is discarded rather than sealed.

Reclamation is the point; X exists only to form a second generation when the current one refuses to
die.

**Single-epoch mode.** Only `current` exists. Evaluated at a batch boundary whenever the floor
advanced or the epoch grew:

```
if floor > maxTS(current):          # death
    discard current; re-seed it empty at a version <= floor
    stay in single mode
elif ownedBytes(current) >= X:      # formation
    seal current as previous; open the other arena as the new current (seed <= floor)
    enter dual mode
```

Whenever the floor advances past `maxTS(current)`, `current` is wholly obsolete: discard its arena,
create a fresh empty `current` seeded at or below the floor, and **remain in single-epoch mode**.
Otherwise `current` is sealed only when its owned allocation reaches X. A single epoch therefore
does **not** have to survive until X — if conflicts stop being generated and the in-flight
transactions finish, the advancing floor kills it earlier and returns the whole arena.

**Repeated sub-X retirement is the intended behaviour, not thrashing.** A short burst of conflicts
leaves a small arena, which is freed whole once the floor proves it obsolete. Keeping it alive
until X would turn a bootstrap threshold into a barrier to reclamation and would hold bytes that
nothing needs. Each such cycle marks a distinct burst of conflict activity separated by enough
quiescence for the floor to make it irrelevant — cost privatized to the workload that caused it,
which is the doctrine (`00-overview` §2).

**Dual-epoch mode.** `previous` is sealed, `current` is writable. Evaluated on each floor advance
(no formation test applies here):

```
if floor <= maxTS(previous):        # previous still needed
    stay in dual mode
elif floor > maxTS(current):        # both obsolete
    free previous; free current
    open a fresh empty current seeded at a version <= floor
    return to single mode
else:                               # rotate
    free previous; seal current as the new previous
    reuse the freed arena as the new current (seed <= floor)
    stay in dual mode
```

The conceptual priority is:

1. **Death frees storage** — tested on every floor advance, in both modes, and taking precedence
   over formation when both fire at the same batch boundary.
2. **X forms a generation only while the single epoch stays alive** — and it is triggered by
   allocation, so a pair can form during a floor plateau.
3. **In dual mode only the death of `previous` permits a rotation.**
4. **If `previous` and `current` have both died, free both and return to single mode.**

```
[*]    --> Single
Single --> Single : current dies (floor > maxTS(current))
Single --> Dual   : current reaches X while still alive, seal current
Dual   --> Dual   : previous dies, current still needed
Dual   --> Single : previous and current both obsolete
```

**Two implementation notes — neither belongs to the state machine.**

- *Discarding an empty epoch must be harmless and cheap.* An empty `current` has `maxTS` equal to
  its seed, so the death test fires on it at every floor advance. That is fine: with no pages and
  no allocations, "discard" reduces to updating metadata or resetting the bump pointer. No
  `hasWrites` predicate enters the correctness argument; the implementation simply must not make
  `discard(empty)` expensive.
- *A dead `current` in dual mode may be reset in place.* §2.a.1 establishes there is no ordering
  between the maxima, so `floor ≤ maxTS(previous)` and `floor > maxTS(current)` can hold together:
  `previous` still carries needed history while `current` is already obsolete. Since no pointer
  crosses an epoch boundary, that `current` can be reset without touching `previous`. This is a
  **memory optimization, not a generational transition** — resetting a wholly obsolete `current` is
  not a rotation, and the only generational transition remains the one conditioned on the death of
  `previous`.

**X must be measured in pages or allocated bytes**, not reachable entries: under splice-and-abandon
the reachable count does not reflect the abandoned nodes inside the arena, and physical memory is
what X exists to control.

### 2.a.1 Why this is correct

1. **Retention ⊇ admission across every state transition and in-place reset.** The admission floor
   is monotone, so a version proved to be below it at one boundary is permanently irrelevant; each
   case below discards only such versions.
   - *Single→dual* discards no history: the live `current` becomes `previous` unchanged, and a
     fresh `current` is opened for subsequent writes. Every conflict represented before the
     transition remains represented afterwards.
   - *Dual→dual:* `floor > maxTS(previous)` proves that **every version represented by `previous`
     is below the admission floor**. Since `previous` was sealed, every subsequent write was
     initially inserted into `current`; any of those writes removed by an intervening `current`
     reset had already been proved below an *earlier* floor, so every remaining post-sealing write
     that could still conflict is represented by `current`. `previous` may therefore be discarded,
     `current` sealed, and a fresh `current` opened. *(This argument makes no claim about where
     retained history "begins": a canonical epoch holds a piecewise function with a seed and
     copied-forward boundary values, not a contiguous version interval. An earlier revision said
     retention began at `maxTS(previous)+1`, which misdescribes the representation.)*
   - *Dual→single:* **the two epochs are tested independently** — there is no ordering between
     their maxima. If `floor > maxTS(previous)` **and** `floor > maxTS(current)`, every version
     represented by either epoch is below the floor, so both arenas may be discarded.
     (`maxTS(current) ≥ maxTS(previous)` does **not** hold in general: at single→dual the new
     `current` is seeded at a version ≤ floor while `previous` may retain a `maxTS` above it, so
     with no subsequent writes `maxTS(current) < maxTS(previous)`. An earlier revision chained that
     inequality and was wrong.)
   - *Single→single* (`floor > maxTS(current)`, the death of a lone epoch) is the same argument
     with one epoch: nothing it holds is at or above the floor, so its arena is discarded outright.
   - *Dual `current` reset* (the optimization, §2.a): if `floor > maxTS(current)` while
     `floor ≤ maxTS(previous)`, everything represented by `current` is below the admission floor and
     may be discarded **independently**. `previous` is untouched and the fresh `current` receives
     every subsequent write; floor monotonicity makes the discarded history permanently irrelevant.

   In every discarding case, **no discarded write has a version at or above the floor**. Any write
   in `[floor, now]` either remains represented by a surviving epoch or was inserted after the
   transition into the live `current`. Since the floor is monotone, discarded history can never
   become admissible again.
2. **One or two live epochs, never three; two arena slots suffice permanently.** Single→dual creates
   the second, dual→dual swaps, dual→single frees both, single→single replaces one with one, and
   the in-place reset of a dead `current` reuses its slot rather than adding one.
3. **Dual-generation formation has a lower bound in allocated work; arena retirement does not.**
   After returning to single mode, no second epoch is formed until a live `current` reaches X.
   **Arena retirement itself is death-driven and intentionally has no X-derived lower bound**: a
   sub-X epoch is discarded whenever the floor proves it wholly obsolete, however often that occurs.
   **X is a bootstrap threshold for forming a second generation, not reclamation hysteresis** —
   repeated single→single retirement reflects distinct bursts of conflict activity separated by
   quiescence, not rotation thrashing. **T3.1 must measure both the unit cost *and* the observed
   rate of discard/re-seed events**, counted separately for single→single, dual→dual and in-place
   `current` reset. The rate is workload-driven and is not a quantity the design seeks to suppress;
   it is needed to integrate the aggregate CPU and latency cost under each workload, which is
   rate × unit cost. *The quantity to watch is not a frequency to be bounded — that was never
   the one that matters — but the frequency still has to be counted to price the mechanism.*
4. **Seeding stays safe, but single→dual needs care.** Dual→dual seeds the reused arena from `maxTS`
   of the just-freed epoch, below the floor by the death condition; dual→single seeds explicitly at
   or below the floor. **At single→dual the newly opened `current` must be seeded at a version
   ≤ floor and must *not* inherit `maxTS(previous)`**, which may still be above the floor — doing so
   would make every untouched key report a version above older readers' `rv` and produce false
   conflicts at scale (§2.c rule 1).

**Quiescence resolves itself, in both modes.** If writes stop after a sealing, `current` stays
empty with `maxTS` equal to its seed, which is ≤ floor, so when `previous` dies both arenas are
freed and the system idles in single mode holding nothing. If writes stop *in* single mode, the
death test frees the lone arena as soon as the floor passes its `maxTS`. An idle resolver therefore
settles on one empty arena, whose repeated "discard" is the no-op of the first implementation note
above. No artificial cycle, and no retention that nothing needs.

**Memory is bounded by the formation threshold plus allocation during the retirement delay — not
by a `2W` history span.** In single mode the current arena may span arbitrarily many versions
before reaching X, but its *allocated size* is bounded approximately by X plus batch overshoot and
allocator slack. Once sealed it becomes freeable after approximately W further versions beyond its
`maxTS`, plus version and batch overshoot — the test is the strict `floor > maxTS` evaluated at a
batch boundary, not an exact W-version deadline; during
that delay the new `current` accumulates workload-dependent allocation `A(Δv)`, `Δv ≤ W`. So the
approximate physical peak is

`M_peak ≲ X + A(Δv ≤ W) + batch overshoot + allocator slack`

and it is not a universal byte bound, because `A(Δv)` depends on the workload. **X is a soft cap on
a live single epoch — subject to batch overshoot and allocator slack — and never a quantity the
epoch must reach: death may reset it first.** Because the death test precedes the formation test, an
epoch that stops receiving conflicts is freed by the advancing floor at whatever size it had; the
system does not carry X bytes waiting for a threshold that quiescence will never deliver. **Temporal
retention can substantially exceed 2W** — an epoch formed slowly holds history far older than the
window. That is harmless **for correctness** (admission is capped at `now − W` regardless) and
provides already-retained history in the direction a longer commit window would need — **but it is
not free**: it consumes arena capacity against X and raises the logical search size until the epoch
dies. *An earlier revision claimed a `2W` retention
bound; it confused the retirement delay with the age of the content.*

### 2.b One caution that survives

**Withdrawing steady-state size-based *sealing* does not withdraw the need for a memory limit.**
The peak above is `X + A(Δv)`, which under a burst is large and is not itself a limit. Backpressure
still
needs a trigger, and it remains **`totalOwnedBytes`** summed across both live arenas — a different
quantity from X, which governs generation formation. Bounding memory below what the window demands
is admission backpressure, not structure; `RESOLVER_STATE_MEMORY_LIMIT`
(`ServerKnobs.cpp:925`, `Resolver.cpp:293–303`) is the existing architectural precedent, though it
bounds the state-transaction buffer, not the conflict set, which has no memory accounting today.


### 2.c Two rules that do not come for free

1. **Seed a reused epoch with a version at or below the floor.** The header's `maxVersion`
   (`ConflictSet.cpp:416–422`) is what every never-written key reads. Seeding a reopened epoch with
   "the current version" — the natural-looking choice, since the epoch starts now — makes every
   untouched key report a version above older readers' `rv` and produces **false conflicts** at
   scale. Under death-driven rotation, `maxTS` of the just-freed epoch is a safe seed by
   construction (§2.a, property 4). The code invites the mistake: `clearConflictSet(cs, v)`
   deliberately uses `SkipList(v)` (`:765–767`), which is right *there* — after losing history you
   want to conservatively pretend everything was written at `v` — and wrong here, where the other
   epoch still holds the real history.
2. **Query both and OR the results.** `∃k ∈ [begin,end): max(cur(k), sealed(k)) > rv` is equivalent
   to `(∃k: cur(k) > rv) ∨ (∃k: sealed(k) > rv)` — the existential distributes over the max — so
   the two are independent `CheckMax` runs with no coordination.

### 2.d The band filter, and what it costs

Each epoch is skippable by **one integer comparison**, because the header's top-level
`getMaxVersion` is already the list's global maximum: `insert` propagates a new version upward
until it reaches a level that already dominates (`:613–618`). So `rv ≥ epoch.maxVersion` skips that
epoch entirely — the same early-out the descent already uses at finer grain (`:680–681`,
`:711–712`), hoisted one level. The *current* epoch is skippable too, when nothing has committed
since the reader's snapshot.

Note the death condition and the band filter are the same test at two timescales: `previous` is
freed exactly when it becomes universally skippable.

**But T2.2 measured this and it does not pay.** Modelled `q = 0.392` against a nominal
comparison-model break-even of `q ≤ 0.056` gives **+31.8 %** expected search cost, and even `q = 0`
saves only 5.3 % because halving N saves one comparison. Death-driven rotation does not improve
that and may worsen it: epoch sizes are set by floor dynamics, so a slowly-advancing floor leaves
*both* epochs large. **Phase B is reopened despite this, on reclamation grounds (§3), not because
the search analysis changed.** §4 and `../benchmarks/measurement-results.md` T2.2 are unrevised.

### 2.e What actually disappears

Gone: `removeBefore` and its `3 × |write ranges| + 10` budget (`:544–576`, `:991`), the
`removalKey` sweep cursor (`:990`, `:992`), the `wasAbove` range-termination rule (`:560–563`) —
unnecessary because each epoch is a self-contained function and no range spans the death boundary —
and, with them, **the entire incremental-reclamation debt that T3.3 measured** (§3). What is *not*
withdrawn is a size threshold for **forming** a generation: X survives for bootstrap and re-entry
(§2.a). Note that it gates *formation only* — reclamation never waits for it, since the death test
runs first in both modes.

Interior deletion is retained: insertion still destroys pre-existing boundaries inside the range
being written, so each epoch stays a canonical piecewise map and logical N tracks live boundaries.
What changes is that unlinked nodes are **spliced and abandoned** rather than freed — they occupy
their arena until the epoch dies. `remove()` therefore loses its second loop, the one that walks
every interior node solely to call `destroy()` (`:590–596`); the splice is 26 pointer writes and
the interior is never touched.

Freeing an epoch is **logically O(1)** — one arena discard. **[measure]** the allocator's real cost
for that discard and for re-initialization; it is not free and the prototype must report it (§6).

*There is no allocator choice here: a per-epoch arena is the design, and keeping
`FastAllocator` with per-unlink freeing is not an alternative.* Keeping per-node freeing keeps the per-node
reclamation debt, which is precisely what T3.3 showed to be the problem; arena-backed epochs are
the point of the design, not one of two options.


### 2.1 Resolved — the obligation and where it came from

*This section recorded an open design obligation through three review rounds. It is closed
by §2; what follows is kept because the reasoning matters for anyone tempted to reintroduce
record-level immutability.*

The current structure keeps N proportional to *distinct live boundaries*, because insertion
destroys every pre-existing boundary strictly inside the range written
(`ConflictSet.cpp:441`, `:579–597`). Writing `[a,z)` over a region holding 1000 boundaries
leaves 2. The level-0 node sequence **is** the step boundaries of a canonical map from key to
last-write-version, and the upper levels are a max-segment index over it (`:252–254`,
`:283–289`). Validation exploits that canonicity: *"is any node in `[begin,end)` above `rv`"*
is a point-max over a step function, and **the predecessor is the single candidate to the
left** — which is why a finger walk suffices (`:663–727`, `:680–681`).

Assume record-level immutability instead — "refreshing a boundary is a new record, never an
in-place version bump" — and interior deletion becomes illegal. Superseded boundaries survive
until their epoch dies, and the query changes character:

- N per window becomes *ranges written*, not *live boundaries*; and
- the value at `k` becomes `max{v : b ≤ k < e}` over **possibly overlapping** records, so a
  read of `[qb, qe)` must consider every record whose interval intersects it — including
  arbitrarily many that start before `qb`. One predecessor becomes many. That is interval
  stabbing, not a step-function lookup, and a skip list keyed on range start does not answer it.

The available exits were a `maxEnd` augmentation in the tower (mutable per-node state again),
splitting records to stay non-overlapping (a mutable canonical map, i.e. the assumption
withdrawn), or rotation-time compaction — which canonicalizes only the epoch being *sealed*,
leaving `current` still needing an online index for whatever accumulated since the last seal.

**None of that is necessary, because immutability was never load-bearing.** What the design
wanted from it was O(1) wholesale death. A per-epoch **arena** delivers that with the map left
canonical: unlinking stops meaning freeing, and the epoch's storage dies in one step (§2.e).
The two-epoch lifetime, the band filter and the pointer-skeleton theorem are all untouched —
none of them ever required immutable records. **Two epochs never caused this problem; one
assumption did.**

What survives from the analysis: the *amplification* question is now a **choice**, not a
consequence. Under the arena variant, unlinked nodes hold their page until the epoch dies, so
physical N still grows with ranges written.

**Measured (T1.2, swept by T2.2).** The overwrite ratio is 0.052 on the tree's workload and
was swept to 0.93; on *throughput* grounds no regime favouring the arena appeared — its
largest advantage over keep-freeing was 2.0 pp, and by then total removable reclamation had
fallen to 4.3 %.

**That comparison does not decide the allocator, because throughput is not the criterion.**
Whole-epoch reclamation requires the arena; keeping per-node freeing keeps the per-node
reclamation debt, which is the whole problem (T3.3, §3). The measured amplification is
therefore an input to the arena's **memory cost** (§2.e), not an argument for a different
allocator.

### 2.2 Consequences of immutability — withdrawn with it


Two obligations followed from record-level immutability. Both lapse with it:

* **Version propagation.** The index maintains per-level maxima by mutating *surviving* nodes
  — `calcVersionForLevel` (`:283–289`), upward propagation in `insert` (`:609–618`), and
  folding a dying node's maxima into its predecessors in `removeBefore` (`:568–569`).
  Immutability would have needed a replacement (per-page maxima, or a mutable index over
  immutable records). Keeping the skip list mutable keeps the mechanism; the third of these
  disappears anyway along with `removeBefore` (§2.e).
* **Epoch purity.** The insert rule writes a node carrying an *older* version when it splits a
  region — `insert(endF, endF.finger[0]->getMaxVersion(0))` (`:438–439`) — so an epoch can
  contain a version below its own start. This is *not* a defect and needs no fix: it is
  exactly the mechanism that makes each epoch a complete map with a "nothing written here"
  default (§2). The band-filter invariant is stated on the epoch's **maximum**, which is all
  the filter reads: *the sealed epoch contains no version above its `maxTS`* — trivially true,
  and the only thing `rv ≥ maxTS` needs.

### Memory doctrine

Storage is allocated per epoch and dies per epoch. What was stated as a doctrine of
*immutable records* is properly a doctrine of *wholesale death*: the epoch is the unit of
reclamation, and no per-object freeing is required on the floor path. Records inside an epoch
may be mutated and unlinked freely — nothing outside the epoch points into it.

**Reclamation is per epoch, not per page.** Nodes within an epoch link to other nodes of that
epoch across page boundaries, so a page whose `max < floor` is not thereby unreferenced —
precisely the dependency the pointer-skeleton theorem describes (§7). Only the whole arena
dies. Per-page maxima, if kept, are a *search filter*; they do not license page-by-page
reclamation. ("The page queue *is* the store, and GC is popping its head while
`head.max < floor`" is withdrawn: the store is the canonical map, and the queue is not it.)

Because commit versions are monotone and batches are processed in order (`Resolver.cpp:324`,
`:537`; `ConflictSet.cpp:986`), an epoch's contents are written in version order, which is what
makes its single `maxTS` both the band filter and the death trigger.


## 3. De-risked sequencing

- **Phase A — dynamic floor on the existing structure.** Still the right first move, and
  the resolver-side diff is genuinely one expression (`Resolver.cpp:359`). **But it is a
  three-component change, not a one-file change** — this is the second significant
  correction.

  The resolver-side expression is only one part of the change. The floor must move
  consistently across **three existing consumers** — the Resolver, the Commit Proxy's
  versioned `keyResolvers` history, and the client-side horizon — and it *consumes* the
  oldest-active-version protocol defined in [`03-floor-tracking.md`](03-floor-tracking.md).
  **Phase A does not introduce a second sensor.** The three consumers are:
  1. **The commit proxy holds a second copy of the same horizon.** `keyResolvers` is pruned
     at `prevVersion − MAX_WRITE…` (`CommitProxyServer.cpp:2104–2110`), and that deque is
     what `getResolversForRange` walks backwards to decide *which* resolvers a read range at
     a given `read_snapshot` is sent to (`:115–150`, loop at `:121–131`). Retain longer at
     the resolver and the extra history is unreachable; retain less and a read range is
     validated against a resolver that dropped it. The two must move together.
  2. **The client holds a third copy.** `ClientKnobs.cpp:235`,
     `fdbclient/include/fdbclient/Knobs.h:136`, consumed at `NativeAPI.cpp:4756` to bound
     the idempotency-id search.
  3. **Retention and admission are the same number today.** `newOldestVersion` decides
     `tooOld` in `addTransaction` (`ConflictSet.cpp:805`) *and* becomes the GC floor
     (`:986`). Making retention dynamic makes admission dynamic — intended, but it means
     Phase A changes client-visible behaviour on its first commit.

  **The floor is demand-driven, not policy-selected.** Phase A consumes the sensor of
  [`03-floor-tracking.md`](03-floor-tracking.md) and applies the Resolver-specific lower
  bound:

  `globalValidationDemand = min(globalOldestClientRV, oldestInFlightCommitRV)`
  `resolverValidationFloor = max(globalValidationDemand, currentVersion − MAX_WRITE_TRANSACTION_LIFE_VERSIONS)`

  **The first term must be `globalValidationDemand`, not `globalOldestClientRV`.** Using the
  client observation alone reintroduces exactly the race `03-floor-tracking.md` §4a closes: after the handoff
  the client may release its registration, and an accepted-but-unvalidated commit would then
  retain nothing.

  The sensor emits **one global observation**; the role separation between storage and conflict
  retention lives in the consumer formulas, not in a second sensor or population (§2,
  `03-floor-tracking.md` §6). The second term is what isolates the commit window.

  **The sensor contract is closed in `03-floor-tracking.md`** and is not restated here. Phase A depends on
  four of its properties — fenced registration, register-before-use, monotone publication, and
  the **commit lifecycle handoff** of `03-floor-tracking.md` §4a (server-side pin installed before the client's
  coverage is released, held until validation completes or the request's generation is fenced
  from influencing a durable decision). The handoff was an unspecified gap
  blocking Phase A; `03-floor-tracking.md` §4a specifies it, and what remains there is choosing its
  linearization mechanism (`03-floor-tracking.md` §8.4).

  Two consequences Phase A must carry:

  - **Empty population.** With no client registrations *and* no accepted-but-unvalidated
    commits, `min(∅) = currentVersion` (`03-floor-tracking.md` §5) and the Resolver floor may advance to
    `currentVersion` — it does not fall back to the fixed-lag bound. Register-before-use is
    what makes that safe. If either source is non-empty, its minimum still pins the floor.
  - **The dividend is negotiation-gated.** In mixed/legacy clusters `03-floor-tracking.md` §6 gives
    `resolverValidationFloor = currentVersion − W_commit` outright, because non-participating
    clients are invisible potential committers. **Phase A therefore ships as a retention no-op
    until participation is negotiated**, and its win is conditional on adoption. That is a
    scheduling fact, not a caveat.

  Given that contract the floor is **monotone**: it may sit at the traditional fixed-lag bound
  or advance when the active population proves older history unnecessary, but it never retreats
  and never extends the v1 commit window. Phase A does not shorten the window by policy or
  under pressure — it advances only on evidence. The existing code already requires
  monotonicity (`if (newOldestVersion > cs->oldestVersion)`, `ConflictSet.cpp:986`), so this
  model needs no change to that invariant.

  **What this does change is the *shape* of reclamation.** The existing GC budget was
  designed for a smoothly advancing fixed-lag floor. A demand-driven floor may **jump
  forward** when the oldest lease closes or expires, making a large population reclaimable
  at once — while the budget stays at `3 × |write ranges| + 10` nodes *examined* per batch
  (`:991`, `:549`), scaled to write traffic rather than to the size of the jump. Because the
  sweep is a key-ordered cursor walk (`:990`, `:992`) and dead nodes are scattered in key
  order, absorbing a W-sized jump costs roughly one full traversal of the live list.
  **Measured (T3.3, 2026-09-04) — Phase A needs two changes here, and this is a blocker.** With a
  monotone floor (a reader pinning `globalValidationDemand = R`, floor `max(R, now − W)` holding at
  *exactly* the same value while `R > now − W`, jumping on release):

  1. **Gating the sweep on the floor advancing lets debt grow through a plateau.** During a plateau
     `newOldestVersion == cs->oldestVersion` — *equality*, not "below" — so the `>` test at
     `ConflictSet.cpp:986` is false and no sweep runs. Measured with 50-batch holds: retained
     population **1 906 280 against 230 207**, an 8.3× gap, with `dead_peak == dead_final`
     exactly, i.e. debt at its maximum on the last sample. Today this never occurs because
     `req.version − W` advances every batch. Carrying explicit `sweepPending` debt, cleared when a
     pass ends having freed nothing, removes it in this schedule.
  2. **The budget is tied to write volume, and that is not fixed by (1).** With writes stopped and
     the floor held, the gated arm makes *zero* progress (examined 0); the `sweepPending` arm gets
     `3 × 0 + 10 = 10` nodes per batch and drains 298 k of debt at 5.5/batch — **~52 600 batches**.
     This is `conflictset-map.md` §3.3's concern, measured. The budget must be driven by
     reclaimable debt or by a time slice.

  Per-batch sweep time stayed ≤ 0.43 ms in every arm, so the *time-spike* condition is met.
  **Bounded catch-up is not established** for either arm — the drain never completes in the run —
  and a better metric would stamp a generation on each floor advance and measure a full sweep lap
  under it, rather than tracking a debt total that new writes keep moving.

  *Two earlier runs of this experiment were wrong in opposite directions — a retreating floor
  schedule, then a control arm that swept unconditionally — and both are retracted.*
  `sweepPending` is **a plausible defensive mechanism whose cost and completion semantics remain
  to be established**, not a settled fix. See `../benchmarks/measurement-results.md` T3.3 and
  `questions.md` Q2. *(This supersedes the
  earlier framing of a "lowered floor lengthening the list": v1 admits no such regime.)*

- **Phase B — the two arena-backed epochs above: reopened by T3.3, for reclamation.** T2.2's
  negative throughput result stands (§4): reclamation is ≤ 10.5 % of Detect and the two-epoch
  search costs more, not less. **T3.3 reopens the structure for an independent reason.** A
  demand-driven floor holds at exactly the same value during a plateau and jumps on release, and
  the current per-node sweep is coupled to both:
  - it runs only when `newOldestVersion > cs->oldestVersion` (`ConflictSet.cpp:986`), so during a
    plateau it does not run at all — measured at **8.3× retained population** (1 906 280 against
    230 207), with `dead_peak == dead_final` exactly;
  - its budget is `3 × |write ranges| + 10`, tied to *current write activity* rather than to the
    reclaimable debt the jump created — so once writes stop, draining ~298 k nodes extrapolates to
    **~52 600 batches**, and the gated arm makes literally zero progress.

  **The structural trade-off, stated honestly.** Sweeping slowly protects latency but retains a
  large reclaimable population and inflates memory occupancy. Sweeping fast enough recovers memory
  sooner but concentrates node traversal and individual destruction into commit batches, and may
  cost latency — especially the tail. **That latency penalty has not been measured**; peak
  per-batch sweep time stayed ≤ 0.43 ms in every arm of T3.3. So the rigorous statement is not
  that incremental reclamation is *unworkable*, but that making it robust **would require designing
  a new controller** — debt-aware, memory-pressure-aware or time-budgeted — to balance memory
  recovery against latency.

  Arena-backed epochs remove that controller from the problem: reclamation cost stops depending on
  how many nodes an epoch holds, how many are reclaimable, subsequent write volume, a sweep cursor,
  or a per-batch deletion budget. **That is the case for prototyping, and it is not a throughput
  case.**

## 4. Cost model — historical target, **not supported by measurement** (and not the reason to build)

These figures are the **historical model**, not a defensible envelope in the current state.
They were held conditional on an unresolved representation; §2.1 settled that, but T0.2 then
found **model and attribution uncertainty, not measurement dispersion** — the premise the chain
rests on is not supported by the tree's own benchmark. The rows below are kept as *provenance*.
T1.1, T1.2 and T2.2 have all **run**. The decomposition bounded the reclamation component; the
crossover sweep then failed to find any regime where it justifies the design, and found the search
term moving against it. These rows are retained as provenance. **The resolver phase has no
throughput case, and reopening the design in §3 does not revise that** — T3.3's argument is about
reclamation behaviour, not speed. T3.1's purpose changes accordingly: it no longer exists to hunt
for a throughput win gated on `q ≤ 0.056`, but to measure whether the **search and memory cost of
two epochs is acceptable in exchange for whole-epoch reclamation**. They are reinstated, re-estimated or withdrawn on its result.

Toy parameters unchanged: 100 tps, 5+5 ranges/txn, W = 5 s, miss = 100 ns ≈ 70 comparisons.

| Design *(historical model)* | ns / entry lifecycle | Structural capacity | Notes |
|---|---|---|---|
| Current (per-entry GC) | ~370 | ~0.57 M txn/s/core | 81 % = cold unlinks — **measured: 9.0 %**, and 0.899 M txn/s (T0.2) |
| Two epochs — arena (§2.e) | ~32 | ~6 M txn/s/core | death = arena drop; no interior walk; physical amplification |
| Two epochs — keep-freeing (§2.e) | **[measure]** | **[measure]** | interior walk retained; death = one linear `SkipList::destroy()`; no splice-and-abandon allocator amplification, but the same generational retention |

| Epoch variant *(historical model)* | Structural μs / txn | End-to-end |
|---|---|---|
| Two independent epochs | 0.17–0.25 | ~1.96× |
| Interlaced OLD/NEW (alt., §7) | 0.23–0.35 | ~1.9× |

> **Measured, 2026-09-04 — T1.1/T1.2 bounded the removable reclamation component.** Decomposing
> `D.MergeWrite` gives: `M.Find` 25.5 % of Detect, the two boundary inserts 11.3 %, the level
> splice 0.2 %, and the **interior walk 0.2 %**. With the sweep at 8.2 %, the two things
> wholesale death removes total **8.4 % of Detect**. Search is **61.5 %** (`D.CheckRead` 36.0 % +
> `M.Find` 25.5 %) and sort 13.6 %: the resolver is search-and-sort bound here, not reclamation
> bound. The **overwrite ratio is 0.052**, which is why the interior walk is negligible and why
> the arena buys only 0.2 pp over keep-freeing (§2.e).
>
> Those figures bound only the **reclamation** term — `1/(1−0.082) = 1.089×`,
> `1/(1−0.084) = 1.092×` — and are **not** a ceiling on the design's performance, because two
> epochs also change lookup cost in both directions. The full comparison is
> `T_epoch − T_current = ΔT_lookup − T_sweep − T_interior + T_rotation + T_band`, and with search
> at 61.5 % the `ΔT_lookup` term dominates.
>
> **T2.2 (2026-09-04) then swept for a crossover and found none.** Raising the overwrite ratio
> from 0.052 to 0.868 *lowers* the removable share from 9.0 % to 0.3 %, because heavy overwrite
> keeps the live set tiny (5 519 entries vs 427 598); wide ranges do the same. **No monotonic
> window effect was discernible** across 50 / 200 / 500 versions at constant work (9.0 / 8.3 /
> 8.6 %), which refutes "gains grow with the window" over the interval tested without proving
> independence of W. Best case for reclamation is 10.5 %, at the **sparsest, lowest-overwrite
> setting tested**.
>
> And on the search side, recent-skewed reads give modelled **q = 0.392** against a
> **comparison-model break-even of `q ≤ 0.056`** — from `(1+q)·log₂(N/2) ≤ log₂(N)`, which assumes
> two equal halves, cost proportional to `log₂ N`, and ignores the one→two→one cycle, unequal
> epoch sizes, range traversal and cache behaviour. On that model expected search cost is
> **+31.8 %**, and even `q = 0` saves only 5.3 %. **The `ΔT_lookup` term that was Phase B's
> remaining upside points the wrong way.** See `../benchmarks/measurement-results.md` T1.1/T1.2 and T2.2.

> **Measured, 2026-09-04 — the model's premise is not supported by the tree's own benchmark.**
> `fdbserver -r skiplisttest` gives `D.RemoveBefore` = **9.0 %** of detect time, not 81 %;
> `D.CheckRead` (validation) is the largest phase at 38.6 %; and detect-only throughput is
> **0.899 M txn/s**, ~1.6× the 0.57 M baseline the model assumes. Removing the sweep entirely
> would cap the structural gain near 1.1× *on that workload*. Two limitations keep this from
> being decisive: the harness models a **50-version window** (`ConflictSet.cpp:1190`), and the
> insert-time interior destroy is inside `D.MergeWrite` (36.7 %), indistinguishable — which is
> exactly what T1.1 separates. See `../benchmarks/measurement-results.md` T0.2 and `../benchmarks/measurement-plan.md`.

**Three caveats.**

1. **Logical and physical N diverge under the arena.** Interior deletion is retained, so logical N
   stays at live canonical boundaries and the `log N` search term is unaffected. *Physical*
   allocation grows with every arena allocation regardless of overwrite — memory, not search. This
   is no longer an allocator choice (§2.1): the arena is the design.
2. **The 81 % attribution is to the wrong denominator.** It should cover *both* unlink paths
   (§1). The floor sweep disappears, and so does the insert-time interior walk: `remove()` splices
   and abandons instead of walking the interior to free it (§2.e). The historical split between the
   two paths explains how much CPU is removed; it no longer chooses an allocator.
3. **The current structure is *engineered as if* memory latency dominated** — the prefetching,
   the 16-way interleaving and the "+25 %" comment (`ConflictSet.cpp:554–558`) are artefacts of
   fighting misses. Whether the measured resolver workload actually *is* miss-bound is **not
   established**: T0.2 shows `D.CheckRead` at 38.6 % but does not separate comparisons, branches
   and misses, and T0.3's 3.6× `find` vs `find_sorted` gap is a `VersionedMap` result on the
   storage side, not a `ConflictSet` one. It needs hardware-counter profiling (T4.1).
   "Search costs barely separate designs; miss-class operations separate them" is therefore a
   **hypothesis under test**, already downgraded in §2.d and made more critical by T0.2 — not a
   conclusion that stands.

End-to-end by Amdahl (overhead 1–2.5 μs/txn): ~2× (1.6–2.4×) — **historical, suspended with
the tables above**: it is derived from the same premise T0.2 did not support, so it is
provenance, not a current expectation. The qualitative dividend stands on its own, though
conditional on there being a throughput gain at all —
if the throughput gain permits a deployment to use fewer resolvers, fewer transactions span
multiple resolvers and the incidence of conservative multi-resolver phantom conflicts falls
— a potential operational benefit, not an automatic consequence of the structure. It has
**extra support**: the proxy
has a single-resolver fast path avoiding the fan-in (`CommitProxyServer.cpp:970–975`,
`:1008–1012`), and the resolver skips its `iopsSample` key sampling entirely when
`resolverCount == 1` (`Resolver.cpp:365–372`, `:823–825`).

## 5. Wire ladder (follow-up stage) — rescoped

Keys must travel (confirmed, §1), but cheaper. **The ladder's first rung is more expensive
than it appears.**

1. **Delta/prefix encoding proxy→resolver — two variants, not one.** The batch is **not
   sorted when it leaves the proxy**. Per *transaction* the ranges are sorted and coalesced,
   because `ReadYourWritesTransaction` flushes them by iterating a
   `CoalescedKeyRefRangeMap` in key order (`ReadYourWrites.h:225`,
   `ReadYourWrites.cpp:1388–1393`, `:1911–1990`). But the proxy concatenates transactions
   (`CommitProxyServer.cpp:154–174`) and the raw NativeAPI path does not merge at all
   (`NativeAPI.cpp:3961`, and the explicit comment at `:4701`). The resolver is the first
   component that sorts, and over *points*, not ranges (`ConflictSet.cpp:953`).
   - **Cross-transaction delta encoding** is no longer a free first PR: it requires sorting
     the concatenated batch, or a k-way merge of its per-transaction sorted runs, on the
     commit-critical path — and it must not disturb the per-transaction grouping the reply
     protocol depends on (`CommitProxyServer.cpp:1157–1165` indexes replies by per-resolver
     transaction order).
   - **Per-transaction encoding** reuses the runs that are already sorted and resets the
     delta at each run boundary. No global merge, no reordering, and the grouping is
     untouched; the price is lower compression, plus a fallback or a local normalization
     pass for raw NativeAPI transactions, which are not sorted even within a transaction.
   Measure both before choosing the wire format. The weaker variant is still a credible
   first PR.
2. **zstd with a trained dictionary.**
3. Rejected for v1: stateful interning.

In-structure: discriminator prefixes inline in index nodes; full keys in page records.

## 6. Benchmark plan — extended

*T1.1, T1.2, T2.2 and T3.3 have run; items 7–10 are done. The rest is now the **prototype's**
measurement list (§3), not a plan conditional on a throughput criterion.*

**Prototype scope.** Two canonical SkipLists; one arena per epoch; inserts into `current` only;
both queried with OR and a `maxTS` band filter; splice-and-abandon for interior nodes;
death-driven rotation (§2.a); whole-arena release of `previous`. **Do not** design an adaptive
threshold, sophisticated backpressure, index interleaving or a sweep controller — the point is a
direct comparison:

```
incremental per-node GC : memory recovery vs commit-latency trade-off
two arena-backed epochs : lookup and retained-memory cost vs whole-epoch reclamation
```

**Prototype must measure:** validation cost consulting zero, one or two epochs; the real
distribution of how many queries reach both; throughput *and* latency including the tail; bytes
allocated per arena and total peak memory; amplification from abandoned nodes; **the unit cost of
discarding and re-seeding an arena, including the empty case, *and* per-event-type counters for the
observed rate** — single→single retirement, dual→dual rotation, in-place `current` reset — since
sub-X retirement is expected and unbounded in frequency by design, so only rate × unit cost gives
the aggregate (§2.e, §2.b caution (i), §2.a.1 property 3); and behaviour across floor plateaus and
jumps, **including formation at X during a plateau, when no death test can fire**.

The six numbers remain the right ones to collect
(NEW-only lookup · OLD lookup · OLD+NEW as two searches · OLD+NEW as one interleaved walk ·
insert · **q**), yielding I, S and `q* = I/S`. Added:

7. **Split the two unlink paths** in the existing counters — floor sweep vs insert-time
   interior deletion (`ConflictSet.cpp:47–49`, `:952–994`). Hours, not days; fixes §4's
   caveat 2.
8. **Footprint of the conflict set — three *live* numbers, not one, and not cumulative ones.**
   A live-population counter alone cannot price the arena's memory cost (§2.b). Thread through
   `insert`/`remove`/`removeBefore` and publish as `specialCounter`s (`Resolver.cpp:216–218`),
   **per live epoch**:
   - **`reachableBytes`** — nodes reachable now. Drives logical and search cost.
   - **`ownedBytes`** — bytes the currently live arenas own or hold reserved. This is the
     physical footprint. *It is no longer a steady-state seal trigger — dual-mode rotation is
     death-driven (§2.a) — but it serves two other roles: **X**, the bootstrap/re-entry threshold
     that forms a generation in single-epoch mode, and, summed across both live arenas as
     `totalOwnedBytes`, the **backpressure** trigger (§2.b).*
   - **`unreachableOwnedBytes`** — owned but no longer reachable. This is the amplification
     attributable to splice-and-abandon.

   With `ownedBytes = reachableBytes + unreachableOwnedBytes + allocatorSlack`.

   Two corrections to an earlier draft of this item. **(i)** "cumulatively allocated minus
   reachable" is *not* the amplification once rotation has happened, because a cumulative
   counter includes arenas already freed; the quantity must be scoped to live arenas.
   **(ii)** The trigger must **not** watch `unreachableOwnedBytes` alone: a workload that
   continuously introduces *distinct* keys keeps almost everything reachable, so that number
   stays small while the arena grows without bound. Watching `ownedBytes` covers both extremes
   — many distinct keys (reachable growth) and many overwrites (unreachable growth).

   Cumulative historical counters — `totalAllocatedBytes`, `totalUnlinkedBytes`,
   `totalFreedAtRotationBytes` — are worth adding for **rate profiling**, but they describe
   neither footprint nor a trigger.

   The arena's memory cost then reads off ratios: `unreachableOwnedBytes / ownedBytes`, unlinked
   bytes per second, and CPU saved per byte retained until rotation.
9. **Overwrite ratio** — boundaries destroyed by insert-time deletion per boundary inserted;
   the same instrument as (7). It now prices the **arena's memory cost** rather than choosing an
   allocator (that choice is withdrawn, §2.1): a high ratio means a large interior walk is skipped
   but correspondingly large physical amplification is carried until the epoch dies.
10. **GC sweep headroom under floor jumps** — reclaimable nodes exposed when the oldest
    active lease closes or expires, catch-up time under the existing examination budget, and
    peak dead-state retention. Not a "lowered floor lengthening the list" (v1 admits no such
    regime) but a burst the fixed budget must absorb. Phase A prerequisite
    (`questions.md` Q2).

## 7. Alternatives considered

- **Global pointer-skeleton index over pages.** Killed by the theorem: *a linked ordered
  index cannot die by the floor.* **Confirmed by the code**: `removeBefore` may delete a
  node only when both it *and its predecessor* are below the floor —
  `if (isAbove || wasAbove) keep` (`ConflictSet.cpp:560–563`). A node adjacent to a live one
  survives purely to terminate the live range. The theorem is a shipped constraint, not a
  conjecture.
- **Generational index rebuild.** Superseded by epochs, briefly reconsidered as a
  rotation-time compaction option, then **withdrawn**: canonical epochs leave nothing to
  compact.
- **Redundant next-pointers + era heal-sweep.**
- **Asymmetric interlaced OLD/NEW generations**: cross-linking is worthwhile
  iff measured cross-generation fraction exceeds `q* = I/S`, which lands near 1.
- **Per-epoch ART/radix instead of skip lists.** "FDB's highly tuned SkipList"
  is confirmed as an accurate characterization of the baseline (§1).

**Scope note.** These results are about node lifetime, navigation and filtering — and that is
now sufficient, because the representation inside each epoch is the existing canonical map
(§2.1). The **generational rebuild** alternative is withdrawn: with the map kept
canonical there is nothing to compact.

## 8. Open questions

**Answered by the code:**
*Does the resolver or proxy sort batch ranges?* The resolver, inside `detectConflicts`
(`ConflictSet.cpp:953`); the proxy does not (§5). *What does the proxy send per resolver?*
One `CommitTransactionRef` per (resolver, transaction) with only that resolver's ranges
(`CommitProxyServer.cpp:104–174`); metadata mutations to resolver 0, system keys to all
when `PROXY_USE_RESOLVER_PRIVATE_MUTATIONS` (`:141–146`). *How is `removeBefore`
amortized, over which allocator?* `3 × |write ranges| + 10` examined nodes per batch from a
persisted cursor (`:990–992`); `FastAllocator<64>`/`<128>`/`new char[]` by node size
(`:262–271`, `:294–302`). *Intra-batch invariants to inherit?* `checkIntraBatchConflicts`
(`:913–938`) over a word bitset (`:849–911`), in transaction order, accepted writers
marking their ranges for later transactions (`:934–936`).

**Still open, for the community:**

- **Phantom writes of rejected multi-resolver transactions**, now with evidence:
  `transactionConflictStatus` is set only by *local* checks (`:960`, `:964`), so a
  transaction rejected at resolver B still has its writes inserted at resolver A
  (`:1038`), while the proxy aborts it via the `min` across resolvers
  (`CommitProxyServer.cpp:1157–1163`). The acceptability argument depends on the short
  window — what changes when the write window grows?
- **Is the direction of conservatism stated anywhere?** The only statement in the tree is
  one comment: *"Determine which transactions actually committed (conservatively)"*
  (`CommitProxyServer.cpp:1148`). `design/` has no resolver document. Proposal: contribute
  one — it costs half a day and gives this redesign a reviewed contract to preserve.
- **Resolver key-range reassignment.** The receiving resolver has no history for a moved
  range; the proxy compensates via the versioned `keyResolvers` deque
  (`CommitProxyServer.cpp:918–922`, `:115–150`). Is that argument sufficient, and what does
  it become under a dynamic or longer floor?
- **New:** does the overwrite-ratio measurement in §6 put any real workload in a regime where
  the arena's physical amplification dominates the interior-destruction and epoch-reclamation
  work it saves?
