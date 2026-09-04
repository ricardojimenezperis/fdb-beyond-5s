# Floor Tracking — the Oldest Active Read Version

*Status: design frozen at the protocol level, reviewed against `apple/foundationdb` main @
`a443d3ee60`. **One** implementation choice remains deliberately open (§8): the lease
parameters. Watermark transport, lease placement and the conditional-install authority are
decided for v1. Three ordering
requirements are closed as **rules**: the initial registration (§3), the lease's temporal
contract (§2), and the commit lifecycle handoff (§4a) — the last of which was what blocked
Resolver Phase A (`01-resolver.md` §3); its linearization mechanism is decided in §8.4 — the
generation's sequencer. Claims
about current FDB carry `file:line`.*

## 1. Why this protocol must exist

**Two populations need protection, and they are aggregated separately.** Live transactions
still executing in client libraries, and commits already received by a Commit Proxy whose
validation has not finished. Each is reduced to a minimum by the component that already holds
the detail; only aggregated minima travel over the network.

`globalOldestClientRV   = min { clientOldestActiveRV over clients with a valid lease }`
`oldestInFlightCommitRV = min { commitProxyOldestInFlightRV over Commit Proxies }`

Storage derives its read-retention demand from the client minimum alone: a pending commit
needs conflict history, not historical *values* — its reads already happened. **Resolver
validation needs both**, because after the lifecycle handoff of §4a it must also cover
accepted-but-unvalidated commits whose client-side lifetime may already have ended —

`globalValidationDemand = min(globalOldestClientRV, oldestInFlightCommitRV)`

— and the handoff guarantees continuous coverage between the two sources. **Retention is
therefore not one number but two derivations from a common reduction:** storage consumes the
client observation; the resolver additionally consumes the Commit Proxies' pending minimum.

Today's FoundationDB does not produce the client-side observation either. **Verified:** `GrvProxyData`
(`fdbserver/grvproxy/GrvProxyServer.cpp:187–211`) keeps no per-client state of any kind, and
`GetReadVersionRequest` (`fdbclient/include/fdbclient/GrvProxyInterface.h:65–141`) carries no
client identity — only `transactionCount`, flags, priority, tags and `maxVersion`. Nothing
server-side observes a transaction's lifetime. **The only place that knows it is the client
library.**

## 2. Client side: a monotonic minimum

The client library already knows the read version of every transaction it has alive, so it
keeps the detail and publishes only a scalar:

```
activeReadVersions = { RV of active transactions }
                   ∪ { RV of transactions whose commit was sent but has no terminal result }

clientOldestActiveRV = min(activeReadVersions)
```

and publishes `{clientID, leaseGeneration, clientOldestActiveRV}` — **never one entry per
transaction**. When `activeReadVersions` is empty the client reports
`max(clientOldestActiveRV, latestGrantedRV)`, and the reported value never decreases.

The library can advance its minimum precisely because it holds the detailed set:

```
active RVs: {100, 130, 170}   →  publishes 100
transaction with RV 100 ends
active RVs: {130, 170}        →  publishes 130
```

Monotonicity is load-bearing: when the oldest transaction ends the floor advances, and that
version is irrevocably abandoned by this client. Published values may lag
(`reportedMin ≤ localMin`); a *lagging report* only over-retains, never under-retains.

**A lost renewal is the opposite case, and it is not covered by that sentence.** If a renewal
is lost or delayed the *server* may expire the entry while a live client still believes its
snapshot is usable — under-retention, and a safety failure rather than an availability one. The
lease therefore needs an explicit temporal contract:

- an acknowledgement grants a **defined usage window**, and the client may use versions of that
  generation only within it;
- the client renews with margin, well before the window closes;
- if no acknowledgement arrives before its own conservative deadline, the client **stops using
  and locally revokes** the snapshots of that generation, rather than assuming the server still
  holds them;
- a GRV reply that arrives too late to supply a full window is discarded, or re-registered
  before use.

Safety comes from the direction of the inequality: **the server's lease must outlast the
client's self-imposed window**, measured from *before* the request was sent and allowing for
bounded clock drift. Then a lost message costs availability — the client stops early — and
never lets the server reclaim under a live reader.

This is the one place the protocol depends on time rather than on fencing, and it is worth
stating plainly: elsewhere (barrier release, §4a) timers are explicitly *not* evidence. The
same guarantee could be obtained by a non-temporal fencing mechanism instead.

**Decision for v1: the temporal *rule* above is frozen, not left open.** Its parameters are
not — the exact inequality between server lease duration, client window and maximum clock
drift belongs to §8.1, and is unset until the lease instrumentation sizes it — which ships
with the leases themselves, not before them. The rule is the standard lease
argument, and its failure direction is availability rather
than safety; leaving it open would block Phase A on a decision that already has a working
default. A non-temporal replacement remains a possible later change — it is not one of the
open choices in §8. Without one of the two, register-before-use protects only the instant of
grant, not the declared lifetime of the transaction.

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

**Read-only transactions are covered here and nowhere else.** They never reach a Commit
Proxy, so this client-side minimum is the only thing protecting their snapshots — which makes
it indispensable for the *storage* side, the consumer that exists to serve them
(`storageReadFloor`, §6).

**Manually set read versions bypass the sensor.** `Transaction::setVersion(v)`
(`fdbclient/NativeAPI.cpp:3594–3603`) validates only `v > 0` and that no read version is
already set; it contacts no proxy and records
`trState->readVersionObtainedFromGrvProxy = false` (`:3602`). Such a transaction is invisible
to the aggregation of §5. Requiring `v ≥ clientFloor` bounds this **only within one client
process**, and a freshly constructed `DatabaseContext` starts at `clientFloor = 0`, so the
rule cannot be made global. Two resolutions are admissible:

- **(a) Register on set** — treat `setVersion` as a registration point, which makes the rule
  enforceable but adds a round trip to a path that has none today; or
- **(b) Document the status quo** — a manually set read version carries today's semantics
  (the cluster was never obliged to still have it) and receives **no** retention guarantee
  from this protocol. It must therefore not be used to reach the extended read window.

**Decision for v1: (b).** Manually supplied read versions do not acquire retention
protection. `setVersion(v)` retains today's best-effort semantics and may be used only while
`v` remains within the Storage Server's actually retained capability — advertised explicitly
once `02-storage.md`'s `minimumRetainedVersion` exists (it is a design concept today, absent
from the code). Access to the *extended* window requires a read version obtained and leased
through the protocol of §3. Register-on-set (a) remains a possible later extension; it is not
an open choice for v1, because it would add a round trip to a path that has none.

This has to be written down precisely because the extended window makes "set an old version
and read" newly attractive.

## 3. Transport: piggyback on GetReadVersion, lease when idle

The normal path adds **zero messages**: every `GetReadVersion` carries the client's
registration. `GetReadVersionRequest` is flatbuffers-serialized
(`GrvProxyInterface.h:126–141`), which decides how that is encoded:

> **Structural compatibility is not the same problem as semantic activation.** FlatBuffers
> carries the schema, so an added field needs no protocol gate; what it needs is to be an
> `Optional`, because a field an older sender omitted is deserialized as its **wire default**
> and not as the member's initializer. A sentinel value therefore cannot represent absence.
> Cluster-wide negotiation gates the *behaviour* — whether the floor may be demand-driven at
> all (§6) — never the physical presence of the field.

So the registration travels as a single optional unit rather than three loose fields:

```cpp
Optional<ClientFloorReport> clientFloor;
struct ClientFloorReport {
    ClientID clientID;
    Version  clientOldestActiveRV;
    Generation leaseGeneration;
};
```

Wrapping the whole unit is deliberate: separate `Optional`s would admit a floor without the
generation that fences it. *This was learned the hard way — the first implementation of the
resolver's carried floor used a sentinel and split its `serializer(...)` call to imitate a
gate, which left the entire table unpopulated. Mixed-version behaviour must still be covered
by tests in both directions.*

Explicit traffic appears in exactly one case: a client holding a long-lived read version while
no longer requesting new ones sends a periodic `RenewOldestReadVersion`. The new traffic is
generated precisely by the transactions that use the new capability.

Bootstrap: for a client with no active transactions, the GRV proxy registers the chosen
version as the client's floor *before* replying, so a read version is protected from the
instant the library receives it. **This is register-before-use, and Resolver Phase A depends
on it** (`01-resolver.md` §3): it is what makes an empty population safe to reclaim
against (§5).

**Registering locally is not enough — it is the same race as the commit handoff, one step
earlier.** If the proxy chooses `r`, records it locally, and lets the contribution propagate
eventually, the authoritative floor can advance past `r` before the contribution arrives, and
the client then receives and uses a read version nothing protects. The initial installation —
and any later update that *lowers* a source's minimum — therefore needs the same authoritative
primitive as the Commit Proxy side (§4a):

```
conditionalInstallClientMinimum(sourceGeneration, publicationSequence,
                                candidateMinimum, requiredReadVersion,
                                authoritativeStorageAdmissionFloor)
```

installing atomically, with **the GRV reply sent only after the acknowledgement**. It may share
machinery with `conditionalInstallProxyMinimum`; what it may not be is eventual propagation.
*Zero added client messages does not mean zero internal coordination.*

> **The two installs must not share a guard.** A client registration protects *historical
> values* in Storage, where the entire point is to allow
> `clientOldestActiveRV < currentVersion − W_commit`. Guarding it with the resolver's effective
> floor would make any reader older than `W_commit` unable to reinstall its contribution — on
> landing at another GRV proxy, or during recovery — which destroys the extended read window
> this project exists to provide.
>
> **Nor is physical retention the right guard.** "The bytes still exist" is not "it is still
> lawful to promise them":
>
> ```
> publishedGlobalOldestClientRV      = 200      // already published, and monotone
> authoritativeStorageRetentionFloor = 100      // Storage happens to still over-retain
> candidateMinimum                   = 150
> ```
>
> A guard against physical retention lets this install win, yet publication cannot retreat:
> `publishedFloor = max(200, 150) = 200`, so Storage remains authorised to reclaim to 200 and
> the lease was granted with no real protection. An advance order to 200 may even be in flight
> already. The authority is therefore **`authoritativeStorageAdmissionFloor`** — the greatest
> reclamation floor already made irreversible or authorised/published to any Storage Server:
>
> | Install | Guarded against | Outcome |
> |---|---|---|
> | `conditionalInstallClientMinimum` | `authoritativeStorageAdmissionFloor` | wins only if `requiredReadVersion ≥` it — a client's registration is refused outright when reclamation has already been authorised past that version, and the refusal is visible (§7a/§11) |
> | `conditionalInstallProxyMinimum` | `authoritativeResolverEffectiveFloor = max(globalValidationDemand, authoritativeCurrentVersion − W_commit)` | never fails: it returns `{F, I}` with `I = min(C, max(p, F))`. The *exact* proposal is taken only when `p ≥ F`; below it, conservative coverage is installed and the proxy rejects individually |
>
> Physical `minimumRetainedVersion` (`02-storage.md`) remains useful for best-effort
> `setVersion()` and for diagnostics; it is not authority to grant a new guarantee.
>
> *The alternative — reopening the demand floor whenever every Storage Server can prove it
> still holds `r`, cancelling any advance already dispatched — needs a new distributed protocol
> and breaks the simplicity of I3. Not justified.*
>
> **This composes with the lease contract (§2).** If another valid copy still protects the
> client, the admission floor cannot have passed `r`. If every copy expired, the client should
> already have revoked locally, and refusing reinstallation is the correct answer. During GRV
> proxy failure the generation barrier prevents the admission floor from advancing before
> coverage is rebuilt, **or** until every client usage window that generation could authorise
> has conservatively expired (§2). Note this is the *client* side, where the temporal contract
> already bounds authorisation; it does not make a timeout evidence on the Commit Proxy side,
> where release stays fencing-driven (§4a).

**When coordination is needed.** What is installed is not the freshly granted read version but
`candidateMinimum = min(clientOldestActiveRV, newlyGrantedRV)`, so a new *grant* does not imply
a new *contribution*. The rule is exactly `candidateMinimum ≥ acknowledgedSourceMinimum` for
that copy — the recency of `newlyGrantedRV` proves nothing by itself. Under multi-copy leases
(§5) a client already holding an old read version can land on a GRV proxy that has never seen
it and lower that proxy's minimum sharply, so:

- a renewal reaching a copy that already exists does not lower it, and is free;
- a renewal or GRV that *creates* a new copy can lower it, and is not;
- another acknowledged copy may in principle demonstrate continuous coverage, but exploiting
  that requires **generation-fenced evidence** of it; without such evidence, take the slow path.

## 4. Leases, not unregistration

`commit/cancel/destructor → unregister` fails on the one case that matters: a crashed client
sends nothing. Each GRV proxy keeps, per client process, `{minRV, leaseExpiration}`; an
expired lease removes the client and recomputes the proxy minimum. A dead client over-retains
for one lease timeout. Correctness never depends on promptly detecting death.

> **Caution.** The phrase "`commit` → unregister" above describes the *rejected* design. It must not be read as licensing release of the registration at commit
> **submission** — see §4a.

## 4a. Commit lifecycle handoff — the gap that blocked Phase A

Defining liveness as "transactions still alive" without fixing the boundary at commit
submission leaves a race:

> A client submits a commit with read version `r`, then the transaction ends client-side (or
> the process dies and its lease expires). `clientFloor` advances past `r`, the watermark
> propagates, and the Resolver floor advances past `r` — while the commit request is still in
> flight. The Resolver reaches the request only after its history at `r` is gone.

**What actually goes wrong, precisely.** Today admission and retention are the *same value*:
`newOldestVersion` (`fdbserver/resolver/Resolver.cpp:359`) is passed both to `addTransaction`,
where it decides `tooOld` (`ConflictSet.cpp:805`), and to the floor advance (`:986`). While
they stay coupled, a floor that overtakes `r` makes the request **rejected**, not falsely
accepted — the failure is a *spurious* `transaction_too_old` for a commit that was legitimately
in flight, which is an availability regression (a lease lost to a GC pause or a partition
aborts a live client's commit), not a serializability violation. **Decouple them, however —
admission at `now − W_commit` while retention follows the dynamic floor — and the same race
becomes a false accept.** Both outcomes are unacceptable, and both are removed by the same
rule.

**A client-side hold is useful but insufficient.** A live client may well keep the
transaction in its active minimum until the commit reply — that is the normal case, and it
helps. But a *crashed* client stops renewing, and its lease may expire while an
already-received commit is still being validated. Server-side coverage is therefore required
after admission regardless of well-behaved client behaviour.

The rule is the mirror of register-before-use:

> **Handoff-before-release.** Before an accepted commit request can cease to be covered by its
> client lease, **server-side** retention responsibility must already have been established for
> its read version, and it remains until conflict validation completes. Client survival or
> lease renewal after acceptance is not required for safety.

### Three states, not two

Commit *submission* is not the handoff — a message in flight can be lost.

`CLIENT_COVERED` → `HANDOFF_ACCEPTED` → `VALIDATION_COMPLETE`

- **`CLIENT_COVERED`** — only the client's lease guarantees retention.
- **`HANDOFF_ACCEPTED`** — the commit's `read_snapshot` is covered by the Commit Proxy's
  pending minimum, and that minimum participates in the reduction. From here, client death or
  lease expiry is harmless.
- **`VALIDATION_COMPLETE`** — the batch is terminal; its contribution is withdrawn.

> Commit submission alone does not transfer retention responsibility. The handoff completes
> only when the proxy's updated minimum is in the reduction. Until then the client lease
> remains responsible.

**What `HANDOFF_ACCEPTED` promises, exactly.** It removes the race that the demand-driven
floor introduces: after it, the disappearance of client demand cannot strand the commit. It
does **not** promise that an admitted commit will never receive `transaction_too_old` — the
ordinary `currentVersion − W_commit` term can still overtake it in transit, exactly as today.
The handoff closes a new hole; it does not extend the current maximum lifetime of a commit.

### The Commit Proxy contributes an aggregated minimum, not per-transaction pins

**No new per-transaction server state is created.** A commit that reaches a Commit Proxy is
already held in that proxy's existing batch structures until validation finishes; the floor
protocol reuses that detail rather than duplicating it. What the proxy publishes is a scalar:

```
batchOldestReadSnapshot      = min(read_snapshot of the commits admitted into that batch)
commitProxyOldestInFlightRV  = min(batchOldestReadSnapshot over its non-terminal batches)
oldestInFlightCommitRV       = min(commitProxyOldestInFlightRV over all Commit Proxies)
```

with `min(∅) = currentVersion` as everywhere else. The per-batch value is computed once,
during the traversal the proxy already performs while building the batch.

**Name it `readSnapshot` (or `inFlightReadVersion`), never `commitVersion`.** The history that
must survive is the history from the commit's *read* version, not the version eventually
assigned to it. Where other documents use CTS for this read version, say so explicitly.

**Batches within one proxy reach their terminal state in order — verified.** The commit
pipeline serialises batches by `localBatchNumber`: a batch cannot enter resolution until its
predecessor has (`CommitProxyServer.cpp:850`) nor logging until its predecessor has (`:867`),
with assertions that the predecessor is exactly `N−1` (`:465`, `:864`, `:868`, `:1843`); and
resolvers process a proxy's batches in version order (`Resolver.cpp:324`). Out-of-order
completion therefore does not arise, and the structure the minimum needs is a queue with
**FIFO removal** — not a random-access set.

Read versions, however, are *not* ordered by batch number: a transaction holding an old read
version can be submitted late. The minimum is therefore not the head of the queue, and this
is exactly the sliding-window-minimum problem — solved exactly by a **monotonic deque**:

```
admit batch N with value v:   while (back().value >= v) pop_back();  push_back({N, v})
retire batch N:               if (front().batch == N) pop_front()
current minimum:              front().value, or currentVersion when empty
```

Amortised O(1) on both paths, exact, with no scan, no cached minimum, no holder count, no
sentinel and no capacity policy — its size is bounded by the pending batches themselves. An
entry dropped from the back can never be needed again: it is dropped only in favour of a
*later* batch with a value no greater, and by FIFO removal that later batch outlives it.

**A batch's identity in the deque is its `localBatchNumber`, not a slot index.** An entry may
leave through `pop_back` long before its batch completes, so the batch must not hold a pointer
or index into the structure.

**Where retirement happens makes FIFO structural rather than assumed.** Placing it at the
already-serialised logging transition (`CommitProxyServer.cpp:867–869`, which asserts the
predecessor is `N−1`) inherits the ordering from the pipeline instead of depending on callback
order, and it lands *after* that batch's resolution completed — conservative, which is the
safe side. An `ASSERT(localBatchNumber == nextFloorRetirementBatch)` is still worth keeping as
an executable statement of the invariant.

> **Rejected alternative:** a preallocated ring of per-batch minima with `VERSION_MAX`
> sentinels for terminal slots, a cached minimum with a holder count, and an O(B) vectorisable
> rescan when the last holder leaves. It exists to tolerate holes left by out-of-order
> completion — which the pipeline above excludes — and it buys a scan over a few dozen 8-byte
> values, less than one cache miss, at the price of a fixed capacity that has no firm bound to
> size it against (`COMMIT_BATCHES_MEM_BYTES_HARD_LIMIT` is a byte budget, `ServerKnobs.cpp:855`;
> `RESET_MASTER_BATCHES` and `RESET_RESOLVER_BATCHES` are diagnostics, not limits).

### One authoritative transition per batch

The reduction is cheap; what is expensive would be a *separate* network interaction to
linearise a lowered contribution. There is none: the transition rides the request every batch
already makes and awaits, so it runs unconditionally. Four values must be distinguished:

- `proposedBatchMinimum` (`p`) — the minimum `read_snapshot` over what the batcher accepted;
- `authoritativeEffectiveFloor` (`F`) and `authoritativeInstalledProxyMinimum` (`C`) — both
  read at the authority;
- the **exact minimum over the survivors** of the `F` filter, which is what enters the proxy's
  pending-batch deque and governs future raises. It is not `p`.

Only batches that lower the installed contribution mutate the reduction; every batch pays the
cheap check:

```
p = proposedBatchMinimum          // min read_snapshot over the transactions in the batch
F = authoritativeEffectiveFloor   // max(publishedGlobalValidationDemand, currentVersion − W_commit)

C = authoritativeInstalledProxyMinimum(source)   // read at the authority, not sent by the proxy
I = min(C, max(p, F))                            // computed on every batch; an install never raises
if (I < C) install(source, I)

reply carries F and installedProxyMinimum;
afterwards the proxy rejects individually every transaction with read_snapshot < F
```

**Every batch runs the transition; there is no local fast path.** A proxy's copy of the
installed minimum can be stale in the one direction that matters: if it holds 100 while the
authority has already installed a raise to 200, a batch with `p = 150` would look covered and
skip the proposal, when the installed contribution does not cover it. Sequence numbers discard
late acknowledgements but do not close that interval, and keeping the optimisation would mean
revoking the local capability before every raise. Since the request is made and awaited on
every batch anyway, running the transition unconditionally costs no round trip.

The authority installs `min(C, max(p, F))` **in the same transition and returns both `F` and
what it installed** — no retry, and no install ever raises a contribution. A proposal contaminated by a single transaction too old is refused as an *exact*
minimum while the survivors still receive conservative coverage, and the proxy, having been
told `F`, rejects the stragglers individually. **This is not the silent substitution forbidden
on the client path** (§7a): there, accepting a reported floor under a newer value without
saying so remains illegal, precisely because the client would believe a snapshot protected that
is not.

**That guarantee covers only the demand-derived component.** The `currentVersion − W_commit`
term can still overtake `p` while the batch travels — with `C = 100`,
`p = 120` and `currentVersion − W_commit = 130`, the resolver floor is 130 even though the
proxy holds a contribution at 100. That is today's behaviour preserved: a transaction can
become too old in transit, and the resolver keeps the final decision.

Symmetrically, when the deque advances to a **higher** minimum, publication may be deferred: a
contribution lower than the exact survivor minimum only over-retains, and it keeps later
batches covered by the older, lower value.

**The installation must be conditional, not a send.** Between the proxy's `effectiveFloor`
test and the arrival of its update, the global floor can advance — so the aggregator applies a
compare-and-set:

```
installIfStillAdmissible(proxyGeneration, publicationSequence, b)
    → authoritativeEffectiveFloor > b : FAIL, and the proxy rejects it as transaction_too_old
    → otherwise          : b enters the reduction, and only then is it acknowledged
```

That makes "the effective floor advanced past `r` first" and "the proxy contribution was
installed while the effective floor was still ≤ `r`" mutually exclusive (§4a) — expiry is no
longer the contender in this operation. Without it
the proxy's own test is a time-of-check/time-of-use gap and a batch can be admitted under a
floor that has already passed it.

**Acknowledgements carry `{proxyGeneration, publicationSequence, coveredThroughBatch}` and
stale ones are discarded**, so that a late acknowledgement of a *raise* cannot overwrite a
lower minimum installed since. And a *raise* is guarded at the authority by a per-source
revision that every admission advances — including admissions where nothing was written, since
those are precisely the ones that create newly covered work. A raise carries the revision it
was computed against and applies only if that revision is still current; refused, it is
recomputed from the deque and resent. A late lowering merely over-retains; a late raise would
strip coverage from work admitted since, which is why only raises need the compare.

### The proxy's pre-filter is conservative; the Resolver stays authoritative

Computing the minimum over *admitted* transactions requires the proxy to apply its own
`read_snapshot ≥ effectiveFloor` test. Today that decision belongs to the resolver
(`ConflictSet.cpp:805`) and the proxy only translates the reply
(`CommitProxyServer.cpp:2043`), so this adds a second rejection point and the two must be
ordered explicitly:

| | Outcome |
|---|---|
| proxy admits, resolver admits | commit proceeds |
| proxy admits, resolver rejects | the resolver's `transaction_too_old` stands — its floor may have advanced in transit |
| proxy rejects | the commit never reaches the resolvers |
| proxy under-filters | safe: the minimum is lower than necessary, so it over-retains |
| proxy over-filters (floor read too far ahead) | **availability regression** — a live commit is refused |

Hence the rule: **the proxy's filter is conservative and the resolver keeps the final say.**


### What the handoff is, and is not

The handoff transfers **coverage**, not a set of transactions:

```
covered by globalOldestClientRV
              ↓ overlap
covered by oldestInFlightCommitRV
```

The required property:

```
COMMIT_ADMITTED ⇒ its read_snapshot is covered by the client minimum
                ∨ covered by some Commit Proxy's pending minimum
                ∨ its generation is fenced from durable decisions
                ∨ a terminal result already exists
```

**Per-client server-side commit state is explicitly excluded from the design.** Constructs of
the shape `oldestPendingRV[client]`, `acceptedPendingCount[client]`, or any server-side
collection of commits grouped by client contribute nothing to the floor and complicate
everything else: commits from one client reaching different proxies, out-of-order completion,
proxy recovery, per-client memory, cleanup and ownership, and identifying which read version
becomes the next minimum. The division of responsibility is:

| Component | Keeps | Publishes |
|---|---|---|
| Client library | the detail of its own live transactions | one minimum per client |
| Commit Proxy | the batch detail it already has | one minimum over its pending work |
| Aggregator | per-source minima | the global minimum, monotonically |
| Resolver | — | applies the `currentVersion − W_commit` lower bound |
| Storage | — | consumes the client minimum only |

### Why the combined minimum stays monotone

`oldestInFlightCommitRV` need **not** be monotone in isolation: a newly admitted commit may
carry a read version below every other pending one. Monotonicity of the system comes from the
overlap, not from each source being monotone on its own:

1. Before the handoff, the client registration contributes a value no greater than the
   commit's `read_snapshot` `r`.
2. The proxy's pending minimum covers `r` **before** that contribution may disappear.
3. During the transition both are present.
4. Afterwards the proxy's minimum covers `r` until the batch is terminal.
5. Therefore no contribution `≤ r` ever vanishes while `r` is still needed.
6. A commit whose `r` is below the authoritative effective floor is rejected.

Publication is monotone by construction — **of the floors published irreversibly to
consumers**, not of the source minima, which legitimately fall when a new contribution
installs: `publishedFloor = max(previousPublishedFloor,
newlyDerivedFloor)`.

### Identity is for the leases, not for the handoff

The checks that matter at admission are the ones above: `read_snapshot` against the effective
floor, and the conditional installation of the contribution. Validating a *specific* client
registration is not among them — what makes the commit safe is that its read version is still
above the floor and that its coverage is in the reduction before the floor can move again.

Client identity remains necessary — but only on the **GRV path**, to protect lease entries
against collisions, impersonation and updates from other incarnations of the same client
(§7a), where `GetReadVersionRequest` is a `PublicRequestStream`
(`GrvProxyInterface.h:227`, `:120`) and everything a client sends is untrusted input.

**`clientID` and `leaseGeneration` therefore do not need to be added to
`CommitTransactionRequest`** — an earlier formulation required them there so the proxy could
validate a specific registration during the handoff. With admission decided against the
published floor, that wire change disappears along with the model that motivated it.

**The race is between the floor and the installation — not between expiry and the handoff.**
Lease expiry only removes *a* contribution; it may later allow the floor to advance, but it is
not itself the event that invalidates a commit. If the lease has expired while the floor has
**not** passed `r` — because another client holds it down, or because it simply has not moved
— the commit is perfectly admissible, and rejecting it would be a spurious loss of
availability. The two mutually exclusive outcomes are:

```
authoritativeEffectiveFloor > r  at installation time   → reject transaction_too_old
installation while authoritativeEffectiveFloor ≤ r      → accept
```

**Consequently the handoff needs no client identity.** It is enough to install the proxy's
contribution conditionally on the authoritative effective floor:

```
InstallReply conditionalInstallProxyMinimum(
        ProxyID proxy, Generation generation, PublicationSequence sequence,
        Version proposedBatchMinimum) {
    // executed by the same authority that publishes the floor, in one non-suspending stretch
    const Version F = max(publishedGlobalValidationDemand,
                          authoritativeCurrentVersion − W_commit);
    const Version C = authoritativeInstalledProxyMinimum(proxy);  // empty-source value if absent
    const Version I = min(C, max(proposedBatchMinimum, F));
    if (I < C) {
        installSourceMinimum(proxy, generation, sequence, I);      // an install never raises
    }
    return { F, I };
}
```

`C` is read from the authority's own state. A proxy-side copy of it is diagnostic only: it
cannot decide whether the exchange is needed, because between an authority-side raise and its
acknowledgement the copy is stale in exactly the direction that would skip a necessary
proposal.

**The comparison is against the effective floor, not against the demand minimum alone.** The
resolver's floor is `max(demand, currentVersion − W_commit)`, so testing only the demand
component would admit batches the ordinary age bound has already passed — they would travel to
the resolver merely to be rejected. Winning the install means the batch was admissible *at
that instant*; the time term may still overtake it afterwards, on the way to the resolver,
exactly as today.

**The proxy cannot filter first**: `F` is known only at the authority and arrives in the reply.
It therefore proposes `p`, the minimum over the transactions that entered the batch, and
filters *afterwards*, rejecting individually every commit with `read_snapshot < F`. Every
survivor satisfies `read_snapshot ≥ F` by that filter and `read_snapshot ≥ p` because `p` is the
batch minimum, so `I ≤ max(p, F) ≤ read_snapshot` covers all of them; and every earlier batch
stays covered because `I ≤ C`. **The `min(C, …)` is load-bearing**: with `C = 100` still
covering an in-flight batch, a contaminated `p = 50` and an advanced `F = 150`, installing
`max(p, F)` would raise the contribution to 150 and strand the earlier batch. Raises belong to
retirement, never to an install.

`clientID` and `leaseGeneration` therefore do **not** need to travel on the commit request.
They remain necessary to authenticate and protect lease updates on the GRV path (§7a), but
they play no part in installing a Commit Proxy's minimum.

**The linearization obligation stands**, restated over the right events. One authority must
order:

1. removal and update of source contributions;
2. installation of Commit Proxy minima;
3. the irreversible advance of `publishedFloor`.

The transport is decided (§8.4): both installs ride messages the proxies already send to the
sequencer, and the existing replies are the acknowledgement. The ordering never was a choice.


### Proxy failure does not withdraw coverage

The await of §9 closes the *normal* withdrawal path. It does not close this one: the proxy's
minimum enters the reduction, the commit is admitted, the batch is dispatched to the
Resolvers, and the proxy **dies before all replies arrive**. If the authority drops the dead
proxy's source from the reduction merely because the process disappeared, the floor
advances while a commit at `r` is still alive in a Resolver — the very gap §4a exists to
close. The invariant is the server-side mirror of §4:

> **The death of the process publishing a minimum is not the termination of the work that
> minimum covers.** Loss or replacement of a Commit Proxy may not remove its contribution from
> the reduction merely because the process disappeared. Exactly one of the following must hold
> before withdrawal: (1) a successor recovers or inherits the generation-fenced pending
> minima; (2) every associated Resolver request is fenced or proven unable to complete; or
> (3) a conservative generation barrier retains the proxy's last published minimum until all
> requests of that generation are guaranteed closed. Delayed cleanup may over-retain; process
> disappearance alone is never withdrawal evidence.

Because the contribution is a scalar, option (3) is cheap: the aggregator keeps the dead
proxy's last published minimum. There is nothing per-transaction to inherit or reconstruct.

**What FDB's current architecture already gives us.** A single commit proxy failure does not
get a successor: `waitCommitProxyFailure` is `quorum(failed, 1)` raising `commit_proxy_failed()`
(`fdbserver/clustercontroller/ClusterRecovery.cpp:461–472`, armed at `:1952`), which tears down
the whole generation — master, proxies, **resolvers** and TLogs are recruited as a unit. So:

- Option (1), inheritance, is **not expressible today**: there is no successor proxy within the
  generation.
- Option (2) holds **structurally** once recovery completes: the Resolvers that held the
  in-flight requests are gone with their conflict sets, and
  `recoveryTransactionVersion = lastEpochEnd + MAX_VERSIONS_IN_FLIGHT` (`:1387`) puts every
  pre-recovery read version beyond the window anyway.
- Therefore the requirement reduces to option (3): **a conservative barrier spanning the
  interval from Commit Proxy failure until the old generation is provably unable to complete a
  validation that can influence a durable decision.** `TLOG_TIMEOUT` contributes to
  failure-*detection* latency (`ClusterRecovery.cpp:466`); it does **not** bound recovery
  completion and does not license release of the barrier. **Release is event- and
  fencing-driven, never timer-driven:** the previous proxy's contribution stays represented
  until recovery establishes the new generation cut and every request of the old generation is
  either terminal or unable to affect the commit path.

  Once the new generation is established, old Resolver replies may still physically arrive —
  the old Resolvers keep running until `checkRemoved` observes
  `db->get().recoveryCount >= recoveryCount` and they are absent from the new set
  (`fdbserver/resolver/Resolver.cpp:832–843`). But they are generation-fenced and cannot be
  consumed by a live Commit Proxy or reach a durable decision: recovery locks the previous
  epoch's TLogs and fixes its `epochEnd` (`fdbserver/logsystem/LogSystem.cpp:420`), so no
  old-generation commit can be made durable. *That* — not process disappearance, and not the
  expiry of a timeout — is the proof that the contribution may be withdrawn. This is the same shape as
  the GRV-proxy-generation barrier already required in §7.
- **Forward-looking:** if FDB ever gains single-proxy replacement without a full recovery, this
  reduction fails and inheritance or explicit fencing becomes mandatory. Any such change must
  revisit this section.

### Failure-injection cases

The property to check at every boundary:

`HANDOFF_ACCEPTED ⇒ covering minimum in the reduction ∨ generation fenced from influencing a durable decision ∨ terminal validation outcome already known`

(*"fenced from influencing a durable decision"*, not *"fenced from validation"*: an old
Resolver may still execute and still produce a reply — what must be impossible is that any
authorized party consumes it to decide.)

- proxy dies after its updated minimum enters the reduction but before admitting the commit;
- dies after the handoff and before dispatching to the Resolvers;
- dies after dispatching to only some of them;
- all Resolvers reply, but the proxy dies before withdrawing or publishing the withdrawal;
- a successor generation appears while replies from the previous one are still arriving;
- the global floor advances between the proxy's admissibility test and the arrival of its
  installation — the authority must install `min(C, max(p, F))` and report `F`, so the batch is
  never admitted wholesale under a floor that passed it, never failed wholesale either, and
  never has its earlier coverage raised away;
- a late acknowledgement of a *raised* contribution arrives after a lower one was installed —
  it must be discarded, not allowed to overwrite a value installed since;
- a batch retires while its entry is no longer in the deque (it left through `pop_back`), and
  the minimum must not change;
- retirement order is violated — an assertion that must never fire.

**The rules above are correctness requirements. The handoff's linearization mechanism must be
selected and covered by these cases before Resolver Phase A ships.**

## 5. Aggregation hierarchy

A hierarchical `min` reduction: client library → GRV proxy (over valid leases) → **the
generation's sequencer** → `globalOldestClientRV` → consumer-scoped publication → derived
floors (§6). The sequencer holds the per-source entries, computes the minima and publishes the
floors (§8.4); the Cluster Controller is not the ordinary aggregator, and takes part only in
the handover and fencing between generations.

The property is **not** that no component tracks transactions individually — once §4a exists,
Commit Proxies track a minimum over their pending batches. It is that *the hierarchy
transports minima rather than a
cluster-wide transaction list*: clients track their own active read versions to compute their
minimum, Commit Proxies reuse the batch detail they already hold, and the Cluster
sequencer aggregates per-source minima. No component holds a global registry of transactions.

Empty reductions are defined **independently per source**: `min(∅) = currentVersion`. Two
readings are wrong in opposite directions — that the derived floors "fall back to their policy
defaults", and that the Resolver simply reaches `currentVersion`, which ignores the second
source of §4a. Precisely:

- If **both** the client-registration set and the accepted-but-unvalidated commit set are
  empty, `globalValidationDemand = currentVersion` and the Resolver may reclaim up to the
  current version — it does **not** fall back to the fixed-lag bound.
- If **either** source is non-empty, its minimum continues to pin the derived floor. With no
  clients but one in-flight commit at `r`, `globalValidationDemand = min(currentVersion, r) = r`
  and reclamation stops at `max(r, currentVersion − W_commit)`: at `r` while that commit is
  still inside the ordinary commit window, and once `currentVersion − W_commit` overtakes `r`
  the commit may become too old exactly as today.

Register-before-use (§3) and handoff-before-release (§4a) jointly make this safe: every
consumer is covered first by a client registration and, after commit acceptance, by an
overlapping Commit Proxy minimum.

**Resolved for v1 — §3 and §5 were in tension.** Having clients report to a *stable* GRV proxy ("one
lease copy, no deduplication") cannot coexist with §3's piggybacking on `GetReadVersion`,
because GRV requests are **load-balanced across all GRV proxies today** —
`basicLoadBalance(cx->getGrvProxies(...), &GrvProxyInterface::getConsistentReadVersion, ...)`
(`fdbclient/NativeAPI.cpp:5300`). Both properties cannot hold as written. Two resolutions:

- **Pin GRV requests to the derived proxy.** Preserves "one lease copy", but changes GRV load
  balancing — a behavioural change on the hottest client path, and it interacts with the
  proxy-failure fallback that `basicLoadBalance` provides.
- **Accept multi-copy leases.** Let the report ride whichever proxy the request reaches; each
  proxy holds its own `{clientID → minRV}` entry. **Still correct** — floors are monotone, so
  a stale copy is an older value and the global `min` is conservative (§7). The costs are
  that "no deduplication" is false (up to `numProxies` entries per client) and that the floor
  advances only as fast as the *least recently refreshed* copy, which for an idle client can
  lag a full lease period. **In v1 a renewal refreshes the copies the client knows it holds,
  and any others are left to expire**: renewing one copy does not update the rest, and they
  cannot be assumed to catch up on their own. Directing `RenewOldestReadVersion` (§3) at a
  single designated copy is a possible optimization, not the v1 rule. Safety is conservative
  either way; precision is bounded by the oldest surviving copy.

  **This makes copy identity a protocol requirement.** GRV requests are load-balanced
  (`NativeAPI.cpp:5300`), so a client cannot infer afterwards which proxy installed its
  registration. The acknowledgement must name the granting copy **and give the client a way to
  reach it** — `{grvProxyID, leaseGeneration, grantedServerLeaseDuration, renewalTarget}`, or an
  equivalent recovery of the endpoint from the RPC; an identifier alone names a copy without
  making it reachable. Without this a client cannot obey the rule of refreshing every copy it
  holds, and multi-copy silently becomes "refresh whichever proxy the next request reaches".

  **The grant carries a duration, never an absolute instant.** A server-produced deadline would
  mix clock domains and would let a late reply hand back time the client had already spent; the
  client computes `clientRequestStart + conservativeClientWindow(grantedServerLeaseDuration)`
  on its own clock, from before the request was sent, which is §2's inequality applied
  literally.

**Decided for v1: the second — multi-copy, with no global deduplication.** It leaves the hot
path untouched and pays only in retention precision, and since the handoff no longer resolves
a specific registration (§4a) it costs nothing on that side either. What it does cost is
lease-state multiplication, up to one entry per client per proxy, and a floor advancing no
faster than the least recently refreshed copy.

## 6. Observation vs derived floors

**Sources** (each with `min(∅) = currentVersion`):

- `globalOldestClientRV = min(clientOldestActiveRV over valid client registrations)` (§2)
- `oldestInFlightCommitRV = min(commitProxyOldestInFlightRV)`, each proxy reducing
  `batchOldestReadSnapshot` over its non-terminal batches — §4a
- `globalValidationDemand = min(globalOldestClientRV, oldestInFlightCommitRV)`

**Derived floors:**

- `storageReadFloor = globalOldestClientRV` — normally
  `storageRetentionFloor ≤ globalOldestClientRV`; under pressure policy may exceed it, which
  *is* the priced revocation mechanism (`02-storage.md` §5). Legacy clients obtain no extended
  retention guarantee here either.
- `resolverValidationFloor = max(globalValidationDemand, currentVersion − W_commit)` —
  all-reporting, negotiated.
- `resolverValidationFloor = currentVersion − W_commit` — mixed/legacy mode.

Note the Resolver consumes `globalValidationDemand`, which includes in-flight commits, while
Storage consumes only the client registrations: a pending commit needs conflict history, not
value history.

**Invariant:** *a long read-only transaction must never force conflict-history retention
beyond the ordinary commit window* — enforced by the `now − W_commit` term, not by a second
sensor or a second registration population. This is the formula `01-resolver.md` §2 and §3
now consume verbatim.

Two consequences worth stating explicitly, because Phase A's schedule depends on them:

- **The Resolvers never retain more than today.** The cap guarantees it in every mode.
- **The efficiency dividend is negotiation-gated.** In mixed-version clusters the floor does
  not advance at all: non-participating clients are invisible potential committers. Phase A
  therefore **ships as a retention no-op** until cluster-wide participation is negotiated, and
  its win is conditional on adoption.

## 7. Properties

- **Client floors and published retention floors advance in one direction.** Individual
  in-flight-request minima may move both ways as requests enter and leave (§4a), but the
  overlap rule and rejection below the authoritative effective floor ensure the *effective
  published* floor
  never retreats and never loses coverage. **A delayed publication of a still-valid
  contribution can only over-retain** — but a *lost renewal* is the opposite case and is
  handled by the lease's temporal contract (§2): the client revokes locally before the server
  may expire the entry.
  The existing Resolver code already requires a monotone floor —
  `if (newOldestVersion > cs->oldestVersion)` (`fdbserver/resolver/ConflictSet.cpp:986`) — so
  this model needs no change to that invariant.
- **Recovery coherence. Verified:** a full recovery sets
  `recoveryTransactionVersion = lastEpochEnd + MAX_VERSIONS_IN_FLIGHT`
  (`fdbserver/clustercontroller/ClusterRecovery.cpp:1387`) with
  `MAX_VERSIONS_IN_FLIGHT = 100 × VERSIONS_PER_SECOND` (`fdbserver/core/ServerKnobs.cpp:156`),
  twenty times the ordinary window — so after recovery every pre-recovery read version is
  already too old. "Recovery may invalidate retained history" is today's behaviour, not a new
  concession. For **proxy-generation** changes a barrier remains mandatory: a single proxy
  replacement must not invalidate long readers. **A lease period can only justify releasing a
  GRV-proxy contribution, never a Commit Proxy's**, which is released at the generation fence
  and not on a timer (§4a). Across a generation change the Cluster Controller does not become
  the aggregator: it either hands the previous generation's conservative per-source state to
  the new sequencer, or withholds that sequencer's authority to advance until the old
  generation is fenced. Which of the two is an implementation detail of the fence; publishing a
  *later* floor before it completes is not permitted either way.
- **Backward compatible with no flag day** — a legacy client never reports and therefore never
  holds the floor down; the mixed-mode formula of §6 makes this literal.

## 7a. Adversarial input

`GetReadVersionRequest` arrives on a **`PublicRequestStream`**
(`GrvProxyInterface.h:227`, `fdbclient/include/fdbclient/CommitProxyInterface.h:48`) and its
`verify()` returns `true` unconditionally (`GrvProxyInterface.h:120`). A client-supplied
`clientFloor` is therefore untrusted input that, unbounded, pins cluster-wide history — a
denial-of-service against storage retention with a single malformed field. At minimum the
proxy must **clamp** the reported floor to a
policy-maximum window before it enters the reduction, and the maximum should be
administratively bounded (per-tenant, or a cluster knob). The clamp is also the natural
enforcement point for the retention budgets of `00-overview.md` §2.

**The clamp must be a visible admission, never a silent substitution.** If the server quietly
replaces a client's reported floor with a newer one, a legitimate client believes its snapshot
is protected when it is not. The contract must be:

> A reported floor older than the administratively permitted retention window is **not**
> silently accepted under a newer value. The proxy either rejects the registration or returns
> the effective *granted* floor or capability, so the client can fail or revoke the affected
> snapshot explicitly. Only the granted floor enters the reduction. Storage Servers
> independently reject reads below their actual retained capability.

This is the same shape as the priced revocation of `02-storage.md` §5: a budget may **deny** a
lease, but it may not pretend to have granted one.

**The complementary attack is worse, and the clamp does not stop it.** If `clientID` and
`leaseGeneration` are fields a client supplies freely, a buggy or hostile client can update
*another* client's entry with a *newer* floor — causing **premature reclamation**, i.e. unsafe
under-retention, where a forged low floor only over-retains. The proxy must therefore prevent
one connection from advancing or replacing another client's registration:

- `clientID` bound to an authenticated or unguessable client incarnation, not a
  client-chosen label;
- `leaseGeneration` updates ownership-checked, with old generations rejected;
- updates monotone within a generation;
- a per-tenant / per-process cap on identity creation, so identities cannot be minted without
  bound.

This matters even in a cluster that trusts its clients: it also covers ID collisions, retries
and plain bugs.

**The trust boundary, stated honestly.** Server-side fencing prevents one client incarnation
from modifying another's registration, and admission bounds prevent retention denial-of-service.
Neither proves that a client library reported the *complete* minimum of its own active read
versions — the server cannot verify completeness without server-issued per-transaction
registrations, which are outside v1.

This is the **same boundary FDB already relies on**, not a new concession: a transaction's
read and write conflict ranges are supplied by the client and never verified against the reads
it actually performed (`Transaction::addReadConflictRange`, `fdbclient/NativeAPI.cpp:3961`); a
client that omits one silently forfeits serializability for that access. The floor protocol
inherits the same contract:

> The protocol trusts the official client library to report the complete minimum of its own
> active read versions. A client that advances its own floor incorrectly forfeits protection
> for the omitted snapshot and receives `transaction_too_old` or an old-version read failure —
> a self-inflicted loss, and the containment is *demonstrated*, not merely asserted: reads
> below retained capability fail, and a read-write commit whose read version has fallen below
> the published validation floor cannot complete the handoff (§4a step 2). Byzantine
> completeness would require
> server-issued per-transaction registrations, outside v1.

## 8. Implementation choices

One remains open — the lease parameters (1). Entries 2, 3 and 4 are **decided for v1** and kept
here with the reasoning that settled them.

**One implementation choice remains open, and no safety rule does.** Every admissible choice
must satisfy the frozen ordering and temporal constraints: lease parameters that violate
`clientUsageWindow + driftMargin < serverLeaseDuration` break safety, and a conditional-install
placement that does not guarantee atomicity against the floor advance, or generation fencing,
breaks it too. What is open is *which values and which mechanism satisfy the rules* — not
whether the rules hold.

1. **Lease duration and renewal frequency** — *open.* — a latency/over-retention trade-off. The safety
   *rule* is frozen (§2); what is open are its parameters, subject to

   ```
   clientUsageWindow + driftMargin < serverLeaseDuration
   renewInterval                   < clientUsageWindow
   ```

   with local revocation when no acknowledgement arrives in time. The measurements that size
   them (lease lifetimes, renewal and expiry rates, reported-floor lag) presuppose the
   registrations exist, so they ship with the lease implementation rather than preceding it.
2. **Watermark distribution transport** — *decided for v1: consumer-scoped dedicated
   publication*, not `ServerDBInfo`. Each consumer receives the value on a path it already
   needs: the Resolver on the resolve request, the Commit Proxy through the
   install/acknowledge exchange (§4a), and an additional stream only towards Storage Servers.
   **The concern that settled it is corroborated:** `ServerDBInfo.id` "Changes each time any other member
   changes" (`fdbserver/core/include/fdbserver/core/ServerDBInfo.h:40`), so a
   frequently-updated integer would rebroadcast the whole structure to every worker. Note also
   that `ServerDBInfo` is "not available to the client" (`:36–38`) — which is fine, since every
   consumer of the watermark is a server.
3. **Stable-proxy vs multi-copy leases** (§5) — *decided for v1: multi-copy, with no global
   deduplication.* Not a free choice: it decides
   whether the hot GRV path changes, and under multi-copy it decides how stale copies are
   refreshed or expired.
4. **Where the conditional-install authority executes** — *decided for v1: the generation's
   sequencer (master).* It is the authority for both conditional installs and for the published
   floors, holding state per **source** — one entry per GRV Proxy and per Commit Proxy — never
   per client and never per transaction. Each operation is linearised in a non-suspending
   stretch together with any advance that could make the candidate inadmissible, and recovery
   initialises the new authority only after the previous generation is fenced.

   - `conditionalInstallClientMinimum`, ordered against `authoritativeStorageAdmissionFloor`;
   - `conditionalInstallProxyMinimum`, ordered against `authoritativeResolverEffectiveFloor`.

   **They may share transport and machinery, never guards** (§3, §4a).

   **The ordering is inherited, not built.** `getVersion` (`masterserver.cpp:74`) is the sole
   writer of the cluster's version, and from its ordering wait (`:91`) through the reply and
   the sequence advance there is no suspension point; the compare, the install and the
   publication belong inside that stretch. The per-proxy `latestRequestNum` only orders one
   proxy's requests against each other — it is being the sole writer, plus the absence of a
   `co_await`, that gives the global order.

   **The transport is inherited too.** Both installers already reach the sequencer once per
   batch on existing messages — `GetCommitVersionRequest` from the Commit Proxy, and
   `getLiveCommittedVersion` from the GRV Proxy after it groups requests
   (`masterserver.cpp:254`, `GrvProxyServer.cpp:727`) — and those replies already serve as the
   acknowledgement. A lowering install therefore adds fields to a round trip that must happen
   anyway before resolution, not a round trip of its own.

   **Fencing is not free, and four cases must be closed explicitly**, rather than inferred from
   "the master is the generation": at a generation change the new sequencer may neither forget
   an authorised floor nor publish a later one until the old generation is fenced; a dead GRV
   Proxy's contribution stays until every usage window it could have granted has expired,
   since process disappearance is not evidence; a dead Commit Proxy's contribution does not
   expire on a timer but at the generation fence; and `authoritativeStorageAdmissionFloor` must
   share an order domain with the irreversible publication to Storage Servers, so a separate
   publisher must acknowledge before the sequencer treats an advance as authorised.

   **Acceptance criterion — both sides.** Ownership and cleanup must be defined across process
   failure on each path: the GRV-proxy generation barrier for the client install, so the
   admission floor cannot advance before coverage is rebuilt or those windows expire (§2, §7);
   and the Commit-Proxy barrier for the commit install (§4a).

   *Rejected: the Cluster Controller — it survives generations and already aggregates, but sits
   on no per-batch path, so every lowering install would need a new round trip and the
   linearisation would have to be constructed rather than inherited.*

Open here means *how*, not *whether*: the §4a handoff rule itself is a correctness requirement,
not an implementation preference.

## 9. Cost note — new state on a stateless role

GRV proxies keep **no per-client state today** (`GrvProxyServer.cpp:187–211`). This protocol
adds an entry per client *process* per proxy, plus a lease-expiry sweep. For clusters with
many thousands of client processes that is new memory and new periodic work on a latency-
critical role; under the multi-copy resolution of §5 it multiplies by the proxy count. Size it
before choosing lease duration (§8.1) — the two decisions are coupled.

**The sequencer acquires the authority's state (§8.4):** one entry per source — per GRV Proxy
and per Commit Proxy — plus the two reductions, maintained inside the non-suspending stretches
it already runs. The load lands on a singleton already on the critical path of every commit and
every GRV batch, which is the price of inheriting its order; what it does *not* add is a round
trip, since both installs ride messages those proxies already send and the existing replies
serve as the acknowledgement.

**Commit Proxies acquire far less (§4a):** a monotonic deque of `{localBatchNumber, minimum
read_snapshot}`, bounded by the pending batches and usually far smaller, since every entry
dominated by a later one is discarded on arrival. The transactions themselves are already in
the batch, so no per-commit collection is added, and the amortised cost of both the admit and
the retire path is O(1).

The measurable cost is not the reduction but the **coordination rate**: the fraction of
batches that actually **lower** the installed contribution — `I < C` — and therefore mutate the
reduction. The round trip and its linearisation exist for every batch already, so what is
incremental is carrying `p`, reading `C`, computing `F` and `I`, and writing only in that
fraction. That fraction, the duration added to the authority's non-suspending stretch, and the
gap between the installed contribution and the exact survivor minimum (which prices deferred
raises as over-retention) are the numbers that decide whether this design is affordable.

Two observations that bound this:

- **Withdrawal needs no new synchronization *on the normal path*.** The failure path still
  requires generation-fenced inheritance, a cancellation proof, or the conservative
  proxy-generation barrier of §4a; process disappearance alone is not withdrawal evidence.
  On the normal path, the proxy already awaits all of a batch's
  resolver replies before proceeding — `co_await singleResolverReply` /
  `co_await getAllAsync(std::move(replies))`
  (`fdbserver/commitproxy/CommitProxyServer.cpp:1008–1016`) — so `VALIDATION_COMPLETE` maps
  onto an existing await point. Only the *installation* side needs new ordering.
- **Installation adds work to the commit critical path, but no message.** §8.4 settles the
  ordered channel: both installs ride requests the proxies already send to the sequencer, whose
  replies are the acknowledgement, so the expected increase in message count is **zero**. What
  must be measured is the load on the sequencer, the bytes added to those two messages, and the
  duration added to its non-suspending stretch. **Correctness does not permit replacing the
  acknowledgement with eventual propagation** (§4a) — that is the one economy not available
  here.
- **The batch minimum is known before the request is sent.** `commitBatcher`
  (`CommitProxyServer.cpp:237`) already handles each arriving transaction, so
  `batchOldestReadSnapshot` accumulates there at no extra traversal and is ready before
  `commitBatch` begins — well before `GetCommitVersionRequest` is built and awaited
  (`:901–907`, awaited immediately rather than overlapped). That minimum is taken over
  the transactions the **batcher accepted**, before any authoritative filtering — not over the
  survivors of that filter, which is a different and higher value, computed after the reply and
  used for the deque and for future raises.

This cost may exceed the lease map's and deserves its own benchmark.

## In one sentence

> Each client library maintains a monotonic minimum active read version, piggybacked on GRV
> requests and kept alive by a lease only while snapshots live; GRV proxies reduce those to a
> per-proxy minimum and the generation's sequencer reduces the sources into a single global
> watermark, publishing it; **demand-side** retention
> responsibility is handed to the Commit Proxies' minimum over their pending batches before the
> client's coverage is released, and that contribution is held until validation completes or
> the generation is fenced from influencing a durable decision — while the ordinary
> `currentVersion − W_commit` bound may still overtake the commit, as it does today. Delays may over-retain; reclamation may precede the last consumer only through
> explicit recovery or priced-revocation semantics, under the existing `transaction_too_old`
> contract — never silently.
