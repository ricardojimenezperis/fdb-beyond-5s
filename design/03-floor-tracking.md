# Floor Tracking — the Oldest Active Read Version

*Version: **2.0** · supersedes 1.0 — this file's initial commit.*
*Status: design frozen at the protocol level, reviewed against `apple/foundationdb` main @
`a443d3ee60`. **Four** implementation choices remain deliberately open (§8); the one
correctness gap found in 1.0 — the commit lifecycle handoff — is closed as a **rule** in §4a,
with its linearization mechanism left to §8.4. It was blocking Resolver Phase A
(`01-resolver.md` §3). Claims about current FDB carry `file:line`. Changes are listed in
§10.*

## 1. Why this protocol must exist *(revised — see §10 F34)*

Every retention decision begins with one client-side observation:

`globalOldestClientRV = min { RV(t) : client transaction t still holds a live snapshot }`

Storage derives its read-retention demand directly from it. **Resolver validation does not**:
after the lifecycle handoff of §4a it additionally includes accepted-but-not-yet-validated
commits whose client-side lifetime may already have ended —

`globalValidationDemand = min(globalOldestClientRV, oldestInFlightCommitRV)`

— and the handoff guarantees continuous coverage between the two sources. 1.0's framing
("every retention decision consumes one number") predates the server-side pin and is no longer
literally true.

Today's FoundationDB does not produce the client-side observation either. **Verified:** `GrvProxyData`
(`fdbserver/grvproxy/GrvProxyServer.cpp:187–211`) keeps no per-client state of any kind, and
`GetReadVersionRequest` (`fdbclient/include/fdbclient/GrvProxyInterface.h:65–141`) carries no
client identity — only `transactionCount`, flags, priority, tags and `maxVersion`. Nothing
server-side observes a transaction's lifetime. **The only place that knows it is the client
library.**

## 2. Client side: a monotonic minimum *(unchanged, with one hole named)*

- `clientFloor = min(activeRVs)` — while transactions are active
- `clientFloor = max(clientFloor, latestGrantedRV)` — when `activeRVs` is empty
- and `clientFloor` never decreases

Monotonicity is load-bearing: when the oldest transaction ends the floor advances, and that
version is irrevocably abandoned by this client. Published values may lag
(`reportedMin ≤ localMin`); lag only over-retains, never under-retains.

**New in 2.0 — manually set read versions bypass the sensor.** `Transaction::setVersion(v)`
(`fdbclient/NativeAPI.cpp:3594–3603`) validates only `v > 0` and that no read version is
already set; it contacts no proxy and records
`trState->readVersionObtainedFromGrvProxy = false` (`:3602`). Such a transaction is invisible
to the aggregation of §5. 1.0's rule `set_read_version(v)` requires `v ≥ clientFloor` bounds
this **within one client process**, but a freshly constructed `DatabaseContext` starts at
`clientFloor = 0`. Two resolutions were admissible:

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
`hasMutationChecksum()` in `CommitTransaction.h:351`. **Corrected in review:** "old servers
ignore trailing bytes, old clients send none" is too automatic — a *new* server receiving the
old encoding must still distinguish absence from a zero floor. The fields are therefore
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

## 4. Leases, not unregistration *(unchanged)*

`commit/cancel/destructor → unregister` fails on the one case that matters: a crashed client
sends nothing. Each GRV proxy keeps, per client process, `{minRV, leaseExpiration}`; an
expired lease removes the client and recomputes the proxy minimum. A dead client over-retains
for one lease timeout. Correctness never depends on promptly detecting death.

> **Caution inherited from 1.0.** The phrase "`commit` → unregister" above describes the
> *rejected* design. It must not be read as licensing release of the registration at commit
> **submission** — see §4a.

## 4a. Commit lifecycle handoff — NEW, and the gap that blocked Phase A

1.0 defined liveness as "transactions still alive" without fixing the boundary at commit
submission. That leaves a race:

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

**A client-side hold does not fix it.** An earlier draft of this section proposed keeping the
client's registration until the commit reply. That contradicts §4: leases exist precisely
because a crashed client sends nothing. Once the commit is accepted, the pin must be owned by
a server.

The rule is the mirror of register-before-use:

> **Handoff-before-release.** Before an accepted commit request can cease to be covered by its
> client lease, **server-side** retention responsibility must already have been established for
> its read version, and it remains until conflict validation completes. Client survival or
> lease renewal after acceptance is not required for safety.

### Three states, not two

Commit *submission* is not the handoff — a message in flight can be lost.

`CLIENT_COVERED` → `HANDOFF_ACCEPTED` → `VALIDATION_COMPLETE`

- **`CLIENT_COVERED`** — only the client's lease guarantees retention.
- **`HANDOFF_ACCEPTED`** — the Commit Proxy has validated the registration and installed a
  generation-fenced server-side pin for the request's read version in the floor-reduction
  path. From here, client death or lease expiry is harmless.
- **`VALIDATION_COMPLETE`** — every Resolver has finished; the pin is withdrawn.

> Commit submission alone does not transfer retention responsibility. The handoff completes
> only when the Commit Proxy has validated the registration and installed the pin. Until that
> acknowledgement the client lease remains responsible. A request whose client registration
> expires before the handoff is rejected as `transaction_too_old` — not new behaviour; the
> client retry loop already handles it.

### The Commit Proxy becomes a second source in the reduction

The pin must be *ordered* with respect to floor advance: it is not enough for the proxy to
hold it locally and propagate it eventually, because the floor could advance during that
propagation. The clean formulation makes accepted-but-unvalidated commits a second input to
the `min` (formulas in §6), with the proxy:

1. receiving the commit;
2. checking the registration is still valid and that `r` is not below the published floor;
3. adding `r` to `oldestInFlightCommitRV`;
4. making that pin visible to the floor protocol;
5. only then accepting the handoff and releasing the client's coverage;
6. withdrawing the pin once all Resolvers have completed validation.

The Commit Proxy is a natural custodian: it already holds per-batch state and already computes
a resolver-visible horizon (`fdbserver/commitproxy/CommitProxyServer.cpp:2104`). The
alternative — the client holds the lease until the full result *and* the proxy installs the
pin before expiry — can be made safe, but proving it still requires defining the order between
registration, expiry and publication. The first is therefore preferred.

### Why the combined minimum stays monotone

`oldestInFlightCommitRV` need **not** be monotone in isolation: a newly accepted request may
carry an RV below every other in-flight request. Monotonicity of the system comes from the
handoff overlap, not from each source being monotone on its own:

1. Before handoff, the client registration contributes a value no greater than the request's
   RV `r`.
2. The proxy installs the request pin at `r` **before** that contribution may disappear.
3. During the transition both are present.
4. Afterwards the pin remains, until validation completes.
5. Therefore no contribution `≤ r` ever vanishes while `r` is still needed.
6. A request whose `r` is below the already published floor is rejected.

The published floor is additionally defined as
`publishedFloor = max(previousPublishedFloor, newlyDerivedFloor)`, so publication is monotone
by construction even if a derived value momentarily is not.

### Identity: what the proxy actually checks

"The registration is still valid" needs an identity to check *against*. The commit request
carries the `clientID` and `leaseGeneration` under which its read version was protected — the
read version itself is already on the wire as `transaction.read_snapshot`, so only the two
identity fields are new, appended to `CommitTransactionRequest::serialize`
(`fdbclient/include/fdbclient/CommitProxyInterface.h:203–229`) under the same
protocol-version gating as §3. Note this is also a `PublicRequestStream` (`:44`), so those
fields are untrusted input and fall under §7a's ownership rules.

Handoff validation binds the server-side pin to that identity, generation and RV. The proxy
must verify the registration against the **authoritative lease state** — or through a
generation-fenced handoff operation in the floor protocol — rather than trusting a
possibly-stale broadcast watermark.

**The critical operation must be linearized with respect to expiry.** Exactly one of two
events wins:

- lease expiry **before** handoff → the request is rejected (`transaction_too_old`); or
- installation of the server-side pin **before** expiry → subsequent client death is
  irrelevant.

The transport that achieves this is an implementation choice (§8.4); the linearization
condition is not.

### Proxy failure does not withdraw coverage

The await of §9 closes the *normal* withdrawal path. It does not close this one: the proxy
installs the pin, accepts the handoff, dispatches to the Resolvers, and **dies before all
replies arrive**. If the Cluster Controller drops the dead proxy's source from the reduction
merely because the process disappeared, the floor advances while a request that still holds `r`
is alive in a Resolver — the very gap §4a exists to close. The invariant is the server-side
mirror of §4:

> **The death of a pin's custodian is not the termination of the requests that pin protects.**
> Loss or replacement of the Commit Proxy owning an in-flight pin may not remove that pin from
> the reduction merely because the process disappeared. Exactly one of the following must hold
> before its contribution is withdrawn: (1) a successor recovers or inherits the
> generation-fenced in-flight pins; (2) every associated Resolver request is fenced or proven
> unable to complete; or (3) a conservative generation barrier retains the proxy's last
> published minimum until all requests of that generation are guaranteed closed. Delayed
> cleanup may over-retain; process disappearance alone is never withdrawal evidence.

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
  expiry of a timeout — is the proof that the pins may be retired. This is the same shape as
  the GRV-proxy-generation barrier already required in §7.
- **Forward-looking:** if FDB ever gains single-proxy replacement without a full recovery, this
  reduction fails and inheritance or explicit fencing becomes mandatory. Any such change must
  revisit this section.

### Failure-injection cases

The property to check at every boundary:

`HANDOFF_ACCEPTED ⇒ pin visible ∨ request generation fenced from influencing a durable decision ∨ terminal validation outcome already known`

(*"fenced from influencing a durable decision"*, not *"fenced from validation"*: an old
Resolver may still execute and still produce a reply — what must be impossible is that any
authorized party consumes it to decide.)

- proxy dies after installing the pin but before accepting the handoff;
- dies after the handoff and before dispatching to the Resolvers;
- dies after dispatching to only some of them;
- all Resolvers reply, but the proxy dies before withdrawing or publishing the withdrawal;
- a successor generation appears while replies from the previous one are still arriving.

**The rules above are correctness requirements. The handoff's linearization mechanism must be
selected and covered by these cases before Resolver Phase A ships.**

## 5. Aggregation hierarchy

A hierarchical `min` reduction: client library → GRV proxy (over valid leases) → Cluster
Controller → `globalOldestClientRV` → broadcast → derived floors (§6).

**Corrected in review:** 1.0's "no component tracks transactions individually" is not literally
true once §4a exists. The real property is that *the hierarchy transports minima rather than a
cluster-wide transaction list*: clients track their own active read versions to compute their
minimum, Commit Proxies track only their accepted-but-unvalidated request pins, and the Cluster
Controller aggregates per-source minima. No component holds a global registry of transactions.

Empty reductions are defined **independently per source**: `min(∅) = currentVersion`.
1.0 said the derived floors then "fall back to their policy defaults", which is wrong in one
direction; an earlier draft of this document then said the Resolver simply reaches
`currentVersion`, which is wrong in the other, because it ignored the second source of §4a.
Precisely:

- If **both** the client-registration set and the accepted-but-unvalidated commit set are
  empty, `globalValidationDemand = currentVersion` and the Resolver may reclaim up to the
  current version — it does **not** fall back to the fixed-lag bound.
- If **either** source is non-empty, its minimum continues to pin the derived floor. With no
  clients but one in-flight commit at `r`, `globalValidationDemand = min(currentVersion, r) = r`
  and `resolverValidationFloor = max(r, currentVersion − W_commit)` — reclamation stops at `r`.

Register-before-use (§3) and handoff-before-release (§4a) jointly make this safe: every
consumer is covered first by a client registration and, after commit acceptance, by an
overlapping server-side pin.

**Open in 2.0 — §3 and §5 are in tension.** 1.0 has clients report to a *stable* GRV proxy
("one lease copy, no deduplication") while §3 piggybacks the report on `GetReadVersion`. But
GRV requests are **load-balanced across all GRV proxies today** —
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
precision — but 1.0 asserts the first. **Choose explicitly.**

## 6. Observation vs derived floors *(unchanged, and load-bearing)*

**Sources** (each with `min(∅) = currentVersion`):

- `globalOldestClientRV = min(valid client registrations)`
- `oldestInFlightCommitRV = min(accepted, not-yet-validated commit requests)` — §4a
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

## 7. Properties *(verified)*

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

## 7a. Adversarial input — NEW

`GetReadVersionRequest` arrives on a **`PublicRequestStream`**
(`GrvProxyInterface.h:227`, `fdbclient/include/fdbclient/CommitProxyInterface.h:48`) and its
`verify()` returns `true` unconditionally (`GrvProxyInterface.h:120`). A client-supplied
`clientFloor` is therefore untrusted input that, unbounded, pins cluster-wide history — a
denial-of-service against storage retention with a single malformed field. 1.0 has no
adversarial section. At minimum the proxy must **clamp** the reported floor to a
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

## 8. Open implementation choices *(deliberately)*

1. **Lease duration and renewal frequency** — a latency/over-retention trade-off.
2. **Watermark distribution transport** — `ServerDBInfo` field vs a light dedicated broadcast.
   **The concern is corroborated:** `ServerDBInfo.id` "Changes each time any other member
   changes" (`fdbserver/core/include/fdbserver/core/ServerDBInfo.h:40`), so a
   frequently-updated integer would rebroadcast the whole structure to every worker. Note also
   that `ServerDBInfo` is "not available to the client" (`:36–38`) — which is fine, since every
   consumer of the watermark is a server.
3. **New in 2.0 — stable-proxy vs multi-copy leases** (§5). Not a free choice: it decides
   whether the hot GRV path changes, and under multi-copy it decides how stale copies are
   refreshed or expired.
4. **New in 2.0 — where the §4a handoff is linearized against lease expiry.** Candidates: a
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

**Commit Proxies also acquire new transient state (§4a):** one generation-fenced pin per
accepted request — or an equivalent counted or per-batch representation — maintained as
`oldestInFlightCommitRV` and retained until every Resolver has completed validation.

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

## 10. Changelog 1.0 → 2.0

| # | Change | Basis |
|---|---|---|
| F1 | **§4a added — commit lifecycle handoff.** 1.0 never fixed the liveness boundary at commit submission; the race lets the floor pass an in-flight request's read version. Blocking for Resolver Phase A | review; `01-resolver.md` §3 |
| F2 | §5: **§3/§5 tension surfaced** — GRV requests are load-balanced today (`NativeAPI.cpp:5300`), so "stable proxy" and "piggyback on GetReadVersion" cannot both hold as written. Two resolutions given; multi-copy is correct but contradicts "no deduplication" | `NativeAPI.cpp:5300` |
| F3 | §5: empty reductions **corrected** — they are per-source, and the Resolver reaches `currentVersion` only when *both* client registrations and in-flight commit pins are empty; otherwise the non-empty source still pins the floor. (1.0's "fall back to policy defaults" was wrong in one direction, this document's first draft in the other) | §5 + §6 arithmetic |
| F4 | §7a added — **untrusted `clientFloor`**. `GetReadVersionRequest` is a `PublicRequestStream` with `verify() { return true; }`; an unbounded reported floor is a retention DoS. Clamp required | `GrvProxyInterface.h:120`, `:227` |
| F5 | §2: **`setVersion()` bypasses the sensor** (`NativeAPI.cpp:3594–3603`), and a fresh `DatabaseContext` has `clientFloor = 0`. Two resolutions; documenting the status quo preferred | `NativeAPI.cpp:3594–3603` |
| F6 | §9 added — GRV proxies hold **no per-client state today**; this adds state and a sweep to a latency-critical role, multiplied by proxy count under F2's second resolution | `GrvProxyServer.cpp:187–211` |
| F7 | §4: caution added so "`commit` → unregister" is not misread as licensing release at submission | F1 |
| F8 | §1, §3, §7, §8 **verified against code** — no per-client state today; the request format has established **protocol-version-gated** extension patterns (see F14); recovery already invalidates old read versions; `ServerDBInfo` churn concern is real | see cites inline |
| F9 | §6: the negotiation gate stated as a **scheduling fact** — Phase A ships as a retention no-op until participation is negotiated | `03` §6 |

Second review round — closing F1 properly:

| # | Change | Basis |
|---|---|---|
| F10 | §4a **rewritten**. The first draft's "hold the client registration until the commit reply" is withdrawn: it depends on client survival and so contradicts §4, whose whole premise is that clients crash. The pin must be **server-side** once the commit is accepted | review |
| F11 | §4a: failure mode stated precisely — admission and retention are **the same value today** (`Resolver.cpp:359` → `ConflictSet.cpp:805`, `:986`), so while coupled the race yields a **spurious `transaction_too_old`** (availability), and only a decoupled admission produces a false accept. Both removed by the same rule | `Resolver.cpp:359`, `ConflictSet.cpp:805`, `:986` |
| F12 | §4a: three states introduced — `CLIENT_COVERED` → `HANDOFF_ACCEPTED` → `VALIDATION_COMPLETE`; submission is not the handoff | review |
| F13 | §4a, §6: accepted-but-unvalidated commits become a **second source** in the reduction (`oldestInFlightCommitRV`), with the pin ordered before the client's coverage is released | review |
| F14 | §3: protocol-version **fencing** required — the in-tree idiom is `ar.protocolVersion().hasX()` (`CommitProxyInterface.h:151–153`, `CommitTransaction.h:351`); "append and old readers ignore" was too automatic | `CommitProxyInterface.h:151–153` |
| F15 | §7a: **impersonation** added — a forged update to another client's entry causes unsafe *under*-retention, which the clamp does not address; identity binding, ownership-checked generations, monotone updates and identity-creation caps required | review |
| F16 | Editorial: fenced blocks in §2 and §6 replaced with plain formula lines; "the GRV proxy as aggregation point" made conditional on the §5 choice; multi-copy staleness no longer described as refreshing "lazily" | review |

Third review round — discharging the proof obligations:

| # | Change | Basis |
|---|---|---|
| F17 | §4a: **monotonicity proof added.** `oldestInFlightCommitRV` is *not* monotone in isolation; the property comes from handoff overlap. Published floor defined as `max(previousPublishedFloor, newlyDerivedFloor)`. §7's "everything advances in one direction" narrowed accordingly | review |
| F18 | §4a: **identity added** — the commit carries `{clientID, leaseGeneration}` (its RV already travels as `transaction.read_snapshot`), gated like §3 and untrusted like §7a, since `CommitTransactionRequest` is also a `PublicRequestStream` | `CommitProxyInterface.h:44`, `:203–229` |
| F19 | §4a: **linearization condition** stated — exactly one of {expiry before handoff → reject, pin before expiry → client death irrelevant} wins; the transport is §8.4, the condition is not optional | review |
| F20 | §7a: the clamp made a **visible admission** — reject, or return the granted floor/capability; only the granted floor enters the reduction; a budget may deny a lease but not pretend it granted one | review; `02-storage.md` §5 |
| F21 | §5: "no component tracks transactions individually" **corrected** — the hierarchy transports minima, not a transaction list; clients track their own RVs and proxies track their pins | review |
| F22 | §8: fourth open choice added (where the handoff linearizes); "not on this list" reworded to *how, not whether* | review |
| F23 | Changelog F8 reconciled with F14 (protocol-version gating); §4a's blocking sentence replaced by the simulation requirement | review |
| F24 | §2: **manual-RV policy closed for v1** — `setVersion(v)` provides no extended-retention lease; protected old-version access requires the registered GRV path. This was a fifth open decision masquerading as a preference; §8 stays at four | review |
| F25 | §4a: garbled sentence about the alternative handoff repaired | review |

Fourth review round — implementation-cost and trust boundary:

| # | Change | Basis |
|---|---|---|
| F26 | §8.4: under **multi-copy leases** the handoff identity must locate one specific live registration — `{clientID, leaseGeneration}` does not name the custodian proxy, and the CC holds only aggregated minima. A generation-fenced `registrationID` or lease capability is required; the cost falls only on multi-copy, which the §5 trade-off should carry | review |
| F27 | §9: **Commit Proxy cost added** — per-request generation-fenced pins until `VALIDATION_COMPLETE`. Withdrawal maps onto the existing resolver-reply await (`CommitProxyServer.cpp:1008–1016`), so only installation needs new ordering; acknowledgement may not be downgraded to eventual propagation | `CommitProxyServer.cpp:1008–1016` |
| F28 | §7a: **trust boundary declared** — fencing stops cross-client tampering and retention DoS, but completeness of a client's own reported minimum is not provable. Same boundary FDB already relies on for client-supplied conflict ranges (`NativeAPI.cpp:3961`); Byzantine completeness is outside v1 | `NativeAPI.cpp:3961` |

Fifth review round — server-side symmetry:

| # | Change | Basis |
|---|---|---|
| F29 | §4a: **proxy failure does not withdraw coverage.** The §9 await closes only the normal path; a custodian that dies mid-validation would otherwise let the floor advance past a live request. Invariant added with its three admissible discharges | review |
| F30 | §4a: current FDB recovery **eliminates the need for proxy-local inheritance** — one proxy failure replaces the whole generation (`quorum(failed, 1)` → `commit_proxy_failed()`, `ClusterRecovery.cpp:461–472`, `:1952`), so option (1) is not expressible and option (2) holds structurally after recovery. A conservative barrier is nevertheless required until the old generation is fenced from influencing a durable decision; its release condition was subsequently corrected in F33. Forward-looking clause added if single-proxy replacement is ever introduced | `ClusterRecovery.cpp:461–472`, `:1387`, `:1952` |
| F31 | §8.4 acceptance criterion, §9 failure-path qualification, and five failure-injection cases with the property `HANDOFF_ACCEPTED ⇒ pin ∨ fenced ∨ terminal` | review |
| F32 | §7a: containment of a misreported client floor **demonstrated** via handoff rejection below the published floor, not merely asserted | review |
| F33 | §4a: barrier release **corrected from timer-driven to fencing-driven**. `TLOG_TIMEOUT` bounds detection, not recovery completion; the barrier holds until the old generation is provably unable to influence a durable decision — old Resolvers may still reply (`Resolver.cpp:832–843`) but the previous epoch's TLogs are locked at `epochEnd` (`LogSystem.cpp:420`). The simulation property now reads *fenced from influencing a durable decision* | review; `ClusterRecovery.cpp:466`, `Resolver.cpp:832–843`, `LogSystem.cpp:420` |

Sixth review round — internal coherence after the pin:

| # | Change | Basis |
|---|---|---|
| F34 | §1 **rewritten**: "every retention decision consumes one number" predates §4a. Storage consumes `globalOldestClientRV`; the Resolver consumes `min(globalOldestClientRV, oldestInFlightCommitRV)`. `globalOldestRV` standardized to `globalOldestClientRV` throughout | review |
| F35 | Closing sentence corrected: the no-gap guarantee holds **within a live generation**; recovery and priced revocation may end it explicitly under the existing `transaction_too_old` contract. Only *silent* early reclamation is forbidden | review; §7 |

**Unchanged and confirmed:** the client-side monotonic minimum, leases-not-unregistration, the
hierarchical `min` reduction, GRV proxies as the aggregation point for client registrations
(subject to the §5 choice), the observation/derived-floor
split, the §6 invariant, one-directional advance, and backward compatibility with no flag day.

## In one sentence

> Each client library maintains a monotonic minimum active read version, piggybacked on GRV
> requests and kept alive by a lease only while snapshots live; GRV proxies and the Cluster
> Controller reduce it by `min` into a single global watermark; retention responsibility is
> handed to a server-side pin on the accepted commit before the client's coverage is released
> and held until validation completes or the request's generation is fenced from influencing a
> durable decision. Delays may over-retain; reclamation may precede the last consumer only through
> explicit recovery or priced-revocation semantics, under the existing `transaction_too_old`
> contract — never silently.
