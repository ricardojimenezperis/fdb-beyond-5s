# Floor Tracking — the Oldest Active Read Version

*Status: design frozen; two implementation choices deliberately left open (§8). This chapter supplies the input signal that the dynamic retention floor (Resolver phase A) and the Storage retention machinery consume.*

## 1. Why this protocol must exist

Every retention decision in this project — the Resolver's dynamic floor, the Storage Server's `minimumRetainedVersion`, retention-floor accounting — consumes one number:

```
OldestActiveRV = min { RV(t) : transaction t is still alive }
```

Today's FoundationDB does not produce it, because it does not need to: the five-second constant makes transaction liveness irrelevant. Transactions are client-side constructs; a transaction can obtain a read version and sit idle, be read-only, be cancelled locally, or die with its process — and no server-side component observes any of that. Commits reveal only the transactions that end in a commit; Storage Servers see reads, not lifetimes. **The only place that knows a transaction's logical lifetime is the client library.** This chapter defines how that knowledge becomes one cluster-wide integer.

## 2. Client side: a monotonic minimum

Each client process (per `DatabaseContext`) tracks the read versions of its live transactions and maintains:

```
clientFloor = min(activeRVs)                        while transactions are active
clientFloor = max(clientFloor, latestGrantedRV)     when activeRVs is empty
— and clientFloor never decreases
```

With no active transactions the floor parks at the newest granted read version rather than resetting, so monotonicity survives idle periods; a new transaction may only use `rv ≥ clientFloor`.

Monotonicity is the load-bearing rule. When the oldest transaction ends, `clientFloor` advances — and that version is *irrevocably abandoned* by this client, whether or not the new value has been published yet. The published value may lag (`reportedMin ≤ localMin`); lag only causes the cluster to retain *more* history than needed, never less.

Consequently, `set_read_version(v)` requires `v ≥ clientFloor`. There is no protocol for re-claiming the past — matching current FDB, where setting an old read version has never obliged the cluster to still have it.

## 3. Transport: piggyback on GetReadVersion, lease when idle

The normal path adds **zero messages**. Every `GetReadVersion` request carries `{clientID, clientFloor, leaseGeneration}` — obtaining a new read version, reporting the floor, and renewing the client's lease in one existing round trip.

Explicit traffic appears in exactly one case: a client holding a long-lived read version while no longer requesting new ones sends a periodic `RenewOldestReadVersion`. **The new traffic is generated precisely by the transactions that use the new capability** — cost privatization, applied to the wire.

Bootstrap: for a client with no active transactions, the GRV proxy registers the chosen version as the client's floor *before* replying, so a read version is protected from the instant the library receives it.

## 4. Leases, not unregistration

Relying on `commit/cancel/destructor → unregister` fails on the one case that matters: a crashed client process never sends anything. Each GRV proxy therefore keeps, per client process, `{minRV, leaseExpiration}`; an expired lease removes the client and recomputes the proxy minimum. A dead client merely over-retains history for one lease timeout. Correctness never depends on promptly detecting death.

## 5. Aggregation hierarchy

No component ever tracks transactions individually — the structure is a hierarchical `min` reduction:

```
millions of transactions
        ↓ min                    (client library)
one integer per client process
        ↓ min                    (GRV proxy, over valid leases)
one integer per GRV proxy
        ↓ min                    (Cluster Controller, periodic reports)
globalOldestRV
        ↓ broadcast              (ServerDBInfo or a light dedicated channel — §8)
derived floors (§6): storage retention · resolver validation · admission policy
```

Empty reductions are defined: with no valid leases anywhere, `min(∅) = currentVersion` (equivalently a +∞ sentinel that every consumer clamps), so all derived floors fall back to their policy defaults.

Clients report to a **stable** GRV proxy (derived from `clientID` and the current proxy set, re-derived on `ClientDBInfo` generation changes): one lease copy, no deduplication, natural balancing.

The GRV proxy is the right aggregation point by symmetry — *it hands out new read versions and receives the watermark of the ones still alive*. Alternatives considered and rejected: Cluster Controller directly (per-client state on a global singleton), Resolver or Commit Proxy (they observe only transactions that attempt to commit), Storage Servers (multi-shard fan-out, and transactions can exist without reading).

## 6. Observation vs derived floors

`globalOldestRV` is an **observation**: the oldest snapshot some live transaction still claims. It is one sensor — but its consumers derive **different floors**, because applying one indiscriminate minimum everywhere would re-socialize exactly the cost this project privatizes:

```
normally:    storageRetentionFloor ≤ globalOldestRV
revocation:  storageRetentionFloor > globalOldestRV        (priced — 02-storage.md §5)

mixed/legacy mode:            resolverValidationFloor = now − W_commit
all-reporting (negotiated):   resolverValidationFloor = max(globalOldestRV, now − W_commit)
```

- **Storage retention** protects snapshots: it follows the oldest active reader (and other retention pins such as change feeds), modulated by policy. Under extreme pressure the policy may exceed the observation — which *is* the priced revocation mechanism of the Storage design (`minimumRetainedVersion` watermark).
- **Resolver validation** covers only transactions still certifiable under the ordinary protocol. The `now − W_commit` cap means a long *reader* never forces the Resolvers to retain history beyond the ordinary commit window — v1's scope line, enforced by a formula. Two consequences: the Resolvers never retain *more* than today; and, **once participation is cluster-negotiated** (standard capability negotiation), they may reclaim earlier whenever no old certifiable transaction exists — the dynamic floor's efficiency dividend. In mixed-version clusters the ordinary commit window remains the mandatory floor: non-participating clients are invisible potential committers, so `globalOldestRV` may not advance the resolver floor past them. After promotion (read-reservations phase), ordinary history may conservatively remain until the normal commit horizon advances past the transaction's read version — the `now − W_commit` cap already bounds that cost; reservations provide correctness beyond the fence, and a promotion-aware commit floor is a later optimization (`04-read-reservations.md`).

**Invariant:** *a long read-only transaction must never force conflict-history retention beyond the ordinary commit window.*

## 7. Properties

- **Everything advances in one direction** — client floor, proxy floor, global watermark, retention floor. Delayed or lost messages can only cause conservative over-retention, never premature reclamation. Stale metadata never breaks correctness.
- **Recovery coherence.** Lease state lives in GRV proxies and dies with a transaction-system recovery — and that is *already* the adopted contract: cluster recovery may invalidate all retained history, preserving existing `transaction_too_old` semantics (see `02-storage.md` §6). The two chapters lock together for full recoveries. For proxy-generation changes, however, a barrier is **mandatory** — a single proxy replacement must not invalidate long readers: *a proxy-generation change cannot advance the global floor past any lease that could still have been valid in the previous generation.* Minimal implementation: the CC holds the last known watermark for one full lease period after the change while clients re-register; lease-state transfer or replication are optimizations, not requirements.
- **Backward compatible with no flag day.** A legacy client never reports, therefore never holds the floor down — and the policy layer's default minimum window (~5 s) preserves exactly today's contract for it — made literal by the mixed-mode resolver formula of §6. Only clients that want long reads need to speak the protocol.

## 8. Open implementation choices (deliberately)

1. **Lease duration and renewal frequency** — a latency/over-retention trade-off to be set with measurements.
2. **Watermark distribution transport** — `ServerDBInfo` field (simplest) vs a light dedicated broadcast (if updating a frequently-changing integer would churn `ServerDBInfo`). Semantics are fixed — the CC owns and distributes `globalOldestRV`; the vehicle is a code-level decision.

## In one sentence

> Each client library maintains a monotonic minimum active read version, piggybacked on GRV requests and kept alive by a lease only while snapshots live; GRV proxies and the Cluster Controller reduce it by `min` into a single global watermark, and delays can only ever retain too much — never reclaim too soon.
