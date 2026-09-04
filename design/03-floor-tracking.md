# Floor Tracking — the Oldest Active Read Version

*Status: design frozen at the protocol level, reviewed against `apple/foundationdb` main @
`a443d3ee60`. **Four** implementation choices remain deliberately open (§8). The commit
lifecycle handoff — the one correctness gap in this protocol, and what was blocking Resolver
Phase A (`01-resolver.md` §3) — is closed as a **rule** in §4a, with its linearization
mechanism left to §8.4. Claims about current FDB carry `file:line`.*

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
(`reportedMin ≤ localMin`); lag only over-retains, never under-retains.

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

The normal path adds **zero messages**: every `GetReadVersion` carries
`{clientID, clientFloor, leaseGeneration}`. **Mechanically confirmed** — `GetReadVersionRequest`
is flatbuffers-serialized (`GrvProxyInterface.h:126–141`), and FDB has an established idiom
for exactly this: fields appended to `serializer(...)` **gated on the negotiated protocol
version**, e.g. `if (ar.protocolVersion().hasNativeCdc())` in `ClientDBInfo::serialize`
(`fdbclient/include/fdbclient/CommitProxyInterface.h:151–153`) and
`hasMutationChecksum()` in `CommitTransaction.h:351`. **Gating is required, not optional:**
"old servers ignore trailing bytes, old clients send none" is too automatic — a *new* server
receiving the old encoding must still distinguish absence from a zero floor. The fields are therefore
enabled only when the negotiated protocol version advertises support; legacy requests
deserialize without them and legacy servers never receive the extended encoding. Mixed-version
simulation must cover the compatibility behaviour — which dovetails with the negotiation gate
of §6.

Explicit traffic appears in exactly one case: a client holding a long-lived read version while
no longer requesting new ones sends a periodic `RenewOldestReadVersion`. The new traffic is
generated precisely by the transactions that use the new capability.

Bootstrap: for a client with no active transactions, the GRV proxy registers the chosen
version as the client's floor *before* replying, so a read version is protected from the
instant the library receives it. **This is register-before-use, and Resolver Phase A depends
on it** (`01-resolver.md` §3): it is what makes an empty population safe to reclaim
against (§5).

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

**A client-side hold does not fix it.** Keeping the client's registration until the commit
reply contradicts §4: leases exist precisely because a crashed client sends nothing. Once the
commit is admitted, coverage must be owned by a server.

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
> remains responsible. A commit whose coverage lapses before that point is rejected as
> `transaction_too_old` — not new behaviour; the client retry loop already handles it.

### The Commit Proxy contributes an aggregated minimum, not per-transaction pins

**No new per-transaction server state is created.** A commit that reaches a Commit Proxy is
already held in that proxy's existing batch structures until validation finishes; the floor
protocol reuses that detail rather than duplicating it. What the proxy publishes is a scalar:

```
batchOldestReadSnapshot      = min(read_snapshot of the commits admitted into that batch)
commitProxyOldestInFlightRV  = min(batchOldestReadSnapshot over its non-terminal batches)
oldestInFlightCommitRV       = min(commitProxyOldestInFlightRV over all Commit Proxies)
```

with `min(∅) = currentVersion` as everywhere else. The new state is at most **one scalar per
batch**, computed while the batch is being built, and held until the whole batch reaches a
terminal state. Holding a batch's contribution until its last transaction finishes
over-retains slightly and is safe. With a single active batch a single scalar suffices; with
several concurrent batches, a small queue of the batches that already exist — pop the head
when batches finish in order, or mark terminal and advance the head when they can finish out
of order.

**Name it `readSnapshot` (or `inFlightReadVersion`), never `commitVersion`.** The history that
must survive is the history from the commit's *read* version, not the version eventually
assigned to it. Where other documents use CTS for this read version, say so explicitly.

The Commit Proxy is the natural custodian precisely because it needs no new bookkeeping: the
transactions are already in the batch, and it already computes a resolver-visible horizon
(`fdbserver/commitproxy/CommitProxyServer.cpp:2104`).

**What must be ordered is the contribution, not an object.** It is not enough for the proxy
to update a local minimum and propagate it eventually — the floor could advance during that
propagation. The sequence is:

1. the commit arrives;
2. the proxy checks `read_snapshot ≥ publishedFloor`;
3. the commit's `read_snapshot` enters the proxy's pending minimum;
4. that minimum is made visible to the aggregation;
5. only then is the commit admitted for validation, and the client's coverage may lapse;
6. the batch's contribution is withdrawn once the batch reaches a terminal state.

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
6. A commit whose `r` is below the already published floor is rejected.

Publication is monotone by construction: `publishedFloor = max(previousPublishedFloor,
newlyDerivedFloor)`.

### What the proxy actually has to check

The check that matters at admission is **`read_snapshot ≥ publishedFloor`**, together with
installing the new contribution atomically with respect to any subsequent floor advance.
Validating a *specific* client registration is not required for this step: what makes the
commit safe is that its read version is still above the floor and that its coverage is in the
reduction before the floor can move again.

Client identity remains necessary — but for a different purpose: protecting lease entries
against collisions, impersonation and updates from other incarnations of the same client
(§7a). `clientID` and `leaseGeneration` ride the commit request, appended to
`CommitTransactionRequest::serialize`
(`fdbclient/include/fdbclient/CommitProxyInterface.h:203–229`) under the same protocol-version
gating as §3; note this is a `PublicRequestStream` (`:44`), so both are untrusted input.

**The linearization obligation stands.** What must be ordered, by one authority, is:

1. the disappearance of a client contribution through lease expiry;
2. the incorporation of the commit into the proxy's effective minimum;
3. the irreversible advance and publication of the floor.

Exactly one of two events wins: expiry **before** the contribution is in the reduction, and
the commit is rejected `transaction_too_old`; or the contribution enters the reduction
**before** expiry, and subsequent client death is irrelevant. One workable shape is for the
aggregator to acknowledge the proxy's updated minimum only once that contribution
participates in the reduction, and for the commit to count as admitted only then. The
transport is an implementation choice (§8.4); the ordering is not.


### Proxy failure does not withdraw coverage

The await of §9 closes the *normal* withdrawal path. It does not close this one: the proxy's
minimum enters the reduction, the commit is admitted, the batch is dispatched to the
Resolvers, and the proxy **dies before all replies arrive**. If the Cluster Controller drops
the dead proxy's source from the reduction merely because the process disappeared, the floor
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
- a successor generation appears while replies from the previous one are still arriving.

**The rules above are correctness requirements. The handoff's linearization mechanism must be
selected and covered by these cases before Resolver Phase A ships.**

## 5. Aggregation hierarchy

A hierarchical `min` reduction: client library → GRV proxy (over valid leases) → Cluster
Controller → `globalOldestClientRV` → broadcast → derived floors (§6).

The property is **not** that no component tracks transactions individually — once §4a exists,
Commit Proxies track a minimum over their pending batches. It is that *the hierarchy
transports minima rather than a
cluster-wide transaction list*: clients track their own active read versions to compute their
minimum, Commit Proxies reuse the batch detail they already hold, and the Cluster
Controller aggregates per-source minima. No component holds a global registry of transactions.

Empty reductions are defined **independently per source**: `min(∅) = currentVersion`. Two
readings are wrong in opposite directions — that the derived floors "fall back to their policy
defaults", and that the Resolver simply reaches `currentVersion`, which ignores the second
source of §4a. Precisely:

- If **both** the client-registration set and the accepted-but-unvalidated commit set are
  empty, `globalValidationDemand = currentVersion` and the Resolver may reclaim up to the
  current version — it does **not** fall back to the fixed-lag bound.
- If **either** source is non-empty, its minimum continues to pin the derived floor. With no
  clients but one in-flight commit at `r`, `globalValidationDemand = min(currentVersion, r) = r`
  and `resolverValidationFloor = max(r, currentVersion − W_commit)` — reclamation stops at `r`.

Register-before-use (§3) and handoff-before-release (§4a) jointly make this safe: every
consumer is covered first by a client registration and, after commit acceptance, by an
overlapping Commit Proxy minimum.

**Open — §3 and §5 are in tension.** Having clients report to a *stable* GRV proxy ("one
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
  lag a full lease period. The dedicated `RenewOldestReadVersion` path (§3) can target the
  stable proxy, but stale copies on other proxies must then be **explicitly refreshed or
  allowed to expire** — they cannot be assumed to catch up on their own. Safety is
  conservative either way; precision is bounded by the oldest surviving copy.

The second is preferred — it leaves the hot path untouched and pays only in retention
precision. **Choose explicitly.**

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
  overlap rule and rejection below the published floor ensure the *effective published* floor
  never retreats and never loses coverage. Delayed or lost messages can only over-retain.
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
  replacement must not invalidate long readers. Minimal implementation: the CC holds the last
  known watermark for one full lease period while clients re-register.
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

## 8. Open implementation choices

1. **Lease duration and renewal frequency** — a latency/over-retention trade-off.
2. **Watermark distribution transport** — `ServerDBInfo` field vs a light dedicated broadcast.
   **The concern is corroborated:** `ServerDBInfo.id` "Changes each time any other member
   changes" (`fdbserver/core/include/fdbserver/core/ServerDBInfo.h:40`), so a
   frequently-updated integer would rebroadcast the whole structure to every worker. Note also
   that `ServerDBInfo` is "not available to the client" (`:36–38`) — which is fine, since every
   consumer of the watermark is a server.
3. **Stable-proxy vs multi-copy leases** (§5). Not a free choice: it decides
   whether the hot GRV path changes, and under multi-copy it decides how stale copies are
   refreshed or expired.
4. **Where the §4a handoff is linearized against lease expiry.** Candidates: a
   generation-fenced handoff operation in the floor protocol, or the Commit Proxy participating
   as a source in the reduction and awaiting confirmation before accepting the handoff. May be
   subsumed into the watermark transport choice (2).

   **Under the multi-copy lease option (§5) the handoff identity must locate one specific live
   registration.** `{clientID, leaseGeneration}` alone does not say *which* GRV proxy holds the
   authoritative copy, and the Cluster Controller knows only aggregated minima — it cannot
   validate a concrete identity. The GRV reply must therefore return a generation-fenced
   `registrationID` (naming the custodian proxy and its generation) or a verifiable lease
   capability, carried by the commit request and consumed atomically by the handoff; expiry and
   handoff for that registration must be linearized by the *same* authority. Choosing between
   an ID, a token and a directed RPC belongs here, not in §4a. Note this cost falls **only** on
   multi-copy: under the stable-proxy option the custodian is already derivable from `clientID`
   and the proxy set (§5), which is a point in its favour that the §5 trade-off should carry.

   **Acceptance criterion for any candidate:** it must define ownership and cleanup across
   *Commit Proxy* failure, not only client failure. A replacement proxy or the Cluster
   Controller must preserve the previous generation's minimum until that generation's accepted
   requests are inherited or fenced closed (§4a).

Open here means *how*, not *whether*: the §4a handoff rule itself is a correctness requirement,
not an implementation preference.

## 9. Cost note — new state on a stateless role

GRV proxies keep **no per-client state today** (`GrvProxyServer.cpp:187–211`). This protocol
adds an entry per client *process* per proxy, plus a lease-expiry sweep. For clusters with
many thousands of client processes that is new memory and new periodic work on a latency-
critical role; under the multi-copy resolution of §5 it multiplies by the proxy count. Size it
before choosing lease duration (§8.1) — the two decisions are coupled.

**Commit Proxies acquire far less (§4a):** one scalar per non-terminal batch — the minimum
`read_snapshot` admitted into it — reduced to `commitProxyOldestInFlightRV`. The transactions
themselves are already in the batch, so no per-commit collection is added.

Two observations that bound this:

- **Withdrawal needs no new synchronization *on the normal path*.** The failure path still
  requires generation-fenced inheritance, a cancellation proof, or the conservative
  proxy-generation barrier of §4a; process disappearance alone is not withdrawal evidence.
  On the normal path, the proxy already awaits all of a batch's
  resolver replies before proceeding — `co_await singleResolverReply` /
  `co_await getAllAsync(std::move(replies))`
  (`fdbserver/commitproxy/CommitProxyServer.cpp:1008–1016`) — so `VALIDATION_COMPLETE` maps
  onto an existing await point. Only the *installation* side needs new ordering.
- **Installation may add coordination to the commit critical path.** §8.4 should prefer
  batching, or an existing ordered channel, and must measure the added latency, message count
  and Cluster Controller load. **Correctness does not permit replacing the acknowledgement with
  eventual propagation** (§4a) — that is the one economy not available here.

This cost may exceed the lease map's and deserves its own benchmark.

## In one sentence

> Each client library maintains a monotonic minimum active read version, piggybacked on GRV
> requests and kept alive by a lease only while snapshots live; GRV proxies and the Cluster
> Controller reduce it by `min` into a single global watermark; retention responsibility is
> handed to the Commit Proxies' minimum over their pending batches before the client's
> coverage is released, and held until validation completes or the generation is fenced from
> influencing a durable decision. Delays may over-retain; reclamation may precede the last consumer only through
> explicit recovery or priced-revocation semantics, under the existing `transaction_too_old`
> contract — never silently.
