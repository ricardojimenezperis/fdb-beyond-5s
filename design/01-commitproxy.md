# Commit Proxy design

*Design draft. Implementation status is summarized in §6.*

The design baseline for existing FoundationDB internals is
`apple/foundationdb` main at `a443d3ee60`.

This document defines how Commit Proxies retain Resolver-assignment
history and reject transactions when the routing information needed to
validate their reads is no longer available. It complements the
[Resolver design](02-resolver.md) and
[Storage Server design](03-storage.md).

## 1. Why routing history is needed

A Commit Proxy sends a transaction's conflict ranges to the relevant
Resolvers. Resolver assignments can change while the transaction is
running. Earlier writes remain represented at the previous Resolver,
while subsequent writes are represented at the new one.

The proxy must therefore retain assignment history to route read-conflict
checks to every Resolver that may hold relevant writes after the
transaction's read version. Looking only at the current assignment can
miss a conflict.

For example, suppose a range moves from Resolver A to Resolver B at
version 200. A transaction reading that range at version 150 and
committing after the move may need checks at both Resolvers: A can hold
conflicting writes between the read version and the move, and B can hold
later ones.

The existing `keyResolvers` structure records versioned assignments per
key range. The proposed retention policy must allow this history to
outlive the existing fixed transaction window, consistently with longer
record and conflict retention.

## 2. State and assignment updates

Each Commit Proxy keeps:

- Its existing `keyResolvers` history, indexed by key range and containing
  versioned Resolver assignments.
- `routingMinRV`, the lowest read version for which the retained routing
  history is sufficient. It starts at `recoveryTransactionVersion` and
  never retreats during the proxy's lifetime.
- The existing `systemKeyVersions` history when
  `PROXY_USE_RESOLVER_PRIVATE_MUTATIONS` is enabled. Its pruning must
  preserve the information needed by work admitted at the local boundary.

Resolver assignment changes arrive through the existing
`GetCommitVersionReply`. The proxy incorporates the changes relevant to a
batch before selecting its destinations. Each change retains its effective
version so the proxy can identify both current and historical destinations.

`routingMinRV` describes the proxy's retained routing information. Each
Resolver independently maintains its own conflict-validation boundary.
The proxy's boundary is not a promise that a Resolver still holds all
history that the proxy knows how to locate.

## 3. Admission, routing and pruning

### 3.1 Routing a transaction

Before routing a transaction with read-conflict ranges, the Commit Proxy
compares its read version with `routingMinRV`:

- If the read version is below the boundary, the transaction receives
  `transaction_too_old`.
- Otherwise, the proxy uses the retained assignment history to select all
  Resolvers needed for its read-conflict checks.

Transactions without read-conflict ranges retain their existing exemption
from this routing-history age check. Their write-conflict ranges continue
to follow the assignments applicable to the commit.

The proxy combines Resolver responses using the existing commit path. A
Resolver may reject a transaction that passed the proxy's check if its
own validation boundary is newer. This rejection propagates through the
ordinary transaction error handling.

### 3.2 Pruning assignment history

When pruning to a new boundary `b`, the proxy preserves, for every key
range:

1. The assignment in effect at `b`.
2. Every subsequent assignment change.

Earlier entries may then be removed. Adjacent ranges can be coalesced
when their retained assignment histories are equivalent.

Advancing `routingMinRV` and removing the corresponding history form one
coordinated transition. Subsequent routing checks use the new boundary.
The new boundary must not exceed the version through which the proxy has
established its assignment history.

Admission, destination selection and pruning are ordered so that pruning
cannot remove information still needed by an admitted batch before it
has selected its required Resolvers. Any routing work that continues
after a suspension must retain the information it needs or complete
before that information is removed.

Once destinations have been selected, pruning must not invalidate any
references still used by the batch. Resolver-side validation remains
subject to each destination's own boundary when the check executes.

For example, pruning to version 200 after the move in §1 preserves the
assignment to B and later changes. A transaction at RV 150 is rejected
before routing. A transaction at RV 200 can be routed using the retained
history; writes at version 200 itself cannot conflict with a read at
that version.

Where `systemKeyVersions` is enabled, its pruning and use must be reviewed
alongside `keyResolvers`. Any history required by already admitted work
must remain available until that work has consumed it.

### 3.3 Retention policy and memory use

Routing history grows with assignment changes and range fragmentation.
Without reassignment, extending retention adds no entries to a range's
assignment history. Retaining different histories can also prevent
adjacent ranges from being coalesced.

The initial work will measure this growth before introducing a dedicated
routing-history budget. The measurements will guide the policy that
chooses when to advance `routingMinRV`. That policy remains to be
specified; the correctness rules in §3.2 apply to every pruning decision.

The existing fixed-window cutoff cannot remain an unconditional pruning
rule for extended transactions. Keeping all assignments for the entire
epoch is a possible measurement baseline, but is not a bounded memory
policy. Removing the cutoff alone does not complete resource handling.

Metrics will include the number of key ranges, total assignment entries,
maximum history depth and allocated or estimated memory. Periodic samples
and measurements around pruning and coalescing will distinguish peak
usage from sustained growth. `systemKeyVersions` will be measured in the
configuration that enables it.

## 4. Recovery

A newly recruited Commit Proxy starts with the new epoch's Resolver
assignments and initializes `routingMinRV` from
`InitializeCommitProxyRequest::recoveryTransactionVersion`.

The new proxy inherits neither the previous proxy's assignment history
nor its unfinished batches. FoundationDB recovery determines the outcome
of old work. The initialized routing boundary rejects transactions with
read-conflict ranges that require earlier history, independently of any
fixed retention-window calculation.

A transaction whose older reads succeeded at surviving Storage Servers
may consequently receive `transaction_too_old` when it attempts to commit
through the new transaction system.

## 5. Validation and measurements

### 5.1 Routing correctness

Tests will compare selected destinations with a model of versioned
Resolver assignments. They will cover:

- No reassignment, one reassignment and repeated moves of the same range.
- Splitting and coalescing ranges with different assignment histories.
- Read versions immediately below, at and above an assignment change and
  the routing boundary.
- Conflicting writes held only by a previous Resolver, only by the
  current Resolver, or by several historical destinations.
- Transactions spanning multiple ranges and Resolvers.
- Transactions without read-conflict ranges.

A transaction accepted for routing must reach every Resolver needed to
validate its reads. If the proxy no longer retains sufficient routing
history, it must reject the transaction rather than send an incomplete
set of checks.

### 5.2 Pruning and work in flight

Tests will interleave assignment updates, admission, destination selection,
pruning and batch completion. They will verify that the assignment at the
boundary survives, older entries are removed only when permitted, and
batches retain valid routing information until it has been consumed.

Tests will exercise a proxy admitting a transaction that a Resolver later
rejects after recycling conflict history. They will also cover the reverse
ordering of local boundaries: the proxy can reject a transaction even if
some Resolvers still retain its conflicts.

The private-mutation configuration will receive corresponding tests for
`systemKeyVersions` and its interaction with routing and pruning.

### 5.3 Recovery and integration

Deterministic simulation will combine long transactions, Resolver load
balancing, local conflict-history recycling and recovery. It will verify
initialization at `recoveryTransactionVersion`, established commit outcomes
and rejection when the new epoch lacks the required history.

Runs will cover ordinary and server-internal transactions, with the
feature enabled and disabled. Available-history differences will be
separated from missed conflicts or incorrect successful commits.

### 5.4 Costs

Workloads will vary assignment-change frequency, retained history length,
key-range fragmentation and transaction read-version age. Measurements
will include:

- Routing-history memory and growth over extended runs.
- The number of Resolvers contacted per transaction.
- Destination-selection, pruning and coalescing costs.
- Throughput and tail latency for short and long transactions.
- `transaction_too_old` responses caused by the proxy's routing boundary
  versus a Resolver's validation boundary.

These measurements will support the retention-policy decision in §3.3.

## 6. Status

The existing Commit Proxy provides versioned Resolver assignments,
conflict routing and the commit-processing path.

The local routing boundary, revised pruning and admission rules,
routing-history capacity policy, recovery integration and end-to-end
validation remain pending.
