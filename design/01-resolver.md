# Resolver Phase — Generational Paged Conflict Set

*Version: **2.0** · supersedes 1.0 — this file's initial commit, frozen pre-code-contact.*
*Status: **correctness closed; implementation prototype justified by reclamation behaviour.**
T1.1/T1.2/T2.2 found no throughput case for two generational SkipLists, and **those results
stand**. T3.3 supplies an independent reason to prototype them: a demand-driven floor exposes
reclaimable state in bursts, while the current per-node sweep is coupled both to floor advancement
and to subsequent write volume. Making that mechanism robust would require a debt- or
time-controlled GC whose reclamation rate trades commit latency against retained memory.
Arena-backed epochs replace that incremental debt with whole-epoch retirement. What 1.0 marked
"to verify" is now verified; citations are `file:line`. Reopening criteria remain explicit
and measurable (§7). Remaining unknowns are marked **[measure]** with plans in
`questions.md`.*

**Changes in 2.0 are listed in §9.**

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

**The current structure — verified, and richer than 1.0 assumed.** A custom
batch-optimized skip list: 16-way interleaved finger construction (`:490–542`) and
validation (`:446–488`), insertion in stripes of 16 (`:1015–1025`), `_mm_prefetch` in the
finger walk (`:355–361`), vtune annotations still in the source (`:467`, `:494`, `:528`).
`removeBefore(V)`'s `V` really is the window constant in disguise:
`newOldestVersion = req.version − MAX_WRITE_TRANSACTION_LIFE_VERSIONS`
(`Resolver.cpp:359` → `ConflictSet.cpp:986–991`) — a *sliding* floor with a constant lag,
and `cs->oldestVersion` is already a variable (`:754`), which makes Phase A a smaller
structural diff than 1.0 assumed (§3).

**Correction — there are two per-entry unlink paths, not one.** 1.0's model counts only the
floor sweep. *An earlier draft guessed the commit-path one might be larger; T1.1 measured it at
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

**Gap statement (2.0):** *FDB already prunes conflict history by a sliding, floor-driven
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

> **This supersedes 2.0's "open indexed representation" (§2.1, R11/R17).** That obligation
> came from assuming *record-level immutability* — "refreshing a boundary is a new record" —
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

*2.0 withdrew size-based sealing too broadly. Size does not drive steady-state dual-epoch
rotation, but a **bootstrap/re-entry threshold X remains necessary** — it decides whether a *live*
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
- *A dead `current` in dual mode may be reset in place.* R69 established there is no ordering
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
   rate × unit cost. *This restates the caution 2.0 recorded here rather than retiring it: the
   quantity it named — a frequency to be bounded — was never the one that matters, but the
   frequency still has to be counted to price the mechanism.*
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

*2.0 presented an allocator choice here — a per-epoch arena versus keeping `FastAllocator` and
freeing on unlink. That choice is withdrawn.* Keeping per-node freeing keeps the per-node
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

**Superseded reasoning, kept for the record.** T1.2 and T2.2 favoured keep-freeing when *throughput*
was the criterion — the arena's largest advantage was 2.0 pp against 4.3 % total removable
reclamation. **T3.3 changed the criterion.** Whole-epoch reclamation requires the arena; keeping
per-node freeing keeps the per-node reclamation debt, which is the whole problem. The measured
amplification is now an input to the arena's **memory cost**, not an allocator choice (§2.e, R66).
**Measured (T1.2, swept by T2.2).** The overwrite ratio is 0.052 on the tree's workload and was
swept to 0.93; **no regime favouring the arena appeared** — its largest advantage over
keep-freeing was 2.0 pp, and by then total removable reclamation had fallen to 4.3 %.
*That was the conclusion under the former throughput-only criterion and is superseded by R66.*

### 2.2 Consequences of immutability — withdrawn with it

2.0 recorded two obligations that followed from record-level immutability. Both lapse:

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

### Memory doctrine *(revised)*

Storage is allocated per epoch and dies per epoch. What 2.0 stated as a doctrine of
*immutable records* is properly a doctrine of *wholesale death*: the epoch is the unit of
reclamation, and no per-object freeing is required on the floor path. Records inside an epoch
may be mutated and unlinked freely — nothing outside the epoch points into it.

**Reclamation is per epoch, not per page.** Nodes within an epoch link to other nodes of that
epoch across page boundaries, so a page whose `max < floor` is not thereby unreferenced —
precisely the dependency the pointer-skeleton theorem describes (§7). Only the whole arena
dies. Per-page maxima, if kept, are a *search filter*; they do not license page-by-page
reclamation. (1.0's "the page queue *is* the store, and GC is popping its head while
`head.max < floor`" stays withdrawn.)

Because commit versions are monotone and batches are processed in order (`Resolver.cpp:324`,
`:537`; `ConflictSet.cpp:986`), an epoch's contents are written in version order, which is what
makes its single `maxTS` both the band filter and the death trigger.


## 3. De-risked sequencing

- **Phase A — dynamic floor on the existing structure.** Still the right first move, and
  the resolver-side diff is genuinely one expression (`Resolver.cpp:359`). **But it is a
  three-component change, not a one-file change** — this is the second significant
  correction in 2.0.

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
  from influencing a durable decision). 2.0 recorded the handoff as an unspecified gap
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
     `ConflictSet.cpp:1016` is false and no sweep runs. Measured with 50-batch holds: retained
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
  - it runs only when `newOldestVersion > cs->oldestVersion` (`ConflictSet.cpp:1016`), so during a
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
2.0 held them conditional on an unresolved representation; §2.1 settled that, but T0.2 then
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

**Three caveats new in 2.0.**

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
than 1.0 assumed.**

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
2. **zstd with a trained dictionary** *(unchanged)*.
3. Rejected for v1: stateful interning *(unchanged)*.

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

The six numbers from 1.0 remain the right ones to collect
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

## 7. Alternatives considered *(unchanged, with one confirmation)*

- **Global pointer-skeleton index over pages.** Killed by the theorem: *a linked ordered
  index cannot die by the floor.* **Confirmed by the code**: `removeBefore` may delete a
  node only when both it *and its predecessor* are below the floor —
  `if (isAbove || wasAbove) keep` (`ConflictSet.cpp:560–563`). A node adjacent to a live one
  survives purely to terminate the live range. The theorem is a shipped constraint, not a
  conjecture.
- **Generational index rebuild.** Superseded by epochs in 1.0, reconsidered during 2.0 as a
  rotation-time compaction option, then **withdrawn by R28**: canonical epochs leave nothing to
  compact.
- **Redundant next-pointers + era heal-sweep** *(unchanged)*.
- **Asymmetric interlaced OLD/NEW generations** *(unchanged)*: cross-linking is worthwhile
  iff measured cross-generation fraction exceeds `q* = I/S`, which lands near 1.
- **Per-epoch ART/radix instead of skip lists** *(unchanged)*. "FDB's highly tuned SkipList"
  is confirmed as an accurate characterization of the baseline (§1).

**Scope note.** These results are about node lifetime, navigation and filtering — and that is
now sufficient, because the representation inside each epoch is the existing canonical map
(§2.1). The **generational rebuild** alternative is withdrawn (R28): with the map kept
canonical there is nothing to compact.

## 8. Open questions

**Answered by the code** (were 1.0 §8 bullet 1):
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

- **Phantom writes of rejected multi-resolver transactions** *(1.0, now with evidence)*:
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

## 9. Changelog 1.0 → 2.0

| # | Change | Basis |
|---|---|---|
| R1 | §2.1 added: epochs cannot delete interior boundaries ⇒ N becomes *ranges written*; three mitigations | crosscheck X3 |
| R2 | §1: a **second** per-entry unlink path identified (insert-time `remove`); 81 % attribution reopened | X3, A1 |
| R3 | §3: Phase A rescoped as **three coordinated consumers of the existing `03-floor-tracking.md` sensor** — Resolver retention/admission, Commit Proxy `keyResolvers` history, client-side horizon — plus GC burst headroom as a prerequisite | X2, A6 |
| R4 | §5: the proxy batch is not globally sorted, so the initial cross-transaction proposal requires sorting or merging runs | X5 |
| R5 | §2.2: immutability must replace in-place version propagation; epoch purity restated | A5, A4 |
| R6 | §1 gap statement rewritten — the floor is already a variable; the *lag* is the constant | C6 |
| R7 | §6: four measurements added (unlink split, footprint counter, overwrite ratio, sweep headroom) | questions.md |
| R8 | §7: generational rebuild partially reinstated as rotation-time compaction | R1 |
| R9 | §8: three of four open questions closed with citations; one new one added | C14 |
| R10 | §2, §4, §7: rotation feasibility, band filter, the pointer-skeleton theorem and "highly tuned baseline" all confirmed against code | C5, C8, C9, C10 |

Applied after review of the 2.0 draft:

| # | Change | Basis |
|---|---|---|
| R11 | §2.1 escalated from a cost-model caveat to a **representation change**: canonical piecewise last-write-version map → accumulated interval set, with the two obligations Phase B must now discharge | review, verified at `ConflictSet.cpp:441`, `:579–597` |
| R12 | §5 rung 1 subsequently split into cross-transaction and per-transaction variants; the latter remains a credible early PR without global reordering | review |
| R13 | §7 scope note added — the band filter and the skeleton theorem do not close §2.1 | review |

Second review round:

| # | Change | Basis |
|---|---|---|
| R14 | §2: the "degenerate regime" of accumulating sealed epochs **removed**; replaced by the **two-epoch bound**, which v1 closes because the commit window is unchanged and read-only transactions never reach a resolver | review, verified at `NativeAPI.cpp:4798–4805` |
| R15 | §2 retitled — lifetime structure closed, indexed representation open; "Final design" was too definitive while §2.1 is open | review |
| R16 | Memory doctrine: per-page reclamation **withdrawn**. Intra-epoch links may cross pages, so `head.max < floor` does not prove a page unreferenced — the pointer-skeleton dependency. Only whole-arena death is sound; per-page maxima remain a search filter | review, §7 |
| R17 | §2.1 split into a **correctness obligation** (exact overlap/max-version, including intervals starting before the query) and **amplification mitigations** that apply only once it is discharged; rotation-time compaction demoted to a hypothesis needing a specified overlay algorithm, memory bound and schedule | review |
| R18 | §4 retitled *Conditional cost model*; the figures are the target envelope of the original two-epoch model, not predictions of a specified Phase B | review |
| R19 | Three claims narrowed: wire scope (metadata mutations do travel that path), "a hash cannot serve this workload" → a hash *alone* cannot do exact range overlap, and the fewer-resolvers dividend made conditional on a deployment actually reducing resolver count | review |

Third review round — naming the sensor:

| # | Change | Basis |
|---|---|---|
| R20 | §3: the floor made **demand-driven and monotone** through the existing oldest-active-version sensor; Phase A advances only on evidence and never shortens the window by policy. The precise global-observation / consumer-formula split was subsequently corrected in R24a | review; monotonicity already required at `ConflictSet.cpp:986` |
| R21 | §3, §6: the GC measurement is re-aimed from "a lowered floor lengthens the list" (a regime v1 does not admit) to **absorbing a forward jump** when the oldest lease closes or expires | review |
| R22 | §2: storage and resolver retention separated explicitly — long read leases must not pin conflict history. Bound sharpened: what must be covered is every transaction that *queries* history, and `tooOld` requires a non-empty read conflict set (`ConflictSet.cpp:805`). Stated here as two populations; corrected in R24a to one global observation with two consumer formulas | review |
| R23 | Three residues fixed: "splice the tower" made conditional on §2.1; "death = page pop" → **arena drop**; eliminating insert-time deletion identified as a saving of the *accumulative* representation only | review |
| R24 | §3: Phase A bound to the **verified** `03-floor-tracking.md` sensor contract — fenced registration (`03` §3–§5), register-before-use (§3, §2), monotone publication (§2, §7). Provisional "unverified obligation" block removed | `03-floor-tracking.md` read |
| R24a | §3: **mechanism corrected** — `03` emits one global `globalOldestRV` and the resolver floor is `max(globalOldestRV, now − W_commit)` (§6). The role split of §2 is a property of the two *formulas*, not of two sensors; `min(∅) = currentVersion` (§5) | `03` §5–§6 |
| R24b | §3: the efficiency dividend is **negotiation-gated** — `03` §6 gives `now − W_commit` for mixed/legacy clusters, so Phase A is a retention no-op until participation is negotiated | `03` §6 |
| R24c | §3: the fourth assumed property, **lifecycle handoff at commit submission, is absent from `03`**, and §4's `commit → unregister` phrasing leans to the unsafe reading. Recorded as an action on `03`, blocking Phase A | `03` §2, §4, §7 |
| R24d | §3: proxy-generation barrier added as a Phase A dependency | `03` §7 |
| R25 | §2.1: rotation-time reconstruction can canonicalize only the **sealed** epoch; `current` still needs an online overlap/max-version index. "both reclamation paths" → "both deletion paths" | review |
| R27 | *(seal-on-size superseded by R66)* §2 **rewritten**: rotation is **demand-driven** — seal on size, free the sealed epoch when `floor > maxTS`, reuse its storage. The free condition *is* the retention guarantee, so 2.0's "do not rotate before the current epoch spans W" dissolves. Live epochs oscillate between one and two | review |
| R28 | §2.1 **resolved and the obligation withdrawn.** The overlap/max-version problem followed from *record-level immutability*, not from two epochs; two canonical maps are two step-function queries. A per-epoch arena delivers wholesale death with the map left canonical, so no new index is needed. §2.2's consequences lapse with it, and §7's rotation-time compaction is withdrawn again | review |
| R29 | §2.b: seal criterion analysed — **S can only make epochs larger, never smaller**, because a sealed epoch cannot die sooner than the floor allows (≈W). `S ≥ W × rate` or the new epoch overshoots; S is a soft threshold; bounding memory below that is admission backpressure, not structure | review |
| R30 | §2.c: two rules that do not come free — seed a reopened epoch at or below the floor (seeding it at "now" produces false conflicts at scale, and `clearConflictSet`'s `SkipList(v)` invites exactly that mistake), and OR the two queries (the existential distributes over the max) | review; `ConflictSet.cpp:416–422`, `:765–767` |
| R31 | §2.d: the band filter costs **one integer per epoch** (the header's top-level max is already the global max, `:613–618`), the current epoch is skippable too, and the two search costs move in opposition. Stated plainly that validation is not faster than today; the gain is the vanished sweep | review |
| R32 | §2.e: what disappears (`removeBefore`, its budget, `removalKey`, `wasAbove`) and the allocator choice — arena also removes `remove()`'s interior walk (`:590–596`); keep-freeing avoids amplification and makes epoch death one `SkipList::destroy()` pass (`:324–330`). Price of wholesale death recorded (*the W..2W figure given there was withdrawn by R35*) | review |
| R33 | §3, §4, §7, status: Phase B **no longer gated on a representation question**; the cost model is conditional on measurement rather than on structure | review |
| R34 | §2: **two-epoch bound restated operationally** — it never depended on an epoch spanning W. `current` is sealed only when no sealed epoch exists, so a third is never needed; W bounds how long the sealed one takes to die, not the epoch count. The "every admissible rv falls in previous ∪ current" argument is no longer needed | review |
| R35 | §2.e: **"retention oscillates W..2W" withdrawn** — under size-based sealing a light epoch may span hours before sealing, so temporal retention can far exceed 2W. The bounded quantity is the *number* of live epochs, not their span | review |
| R36 | §2.b: the S analysis **separated into `S_logical`, `S_physical` and `A(delay)`** (renamed `A(Δv)` by R45). `W × write-rate` is not a general bound on either; "memory sits between S and 2S" is withdrawn — a burst during the wait can exceed it. S is a soft trigger, not a memory bound, in either variant | review |
| R38 | §3 and the closure block: the Phase A floor **must use `globalValidationDemand`**, not `globalOldestClientRV` — using the client observation alone reintroduces the race `03-floor-tracking.md` §4a closes, since the client may release after the handoff | `03-floor-tracking.md` §4a, §6 |
| R39 | §2: the two-epoch bound is **independent of W**. Extending the writing-transaction window would not invalidate it; it would enlarge the rotation stall and `A(delay)`, making extra epochs a capacity choice rather than a correctness one. Closing sentence restated accordingly | review |
| R40 | §4: the cost table **split by allocator** — "death = arena drop" described only one variant, and the ~32 ns target belongs to it; keep-freeing needs its own measured envelope | review; §2.e |
| R42 | §2.d: **"`N_A + N_B` is one window's worth" withdrawn** — a leftover from the temporal-rotation model R35 retired. A canonical map drops overwrites but keeps one boundary per region touched for the epoch's whole life, so the sum is unbounded by W's writes. Lookup neutrality is downgraded from claim to measurement | review; R35 |
| R44 | §2.b: `S_physical` corrected — it tracks **allocated versus unlinked bytes**, not write count; today's `insert` always allocates (`ConflictSet.cpp:442`, `:599–619`), so "may allocate nothing" describes an *added* in-place update available only once nodes are arena-owned | review; `ConflictSet.cpp:442`, `:599–619` |
| R45 | §2.b: `A(delay)` → **`A(Δv)`** — the death bound `currentVersion > maxTS + W` is in **versions**, not wall-clock; wall-clock follows only under normal version advancement | review; `ServerKnobs.cpp:164–166` |
| R47 | §2.b: the light-load "no extra cost" case **split by variant** — true for keep-freeing, false for the arena, where today's allocate-and-replace insert grows `S_physical` while `S_logical` stays flat. **Arena sealing must therefore trigger on physical bytes**, or a rewrite-heavy workload never seals and grows without bound | review; R44 |
| R48 | §6.8: the footprint instrument extended to **three numbers**, because a live-population counter cannot choose the allocator | review |
| R50 | §2.b: **sealing and backpressure separated** — the seal trigger watches `ownedBytes(current)`, but the memory budget must watch `totalOwnedBytes` across both live arenas, or a large sealed epoch plus an under-threshold current one exceeds the budget with nothing firing | review |
| R75 | **R74 finished.** Three sentences it left standing are replaced: the dual→dual argument now **folds the intervening-reset case into the main clause** (writes removed by a `current` reset were already proved below an earlier floor) instead of appending it; the closing sentence, which said "no *surviving* write had version ≥ floor" — the wrong side of the discard — becomes **"no *discarded* write has a version at or above the floor"**, with `[floor, now]` covered either by a surviving epoch or by post-transition insertion; and §2's opening "Two epochs… one is current, one is sealed" now reads **at most two live epochs backed by two reusable arena slots**, naming what is live in each mode | review |
| R74 | **Proof completed for every permitted operation.** §2.a.1 property 1 now covers **four transitions plus the in-place `current` reset**, not "three": single→dual (discards nothing), dual→dual, dual→single, single→single, and the local reset argument. The dual→dual case is restated **without claiming where retention "begins"** — a canonical epoch holds a piecewise function with a seed and copied-forward boundary values, not a contiguous version interval, so the earlier `maxTS(previous)+1` phrasing misdescribed the representation; the replacement argues from `floor > maxTS(previous)` and from where post-sealing writes went, and survives a prior in-place reset by floor monotonicity. Also: "exactly two epochs" → **at most two live epochs backed by two reusable arena slots**, separating the structural bound from the active count | review |
| R73 | **Two triggers separated, and the rate restored to the measurement set.** Death checks fire on **floor advancement**; the X formation check fires on **allocation growth** in single mode — so a `current` can reach X and form a pair *during a floor plateau*, when no death test can fire. When both are observed at one serialized batch boundary, **death precedes formation**. R72 also went too far in saying T3.1 prices only the unit cost: the aggregate is **rate × unit cost**, so per-event-type counters (single→single, dual→dual, in-place reset) are required — not to decide on hysteresis, but to integrate CPU and latency under each workload. Plus: the sealed-epoch retirement delay restated as *approximately W further versions plus version/batch overshoot*, since the test is the strict `floor > maxTS` evaluated at a batch boundary | review |
| R72 | **X restated: bootstrap, not hysteresis.** R71 read X as general reclamation hysteresis; its function is only to decide whether a *live* single epoch has earned a second generation. Death always prevails over X, and a sub-X epoch may be retired as often as the workload dictates — that is the intended privatization of cost (a short burst leaves a small arena, freed whole), not thrashing; holding it to X would make bootstrapping a barrier to reclamation. T3.1 accordingly prices the *unit* cost of discard/re-seed, not a frequency to suppress. The R71 empty-epoch guard and dual-mode `current` reset are **demoted from the state machine to implementation notes**: `discard(empty)` must merely be a cheap no-op, and resetting an obsolete `current` is a memory optimization — the only generational transition stays conditioned on the death of `previous` | review |
| R71 | **Three consequences of R70.** (a) The hysteresis claim is narrowed: **X bounds generation-formation frequency, not arena-retirement frequency** — single→single may retire a sub-X arena every few batches when the floor tracks `now`; T3.1 must measure whether that is cheap enough. (b) **Empty ≠ dead**: an empty epoch satisfies the death test at every floor advance but has nothing to reclaim, so it is kept and re-seeded, not discarded in a cycle. (c) **`current` may die while `previous` lives** — R69's absence of ordering makes this reachable, and with no cross-epoch pointers the dead `current` is reset in place, without a rotation and without a third epoch. Plus: X restated as a *soft* cap | review |
| R70 | **§2.a state machine closed for quiescence.** The mode check fires on **every effective floor advance** and tests **death before X** in both modes: in single mode `floor > maxTS(current)` frees the lone arena and re-seeds a fresh empty `current`, *staying* in single mode — reaching X is not a precondition for reclamation. X only forms a generation when the epoch is still alive. Consequences: a fourth transition (single→single) in §2.a.1, and X is now a **cap** on what a single epoch holds rather than a size it must reach | review |
| R69 | Four corrections: (a) **`maxTS(current) ≥ maxTS(previous)` is false** — at single→dual the new `current` is seeded ≤ floor while `previous` may sit above it, so dual→single must test both epochs **independently**; (b) single→dual seeding must not inherit `maxTS(previous)`; (c) old history is harmless for correctness but **not free** — it consumes X and raises logical search size; (d) remaining "diverges"/"indefinitely" language replaced by what the finite runs actually show | review |
| R68 | Propagation of R67, and one error corrected: the **`2W` memory bound is withdrawn** — in single mode an epoch may span arbitrarily many versions before reaching X, so W bounds the *retirement delay*, not the age of the content; the approximate peak is `X + A(Δv) + overshoot + slack`. Also: "unmodified `SkipList`" → *representation and lookup algorithm adapted to an epoch-local arena*; §2.1's keep-freeing preference marked **superseded**; §4 and §6 no longer speak of "both variants" or an allocator choice; the closure's "size threshold withdrawn" narrowed to steady-state rotation | review |
| R67 | §2.a **corrected: two modes.** R66 withdrew size-based sealing too broadly. Size does not drive steady-state dual-epoch rotation, but a **bootstrap/re-entry threshold X** is necessary and supplies the hysteresis R66 lacked: in single mode `current` accumulates to X and is sealed; in dual mode `floor > maxTS(previous)` rotates, and if `floor > maxTS(current)` too, **both** arenas are freed and the system returns to single mode. This retires R66's "rotation frequency has no lower bound" caution. X is measured in pages or allocated bytes, not reachable entries, and is distinct from `totalOwnedBytes` (backpressure) | review |
| R66 | **Phase B reopened by T3.3 for reclamation, not throughput.** T2.2's negative throughput result remains valid. T3.3 shows that demand-driven floor plateaus and jumps make the current per-node reclamation mechanism dependent on a new debt-aware GC controller: gated sweeping retained 8.3× more nodes, and draining ~298 k nodes after writes stopped extrapolated to ~52 600 batches. The design therefore reopens the two arena-backed canonical SkipLists as a way to replace incremental reclamation debt with whole-epoch retirement. Rotation is corrected to **death-driven ping-pong**: free `previous` when `floor > maxTS(previous)`, immediately seal `current`, and reuse the freed arena. Size-based sealing and `S` are withdrawn. Two new cautions recorded: rotation frequency has no lower bound, and `totalOwnedBytes` must still trigger backpressure | `../benchmarks/measurement-results.md` T3.3 |
| R65 | **§3 measured (T3.3, third revision) — two Phase A blockers.** (a) Gating the sweep on the floor advancing lets debt grow through a plateau, where `newOldestVersion == cs->oldestVersion` — it grew for the whole observed plateau and peaked at the final sample: 8.3× retained population, `dead_peak == dead_final`. (b) The budget is tied to write volume, so once writes stop, draining 298 k of debt extrapolates to ~52 600 batches even with (a) fixed. Both must be decoupled before Phase A ships. Bounded catch-up still unestablished | `../benchmarks/measurement-results.md` T3.3; `ConflictSet.cpp:1016` |
| R64 | **§3 measured (T3.3).** *First run retracted:* its "4.4× regression" and validated fix came from a schedule with a **retreating** requested floor, which the monotone contract forbids. Re-run with a monotone floor: the aggressive floor **reduces** retained population (430 k → 230 k → 116 k), debt is bounded and stable everywhere, and the candidate fix changes nothing measurable. The gating of the sweep on the floor advancing remains a **design note** — plateaus stop the sweep — but is not a demonstrated defect. Bounded catch-up *time* still unmeasured | `../benchmarks/measurement-results.md` T3.3; `ConflictSet.cpp:1016` |
| R63 | Final propagation: T2.2's conclusion and the *What this changes* row now say **lowest modelled `q`** and **nominal** comparison-model break-even; the T1.1/T1.2 caveat restated in the past tense; §6 marked as a plan **conditional on reopening**, with items 7–9 done | review |
| R62 | *(superseded by R66)* Residual propagation: §3's "what remains is the allocator choice…" replaced by *parked*; §2.1's `[measure]` on the overwrite ratio replaced by the T1.2/T2.2 result; the reopening criterion restated as **nominal**, since unequal sizes, cache and the epoch cycle can shift the real crossover | review |
| R61 | *(superseded by R66)* Status and closure restated: **correctness closed, Phase B parked**. Allocator and S are no longer active work; a single reopening criterion (T4.2 → `q ≤ 0.056`, then T3.1) replaces them. §2.e updated for T2.2's overwrite sweep. §4's broken paragraph rebuilt. Claims narrowed: *no monotonic window effect over the interval tested*, *sparsest lowest-overwrite setting tested*, **comparison-model** break-even with assumptions stated | review |
| R60 | **§4 measured (T2.2) — no crossover.** The removable reclamation share *falls* as overwrite rises (9.0 % → 0.3 %), window has no effect at constant work, best case 10.5 %; and modelled `q = 0.392` against a break-even of `q ≤ 0.056` makes the two-epoch search **+31.8 %**. §4 restated from *suspended* to **not supported by measurement**. Revival requires a measured lag distribution (T4.2) plus T3.1 | `../benchmarks/measurement-results.md` T2.2 |
| R59 | Propagation round 2: §4's "put a ceiling on the whole design" → **bounded the removable reclamation component**; the §4 table row and §6.9 given the *splice-and-abandon allocator* qualifier; `../benchmarks/measurement-results.md` "Next" repaired (broken splice), restated as "no demonstrated **reclamation-based** case", and T2.2 reframed as the **gating** measurement rather than the deciding one — only the prototype decides the net | review |
| R58 | Propagation: §1's "the existing counters already separate them" corrected (they isolated only the sweep; T1.1 added the split); §2.e's keep-freeing "no amplification at all" qualified to *no splice-and-abandon amplification*, since sweep-eligible nodes are still deferred to epoch destruction; §4's "put a ceiling on the design" → **bounded the reclamation component** | review |
| R57 | §4, §2.e, §1: **"ceiling for the whole design" withdrawn** — 8.4 % is the gross removable *reclamation* share, not a performance ceiling; two epochs also change lookup, and search is 61.5 %. Net formula extended to `ΔT_lookup − T_sweep − T_interior + T_rotation + T_band`. keep-freeing's 8.2 pp restated as **deferred and batched**, not eliminated (`SkipList::destroy()` at rotation). Byte figures relabelled *deferred reclamation*, not residency. §1's "the larger cost may be on the commit path" and §2.d's "the gain not in doubt is elsewhere" both invalidated by T1.1 | review; `../benchmarks/measurement-results.md` T1.1 |
| R56 | §2.e, §4: **memory attribution corrected** — the ~143.9 MB the sweep frees today is retained to rotation in *both* variants (the generational design's own price for 8.2 pp); only ~9.8 MB, the interior-destroyed bytes, is the arena's differential. §4 retitled *pending workload validation*: the decomposition is done, only T2.2 remains, and it starts from "no case", not from neutrality | review; `../benchmarks/measurement-results.md` T1.1/T1.2 |
| R55 | **§4, §2.e measured (T1.1/T1.2).** `D.MergeWrite` decomposed: `M.Find` 62.5 % of it, interior walk 0.4 %. Ceiling for wholesale death = sweep + interior walk = **8.4 % of Detect ≈ 1.09×**. Search is 61.5 % of Detect. Overwrite ratio **0.052** → the arena buys 0.2 pp, so keep-freeing is the indicated default. T2.2 is the only remaining measurement that can move this | `../benchmarks/measurement-results.md` T1.1/T1.2 |
| R54 | §4: the **~2× end-to-end Amdahl figure** suspended with the tables it derives from; the fewer-resolvers dividend kept, conditional on a throughput gain existing | `../benchmarks/measurement-results.md` T0.2 |
| R53 | §4 restated from "target envelope" to **historical target, suspended**: T0.2 found model/attribution uncertainty, not measurement dispersion. Table rows labelled *historical model*. Caveat 3's "miss-bound … stands" downgraded to a hypothesis needing hardware counters — T0.3's `find` vs `find_sorted` gap is a storage-side `VersionedMap` result, not evidence about `ConflictSet` | `../benchmarks/measurement-results.md` T0.2, T0.3 |
| R52 | **§4 measured (T0.2).** The 81 %-in-cold-unlinks premise is **not supported**: the floor sweep is 9.0 % of detect time, validation is 38.6 %, and the baseline throughput is 0.899 M txn/s against the model's 0.57 M. The cost model's chain (370→32 ns, 10.5×, ~2× end-to-end) rests on that premise. T1.1/T1.2 now decide whether it was a wrong attribution or a different regime | `../benchmarks/measurement-results.md` T0.2 |
| R51 | Editorial: duplicated phrase in §2.e removed; §7's generational rebuild stated once as withdrawn by R28 | review |
| R49 | §6.8 **corrected**: the three numbers must be *live and per-arena* — `reachableBytes`, `ownedBytes`, `unreachableOwnedBytes` — not cumulative. "Cumulatively allocated − reachable" stops being the amplification after the first rotation. And the seal trigger watches **`ownedBytes`**, not unreachable bytes: a workload introducing distinct keys keeps everything reachable while the arena grows without bound. Cumulative counters retained for rate profiling only | review |
| R46 | §2.b: `RESOLVER_STATE_MEMORY_LIMIT` demoted from mechanism to **precedent** — it bounds the state-transaction buffer, not the conflict set, which has no memory accounting today; Phase B needs an equivalent trigger tied to epoch memory | `Resolver.cpp:293–303`, `:538–540` |
| R43 | §2.e: **"retaining more than W costs nothing" narrowed** — it needs no extra reclamation mechanism, but costs logical memory and search under keep-freeing, physical memory under the arena, and one extra epoch query for pre-seal readers | review |
| R41 | §6.9 and §8: the overwrite ratio now decides only the **allocator choice**, not a withdrawn representation obligation; the §8 question reframed as amplification versus saved interior work | review |
| R37 | §3 refreshed against `03-floor-tracking.md`: the handoff gap is **closed** (§4a), `globalOldestRV` → `globalOldestClientRV`, `globalValidationDemand` includes in-flight commits, and the stale stable-proxy / `set_read_version` restatements removed | `03-floor-tracking.md` §4a, §5, §6 |
| R26 | §1: *"first-committer-wins"* dropped as too broad — write-write overlap alone is not a conflict in FDB; only backward validation is claimed | review; verified at `ConflictSet.cpp:805`, `:924–925`, `:934–936` |

**Closed for v1.** At most two live epochs, backed by two reusable arena slots; commit window bounded by W; no pointers across epochs;
per-epoch allocation and wholesale death; **two-mode rotation, death tested before X** — a lone
epoch is freed and re-seeded in place when the floor passes its `maxTS`, a *live* epoch reaching X
forms a generation, and in dual mode `floor > maxTS(previous)` rotates (freeing both arenas and
returning to single mode when `current` is obsolete too); band-filter selection by one integer per
epoch; **the representation inside each
epoch is the existing canonical `SkipList` representation and lookup algorithm, adapted to an
epoch-local arena**; a monotone floor derived from the
`03-floor-tracking.md` sensor by the consumer-side formula
`max(globalValidationDemand, now − W_commit)` — where
`globalValidationDemand = min(globalOldestClientRV, oldestInFlightCommitRV)` so that accepted
commits keep retaining after their client releases — keeping conflict retention distinct from
storage retention.

**Phase A dependencies (in `03-floor-tracking.md`, not here).** The commit-submission lifecycle handoff is
now **specified** — `03-floor-tracking.md` §4a: handoff-before-release, a server-side pin surviving client
death, identity and generation fencing, and the rule that a request whose registration expired
before handoff is rejected. What remains open there is the *linearization mechanism*
(`03-floor-tracking.md` §8.4), which must be selected and covered by failure-injection simulation before
Phase A ships. Phase A's retention win is additionally gated on cluster-wide participation
negotiation (`03-floor-tracking.md` §6). The `q* = I/S` criterion, the alternatives analysis and the
cost-model *methodology* also stand.

**Open — the prototype, and what it must settle.** The allocator question is closed (arena; §2.e).
**Steady-state size-based rotation is withdrawn; X remains as the single-mode formation and
re-entry threshold** (§2.a). What remains open is whether the search and memory cost of two epochs
is acceptable in exchange for whole-epoch reclamation — **T3.1's purpose, restated**:
it no longer exists to hunt a throughput win gated on `q ≤ 0.056`, but to price the trade T3.3
created. Two cautions the prototype must resolve: `totalOwnedBytes` must trigger backpressure, a
quantity distinct from X (§2.b); and **discarding an arena must be cheap per event, with the event
rate counted per type** — X bounds how often a *pair* forms, not how often an arena is retired, so
the aggregate is rate × unit cost and neither factor can be assumed (§2.a.1 property 3).

**Phase A is not waiting on this.** Its value — a demand-driven retention floor — never depended on
the epoch structure. But T3.3 shows the two are coupled in one direction: shipping Phase A on the
current structure requires a debt- and time-aware sweep controller (§3), which arena-backed epochs
would make unnecessary.

*Two epochs are sufficient by the no-third-rotation protocol; the unchanged commit-window bound
limits only how long rotation can remain blocked. The representation
inside each epoch is the map FDB already maintains; what the design changes is when its storage
dies, not how it is searched.*
