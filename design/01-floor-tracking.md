# Floor Tracking Design

*Design baseline: `apple/foundationdb` main at `a443d3ee60`.
The implementation status is summarized in §8.*

## 1. Why this protocol is needed

Today, only the client library knows which transactions remain active
and which read versions they use. GRV requests do not give the servers
enough information to follow a transaction's lifetime, and FoundationDB
does not monitor the health of individual clients. Storage Servers and
Resolvers therefore retain history for a fixed window without knowing
whether a live transaction still needs it.

The new protocol will let clients periodically report their oldest
active read version. Each GRV Proxy will combine these reports and send
its minimum to the Master. Client reports will be protected by leases:
if a client dies or stops reporting, its lease will expire and its
version will stop holding back retention. This avoids adding health
monitoring and failure detection for every client.

Commit Proxies will independently report the oldest read version among
their accepted commits whose validation has not finished. The Master
will combine the contributions from both types of proxy into one
cluster-wide retention floor.

Storage Servers and Resolvers will use that floor to preserve the
history needed by these transactions. Whether they also preserve the
ordinary five-second window will depend on the legacy-client support
mode described in §5.

## 2. Client responsibilities

The client will maintain, for each `DatabaseContext`, the oldest read
version used by its live transactions. A transaction will remain in
this calculation until it finishes, including while its commit has
been sent but has not received a terminal result. Read versions
supplied manually through `setVersion()` will not receive extended
retention protection.

Within one `DatabaseContext` and recovery generation, the client will
assign newly granted read versions in non-decreasing order. If a GRV
reply arrives late with an older version, the transaction will use the
newest version already granted to that context. Replies from an earlier
recovery generation will be rejected. This will prevent a newly created
transaction from lowering a minimum the client has already reported.

The client will send its lease identifier and that single oldest
version with its GRV requests. While transactions remain active, it
will also publish periodically if ordinary requests are insufficient
to keep its lease renewed. It will never send one registration per
transaction. With no active transactions, it will report no minimum.

The client will receive the normal GRV reply, without a lease grant
or confirmation that retention was installed. If the required history
is unavailable, the transaction will fail with `transaction_too_old`.

## 3. Transport and installation

The protocol will add an optional registration containing the client's
lease identifier and oldest active read version to
`GetReadVersionRequest`. Reports attached to existing GRV requests
will add no message or round trip. Clients with long-running
transactions but no new GRV requests will send periodic reports.

Older clients will omit the registration. Protocol negotiation will
prevent activation while participating server roles cannot process
the new fields.

Each GRV Proxy will combine the reports in a batch with its existing
live leases. It will send the resulting snapshot to the Master through
the `getLiveCommittedVersion` exchange it already performs.

The Master will process the snapshot and acknowledge the contribution
installed for that proxy. The GRV Proxy will then consider each client
report separately. A report that cannot be protected will not prevent
other reports in the batch from installing or renewing leases.

The read version returned by a GRV request will not be registered
before it is used. With legacy-client support enabled, the ordinary
five-second window will initially protect it.

With legacy-client support disabled, there will be no fixed-window
fallback. A newly granted version will acquire retention protection
only after a report containing it has been installed. If its history
is reclaimed first, the transaction may receive `transaction_too_old`,
even before five seconds have elapsed.

In either mode, a retention report cannot restore history that has
already been discarded.

## 4. Proxy contributions

### 4.1 Client leases

Each GRV Proxy will keep a client's reported minimum for a limited
lease period. Reports accepted through the protocol will renew that
lease. If the client stops reporting, the lease will expire and its
version will stop contributing to the proxy's minimum.

A report with no active read version will stop renewing an existing
lease rather than remove it immediately. This prevents a delayed
empty report from discarding protection renewed by a newer report.

A refused renewal will leave the previous lease and its deadline
unchanged. Withdrawing an entry will prevent further renewals without
releasing protection before its existing deadline.

### 4.2 Protecting commits in flight

Client leases alone are insufficient for write transactions. A client
may die after sending a commit, and its lease may expire while
validation is still pending. The Commit Proxy will therefore install
protection for the commit's read version before admitting it.

For each batch, the Commit Proxy will compute the oldest read version
and send it to the Master through the existing commit-version exchange.
A commit below the floor already reserved by the Master will be
rejected with `transaction_too_old`. Protection for admitted commits
will be installed before validation proceeds.

Storage Servers and Resolvers will still check the history they
actually retain. The Master's admission cannot restore history lost
by an individual server.

The Commit Proxy will maintain a minimum over all its accepted,
non-terminal batches. This contribution will not expire on a timer.
It will remain until the corresponding work finishes or is fenced
from completing.

Replacing a proxy must not immediately release its contribution.
A GRV Proxy's contribution will remain until every client lease it
could have granted has expired. A Commit Proxy's contribution will
remain until its accepted work finishes or is fenced.

A full cluster recovery may continue to invalidate transactions from
the previous generation.

### 4.3 GRV Proxy integration

A GRV Proxy will create or renew a client lease only when:

- The client reports a minimum.
- The acknowledged installed minimum is at or below that version.
- The proxy's delegation from the Master is still valid.
- The lease entry has not been withdrawn.

A report that fails these conditions will leave existing lease state
unchanged. Its result will not alter the ordinary GRV reply.

The proxy will integrate the complete batch and schedule all required
expirations before replying to any client. There will be no suspension
between lease integration and the first reply.

Subsequent snapshots will include all live leases, not just entries
created or renewed by the latest batch. A batch that installs no new
lease will not remove protection belonging to earlier batches.

### 4.4 Replaceable proxy snapshots

Each GRV Proxy snapshot will contain the minimum over its live leases
and the client reports in the current batch. Including the batch is
necessary: a new client's read version is not yet in the lease map.

A proxy's minimum can move in either direction as clients arrive,
advance or expire. Snapshots will therefore replace the previous
contribution rather than combine with it using `min`.

Every snapshot will carry an increasing sequence number. The Master
will apply it only if it is newer than the last snapshot accepted
from that proxy incarnation. A snapshot with no minimum will remove
the contribution while preserving the sequence needed to reject
older messages.

Retiring a proxy incarnation will separately prevent any later
message from that incarnation from recreating its contribution,
regardless of sequence number.

The Master will apply its reserved retention floor and administrative
retention limit when installing a snapshot. A client report older
than the installed minimum will acquire no lease. The client will
still receive the ordinary GRV reply.

An acknowledgement will describe the contribution installed when
the Master processed the request. An older or duplicate snapshot
will leave the contribution unchanged and receive its current value.

Reports from other batches awaiting acknowledgement will not be
included unless they already have a live lease. A later snapshot
may therefore supersede a first registration.

A delayed acknowledgement may also arrive after the Master has
replaced the contribution it describes. A lease installed from that
reply will enter the next snapshot, but this will not restore
history already reclaimed. An affected transaction may receive
`transaction_too_old`.

## 5. Aggregation and retention modes

The protocol will transport aggregated read versions rather than a
cluster-wide transaction list. Each client will report one minimum.
Each GRV Proxy will publish a snapshot over its live leases and
current batch reports. Each Commit Proxy will report the minimum
over its accepted, non-terminal batches.

The Master will combine these contributions into one retention floor.
Individual contributions may move in either direction, but the floor
reserved by the Master will only advance. New registrations below
that floor will not acquire protection.

### 5.1 Legacy-client support

With legacy-client support enabled, Storage Servers and Resolvers
will preserve their ordinary retention windows because legacy
clients do not report active read versions. Reported versions will
only extend retention beyond those windows.

With legacy-client support disabled, retention will follow installed
client contributions and accepted commits still in flight. If neither
population has an installed contribution, historical versions and
conflict history will be eligible for reclamation. The current
database state will remain intact.

Legacy-client support will be a startup configuration parameter.
The cluster will start with it either enabled or disabled, and
the setting will remain unchanged while the cluster is running.

When disabled, legacy clients will not be admitted.
No runtime transition between the two modes will be supported.

### 5.2 Reports reaching different proxies

GRV requests are currently load-balanced across GRV Proxies. The
protocol will preserve this behaviour, so several proxies may hold
leases for the same client.

Each copy will expire independently if it is not renewed. A stale
copy may retain history longer than necessary, but will not release
history early. This avoids pinning clients to a particular proxy,
at the cost of up to one lease entry per client on each proxy.

### 5.3 Consumers of the floor

Storage Servers will use the floor to retain snapshot versions.
Resolvers will use it to retain conflict history. Commit Proxies
will preserve the corresponding `keyResolvers` mapping.

These histories must remain consistent: keeping conflict history
is insufficient if the mapping needed to locate it has already
been discarded.

Each consumer will remain responsible for the history it actually
has. An operation requiring unavailable history will receive
`transaction_too_old`.

## 6. Lease timing and floor distribution

### 6.1 Lease timing

The GRV Proxy implementation currently uses `FLOOR_LEASE_DURATION`,
set to one second, for client leases. Client publication is not yet
implemented. Its interval will need to be comfortably shorter than
the lease duration.

The Master will grant each GRV Proxy a time-bounded delegation.
A proxy will create or renew client leases only while that
delegation remains valid.

If a proxy dies or loses contact with the Master, it will be unable
to renew its delegation. The Master will retain its contribution
until the delegation and every client lease that could have been
granted under it have expired. The contribution can then be removed.

The delegation mechanism and its timing remain to be implemented.

### 6.2 Publishing the retention floor

The Master will periodically distribute the floor to Storage Servers,
Resolvers and Commit Proxies through the existing DBInfo broadcast
tree. Updates will use a dedicated message and channel. They will
not modify DBInfo or trigger its change handlers.

The Master will reserve each advance before publishing it. Once
reserved, it will reject registrations below that floor. Updates
will identify the Master's generation and carry a sequence number,
allowing recipients to reject stale messages.

Publication will continue while the cluster is idle. Delayed updates
will retain history longer than necessary. Missing updates will not
authorize consumers to advance the floor independently.

Commit Proxies will also include the Master-authorized floor in
commit batches sent to Resolvers. This will coordinate conflict
history with the corresponding `keyResolvers` history.

Both delivery paths will carry the same authorized floor. Relays
will distribute it without making retention decisions.

## 7. Costs to measure

GRV Proxies will acquire per-client lease state and expiration work.
With multiple copies, the worst-case state will grow with both the
number of clients and the number of GRV Proxies. Memory per entry
and renewal and expiration costs must be measured before fixing
publication and lease intervals.

The Master will keep one contribution per GRV Proxy and Commit Proxy.
Reports attached to existing exchanges will add no round trip to
those operations, but will add bytes and processing to the Master's
serialized execution path.

Periodic client reports, delegation renewals and floor broadcasts
will also generate traffic when ordinary requests are unavailable
to carry updates. Their rate and cost must be measured.

Commit Proxies will maintain an aggregate over pending batches,
reusing the transaction state they already hold. The benchmark must
include this state and the cost of publishing contribution changes.

## 8. Implementation status

The GRV Proxy changes are implemented and tested but remain dark:
they do not install client leases or affect production retention.
Client reporting, the Master's authoritative path and floor
delivery to consumers are not yet connected.

- **Client:** the optional registration field exists in
  `GetReadVersionRequest`. Tracking and periodic publication are
  not implemented.

- **GRV Proxy:** snapshot sealing, acknowledgement integration,
  local leases, expiration and ordinary replies are implemented
  and tested. Negotiation returns `false`, and publication following
  lease expiry is disabled.

- **Master:** the ordered snapshot transition exists as a tested
  pure function. The Master does not yet consume snapshots or
  return their acknowledgements. Retired incarnations still need
  to be fenced against later messages.

- **Delegation:** the Master issues no delegation. Without one,
  the GRV Proxy grants no client lease.

- **Commit Proxy:** protection for accepted commits is designed
  but not implemented.

- **Distribution and consumers:** floor broadcasting, delivery
  through commit batches and coordinated retention are not wired.
  Ordinary FoundationDB retention remains active.

- **Activation:** end-to-end activation, mixed-version negotiation
  and enforcement of the startup legacy-client setting remain
  unimplemented.

Clients continue to receive the standard GRV reply, with no
floor-specific result, acknowledgement or lease grant.
