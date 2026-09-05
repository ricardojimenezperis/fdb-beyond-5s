# Floor tracking — implementation guide (self-contained)

*Tree: `apple/foundationdb` main @ `a443d3ee60`. Verify any citation with
`git show a443d3ee60:<path>` — never against a working tree that may carry local
instrumentation.*

**This document repeats, on purpose, everything from `../design/03-floor-tracking.md` that you
need while writing code, plus the consumer sites from the resolver and storage guides. You
should not have to open another document to implement this.**

---

# Part I — What you are building

## 1. The one-line statement

A cluster-wide, **monotone minimum over the read versions that some live participant may
still use**, published to the roles that reclaim history — so that retention follows demand
instead of a five-second constant.

## 2. Why this is the first thing to build

Every other piece of the project consumes this number. Today three places compute a
retention horizon from a constant, and all three become consumers:

| Consumer | Site today | What it computes |
|---|---|---|
| Resolver | `Resolver.cpp:359` | `newOldestVersion = req.version − MAX_WRITE_TRANSACTION_LIFE_VERSIONS` |
| Commit proxy | `CommitProxyServer.cpp:2104` | `oldestVersion = prevVersion − MAX_WRITE_TRANSACTION_LIFE_VERSIONS`, for `keyResolvers` coalescing |
| Storage server | `storageserver.cpp:10453–10462` | `proposedOldestVersion = … − maxVersionsInMemory` (`:10444–10447`) |

Neither the resolver's Phase A nor the storage server's S1 can be *finished* without it.
Instrumentation (§7) runs alongside rather than in front: it sizes the parameters in §6 and
justifies the defaults, but it gates nothing — implementation can start without it.

## 3. The three invariants

Everything else in this document is a choice. These are not.

**(I1) Register before use.** A read version becomes usable only after the registration that
protects it is **visible to the aggregator** — not merely recorded on the GRV proxy. Registering
locally and propagating eventually leaves the same time-of-check/time-of-use race the commit
handoff closes: the floor can advance past `r` before the contribution lands, and the client
then uses a read version nothing protects. The initial installation, and any later update that
*lowers* a source minimum, go through the same authoritative primitive
(`conditionalInstallClientMinimum`, §9), and the **GRV reply is sent only after the
acknowledgement**. Zero added client messages does not mean zero internal coordination.

**(I2) Handoff before release.** An accepted commit's read version must be covered by a
*server-side* minimum — the Commit Proxy's, over its pending batches — before the client's
lease may stop covering it, and that coverage remains until validation completes. Client survival after acceptance is not required for safety.

**(I3) Monotone publication.** `publishedFloor = max(previousPublishedFloor, newlyDerivedFloor)`
— **of the floors published irreversibly to consumers**, not of the source minima, which
legitimately fall when a new contribution installs.
A retreating floor would admit transactions whose history has already been reclaimed. The
existing resolver code already depends on a monotone floor — `if (newOldestVersion >
cs->oldestVersion)` (`ConflictSet.cpp:986`) — so this invariant needs no new machinery
there, only respect.

## 4. The architecture: two populations, two minima, no per-transaction server state

**This is the shape of the whole thing.** Two populations need protection, and each is
reduced by the component that *already holds the detail*. Only aggregated minima travel.

```
client libraries
    └── minimum RV over live transactions          ── clientOldestActiveRV
             ↓
GRV proxies / aggregator
    └── globalOldestClientRV ──────────────────────────→ storageReadFloor
             ↓
Commit Proxies
    └── minimum read_snapshot over pending batches ── commitProxyOldestInFlightRV
             ↓
        oldestInFlightCommitRV
             ↓
globalValidationDemand = min(globalOldestClientRV, oldestInFlightCommitRV)
             ↓
resolverValidationFloor = max(globalValidationDemand, currentVersion − W_commit)
```

### 4.1 The client minimum

The library knows every live transaction's read version, so it keeps the detail and publishes
a scalar:

```
activeReadVersions = { RV of active transactions }
                   ∪ { RV of transactions whose commit was sent but has no terminal result }

clientOldestActiveRV = min(activeReadVersions)          // min(∅) = currentVersion
```

Published as `{clientID, leaseGeneration, clientOldestActiveRV}` — **never one entry per
transaction**. It advances exactly because the library holds the set:

```
active RVs: {100, 130, 170}  →  publishes 100
the RV-100 transaction ends
active RVs: {130, 170}       →  publishes 130
```

Values from one client and generation are monotone; a late report can only over-retain.

**Out-of-order GRV replies do not break monotonicity, and need no extra state.** With no active
transactions and two concurrent GRV requests, replies can arrive 120 then 110; if each
transaction kept the read version of its own reply, inserting 110 after 120 would pull
`clientOldestActiveRV` backwards and violate monotone updates within a generation. The library
therefore canonicalises every granted version:

```cpp
Version effectiveRV = std::max(reply.version, latestGrantedRV);
latestGrantedRV = effectiveRV;
transaction.setReadVersion(effectiveRV);
```

> **Invariant.** Every server-granted read version delivered to a transaction is
> `max(reply.version, latestGrantedRV)`, within the same `DatabaseContext` and recovery
> generation. Consequently, installing a newly granted read version cannot lower the client
> process's active minimum.

In the example both transactions run at 120. A delayed reply of 90 against an existing
transaction at 100 is raised to at least 100. Inserting a transaction never lowers
`min(activeReadVersions)`; ending the oldest one only advances it; and `latestGrantedRV`,
`clientOldestActiveRV` and the published values stay monotone within a generation. No
`pendingGrvLowerBounds` set is needed, GRV requests need not be serialised, and the population
is not split by request mode.

**Raising the version does not create a retention hole.** The protection installed by the stale
reply — a registration at 110, or at a lower `clientFloor` — also covers 120, because a floor at
a lower version protects every version above it.

The boundary for sharing the maximum is **`DatabaseContext` + recovery generation + a valid
lease**, not the mode of the GRV request:

1. **Both versions must have been granted by the cluster.** Values supplied through
   `setVersion()` are excluded — they carry no retention guarantee (§2).
2. **Same `DatabaseContext`, same recovery generation.** Read versions are never mixed across
   recoveries; a reply from an earlier generation does not participate in the maximum and is
   rejected. This is the real constraint behind `FLAG_USE_PROVISIONAL_PROXIES`.
3. **The delivered version must still be covered by a valid lease window.** A reply arriving
   outside its window is discarded or re-registered before use, however its version compares.

*Request mode imposes no further restriction.* `FLAG_CAUSAL_READ_RISKY` may return a version
that lags — raising it to one already granted only uses a fresher snapshot, and a freshness
guarantee is a lower bound, so any version at or above the one the caller was entitled to
satisfies it. `FLAG_USE_MIN_KNOWN_COMMITTED_VERSION` selects where the version comes from; it
is not a ceiling forbidding a later granted version (`GrvProxyInterface.h:73–77`).

**Handle the rest of the reply coherently, but do not transplant it.**
`GetReadVersionReply` also carries `locked`, `metadataVersion` and `ssVersionVectorDelta`
(`GrvProxyInterface.h:33–46`). Process them normally into the `DatabaseContext`'s shared state:
do not naively pair `version = 120` with metadata that only corresponds to processing the 110
reply, and do not re-apply a delta merely because another reply supplied the maximum —
`ssVersionVectorDelta` is a delta against shared client state, not a value to copy across
replies.

**Read-only transactions are covered here and nowhere else** — they never reach a Commit
Proxy — which is why this half is indispensable to the storage side.

If the client dies before delivering a commit, that transaction is genuinely dead: no Commit
Proxy knows it and it can never produce a durable decision, so its contribution may go when
the lease expires.

### 4.2 The Commit Proxy minimum — a monotonic deque

A commit that reaches a proxy is **already** held in that proxy's batch structures until
validation finishes. The floor protocol reuses that detail instead of duplicating it:

```
batchOldestReadSnapshot     = min(read_snapshot of commits admitted into the batch)
commitProxyOldestInFlightRV = min(batchOldestReadSnapshot over non-terminal batches)
oldestInFlightCommitRV      = min(commitProxyOldestInFlightRV over proxies)
```

computed once, during the traversal the proxy already performs while building the batch.
**Call it `readSnapshot` or `inFlightReadVersion`, never `commitVersion`** — what must survive
is history from the commit's *read* version, not the version later assigned to it.

**Batches complete in order; this is verified, not assumed.** The pipeline serialises them by
`localBatchNumber`: resolution waits for the predecessor (`CommitProxyServer.cpp:850`), logging
waits for the predecessor (`:867`), with assertions that it is exactly `N−1` (`:465`, `:864`,
`:868`, `:1843`); resolvers process a proxy's batches in version order (`Resolver.cpp:324`).
Removal is therefore FIFO.

Read versions are *not* ordered by batch number — an old read version can be submitted late —
so the minimum is not the front of the queue. FIFO removal plus arbitrary values is the
sliding-window-minimum problem, and the exact solution is a monotonic deque:

```cpp
struct BatchMinimum {
    uint64_t localBatchNumber;
    Version  oldestReadSnapshot;
};
std::deque<BatchMinimum> minima;

// admitting batch N with minimum v
while (!minima.empty() && minima.back().oldestReadSnapshot >= v) minima.pop_back();
minima.push_back({ N, v });

// retiring batch N — necessarily in order
ASSERT(N == nextFloorRetirementBatch++);
if (!minima.empty() && minima.front().localBatchNumber == N) minima.pop_front();

// the contribution
Version contribution = minima.empty() ? currentVersion : minima.front().oldestReadSnapshot;
```

Amortised O(1) on both paths, exact, and bounded by the pending batches. **Why dropping from
the back is safe:** an entry is dropped only in favour of a *later* batch whose value is no
greater; by FIFO removal that later batch outlives it, so the dropped value can never be the
minimum while its own batch is pending.

**Do not store a slot index in the batch.** An entry can leave through `pop_back` long before
its batch completes, so the batch would hold a reference to something already gone. The
identifier is `localBatchNumber`, carried by the surviving entries.

**Put the retirement at the already-serialised transition.** `CommitProxyServer.cpp:867–869`
waits for the predecessor, asserts it is `N−1`, then sets `latestLocalCommitBatchLogging` to
`N`. Retiring there inherits FIFO ordering from the pipeline instead of depending on callback
order, and it lands *after* that batch's resolution completed — slightly conservative, which
is the safe side. Keep the `ASSERT` anyway: it turns a dependency on current internals into an
executable invariant that will fire if upstream ever relaxes the ordering.

> **Rejected alternative, recorded so it is not re-proposed:** a preallocated ring of per-batch
> minima with `VERSION_MAX` sentinels, a cached minimum with a holder count, and an O(B)
> vectorisable rescan when the last holder leaves. Its machinery exists to tolerate holes from
> out-of-order completion, which cannot happen here; it optimises a scan over a few dozen
> 8-byte values (less than one cache miss); and it needs a fixed capacity with no firm bound to
> size it against — `COMMIT_BATCHES_MEM_BYTES_HARD_LIMIT` is a byte budget
> (`ServerKnobs.cpp:855`) and `RESET_MASTER_BATCHES` / `RESET_RESOLVER_BATCHES` are diagnostics,
> not limits.

### 4.2a One authoritative transition per batch

Every batch already sends and awaits `GetCommitVersionRequest`, so carrying `p` on it and
running the transition **every time** costs no round trip and no scheduling dependency. v1
therefore has **no local fast path**: the proxy does not decide whether coordination is needed,
because it cannot know.

Four quantities, named apart because conflating them produced two incompatible algorithms in
earlier drafts:

- `proposedBatchMinimum` (`p`) — the minimum `read_snapshot` over the transactions the batcher
  accepted into the batch, accumulated in `commitBatcher`. A *proposal*: it precedes any
  authoritative filtering, and it is what drives the transition.
- `authoritativeEffectiveFloor` (`F`) — `max(publishedGlobalValidationDemand,
  authoritativeCurrentVersion − W_commit)`, known only at the authority.
- `authoritativeInstalledProxyMinimum` (`C`) — what this source currently has installed, read
  from the authority's own state.
- `installedProxyMinimum` (`I`) — `min(C, max(p, F))`, which is `C` unchanged whenever that
  expression does not lower it.

```cpp
// inside the sequencer's atomic stretch, on every batch
F = authoritativeEffectiveFloor();
C = source.minimum.present() ? source.minimum.get() : currentVersion;
I = min(C, max(p, F));
if (!source.minimum.present() || I < C)           // an install never raises a present one
    source.minimum = I;

reply(F, I, publicationSequence);
```

**The empty source is not an installed contribution.** `min(∅) = currentVersion` is the right
convention for *computing* the reduction, and the wrong one to inherit as installed state. If a
source has nothing installed and `currentVersion = 100`, a batch with `p = 100` and `F ≤ 100`
yields `C = 100` and `I = 100`; "write only when `I < C`" then writes nothing, the batch is
admitted, and when `currentVersion` reaches 110 the source's implicit minimum follows it to 110
and strands a batch pending at 100. Presence is part of the state, and the first admission must
materialise it:

```cpp
C = source.minimum.present() ? source.minimum.get() : currentVersion;
I = min(C, max(p, F));
if (!source.minimum.present() || I < C)
    source.minimum = I;                 // the first admission always materialises
++source.revision;                      // every admission, even when the value is unchanged
```

The suppression is an optimisation over an *existing* entry, never over an absent one. So the
statement that only batches with `I < C` mutate the reduction is exact only for a source already
present: a batch mutates it when it **creates** a contribution or **lowers** an existing one.

and **after the reply** the proxy rejects individually every transaction with
`read_snapshot < F`, admitting the survivors, which `I` already covers.

**Why there is no local fast path.** A proxy's `acknowledgedProxyMinimum` is a copy that can be
stale in the one direction that matters. Suppose proxy and authority both hold 100; the proxy
retires batches and asks to raise to 200; the authority installs 200 and the acknowledgement is
still in flight when a batch arrives with `p = 150`. Reading its own copy the proxy sees
`150 ≥ 100` and skips the proposal — while the contribution actually installed is 200, which
does not cover 150. Sequence numbers discard *late* acknowledgements; they do not close that
interval. Keeping the optimisation would mean invalidating the local capability before every
raise and coordinating admissions until a new token arrives: a great deal of protocol to save
some fields and a comparison inside an RPC that happens regardless. `acknowledgedProxyMinimum`
therefore survives only as diagnostics, and possibly as a later optimisation once its
revocation is specified.

**A stale *raise* is unsafe, and needs its own guard.** Lowerings are harmless when late — they
only over-retain — but a raise computed against an older state can strip coverage from work
admitted since. With `C = 100`, the proxy computes a raise to 200 and sends it; before it is
processed the authority admits a batch with `p = 150`, covered by `C = 100`; the stale raise
then installs 200 and leaves that batch uncovered. Discarding a late acknowledgement at the
proxy does not help: the *authority* must refuse to apply it.

A per-source revision closes it, advanced by **every** accepted transition — including
admissions that left the value unchanged, since those are exactly the ones that create newly
covered work:

```cpp
struct ProxySourceState {
    uint64_t revision;            // persists across the whole lifecycle
    Optional<Version> minimum;    // absent means "no contribution", not currentVersion
};

// admission at the authority, on every batch
read F;
C = state.minimum.present() ? state.minimum.get() : currentVersion;
I = min(C, max(p, F));
if (!state.minimum.present() || I < C) state.minimum = I;
++state.revision;                       // always, even when the value is unchanged
reply { F, I, state.revision, localBatchNumber };

// raise at the authority, on retirement
conditionalRaise(expectedRevision, newMinimum /* or noPendingBatches */):
    if (expectedRevision != state.revision)
        return Rejected(state.revision);
    if (noPendingBatches) state.minimum.reset();   // NOT state.minimum = currentVersion
    else                  state.minimum = newMinimum;
    ++state.revision;                              // every accepted transition
    return Applied(state.revision);
```

**Retirement of the last batch restores absence — the symmetric operation to materialisation.**
Writing `currentVersion` there would re-create the same defect the first admission avoids, one
step later: a fixed value that stops following the version and pins retention for as long as the
source stays idle. Deleting the record entirely is the opposite error — the revision is what
rejects late messages, so it must outlive the contribution. Hence identity and presence are
separated permanently: `revision` persists, `minimum` is `Optional`.

**The revision advances on every *accepted* transition, not only on admissions.** Every
admission advances it, every accepted raise advances it, and the retirement that empties the
source advances it while leaving `minimum` absent. If an accepted raise left the revision alone,
two retirements sharing a revision could reorder: the later one empties the source, the earlier
and delayed one still passes the compare, and it resurrects a fixed entry with no pending work
behind it — an idle source over-retaining indefinitely. That is not under-retention, but it
breaks convergence. Advancing on every accepted transition makes each compare consume its
precondition exactly once.

The lifecycle is then closed: **absent → materialised → lowered → raised → absent.**

**When a retirement rides another batch's request, the retirement goes first.** A
`GetCommitVersionRequest` can carry both a pending raise with its `expectedRevision` and the
unconditional admission of the new batch with its `p`. Within the same non-suspending stretch:

1. apply or refuse the raise by compare-and-set;
2. advance the revision if it was accepted;
3. **always** run the new batch's admission against the resulting state;
4. advance the revision again for the admission;
5. reply with the final revision, bound to the new `localBatchNumber`.

The reverse order is safe but useless. With `revision = 8`, `C = 100`, a raise proposed to 200
and a new batch with `p = 150`: admitting first takes the revision to 9, so the raise carrying
`expectedRevision = 8` fails — and every raise transported by a batch invalidates itself, so
under continuous traffic piggybacked raises never converge and the source over-retains exactly
where it is busiest. In the stated order the raise installs 200 at revision 9 and the admission
immediately lowers it to 150 at revision 10, leaving the batch covered.

If the raise was genuinely stale it fails at step 1 and the admission proceeds normally against
the state in force. Either way the revision returned is the one *after* the admission, which
correctly forces the proxy to integrate the new batch locally before it may retry.

**The compare alone is not enough: a revision must not become locally usable before its batch is
in the deque.** The authoritative compare rules out a raise computed *before* an admission the
authority has already performed. It says nothing about one computed *after* that admission's
acknowledgement arrived but *before* the proxy folded the batch into the deque, and that window
is real:

1. The authority admits batch A, installs its coverage, advances `sourceRevision` to 8.
2. The acknowledgement `{F, I, 8}` reaches the proxy.
3. Before A's survivor minimum is inserted, retirement computes 200 over the old deque.
4. It sends `{ newMinimum = 200, expectedRevision = 8 }`.
5. The authority is still at 8, accepts, and leaves A uncovered.

Eligibility is therefore local, and separate from arrival. A revision becomes usable only once
the result of its admission is represented in the deque — including when the batch had no
survivors, which is still a fact about coverage — and only as a **contiguous prefix**, never as
the highest revision seen:

```cpp
onAdmissionReply(batch, F, I, sourceRevision):
    filter(batch, F);
    insertSurvivorMinimumIntoDeque(batch);          // also when there are none
    markIntegrated(batch, sourceRevision);
    advanceContiguousIntegratedRevision();

onRetirement:
    send { newMinimum:      exactPendingMinimum(),
           expectedRevision: contiguousIntegratedRevision };
```

With both halves the argument closes on every order:

- A later admission has already reached the authority → its revision fails the compare.
- The raise applies first → the next admission reads the raised `C` and lowers it again through
  `I = min(C, max(p, F))` if it must.
- The acknowledgement arrived but its batch is not yet integrated → its revision cannot back a
  raise, because the prefix has not reached it.
- Replies are observed out of order → only the contiguous prefix advances, so an isolated later
  revision never becomes eligible on its own.

A refusal replies with at least the current authoritative revision, and the proxy **must not
resend until it has locally integrated every admission in that new prefix**. "Recompute from the
deque and resend" is not by itself sufficient: the recomputation would otherwise stand on the
same unintegrated state that caused the refusal, and could be refused — or accepted while still
wrong — again. The contribution stays low meanwhile, which only over-retains.

This is the symmetric counterpart of running admission unconditionally: no admission relies on a
stale local capability, and no retirement can raise the contribution above work admitted after it
was computed.

**One revision per source, and only one.** It is advanced by every accepted authoritative
transition — admissions, accepted raises, and the retirement that empties the source. Only its
*eligibility* rule is specific to admission: the revision is returned bound to the
`localBatchNumber` whose admission advanced it, and becomes locally usable only once that batch
is integrated into the deque. It guards every raise. It is *not* the
sequence used to discard stale diagnostic acknowledgements
(`{proxyGeneration, publicationSequence, coveredThroughBatch}`, §4a) — that one orders
observations, this one is a correctness precondition, and conflating them would make a
diagnostic ordering rule load-bearing.

**Why `min(C, …)` is load-bearing.** Without it, `C = 100` still covering an earlier pending
batch, a contaminated `p = 50` and an advanced `F = 150` would install 150 — raising the
contribution and stranding the earlier batch. Raises belong to retirement (§4.2), never to an
install.

**The cases, mutually exclusive:**

| Condition | Installed `I` |
|---|---|
| `p ≥ C` | `C`, no update |
| `p < C` and `p ≥ F` | `p` |
| `p < F < C` | `F` |
| `p < C ≤ F` | `C`, no update |

Every survivor satisfies `read_snapshot ≥ F` by the filter and `read_snapshot ≥ p` because `p`
is the batch minimum, hence `I ≤ max(p, F) ≤ read_snapshot`. Every earlier batch stays covered
because `I ≤ C`. With no survivors at all, the contribution merely over-retains until that
batch retires.

So a contaminated proposal is refused **as an exact minimum** while the same transition keeps
coverage for whatever survives, and the reply states the rejection threshold explicitly.
**This is not the silent substitution forbidden on the GRV path** (§11): the Commit Proxy is
told `F` and rejects each commit that fell outside it. On the *client* path, raising a reported
floor silently remains illegal.

**Two minima, not one.** `p` drives the transition and is taken over what the batcher accepted.
The *exact* minimum over the survivors of the `F` filter is a different value: it is what enters
the pending-batch deque (§4.2) and what determines future **raises** on retirement. Using `p`
for the deque would keep the contribution lower than necessary; using the survivor minimum for
the transition would require filtering before `F` is known, which the sequence does not permit.

**The guarantee covers only the demand-derived component.** `currentVersion − W_commit` can
still overtake a batch in transit: with `C = 100`, `p = 120` and `currentVersion − W_commit =
130`, the resolver's floor is 130 even though the proxy holds a contribution at 100. That is
today's behaviour preserved — a transaction can become too old while travelling — and it is why
the resolver stays authoritative (§4.2b).

When the deque advances to a **higher** minimum, publication may be deferred: a contribution
lower than necessary only over-retains.

**What this costs.** The round trip and its linearisation exist for every batch already. The
increment is: carrying `p` on the request, reading `C` and computing `F` and `I` at the
authority, and updating the reduction only when the batch **creates** a contribution or
**lowers** one — every batch pays the cheap check, few mutate anything. Measuring that increment is what the dark step (§18 step 5) is for.

**Acknowledgements carry `{proxyGeneration, publicationSequence, coveredThroughBatch}`, and
stale ones are discarded**, so a late acknowledgement cannot overwrite a value installed since.

### 4.2b The proxy's pre-filter is conservative; the resolver stays authoritative

Computing the minimum over *admitted* transactions requires the proxy to apply the
`read_snapshot ≥ effectiveFloor` test itself. Today that decision lives in the resolver
(`ConflictSet.cpp:805`) and the proxy only translates the reply (`:2043`), so there are now two
rejection points and their precedence must be explicit:

| | Outcome |
|---|---|
| proxy admits, resolver admits | commit proceeds |
| proxy admits, resolver rejects | the resolver's `transaction_too_old` stands — its floor may have moved in transit |
| proxy rejects | the commit never reaches the resolvers |
| proxy under-filters | safe: a lower minimum, so it over-retains |
| proxy over-filters | **availability regression** — a live commit refused |

**The proxy's filter is conservative and the resolver keeps the final say.**


### 4.3 What is explicitly excluded

Do not build any of these:

```
oldestPendingRV[client]
acceptedPendingCount[client]
any server-side collection of commits grouped by client
```

They contribute nothing to the floor and complicate everything: commits from one client
reaching different proxies, out-of-order completion, proxy recovery, per-client memory,
cleanup and ownership, and working out which read version becomes the next minimum.

| Component | Keeps | Publishes |
|---|---|---|
| Client library | the detail of its own live transactions | one minimum per client |
| Commit Proxy | the batch detail it already has | one minimum over pending work |
| Aggregator | per-source minima | the global minimum, monotonically |
| Resolver | — | applies the `currentVersion − W_commit` lower bound |
| Storage | — | consumes the client minimum only |

### 4.4 The derived floors, and the direction of each clamp

```
storageReadFloor        = globalOldestClientRV
resolverValidationFloor = max(globalValidationDemand, currentVersion − W_commit)
resolverValidationFloor = currentVersion − W_commit          // mixed/legacy mode
```

Three things to read carefully:

* **The resolver consumes `globalValidationDemand`; storage consumes only the client
  minimum.** A pending commit needs conflict history, not historical values — its reads
  already happened. Keep the names apart in code: merging them is the easiest way to
  introduce a subtle bug.
* **`currentVersion − W_commit` is a *lower bound* on the resolver's floor.** A very old
  read-only transaction must never force conflict history beyond the ordinary commit window.
  The floor may rise above the bound — reclaiming *sooner* when demand proves the history is
  unneeded — but never fall below it, so **the resolver never retains more than today**. The
  extension of old versions is the storage server's job, through `storageReadFloor`, which
  *may* fall below `version − MAX_READ_TRANSACTION_LIFE_VERSIONS` and is bounded instead by
  the administrative ceiling (§11) and the byte budget.
* **Empty means "reclaim up to now", not "fall back to the constant".** With both sources
  empty, `globalValidationDemand = currentVersion`. With no clients but one pending commit at
  `r`, `globalValidationDemand = r` and the resolver's floor is `max(r, currentVersion −
  W_commit)` — it stays at `r` only while `r` is inside the ordinary commit window.


## 5. What FoundationDB has today — the facts that shape the work

All verified against `a443d3ee60`:

* **GRV proxies keep no per-client state.** `GrvProxyData` (`GrvProxyServer.cpp:187–211`)
  holds none, and `GetReadVersionRequest` (`GrvProxyInterface.h:65–141`) carries no client
  identity — only `transactionCount`, flags, priority, tags and `maxVersion`. **Nothing
  server-side observes a transaction's lifetime; the only place that knows it is the client
  library.** That is why this protocol has to exist at all.
* **GRV requests are load-balanced across all GRV proxies** —
  `basicLoadBalance(cx->getGrvProxies(...), &GrvProxyInterface::getConsistentReadVersion, …)`
  (`NativeAPI.cpp:5300`). This collides with "one lease copy per client"; see §6.2.
* **Both request streams are public and unverified.** `GetReadVersionRequest` arrives on a
  `PublicRequestStream` (`GrvProxyInterface.h:227`, `CommitProxyInterface.h:48`) whose
  `verify()` returns `true` unconditionally (`GrvProxyInterface.h:120`). Anything a client
  sends is untrusted input — see §11.
* **A field an older sender omitted arrives as its wire default, not as your initializer.**
  Measured on this serializer path, not assumed: a `Version retentionFloor = invalidVersion`
  member deserialized as **0** from a legacy payload, in a freshly constructed receiver as well
  as a reused one. Stated carefully:

  > On this FDB FlatBuffers serializer path, a field omitted by an older sender is deserialized
  > as its wire default; the C++ member initializer does not preserve absence.

  **Consequence, and it is a correctness one: a sentinel cannot represent absence.** The first
  implementation of F1 used `invalidVersion` to mean "no floor sent". A legacy Commit Proxy
  would therefore have looked like one reporting a floor of zero, the Resolver's equality check
  would have fired against it, and the Resolver would have aborted — during exactly the rolling
  upgrade the change was meant to pass through unnoticed. Neither compilation, nor the
  conflict-set benchmark, nor a homogeneous simulation could see it; only a wire round-trip
  test in both directions did.

  **The rule for every field this protocol adds:**

  ```cpp
  // F1: one optional quantity
  Optional<Version> retentionFloor;

  // F2: an atomic protocol unit, never two independent Optionals
  Optional<RetentionFloorInfo> retentionFloorInfo;
  struct RetentionFloorInfo {
      Version floor;
      Generation generation;
  };
  ```

  A sentinel does not represent absence, and two separate `Optional`s admit states the protocol
  does not define — a floor without its generation reads as "a floor with no fencing", which is
  precisely what the generation exists to prevent (§10).
* **Gating and compatibility are two different problems, and F1 needs only one of them.**

  > Protocol gating is required when the presence of a field enables behaviour an older peer
  > cannot safely participate in. F1 adds only neutral plumbing: structural compatibility is
  > represented by `Optional`, without a protocol-version bump. F2's demand-driven behaviour
  > remains negotiation-gated cluster-wide.

  FlatBuffers carries the schema, so a *structural* addition needs no gate — and claiming one
  would describe the wire falsely. The versioned-binary idiom exists for the positional
  serializers, where fields must be appended under
  `if (ar.protocolVersion().hasNativeCdc())` (`ClientDBInfo::serialize`,
  `CommitProxyInterface.h:151–153`) or `hasMutationChecksum()` (`CommitTransaction.h:351`), and
  the same file states the split explicitly: *FlatBuffer serializers include every schema
  field; versioned binary serializers must omit the newer ones for older peers.* Splitting a
  FlatBuffers `serializer(...)` call into several to imitate the gate breaks the table — in F1
  it left every field, not just the new one, unpopulated.
* **`ServerDBInfo` is the wrong broadcast channel.** Its `id` "changes each time any other
  member changes" (`ServerDBInfo.h:40`), so a continuously-updated integer there would
  rebroadcast the whole structure to every worker. (It is also not available to clients,
  `:36–38`, which is fine — every consumer here is a server.)
* **A single commit proxy failure tears down the whole generation.**
  `waitCommitProxyFailure` is `quorum(failed, 1)` raising `commit_proxy_failed()`
  (`ClusterRecovery.cpp:461–472`, armed at `:1952`); master, proxies, **resolvers** and TLogs
  are recruited as a unit. This matters enormously in §10.
* **Recovery already invalidates old read versions.**
  `recoveryTransactionVersion = lastEpochEnd + MAX_VERSIONS_IN_FLIGHT`
  (`ClusterRecovery.cpp:1387`) with `MAX_VERSIONS_IN_FLIGHT = 100 × VERSIONS_PER_SECOND`
  (`ServerKnobs.cpp:156`) — twenty times the ordinary window. "Recovery may invalidate
  retained history" is today's behaviour, not something you are introducing.
* **Manually set read versions bypass the sensor entirely.** `Transaction::setVersion(v)`
  (`NativeAPI.cpp:3594–3603`) validates only `v > 0`, contacts no proxy, and records
  `readVersionObtainedFromGrvProxy = false` (`:3602`). See the decision in §6.4.

---

# Part II — Decisions

Five entries, of which **one remains open**: the lease parameters (§6.5). The other four are
settled — watermark transport (§6.1), lease placement (§6.2), the conditional-install authority
(§6.3) and manually set read versions (§6.4) — and are recorded here because they still have to
be *implemented* deliberately. *Open choices, not open
safety rules — an admissible answer must still satisfy the frozen ordering and temporal
constraints: parameters violating `clientUsageWindow + driftMargin < serverLeaseDuration`
break safety, and so does an install that is not atomic against the floor advance or not
generation-fenced.*

## 6.1 Watermark transport — decided for v1

**Consumer-scoped dedicated publication.** Not `ServerDBInfo`: its `id` "changes each time any
other member changes" (`ServerDBInfo.h:40`), so a continuously-updated value there would
rebroadcast the whole structure to every worker. Publication is scoped to the consumers that
need it — the Resolver on the resolve request (as F1 already does), the Commit Proxy through
the install/acknowledge exchange (§4.2a), and an additional stream only towards Storage
Servers. Cost: new machinery. Benefit: no worker pays for a value it does not consume.

## 6.2 Stable-proxy vs multi-copy leases

The design's §3 wants one lease copy on a stable proxy; §5 observes GRV requests are
load-balanced (`NativeAPI.cpp:5300`). Both cannot hold.

* **Pin GRV requests to a derived proxy** — preserves one copy, but changes load balancing
  on the hottest client path and interacts with `basicLoadBalance`'s proxy-failure fallback.
* **Accept multi-copy leases** — each proxy holds its own `{clientID → minRV}`. Still
  correct: floors are monotone, so a stale copy is an older value and the global `min` is
  conservative. Costs: up to `numProxies` entries per client, and the floor advances only as
  fast as the *least recently refreshed* copy — for an idle client, a full lease period.
  **In v1 a renewal refreshes the copies the client knows it holds, and any others are left to
  expire** — renewing one copy does not update the rest, and they do not catch up on their own.
  Aiming `RenewOldestReadVersion` at a single designated copy is a possible optimization, not
  the v1 rule.

  **That rule has a protocol consequence: the client must be able to tell which proxy holds
  each copy.** GRV requests are load-balanced (`NativeAPI.cpp:5300`), so after the fact the
  client cannot infer who installed its registration. The acknowledgement must therefore
  identify the granting copy — at minimum

  ```cpp
  struct GrantedReadVersionLease {
      UID    grvProxyID;                  // which copy this acknowledgement installed
      UID    leaseGeneration;
      double grantedServerLeaseDuration;  // a duration, never an absolute instant
      GrvProxyInterface renewalTarget;    // or an equivalent routable handle
  };
  ```

  **The grant carries a duration, not a deadline.** An absolute instant produced by the server
  would mix clock domains, and a late reply would silently hand back time the client had
  already spent waiting. The client derives its own bound, on its own clock, from the moment
  taken *before* the request went out:

  ```cpp
  clientUsageDeadline = clientRequestStart + conservativeClientWindow(grantedServerLeaseDuration);
  ```

  which is §2's inequality applied directly. And `grvProxyID` names the copy but does not let
  the client reach it: a routable target must come with it, or `basicLoadBalance` must report
  which interface won.

  This is not a third open choice — the shape may vary, and an equivalent way of recovering the
  endpoint from the RPC would do — but it **is** a requirement on F2's implementation and its
  tests: without copy identity in the acknowledgement, a client cannot literally obey the rule
  of refreshing every copy it holds, and multi-copy degrades into "refresh whichever proxy the
  next request happens to reach".

**Decided for v1: multi-copy, with no global deduplication.** It leaves the GRV hot path
untouched and pays only in retention precision. **Under the floor-based conditional-install
model it adds no handoff-identity cost** (§6.3), so what remains is lease-state multiplication
— up to one entry per client per proxy — and a floor that advances no faster than the least
recently refreshed copy.

## 6.3 Where the conditional install runs — decided for v1

**The generation's sequencer (master) is the authority for both conditional installs and for
the published floors.** It keeps state per *source* — one entry per GRV Proxy and per Commit
Proxy — never per client and never per transaction. Each operation is linearised in a
non-suspending stretch together with any advance that could make the candidate inadmissible.
Recovery initialises the new authority only after the previous generation is fenced.

**Why the sequencer, in facts rather than preference.**

* **The ordering already exists there.** `getVersion` (`masterserver.cpp:74`) is the only
  writer of `self->version`, and from the ordering wait
  (`latestRequestNum.whenAtLeast(req.requestNum - 1)`, `:91`) through `req.reply.send(rep)` and
  `latestRequestNum.set(...)` there is **no `co_await`**. The compare, the installation and the
  publication must go inside that same stretch. *Note what does the work here: `latestRequestNum`
  only orders the requests of one proxy against each other. Global linearisation comes from
  being the sole writer plus the absence of any suspension, not from that sequence number.*
* **Both installers already talk to it once per batch**, on messages that exist today: the
  Commit Proxy through `GetCommitVersionRequest`/`Reply`, and the GRV Proxy through
  `getLiveCommittedVersion` (`masterserver.cpp:254`, `GrvProxyServer.cpp:727`), which it calls
  after grouping requests. A lowering install can therefore ride a round trip the proxy must
  make anyway *before* resolution, and the existing reply is already the acknowledgement — the
  slow path of §4.2a costs fields, not a round trip.
* **It holds `currentVersion` authoritatively**, which
  `authoritativeCurrentVersion − W_commit` needs.

**What fencing does *not* give for free.** The topology fits, but four cases must be closed
explicitly rather than assumed from "the master is the generation":

1. **Generation change** — the new sequencer may neither forget a floor already authorised nor
   publish a later one until the previous generation is fenced against durable decisions.
2. **GRV Proxy death** — its last contribution stays in the sequencer until every usage window
   that proxy could have granted has expired. Process disappearance is not enough (§10).
3. **Commit Proxy death** — its contribution does *not* expire on a timer; it stays until the
   generation fence.
4. **Storage** — `authoritativeStorageAdmissionFloor` must live in the same order domain as the
   irreversible publication to Storage Servers. If another component publishes, it must
   acknowledge the publication before the sequencer treats the advance as authorised.

**A lowering install is a *proposal*, and its rejection must not fail the batch** — §4.2a gives
the single transition that achieves both: the authority installs `min(C, max(proposedBatchMinimum,
authoritativeEffectiveFloor))` and returns the threshold, so a proposal contaminated by one
transaction too old still leaves the survivors covered, without a second round trip and without
failing the batch.

**Both handlers have the stretch, and both were checked.** The Commit Proxy install belongs in
`getVersion`; the client install belongs in `serveGetLiveCommittedVersion`
(`masterserver.cpp:254`), which likewise runs from receiving the request to `req.reply.send`
with no `co_await` in between. They call the same authoritative state, and the global order
between them comes from cooperative execution in one process — provided neither yields between
reading the guard, installing, and authorising or publishing.

**The cost objection, and how to settle it.** This adds state and per-request work to a
singleton already on the critical path of every commit batch and every GRV batch. Measuring
today's `getVersion` would not answer the question — it does not measure the increment. The
way to settle it is a **dark implementation first**: keep the per-source entries and compute
the minima without changing behaviour.

*What that step can measure*: the per-source lookup and update cost, the maintenance of the two
reductions, the duration added to the critical stretch, the size of the state, and the
serialization of the new fields. *What it cannot*: the fraction of updates that lower a
minimum, or the cost under real distributions — before leases and the pending-batch deque exist
there are no real minima to observe, so those belong to F0b, with the producers. The dark step
therefore prices the authority's **structural floor cost and validates its placement**; it does
not price the mechanism in production. If that structural cost turns out to be high, the
structure gets optimised; it would not by itself justify a new round trip and a second
distributed authority.

*Rejected: the Cluster Controller. It survives generations and already aggregates, but it sits
on no per-batch path, so every lowering install would need a new round trip and the
linearisation would have to be built rather than inherited.*

## 6.4 Manually set read versions — already decided, implement it explicitly

`setVersion(v)` gets **no retention guarantee**. It keeps today's best-effort semantics and
may be used only while `v` is within the storage server's actually retained capability.
Access to the *extended* window requires a read version obtained and leased through this
protocol. Register-on-set was rejected for v1 because it would add a round trip to a path
that has none.

Write this down in the client documentation, because the extended window makes "set an old
version and read" newly attractive, and today `setVersion` is invisible to the aggregation.

## 6.5 Lease duration and renewal frequency

A latency/over-retention trade-off, and **coupled to §6.2**: under multi-copy the memory is
per client process *per proxy*, and the floor lags by the least-recently-refreshed copy.
Size it with the numbers from §7 — do not pick it a priori.

---

# Part III — Build order

## 7. F0 — instrumentation (sizing, not a gate)

**F0 is not a correctness prerequisite and must not block the start of implementation.** It
sizes parameters and justifies defaults; nothing in §8–§10 depends on it being finished.

**Most of what needs measuring cannot be measured before the protocol exists**, so F0 splits in
two. Only the first half is work you can do today.

### 7a. Measurable today, with no protocol — do this first

```
commitVersion − readSnapshot                             // age at commit; also q
validationCompletionTime − commitAdmissionTime           // and the version-delta variant
distinct client processes per GRV proxy                  // by connection endpoint
conflict-set footprint on the resolver                   // §18 step 2
```

`commitVersion − readSnapshot` characterises window usage and models which epochs a resolver
would consult; it is **not** the duration of a Commit Proxy contribution. The
admission-to-validation interval is closer, and is the best available approximation today —
but it does **not** bound the real lifetime either, because the contribution is retired at the
*serialised logging transition* (§4.2), which is later than validation completion. The exact
quantities need the deque to exist, so they belong to §7b:

```
batchContributionLifetime = floorRetirementTime − batchFloorAdmissionTime
loweringInstallLifetime   = floorRetirementTime − conditionalInstallAckTime   // slow path only
``` Client processes have to be approximated by
connection endpoint, because `GetReadVersionRequest` carries no client identity today
(`GrvProxyInterface.h:65–141`) — which is the very gap this protocol closes.

*Discarded: `now − grantedRV` measured when the read version is granted. It is a freshly
granted version, so the answer is always ~0.*

### 7b. Instrumentation *of* F2 and F3 — ships with them, not before

These presuppose registrations, leases and the Commit Proxy contribution, so they cannot
precede the code they measure; they size its parameters after the fact and prove it affordable.

**Which means F2 cannot wait for them — it must bootstrap.** F0b tunes and validates; it
cannot retrospectively decide what it needed in order to exist. So F2 ships with:

- **conservative initial lease values** (a short server duration, a comfortable client margin),
  adjusted once §7b has measured real lifetimes and renewal rates;
- **a provisional watermark transport behind a swappable abstraction**, so the §6.1 choice is
  measured and ratified afterwards rather than guessed now — and so replacing it is a local
  change, not a redesign.

Neither is a placeholder for correctness: the safety rules (§3) hold at any parameter value.
What the measurements buy is efficiency and confidence, not validity.

```
currentVersion − reportedClientFloor      // how far behind demand actually sits
active leases and registered clients per proxy
renewals and expirations per second
effective lease duration
renewals lost or acknowledged after the client's deadline   // must be visible, not silent

size distribution of the monotonic deque
batchContributionLifetime = floorRetirementTime − batchFloorAdmissionTime
loweringInstallLifetime   = floorRetirementTime − conditionalInstallAckTime
fraction of batches that create or lower a contribution      // who actually mutates the reduction
first admissions materialising an absent contribution        // must never be suppressed
retirements returning a source to absent                     // must match sources going idle
time a source spends present with an empty deque             // the over-retention this prevents
duration added to the authority's non-suspending stretch
exactSurvivorMinimum − installedProxyMinimum                 // deferred-raise over-retention
raises refused by the revision CAS                           // resent after integrating the prefix
lag from admission acknowledgement to deque integration      // the window that gates eligibility
raises published with no batch to ride                       // idle sources, else over-retention
GRV registrations taking the slow path                      // candidateMinimum < acknowledgedSourceMinimum
new lease copies per client under multi-copy                // each can force a slow path
proxy pre-rejections, and resolver rejections after passing the filter
retirements by normal completion vs by generation fencing
FIFO-retirement assertion failures                          // must be zero
```

The estimates they feed, for §6:

```
memory              ≈ clients × copies per proxy × bytes per entry
renewal messages    ≈ clients with long transactions / renewalInterval
over-retention after client death  ≲ leaseDuration
```


## 8. F1 — request-carried plumbing with a derived floor (deliberately a no-op)

Build the transport and the clamps first, with a value that reproduces today's behaviour
exactly, so that "did I break the cluster" is answered before "is my floor right".

**Scope it to the request-carried path, and no further.** Proving neutrality needs only the
consumer end: the Commit Proxy computes the value and carries it in
`ResolveTransactionBatchRequest`, the resolver consumes it, and an assert says the two agree.
No aggregator, no cluster-wide watermark, no broadcast — which means **F1 does not force the
transport decision (§6.1)**; that belongs to F2, where a cluster-wide distribution is actually
needed. Deciding it here would be deciding it without the numbers.

1. Add the floor and its generation to the consumer requests — for the resolver, to
   `ResolveTransactionBatchRequest` (`ResolverInterface.h:122`) — using the protocol-version
   gating idiom of §5. Do not invent a new RPC per consumer.
2. Set the transported value to `version − MAX_WRITE_TRANSACTION_LIFE_VERSIONS`: exactly
   what `Resolver.cpp:359` computes today.
3. **Assert at each consumer that the received value equals the locally computed one.**
4. Apply the resolver's clamp:
   `resolverValidationFloor = max(globalValidationDemand, currentVersion − W_commit)`, never
   below `currentVersion − W_commit` and never below the last published value. **The storage
   consumer is not part of F1** — its clamp runs in the opposite direction and arrives with
   F2 (§4.4, §14). Keeping the two apart here is deliberate: they are the easiest pair in the
   protocol to get backwards, and F1 exists to prove neutrality on one path, not to wire both.

**Exit criterion:** full simulation suite green and the assert in (3) never fires. You now
have a provably behaviour-neutral floor pipeline on the request-carried path — the storage
consumer and the cluster-wide watermark follow in F2.

## 9. F2 — registration and leases

Now replace the derived value with an *observed* one. The distinction is load-bearing: only
an observation of live readers can safely extend the window; a derivation from the clock
cannot.

**Client side** — the monotonic minimum of §4.1, computed in the client library:

```
activeReadVersions   = { RV of active transactions }
                     ∪ { RV of transactions whose commit was sent, no terminal result yet }
clientOldestActiveRV = min(activeReadVersions)      // max(…, latestGrantedRV) when empty
                                                    // never decreases
```

Monotonicity is load-bearing: when the oldest transaction ends, the floor advances and that
version is irrevocably abandoned by this client. Published values may lag
(`reportedMin ≤ localMin`); **a lagging report only over-retains, never under-retains** — but
see the lease contract below, where a *lost renewal* fails in the opposite direction.

**Transport** — zero added messages on the normal path: every `GetReadVersion` carries
`{clientID, leaseGeneration, clientOldestActiveRV}` — one scalar, never a per-transaction
list. Explicit traffic appears in exactly one case: a
client holding a long-lived read version while no longer requesting new ones sends a
periodic `RenewOldestReadVersion`. **The new traffic is generated precisely by the
transactions that use the new capability.**

**Server side** — leases, never unregistration. `commit/cancel/destructor → unregister`
fails on the one case that matters: a crashed client sends nothing. Each GRV proxy keeps,
per client process, `{minRV, leaseExpiration}`; an expired lease removes the client and
recomputes the proxy minimum. A dead client over-retains for one lease timeout, and
**correctness never depends on promptly detecting death.**

Where the state goes: `GrvProxyData` (`GrvProxyServer.cpp:187`), fed from
`GetReadVersionRequest` (`GrvProxyInterface.h:65`) served through the stream at `:227`,
reached from the client at `NativeAPI.cpp:5300`.

**Bootstrap is I1, and it needs the authoritative primitive.** For a client with no active
transactions, register the chosen version *before* replying — through

```cpp
conditionalInstallClientMinimum(sourceGeneration, publicationSequence,
                                candidateMinimum, requiredReadVersion,
                                authoritativeStorageAdmissionFloor);
```

installing atomically and **replying only after the acknowledgement**.

> **The two installs share machinery, never a guard.** A client registration protects
> historical *values* in Storage, where `clientOldestActiveRV < currentVersion − W_commit` is
> exactly what must be allowed. Guard it with the resolver's effective floor and any reader
> older than `W_commit` becomes unable to reinstall its contribution — on reaching another GRV
> proxy, or during recovery — destroying the extended read window.
>
> **And physical retention is not the guard either.** "The bytes still exist" is not "it is
> still lawful to promise them":
>
> ```
> publishedGlobalOldestClientRV      = 200      // published, and monotone
> authoritativeStorageRetentionFloor = 100      // Storage happens to still over-retain
> candidateMinimum                   = 150
> ```
>
> Guarding on physical retention lets this win, but publication cannot retreat —
> `publishedFloor = max(200, 150) = 200` — so Storage stays authorised to reclaim to 200 and the
> lease protects nothing. An advance order to 200 may already be in flight. The authority is
> **`authoritativeStorageAdmissionFloor`**: the greatest reclamation floor already made
> irreversible or authorised to any Storage Server.
>
> | Install | Guarded against | Outcome |
> |---|---|---|
> | `conditionalInstallClientMinimum` | `authoritativeStorageAdmissionFloor` | wins only if `requiredReadVersion ≥` it — a client's registration is refused outright when reclamation has already been authorised past that version, and the refusal is visible (§7a/§11) |
> | `conditionalInstallProxyMinimum` | `authoritativeResolverEffectiveFloor = max(globalValidationDemand, authoritativeCurrentVersion − W_commit)` | never fails: it returns `{F, I}` with `I = min(C, max(p, F))`. The *exact* proposal is taken only when `p ≥ F`; below it, conservative coverage is installed and the proxy rejects individually |
>
> Physical `minimumRetainedVersion` stays useful for best-effort `setVersion()` and diagnostics
> — never as authority to grant a guarantee. *Reopening the demand floor when every Storage
> Server can prove it still holds `r`, cancelling advances already dispatched, would need a new
> distributed protocol and break I3; not justified.*
>
> **It composes with the lease contract:** another valid copy still protecting the client means
> the admission floor cannot have passed `r`; if every copy expired the client should already
> have revoked locally, so refusal is right; and during GRV proxy failure the generation
> barrier stops the admission floor advancing until coverage is rebuilt **or** every client
> usage window that generation could authorise has conservatively expired.

**When the slow path is needed.** What gets installed is
`candidateMinimum = min(clientOldestActiveRV, newlyGrantedRV)`, not the fresh read version — a
new *grant* is not a new *contribution*. The test is `candidateMinimum ≥
acknowledgedSourceMinimum` for that copy; the recency of the granted version proves nothing.
Under multi-copy leases a client already holding an old read version can land on a proxy that
never saw it and lower that copy's minimum sharply. So: a renewal to an existing copy is free;
a renewal or GRV that *creates* a copy may not be; and another acknowledged copy could in
principle prove continuous coverage, but only with **generation-fenced evidence** — without it,
take the slow path.

**The lease needs a temporal contract, because a lost renewal is not a benign delay.** A
lagging *report* only over-retains; a lost *renewal* lets the server expire the entry while a
live client still believes its snapshot is usable — under-retention, i.e. a safety failure.
Specify:

- an acknowledgement grants a **defined usage window**; versions of that generation may be used
  only within it;
- the client renews with margin;
- if no acknowledgement arrives before its own conservative deadline, the client **stops using
  and locally revokes** the snapshots of that generation;
- a GRV reply arriving too late to supply a full window is discarded or re-registered.

Safety comes from the inequality's direction: **the server's lease outlasts the client's
self-imposed window**, measured from *before* the request was sent, with a bounded clock-drift
allowance. A lost message then costs availability (the client stops early) and never lets the
server reclaim under a live reader. This is the one place the protocol leans on time rather
than fencing — say so, since elsewhere timers are explicitly not evidence (§10). **It is
frozen for v1, not an open choice**: standard, and failing towards availability rather than
safety. Its *parameters* stay open, subject to

```
clientUsageWindow + driftMargin < serverLeaseDuration
renewInterval                   < clientUsageWindow
```

which §6.5 fixes once F0 has sized them. A non-temporal fencing replacement is a later
evolution, not something that blocks Phase A.

**Ceiling:** never let a registration extend the floor beyond a configured maximum. One
stuck client would otherwise pin unbounded memory on every storage server. When the ceiling
binds, that reader gets `transaction_too_old` (`storageserver.cpp:2100–2102`) — today's
contract with a dynamic trigger. See §11 for why the clamp must be *visible*.

## 10. F3 — the commit lifecycle handoff

This is I2, and the design calls it the gap that blocked Phase A. **Read this section
before writing any of F2's release logic.**

### The race

A client submits a commit with read version `r`, then the transaction ends client-side (or
the process dies and its lease expires). `clientFloor` advances past `r`, the watermark
propagates, the resolver floor advances past `r` — while the commit request is still in
flight. The resolver reaches the request only after its history at `r` is gone.

### Why it is not merely an availability bug

Today admission and retention are the *same value*: `Resolver.cpp:359` feeds both
`addTransaction` (where `tooOld` is decided, `ConflictSet.cpp:805`) and the floor advance
(`:986`). **While they stay coupled**, a floor that overtakes `r` makes the request
*rejected*, not falsely accepted — a spurious `transaction_too_old` for a legitimately
in-flight commit, i.e. an availability regression. **Decouple them** — admission at
`now − W_commit`, retention on the dynamic floor — **and the same race becomes a false
accept**, a serializability violation. Both are unacceptable; the same rule removes both.

### Three states, not two

Commit *submission* is not the handoff — a message in flight can be lost.

```
CLIENT_COVERED  →  HANDOFF_ACCEPTED  →  VALIDATION_COMPLETE
```

* **`CLIENT_COVERED`** — only the client's lease guarantees retention.
* **`HANDOFF_ACCEPTED`** — the commit's read version is covered by the proxy's pending
  minimum and that minimum participates in the reduction. From here, client death or lease
  expiry is harmless.
* **`VALIDATION_COMPLETE`** — the batch is terminal; its contribution is withdrawn.

**What `HANDOFF_ACCEPTED` promises, exactly.** It removes the race the demand-driven floor
introduces — after it, disappearing client demand cannot strand the commit. It does **not**
promise the commit will never receive `transaction_too_old` later: `currentVersion − W_commit`
can still overtake it in transit, exactly as today. The handoff closes a new hole; it does not
extend a commit's current maximum lifetime.

**The race is floor-versus-installation, not expiry-versus-handoff.** Lease expiry only
removes *a* contribution; it may later let the floor advance, but it is not itself what
invalidates a commit. If the lease expired while the floor has **not** passed `r` — another
client holds it down, or it simply has not moved — the commit is admissible, and rejecting it
would be a spurious availability loss. The two exclusive outcomes are
`authoritativeEffectiveFloor > r` at installation time → reject, or installation while
`authoritativeEffectiveFloor ≤ r` → accept.

### The commit proxy's six steps

The *contribution* must be **ordered** with respect to floor advance; updating a local
minimum and propagating eventually is not enough, because the floor could advance during
propagation. Note the order the sequence actually permits: **the proxy cannot filter first**,
because `F` is known only at the authority and arrives in the reply. So:

1. **accumulates `p`** as transactions enter the batch (`commitBatcher`), over those admitted
   into it rather than over requests it rejected there;
2. **sends `p` as a proposal** on the request it already makes — always, since a local copy of
   the installed minimum can be stale in the direction that matters (§4.2a);
3. the authority, in one non-suspending transition, computes `F`, reads `C` from its own state,
   installs `I = min(C, max(p, F))` when the source is absent or `I < C`, and **replies with
   `F` and `I`**;
4. on the reply the proxy **rejects individually** every transaction with `read_snapshot < F`,
   and admits the survivors, which `I` already covers; the batch is never failed wholesale, and
   nothing is retried;
5. records the **exact minimum over the survivors** in its pending-batch deque — a different
   value from `p`, and the one that governs future raises;
6. **withdraws** the batch's contribution once the batch reaches a terminal state — the only
   place a contribution rises.

The commit proxy is the natural custodian: it already holds per-batch state and already
computes a resolver-visible horizon (`CommitProxyServer.cpp:2104`, and the proxy set at
`:1008`).

### Why the combined minimum stays monotone

`oldestInFlightCommitRV` need **not** be monotone in isolation — a newly accepted request
may carry an RV below every other in flight. Monotonicity comes from the *overlap*:

1. before handoff, the client registration contributes a value ≤ `r`;
2. the proxy's pending minimum covers `r` **before** that contribution may disappear;
3. during the transition both are present;
4. afterwards the proxy's minimum covers `r` until the batch is terminal;
5. therefore no contribution ≤ `r` ever vanishes while `r` is still needed;
6. and a request whose `r` is below the authoritative effective floor is rejected.

Publication is then monotone by construction: `publishedFloor = max(previous, derived)`.

### Identity: not needed here

**The commit request carries no new identity fields.** `read_snapshot` is already on the wire,
and admission is decided against the authoritative effective floor by the conditional install
(§4.2a), so
`CommitTransactionRequest` needs no `clientID`, `leaseGeneration`, `registrationID` or lease
capability. An earlier formulation added them so the proxy could validate a specific
registration during the handoff; that model is withdrawn, and the wire change goes with it.

Identity still matters on the **GRV path** — `GetReadVersionRequest` is a
`PublicRequestStream` (`GrvProxyInterface.h:227`, `:120`) and lease entries must be protected
against collision, impersonation and stale incarnations (§11). That is a separate concern
from admitting a commit.

What must be checked against authoritative state is the **floor**, not a lease: the compare
happens inside `conditionalInstallProxyMinimum`, at the authority that publishes it, never
against a possibly-stale broadcast watermark.

### Proxy failure does not withdraw coverage

The dangerous case: the proxy's minimum enters the reduction, the commit is admitted, the
batch is dispatched to the
resolvers, and **dies before all replies arrive**. If the authority drops the dead proxy's
source from the reduction merely because the process disappeared, the floor advances
while a request holding `r` is still alive in a resolver.

> **The death of the process publishing a minimum is not the termination of the work that
> minimum covers.** Before its contribution is withdrawn, one of these must hold: (1) a successor
> inherits the generation-fenced pending minima; (2) every associated resolver request is
> fenced or proven unable to complete; or (3) a conservative generation barrier retains the
> proxy's last published minimum until all requests of that generation are guaranteed
> closed. **Process disappearance alone is never withdrawal evidence.**

What today's architecture gives you, and why this is tractable:

* Option (1) is **not expressible**: a single proxy failure gets no successor — the whole
  generation is torn down (`ClusterRecovery.cpp:461–472`).
* Option (2) holds **structurally** once recovery completes: the resolvers that held the
  requests are gone with their conflict sets, and `recoveryTransactionVersion`
  (`:1387`, `ServerKnobs.cpp:156`) puts every pre-recovery read version beyond the window.
* So the requirement reduces to **option (3)**: a conservative barrier from proxy failure
  until the old generation provably cannot complete a validation that influences a durable
  decision.

**Release is event- and fencing-driven, never timer-driven.** `TLOG_TIMEOUT` contributes to
failure *detection* latency (`ClusterRecovery.cpp:466`); it does not bound recovery
completion and does not license releasing the barrier. Old resolvers keep running until
`checkRemoved` sees `recoveryCount` advance and finds itself absent from the new set
(`Resolver.cpp:832–843`) — their replies may physically arrive, but recovery locks the
previous epoch's TLogs and fixes its `epochEnd` (`LogSystem.cpp:420`), so no old-generation
commit can be made durable. *That* is the proof the contribution may be withdrawn. Because it
is a scalar, the conservative option is cheap: the aggregator simply keeps the dead proxy's
last published minimum — there is nothing per-transaction to inherit or reconstruct.

**Forward-looking:** if FDB ever gains single-proxy replacement without full recovery, this
reduction fails and inheritance or explicit fencing becomes mandatory. Any such change must
revisit this.

### The property to assert at every boundary

```
HANDOFF_ACCEPTED ⇒ covering minimum in the reduction
                 ∨ request generation fenced from influencing a durable decision
                 ∨ terminal validation outcome already known
```

Note "fenced from influencing a durable decision", not "fenced from validation": an old
resolver may still execute and still produce a reply — what must be impossible is that any
authorized party consumes it to decide.

---

# Part IV — Security, aggregation, and the honest trust boundary

## 11. Adversarial input

Both relevant streams are public and unverified (§5). Two distinct attacks:

**Retention denial of service.** An unbounded client-supplied `clientFloor` pins
cluster-wide history with a single malformed field. The proxy must **clamp** the reported
floor to a policy maximum before it enters the reduction, administratively bounded
(per-tenant or a cluster knob). This clamp is also the natural enforcement point for
retention budgets.

**The clamp must be a visible admission, never a silent substitution:**

> A reported floor older than the administratively permitted window is **not** silently
> accepted under a newer value. The proxy either rejects the registration or returns the
> effective *granted* floor, so the client can fail or revoke the affected snapshot
> explicitly. Only the granted floor enters the reduction. Storage servers independently
> reject reads below their actual retained capability.

**Premature reclamation — worse, and the clamp does not stop it.** If `clientID` and
`leaseGeneration` are free-form client fields, a buggy or hostile client can update
*another* client's entry with a *newer* floor, causing unsafe **under**-retention. (A forged
*low* floor only over-retains.) Therefore:

* `clientID` bound to an authenticated or unguessable client incarnation, never a
  client-chosen label;
* `leaseGeneration` updates ownership-checked, old generations rejected;
* updates monotone within a generation;
* a per-tenant/per-process cap on identity creation.

This matters even in a cluster that trusts its clients: it also covers ID collisions,
retries and plain bugs.

**The trust boundary, stated honestly.** Server-side fencing stops one incarnation from
modifying another's registration, and admission bounds stop retention DoS. Neither proves a
client library reported the *complete* minimum of its own active read versions. **This is
the same boundary FDB already relies on**: read and write conflict ranges are supplied by
the client and never verified against the reads actually performed
(`Transaction::addReadConflictRange`, `NativeAPI.cpp:3961`); a client that omits one
silently forfeits serializability. A client that advances its own floor incorrectly forfeits
protection for the omitted snapshot and gets `transaction_too_old` — self-inflicted, and the
containment is demonstrated: reads below retained capability fail, and a read-write commit
whose read version fell below the authoritative effective floor cannot complete the handoff
(§10 step 2).

## 12. Aggregation hierarchy

```
client library → GRV proxy (min over valid leases) → the generation's sequencer
              → globalOldestClientRV → consumer-scoped publication → derived floors (§4)
```

The sequencer holds the per-source entries, computes the minima and publishes the floors
(§6.3). **The Cluster Controller is not the ordinary aggregator**; it takes part only in the
handover and fencing between generations.

The property is *not* that no component tracks transactions — once §10 exists, commit
proxies track a minimum over their pending batches. The real property is that **the hierarchy
transports minima, not a cluster-wide transaction list**: clients track their own active read
versions, proxies reuse the batch detail they already hold, and the sequencer aggregates
per-source minima. No component holds a global registry of transactions.

**Proxy-generation barrier:** a single proxy replacement must not invalidate long readers. **A
lease period can only justify releasing a GRV-proxy contribution, never a Commit Proxy's**,
which is released at the generation fence rather than on a timer. Across a generation change
the Cluster Controller either hands the previous generation's conservative per-source state to
the new sequencer or withholds that sequencer's authority to advance until the old generation
is fenced; publishing a *later* floor before the fence completes is not permitted either way.

## 13. Set expectations: Phase A ships as a retention no-op

**The efficiency dividend is negotiation-gated.** In a mixed-version cluster the floor does
not advance at all, because non-participating clients are invisible potential committers —
hence the legacy formula `resolverValidationFloor = currentVersion − W_commit`. And in every
mode the `currentVersion − W_commit` cap guarantees **the resolvers never retain more than
today**.

So: this work is safe to ship early and its benefit is conditional on adoption. Say so in
the PR description, or a reviewer will reasonably ask what it buys.

---

# Part V — Consumers, tests, and order

## 14. Wiring the consumers

**Resolver** (`Resolver.cpp:359`). Replace the constant with the received floor, clamped as
in §8.4. The value feeds two things that **must not be separated**: `addTransaction`
(`:361` → `tooOld` at `ConflictSet.cpp:805` → `TransactionTooOld` in the reply at `:381–384`
→ `transaction_too_old` at the proxy, `CommitProxyServer.cpp:2043`), and `detectConflicts`
(`:374` → the sweep at `ConflictSet.cpp:986–993`). **You may only admit what you can still
validate.**

**Commit proxy** (`CommitProxyServer.cpp:2104`). Same floor. It coalesces `keyResolvers`
(`ProxyCommitData.h:293`) every `RESOLVER_COALESCE_TIME` (`:2100–2110`); if the resolver
retains more history than this map does, multi-resolver transactions lose the mapping for
old read versions.

**Storage server** (`storageserver.cpp:10453–10462`). `maxVersionsInMemory`
(`:10444–10447`) becomes `version.get() − storageReadFloor`. Keep every existing clamp,
especially `proposedOldestVersion = max(…, oldestVersion.get())` (`:10460`) — that is the
monotonicity the rest of the server relies on. The read-path contract is unchanged:
`storageserver.cpp:2100–2102` still throws `transaction_too_old` below `oldestVersion`.

**The resolver's sweep must be fixed in the same release, and it does not depend on F1–F3.**
A demand-driven floor *plateaus* whenever an old reader holds it, and the sweep is gated on
the floor advancing (`ConflictSet.cpp:986`) — measured: **8.3× retained population** under
50-batch plateaus, still rising at the last sample. Its budget is also tied to write volume
(`3·|write ranges| + 10`, `:991`), so a cluster that stops writing drains at ≤ 10 nodes per
batch — extrapolated **~52 600 batches**. Details and the patch shape are in
`01-resolver.md` §A.3; the point here is that **the floor work creates the
condition that exposes both defects.**

## 15. Knobs

Style: `init( NAME, value );` in `ServerKnobs.cpp` (see `:152–166` for the window knobs,
`:925` for the resolver's own).

* `FLOOR_TRACKING_ENABLED` — kill switch restoring the constant exactly.
* `FLOOR_LEASE_DURATION` / `FLOOR_LEASE_RENEW_INTERVAL` — §6.5.
* `FLOOR_MAX_RETENTION_VERSIONS` — the §11 clamp; the administrative ceiling.
* `RESOLVER_FLOOR_STALENESS_LIMIT` — how long a proxy's floor may go un-refreshed before
  the consumer falls back to the constant.

**`MAX_WRITE_TRANSACTION_LIFE_VERSIONS` exists twice**: `ServerKnobs` and a client copy
(`ClientKnobs.cpp:235`, declared `Knobs.h:136`, used at `NativeAPI.cpp:4756` to decide when
to stop retrying). Extending the server window without telling the client means clients
abandon transactions the cluster would still accept — a client-visible change that belongs
in its own PR.

## 16. Failure injection — the test list

Each becomes a simulation test. **There is no single invariant over "the floor" — the derived
floors have different obligations, and conflating them asserts something false.** The four to
write as executable invariants:

```
globalOldestClientRV      never exceeds an RV still covered by a valid lease,
                          except through explicit, visible revocation
globalValidationDemand    never exceeds an RV still covered by a client
                          contribution or a Commit Proxy contribution
resolverValidationFloor   = max(globalValidationDemand, currentVersion − W_commit),
                          and is monotone
storageRetentionFloor     may exceed storageReadFloor only through the explicit
                          revocation protocol
```

The handoff property constrains the **demand component**; it does not claim that a derived
floor can never pass a reader's read version. `resolverValidationFloor` legitimately does so
via the `currentVersion − W_commit` term, and `storageRetentionFloor` does so under pressure
through priced revocation — both by design.

Write them as executable invariants — that is the project's stated validation language.

Handoff boundary (§10):

* proxy dies after its updated minimum enters the reduction but before admitting the commit;
* dies after the handoff, before dispatching to the resolvers;
* dies after dispatching to only some of them;
* all resolvers reply, but the proxy dies before withdrawing or publishing the withdrawal;
* the global floor advances between the proxy's admissibility test and the arrival of its
  installation — the authority must install `min(C, max(p, F))` and report `F`, so the batch is
  never admitted wholesale under a floor that passed it, never failed wholesale either, and
  never has its earlier coverage raised away;
* a late acknowledgement of a raised contribution arrives after a lower one was installed —
  discarded, never overwriting a value installed since;
* a batch retires whose entry already left the deque through `pop_back` — the minimum must
  not change;
* the FIFO retirement assertion — a test that must never fire;

Source-state lifecycle (§4.2a) — four cases the load tests cannot reach, since the counter of
first materialisations is legitimately zero in any window with no new source:

* **first admission with `p == currentVersion`** — materialises an entry that then stays fixed
  as the version advances, rather than writing nothing and letting the implicit minimum follow
  `currentVersion` away from the pending batch;
* **retirement of the last batch** — turns the contribution absent, so the reduction follows
  `currentVersion` again instead of pinning at the value current when the source went idle;
* **an old raise after that retirement** — refused by revision, never resurrecting a fixed
  entry behind an empty source;
* **two reordered raises** — the second one applied invalidates the first one's compare;
* **a retirement and an admission carried by the same RPC**, with the compare both accepted and
  refused — the raise must be resolved first and the admission must run regardless, or
  piggybacked raises invalidate themselves under continuous traffic;
* a client's commits land on two different proxies — neither proxy sees the other's, and the
  global minimum must still cover both;
* a successor generation appears while replies from the previous one are still arriving.

Lease and client lifecycle (§9):

* client dies holding a lease → the floor advances after expiry, not before;
* a registration is granted but its installation is lost → the client must not use the read
  version, and the reply must not have been sent before the acknowledgement;
* a client older than `W_commit` reaches a GRV proxy that never saw it → its install is guarded
  by the **storage admission** floor, not by the resolver's effective floor, and succeeds while
  reclamation has not been authorised past that version;
* Storage still physically holds `r` but the admission floor has passed it → the install must
  **fail**, because publication cannot retreat; granting here is the bug this guard exists to
  prevent;
* the refusal reaches the client → explicit, visible revocation, never a silent substitution
  (§11);
* a renewal is lost → the client stops using its snapshots before the server can expire the
  entry, never after;
* clock drift at the stated bound → the client's window still closes before the server's;
* **out-of-order GRV replies**, run across combinations of request flags and expecting the same
  numeric outcome every time: with `latestGrantedRV = 100`, requests A and B concurrent, reply
  B = 120 arriving before reply A = 110 → `RV(B) = 120`, `RV(A) = 120`, `latestGrantedRV = 120`,
  and `clientOldestActiveRV` never decreases;
* a reply **from an earlier generation** → does not participate in the maximum, and is rejected;
* a reply arriving **outside its lease window** → discarded or re-registered before use,
  regardless of how its version compares;
* `ssVersionVectorDelta` is not re-applied because another reply supplied the maximum, and
  `metadataVersion` is never paired with a version it does not describe;
* client submits a commit and dies → its Commit Proxy contribution survives until validation
  completes or the generation is fenced, so disappearing client demand alone cannot strand it;
  the ordinary `currentVersion − W_commit` bound may still make it too old, exactly as today;
* GRV proxy dies holding registrations → coverage reconstructed or conservatively held;
* idle client with a stale multi-copy lease (if §6.2 chose multi-copy) → refresh or expiry,
  never silent divergence.

Adversarial (§11):

* client reports a floor below the administrative ceiling → visible rejection or granted
  floor returned, never silent substitution;
* client forges another's `clientID`/`leaseGeneration` → rejected by ownership check;
* client requests a very old read version and never finishes → the ceiling binds and *that*
  client fails, not the cluster.

Version compatibility (§5, §13):

* mixed-version cluster: legacy clients never report, the floor never advances, behaviour
  is identical to today;
* new server, old encoding: absence is distinguished from a zero floor.

## 16a. The validation boundary of this harness

F1 established what the harness can and cannot show, and the same boundary applies to
everything the floor adds afterwards.

| Direction | Evidence |
|---|---|
| current → current | Simulation on `5ad1f15ac9` (clean tree, binary sha256 `a0e3da67…`), `tests/fast/CycleTest.toml`, seeds 101/202/303, buggify on, all three passing. Counters read **per instance and then aggregated**, never as a maximum across instances. **Resolver**, summed over 17 instances: `RetentionFloorFromRequest` 6704, `RetentionFloorDerivedLocally` 0 on every one of them, `ConflictSetSweepsRun` 6590, `ConflictSetSweepsSkippedFloorUnchanged` 114. **Commit Proxy**, 29 instances: `UnsampleableReadVersions` 0. **GRV proxy**, 19 instances: `ProcessTransportPeers` peaks at 25 — a per-process gauge of transport peers, reported as such and never summed or read as a client count. |
| older → current | Unit test: a legacy payload, really deserialized, handed to the real selection function, which takes the fallback branch. |
| current → older | Unit test: the older peer ignores the unknown field and keeps every field it knows. |
| **mixed-version RPC** | **Not covered.** A simulated cluster runs one binary, and restarting tests *replace* the cluster rather than overlapping versions — phase one runs entirely on the old binary, phase two entirely on the new — so no old Commit Proxy ever talks to a new Resolver in this harness. |
| production | `RetentionFloorDerivedLocally` detects a legacy sender or an absent field **only where the receiver is new**. It cannot observe the opposite direction; a new Commit Proxy talking to an old Resolver increments nothing, because the old Resolver has no such counter. |

**Histograms are not observable in simulation, and that is a property of the harness.**
`GetHistogramRegistry()` resolves against `g_network->global(INetwork::enHistogram)`
(`Histogram.cpp:33–41`), so the registry is per-network; under Sim2 each simulated process has
its own, while `histogramReport()` runs once in the orchestrating process
(`fdbserver.cpp:2131`). No Commit Proxy histogram is emitted — not the new ones, and not the
pre-existing `Resolution` or `CommitBatchQueuing`. They were therefore validated on a real
single-process cluster, `configure new single memory`, binary `5ad1f15ac9` (sha256 `a0e3da67…`), `--knob-histogram-report-interval=20`, 75 s of load followed by 35 s of drain
with the server still running so the last samples are published:

| Histogram | Reports | Non-empty buckets | Samples |
|---|---|---|---|
| `Resolution` (pre-existing control) | 6 | 6 | 76 |
| `CommitBatchQueuing` (pre-existing control) | 6 | 1 | 76 |
| `ReadVersionAgeAtCommit` | 6 | 10 | 40 |
| `BatchPipelineEntryToValidation` | 6 | 3 | 76 |

**A diagnostic identity holds per Resolver instance, in all 17 of them:**

```
RetentionFloorFromRequest + RetentionFloorDerivedLocally
    == ConflictSetSweepsRun + ConflictSetSweepsSkippedFloorUnchanged
```

6704 + 0 on the left, 6590 + 114 on the right. Every processed request selects exactly one
floor source, every batch yields exactly one sweep opportunity, and that opportunity ends as
either run or skipped. The `DerivedLocally` term matters even though it is zero here: state the
identity without it and it stops holding precisely when F2 exercises the fallback. It is *not*
a production assertion — an exception or actor cancellation between the two points can leave
the accumulators transiently unequal — but it is a strong cross-check that no counters from
different instances were mixed, which is the mistake it was written to catch.

`ConflictSetSweepsSkippedFloorUnchanged` totalled 114, entirely within four short-lived
resolvers recruited at startup and during early recoveries; the long-lived ones are at zero,
and in the one inspected closely the counter had already reached its final value when only 9
sweeps had run. That is the **bootstrap clamp**, not steady-state repetition: while
`req.version` is still below `MAX_WRITE_TRANSACTION_LIFE_VERSIONS` the raw floor is negative,
clamps to zero, and matches an `oldestVersion` still at zero. **No steady-state baseline was
observed**, so the plateau signal is the counter's *increment* once the effective floor has
first risen above zero — `Δ ConflictSetSweepsSkippedFloorUnchanged` — never its absolute value,
which starts contaminated.

A report is a flush that had samples, not a transaction: `writeToLog()` clears the buckets
when it emits (`Histogram.cpp:137`) and skips the event entirely when empty (`:98–100`), so
`TotalCount` sums across events without double counting.

Two observations from that run, both **observations and not invariants**.
`BatchPipelineEntryToValidation` matched `Resolution` exactly, and did so again in an
independent repeat of the run — which is what makes it a control rather than a coincidence:
same point in the code, same per-batch cardinality as a metric that has been in the tree for
years. And `ReadVersionAgeAtCommit` came in *below* it, 40 against 76 — batches
carrying no sampleable client transactions dominate under this light load. **There is no
general inequality between the two**: their populations and termination points differ, so
nothing should assert a relation in either direction.

`UnsampleableReadVersions` was 0, which says the normal workload contains no excluded
snapshots — not that the guard against a malformed `read_snapshot` works. That branch is
unexercised and belongs to a unit test of the guarded computation, or to an adversarial run.

> **Real mixed-binary RPC coverage is deferred to F2 and is required before enabling
> demand-driven floors**, because the single-binary simulator and restarting tests cannot
> overlap protocol implementations. It needs a cluster of two binaries on real processes;
> role placement cannot be chosen, so it means reading the recruitment traces to confirm the
> topology actually occurred, across several attempts. In F1 a wire fault could abort Resolvers
> and cause an availability failure during a rolling upgrade, but it could not yet alter
> retention or admission semantics, and the deterministic bidirectional wire tests cover the
> serialization contract without the cost and nondeterminism of a two-binary cluster. In F2
> mixed-binary RPC becomes mandatory, because the decoded value controls admission and
> retention safety.

## 17. Build and test reference

```bash
# the project's own image; clang 19.1.5, no host toolchain needed
docker run --rm -v /data/fdb:/data/fdb -w /data/fdb/build \
  foundationdb/build:rockylinux9-latest ninja fdbserver

bin/fdbserver -r simulation -f tests/…      # the real gate
bin/fdbserver -r skiplisttest -C /data/fdb/scratch/fdb.cluster   # resolver structure regression
```

A cluster file is required even for test roles; a dummy file suffices.

## 18. Order of work

The goal is a complete implementation the upstream community can adopt — not a synthetic
floor for a benchmark. It still divides into reviewable changes, and the early ones are
behaviour-neutral, which is what lets a reviewer accept one without accepting the rest.

1. **Fix the clamp inversion in this guide** — done (§4.4); the resolver's traditional bound
   is a *lower* bound on its floor.
2. **Conflict-set footprint metrics** — the resolver publishes no size at all today
   (`Resolver.cpp:162–177`, `216–218`; `RESOLVER_STATE_MEMORY_LIMIT` at `ServerKnobs.cpp:925`
   bounds the state-transaction buffer, not the skip list). Smallest change here, stands alone
   upstream, and without it nothing downstream is observable.
3. **F0a** — only the measurements that need no protocol (§7a); in parallel with step 2, and
   neither changes behaviour. F0b ships with the code it measures.
4. **F1** — the request-carried derived floor, encoded as an `Optional` field with no
   protocol-version bump, with legacy fallback and equality asserts (§8). No aggregator and no
   broadcast.
5. **The sequencer's dark floor state** — per-source entries and their minima maintained inside
   the non-suspending stretches of `getVersion` *and* `serveGetLiveCommittedVersion`, computed
   and measured but **not acted on**: no consumed floor changes, no admission changes, and no
   observable reply changes beyond neutral diagnostic fields (§6.3). It prices the authority's
   structural floor cost and validates its placement; the coordination rate and behaviour under
   real distributions wait for F0b, when the producers exist.

   Names say what the values are, since they will outlive this step:
   `shadowCommitSourceObservation` and `shadowGrvSourceObservation` for the inputs, and
   `shadowCommitAggregate`, `shadowGrvAggregate`, `shadowCombinedAggregate` for the synthetic
   results — *aggregate*, never *demand*. **Both observations are structural placeholders at
   this step**, because neither producer exists yet: the commit-side one becomes real
   (`batchOldestReadSnapshot`) only in step 7, when the proxy sends it, and until then it
   carries `rep.prevVersion`, which has the shape of a per-source minimum and none of its
   meaning. The GRV-side one is a placeholder in a stronger sense and stays one through step 6:
   with no client registrations there is nothing to observe at all, so it carries
   `liveCommittedVersion`, which does **not** approximate retention demand and must never be
   read as if it did. The GRV source identity comes from the reply endpoint, since
   `GetRawCommittedVersionRequest` carries none and a dark step adds no wire field. An idle source can
   also leave its entry stale until the generation's cleanup — tolerable only because nothing
   consumes it, and one more reason the step stays dark.
6. **F2** — incarnation IDs, leases, renewal, expiry, administrative limits, and the
   aggregation of `globalOldestClientRV` (§9), including **`conditionalInstallClientMinimum`**
   — guarded by `authoritativeStorageAdmissionFloor`, never by the resolver's floor — and the
   lease's temporal contract; register-before-use is not closed without both.
7. **The Commit Proxy minimum** — `batchOldestReadSnapshot` over admitted commits, held in a
   monotonic deque and retired at the serialised logging transition (§4.2), plus the
   conservative pre-filter (§4.2b).
8. **The linearization** between commit admission, incorporation of the proxy's contribution,
   and floor advance (§10) — the conditional install, the acknowledgement tagging, and the
   transition run on every batch, since no local copy can decide safely whether it is needed
   (§4.2a).
9. **Generation barriers** for GRV-proxy and Commit-Proxy death (§10, §12).
10. **Mixed-version negotiation, kill switch, staleness fallback** (§13, §15).
11. **Simulation tests**: lifecycle, failures, recovery, adversarial input, compatibility (§16).
12. **Apply the floor coordinately in the resolver and in `keyResolvers`** (§14).
13. **Replace or repair the sweep**; if the current one is kept for now, add the full-lap
    metric per floor generation (§14, `01-resolver.md` §A.3).
14. **The canonical epoch SkipLists**, on a floor that is by then correct.
15. **Measure T3.1**: search cost, memory, and the frequency *and* unit cost of every
    transition and arena discard.

The **sweep repair** (`01-resolver.md` §A.3) depends on none of this and can run
in parallel from day one — it is needed *because* a demand-driven floor plateaus. It does
change behaviour, so it carries its own measurement with the T3.3 harness.

Steps 1–5 are safe in any cluster — the sequencer's floor state is dark until F2 reads it. Beyond them, what matters is not which step you are on but
**what is allowed to be switched on**:

> All F2/F3 machinery stays dark behind the feature gate. Demand-driven floors **and the Commit
> Proxy pre-filter** may be enabled only once both conditional installs, both generation
> barriers, mixed-version negotiation with its staleness fallback, and their simulation tests
> are present.

The pre-filter is named explicitly because it is the one piece that changes availability on its
own: enabled early, it can reject a live commit that the resolver would have accepted (§4.2b).

**A synthetic floor is not enough for step 14.** The prototype's question is how the structure
behaves under a *real* demand-driven floor: plateaus while a reader holds it, jumps when one
finishes, and the rate of sub-X retirements those produce. An injected schedule reproduces
only the shapes chosen in advance — the trap that invalidated the first two runs of T3.3.