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

### 4.2a The fast path: most commits need no coordination

The reduction is cheap. What is expensive is **linearising a lowered contribution against the
floor advance**, because that is a network interaction on the commit path. Distinguish:

- `localMinimum` — the exact minimum of the deque;
- `acknowledgedProxyMinimum` — the last contribution whose global installation is confirmed
  and not yet withdrawn.

```cpp
const Version effectiveFloor = std::max(lastPublishedGlobalFloor,
                                        currentVersion - MAX_WRITE_TRANSACTION_LIFE_VERSIONS);

if (batchOldestReadSnapshot < effectiveFloor) {
    preRejectAsTooOld();                       // conservative pre-filter
} else if (batchOldestReadSnapshot >= acknowledgedProxyMinimum) {
    admitWithoutCoordination();                // installed contribution already covers it
} else {
    installLowerProxyMinimumAndAwaitAck();     // the only slow path
    admit();
}
```

If `b ≥ acknowledgedProxyMinimum` the installed contribution is at least as conservative, so
**the demand-derived component of the floor cannot have passed `b`** — no round trip at all.
Read versions cluster near `now`, so this is the common case.

**The guarantee covers only that component.** `currentVersion − W_commit` can still overtake
`b` in transit: with `acknowledgedProxyMinimum = 100`, `b = 120` and
`currentVersion − W_commit = 130`, the resolver's floor is 130 even though the proxy holds a
contribution at 100. That is today's behaviour preserved — a transaction can become too old
while travelling — and it is why the resolver stays authoritative (§4.2b).

When the deque advances to a **higher** minimum, publication may be deferred:
`acknowledgedProxyMinimum < localMinimum` only over-retains, and it keeps later batches covered
without coordination.

**Installation must be conditional, not a send.** The floor can advance between the proxy's
test and the arrival of its update, so the aggregator does a compare-and-set:

```cpp
InstallResult conditionalInstallProxyMinimum(
        ProxyID proxy, Generation generation, PublicationSequence sequence,
        Version candidateMinimum, Version requiredReadVersion) {
    // runs at the authority that publishes the floor
    const Version authoritativeEffectiveFloor =
        std::max(publishedGlobalValidationDemand,
                 authoritativeCurrentVersion - MAX_WRITE_TRANSACTION_LIFE_VERSIONS);
    if (authoritativeEffectiveFloor > requiredReadVersion) return TooOld;   // proxy rejects
    installOrLowerSourceMinimum(proxy, generation, sequence, candidateMinimum);
    return Installed;                                                        // then acknowledge
}
```

For a batch: `candidateMinimum = requiredReadVersion = min(read_snapshot)` over the commits
that survived the pre-filter, so every admitted commit satisfies
`read_snapshot ≥ candidateMinimum ≥ authoritativeEffectiveFloor`.

**Compare against the effective floor, not the demand minimum alone.** The resolver's floor is
`max(demand, currentVersion − W_commit)`; testing only the demand component would admit
batches the age bound has already overtaken, which then travel to the resolver just to be
rejected. Winning the install means admissible *at that instant* — the time term can still
overtake the batch afterwards, exactly as today.

Without this the proxy's own test is a time-of-check/time-of-use gap and a batch can be
admitted under a floor that has already passed it.

**Acknowledgements carry `{proxyGeneration, publicationSequence, coveredThroughBatch}`, and
stale ones are discarded**, so a late acknowledgement of a *raise* cannot overwrite a lower
minimum installed since. The fast path reads the minimum known to be still installed globally,
never merely the last one sent.

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
