# Floor Tracking — the Oldest Active Read Version

*Reviewed against `apple/foundationdb` main @ `a443d3ee60`. Claims about current FDB carry
`file:line`.*

## 1. Why this protocol is needed

Today, only the client library knows which transactions remain active and which
read versions they use. GRV requests do not carry enough information for the
GRV Proxy or the Master to follow a transaction's lifetime, and FoundationDB
does not monitor the health of individual clients. Storage Servers and
Resolvers therefore retain history for a fixed period and discard it without
knowing whether a live transaction still needs it.

The new protocol will let clients periodically report the oldest read version
used by their live transactions. Each GRV Proxy will combine its clients'
reports and send its oldest reported version to the Master. Reports will have a
lease: if a client dies or stops reporting, its lease will expire and it will
no longer hold the retention floor back. This is less intrusive than adding
server-side health monitoring and failure detection for every client.

Commit Proxies will independently report the oldest read version among the
commits they have accepted but not yet finished validating. The Master will
combine the minima reported by both types of proxy into one cluster-wide
retention floor. This will protect a write transaction continuously as
responsibility passes from the client and its GRV Proxy to a Commit Proxy.
How Storage Servers and Resolvers combine the resulting floor with the ordinary
five-second window will depend on the legacy-client support mode described in
§5.

## 2. Client responsibilities

The client will maintain, for each `DatabaseContext`, the oldest read version
used by any live transaction. A transaction will remain in this calculation
until it finishes, including while its commit has been sent but has not yet
received a terminal result. Read versions supplied manually through
`setVersion()` will not receive extended retention protection.

The client will periodically send the GRV Proxy its lease identifier and that
single oldest version, using requests it already sends. It will never send one
registration per transaction. If it has no live transactions, it will report no
minimum. The client will receive the normal GRV reply and no confirmation that
retention was installed, so it will continue reporting while its transactions
remain alive. If the required history is nevertheless unavailable, the
transaction will fail with `transaction_too_old`.


## 3. Transport and installation

The protocol will add the client's lease identifier and oldest active read
version as an optional field in `GetReadVersionRequest`. Existing GRV requests
will therefore carry the report without adding another message or round trip.
A client with long-running transactions but no new GRV requests will send
periodic reports to keep its lease alive. Older clients will omit the field, and
the protocol will remain disabled until every Master that may receive it
understands it.

Each GRV Proxy will combine the reports received from its clients with its
existing live leases and send the resulting minimum to the Master through the
`getLiveCommittedVersion` request it already makes. The Master will apply that
snapshot and return the minimum it actually installed. Only after receiving
that acknowledgement will the GRV Proxy create or renew the corresponding
client leases and send the GRV replies. Each client report will be considered
individually, so one version that cannot be protected will not prevent other
reports in the same batch from being installed.

The read version returned by a GRV request will not be registered before it is
used. While legacy-client support is enabled, the ordinary five-second window
will protect it until it appears in a later client report. Once legacy-client
support is disabled, there will be no fixed five-second fallback: the version
will acquire retention protection only after a client report containing it has
been installed. If the required history is reclaimed first, the transaction may
fail with `transaction_too_old`, but it can never read or commit using history
that is no longer available.

## 4. Client leases

An explicit unregister message would not handle the most important failure: a
client that crashes sends nothing. Each GRV Proxy will therefore keep a client's
reported minimum only for a limited lease period. A client will renew that lease
through its periodic reports. If it stops reporting, the lease will expire and
its read version will stop holding back retention.

A report with no active read version will stop renewing the existing lease
rather than remove it immediately. This prevents a delayed report from
discarding newer information. At worst, a dead or inactive client will retain
history until its current lease expires.

## 4a. Protecting commits in flight

Client leases alone are not sufficient for write transactions. A client may die
after sending a commit, and its lease may expire while the Commit Proxy is still
waiting for validation. The Commit Proxy will therefore take responsibility for
the transaction's read version before admitting the commit.

For each batch, the Commit Proxy will compute the oldest read version among its
commits and send it to the Master through the existing commit-version exchange.
If the required history has already been discarded, the affected commit will
be rejected with `transaction_too_old`. Otherwise, the Master will include that
read version in the retention floor before validation proceeds.

The Commit Proxy will then report the oldest read version among all its
accepted, non-terminal batches. This protection will not be a lease and will
not expire: it will remain until validation finishes or the Commit Proxy
generation is fenced. The proxy will reuse the batch state it already keeps, so
the protocol will add no per-transaction server state and no additional network
round trip.

A full cluster recovery may continue to invalidate transactions from the
previous generation, as FoundationDB does today. Replacing an individual GRV
Proxy or Commit Proxy must not invalidate long-running transactions: client
leases remain until they expire, and protection for an accepted commit remains
until it finishes or its proxy generation is fenced.

## 4b. GRV Proxy safety rules

When the protocol is activated, GRV Proxy integration will follow four rules:

1. The Master acknowledgement will state the oldest version actually protected
   for that proxy. A client lease will be created or renewed only if that
   protection reaches the version reported by the client. This decision will
   not affect the read version returned in the normal GRV reply.

2. After processing a batch, the proxy will compute its next reported minimum
   over all its live leases, not only over the clients in that batch. A batch
   that installs no new lease will not remove leases created by earlier
   batches.

3. A report that cannot be protected will leave existing state unchanged. It
   will neither shorten a previously granted lease nor reactivate a lease that
   has already been withdrawn.

4. The proxy will finish integrating the complete batch and schedule every
   required expiration before replying to any client.


## 4c. Replaceable proxy snapshots

Today, the GRV Proxy can build and integrate a floor snapshot, and the Master's
state transition exists as tested code. The two sides are not connected: the
Master does not yet read the snapshot or return its acknowledgement, and
protocol negotiation remains disabled.

When activated, each GRV Proxy will publish a snapshot containing the oldest
version among its live leases and the client reports in the current batch. The
current batch must be included because a new client's version is not yet present
in the lease map. Without it, the first report of an older version could never
be installed.

A proxy's minimum can move both forward and backward, so snapshots will replace
the proxy's previous value rather than be merged with it. Every snapshot will
carry an increasing sequence number, and the Master will apply it only if it is
newer than the last snapshot accepted from that proxy incarnation. A snapshot
with no minimum will remove the proxy's contribution.

The Master will return the minimum it actually installed after applying its
retention limits. The GRV Proxy will use that acknowledgement to decide each
client report independently: it will create or renew a lease only when the
installed protection reaches the client's reported version. Reports that are
not covered will leave the lease map unchanged. The proxy will then schedule
the required expirations and send the ordinary GRV replies.

The Master will enforce an administrative limit on how far retention may be
extended. A client report older than that limit will install no lease. The
client will still receive the ordinary GRV reply and may later receive
`transaction_too_old` if it tries to use history that was not retained. This is
a resource policy, not a separate client-validation protocol.

Before activation, three pieces remain: wiring the transition into the Master,
preventing messages from a retired proxy incarnation from recreating its
contribution, and implementing client publication, delegation and protocol
negotiation.


## 5. Aggregation hierarchy

The protocol will aggregate read versions hierarchically rather than send a
cluster-wide list of transactions. Each client will report one minimum to a GRV
Proxy. Each GRV Proxy will report the minimum over its live client leases, and
each Commit Proxy will report the minimum over its accepted, non-terminal
batches. The Master will combine the values reported by both types of proxy into
one cluster-wide retention floor.

Retention will depend on the legacy-client support mode. When legacy clients
are supported, Storage Servers and Resolvers will always preserve the ordinary
five-second window because those clients do not report their active read
versions. In this mode, the reported floor can only extend retention beyond
five seconds.

When legacy-client support is disabled, Storage Servers and Resolvers will
retain only the history required by reported live transactions and accepted
commits still in flight. If neither population reports a minimum, the protocol
will impose no additional retention requirement.

GRV requests are currently load-balanced across GRV Proxies, so reports from
one client may reach different proxies. The protocol will preserve this load
balancing and allow several proxies to hold leases for the same client. Each
copy will expire independently if it is not renewed. A stale copy may retain
history longer than necessary, but it cannot cause required history to be
removed early. This avoids pinning a client to one GRV Proxy, at the cost of up
to one lease entry per client on each proxy.

The Master will publish the selected floor to Storage Servers, Resolvers and
Commit Proxies. Storage Servers will use it to retain snapshot versions,
Resolvers to retain conflict history, and Commit Proxies to retain the
corresponding `keyResolvers` mapping. These three histories must advance
consistently: preserving one is not useful if another required to process the
same transaction has already been discarded.

Disabling legacy-client support requires an explicit activation barrier. The
cluster must first stop granting new read versions to legacy clients, wait for
the ordinary five-second window granted to existing ones to expire, and allow
their accepted commits to finish or be fenced. Only then may it remove the
five-second fallback and retain history exclusively from reported minima.


## 6. Remaining implementation choices

### Lease timing

The GRV Proxy currently uses `FLOOR_LEASE_DURATION`, for now set to one
second, as the lifetime of a client lease. The client publication interval is
not implemented yet. Before activation, that interval must be configured on the
client and kept comfortably shorter than the lease duration.

The Master will grant each GRV Proxy a time-bounded delegation lease. A proxy
will be allowed to create or renew client leases only while that delegation is
valid. If the proxy dies or becomes partitioned from the Master, it will be
unable to renew the delegation. Once the delegation expires and every client
lease that could have been granted under it has also expired, the proxy's last
reported minimum will stop contributing to the cluster-wide retention floor.
A failed proxy will therefore be unable to retain history indefinitely.

The delegation mechanism and its timing are not implemented yet.

### Publishing the retention floor

The Master will periodically distribute the retention floor to Storage
Servers, Resolvers and Commit Proxies through the existing DBInfo broadcast
tree. Floor updates will use a dedicated message and channel: they will not
modify DBInfo or trigger its change handlers.

The Master will reserve each floor advance before publishing it. Once
reserved, it will not accept a registration below that floor. Each update
will identify the Master's generation and its sequence within that
generation, so recipients can reject stale updates.

Publication will continue even when there are no writes. A delayed update
will retain history longer than necessary; it will not authorize early
reclamation. Periodic snapshots will also allow recipients to detect when
updates stop arriving.

Commit Proxies will additionally include the Master-authorized floor in
commit batches sent to Resolvers. This will coordinate conflict-history
retention with the corresponding keyResolvers history.

The broadcast and batch paths will carry the same floor. Neither relays nor
consumers will independently authorize an advance.

### Proxy failure

A GRV Proxy will be allowed to renew leases only until the deadline granted by
the Master. If the proxy becomes unreachable, the Master will keep its
contribution until every lease it could have renewed before that deadline has
expired. A Commit Proxy contribution will not expire on a timer; it will remain
until its commits finish or its generation is fenced.


## 9. Costs to measure

GRV Proxies currently keep no per-client state. The protocol will add up to one
lease entry per client on each proxy that receives one of its reports, together
with expiration work. With multi-copy leases, the worst-case state grows with
both the number of clients and the number of GRV Proxies. Memory per lease and
the CPU cost of renewal and expiration must be measured before fixing the lease
duration and publication interval.

The Master will add one floor entry per GRV Proxy and Commit Proxy. Updates will
use requests those proxies already send, so they will add no round trip. They
will, however, add bytes and processing to the Master’s serialized critical
path. The added latency, CPU cost and update rate must be measured.

Commit Proxies will keep an aggregate over their pending batches, reusing
transaction state they already hold rather than creating a second
per-transaction registry. This state is expected to be smaller, but its cost
must still be included in the benchmark.


## Implementation status

The protocol described in this document is not active end to end. Some of its
internal components are implemented and tested, but the client, Master and
consumers are not yet connected.

| Component          | Implemented today                                                                                                          | Still missing                                                                                                                                         |
| ------------------ | -------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| Client             | `GetReadVersionRequest` contains the optional registration field.                                                          | The client does not track or publish its oldest active read version. Periodic publication is not implemented.                                         |
| GRV Proxy          | Batch sealing, acknowledgement integration, local leases, expiration and normal client replies are implemented and tested. | Protocol negotiation always returns `false`, so this path is not used in production. Publishing a newer minimum after leases expire is also disabled. |
| Master             | The state transition that applies an ordered proxy snapshot exists as a tested pure function.                              | The Master does not call that transition, consume proxy snapshots or return acknowledgements.                                                         |
| Master delegation  | No delegation is issued.                                                                                                   | The Master must give each GRV Proxy a bounded period during which it may create or renew leases. Without a delegation, the proxy grants no lease.     |
| Commit Proxy       | The existing batches contain the read versions needed to compute their minimum.                                            | Transferring protection for accepted commits to the Master is designed but not implemented.                                                           |
| Floor distribution | FoundationDB's ordinary five-second retention behavior remains active.                                                     | Periodic publication to Storage Servers and Resolvers, request-carried delivery to Resolvers and retention of `keyResolvers` are not wired.           |
| Overall activation | —                                                                                                                          | End-to-end activation and mixed-version negotiation remain disabled.                                                                                  |

Clients continue to receive the standard GRV reply. It contains no
floor-specific result, acknowledgement or lease grant.
