# Design Overview

*Status: design baseline complete; implementation in progress.*

The descriptions of existing FoundationDB behaviour refer to
`apple/foundationdb` main at `a443d3ee60`. Proposed changes are described
in the future tense.

## 1. Why FoundationDB has a five-second transaction window

Storage Servers keep recent versions in memory so that a transaction
can continue reading the snapshot selected by its read version.
Keeping that history for longer increases memory consumption.
Once the required history has been discarded, a read returns
`transaction_too_old`.

Resolvers keep a separate history of write-conflict ranges and their
commit versions. They use it to check whether writes made after a
transaction's read version overlap its read-conflict ranges.
This history contains keys and versions, not stored values.

The two histories have separate retention settings:
`MAX_READ_TRANSACTION_LIFE_VERSIONS` for Storage Servers and
`MAX_WRITE_TRANSACTION_LIFE_VERSIONS` for Resolvers. Both default
to approximately five seconds.

The commit path also checks transaction age. Extending retention
therefore requires updating those checks consistently; retaining
older history alone will not enable longer write transactions.

Long transactions introduce two problems:

- Storage Servers must preserve older snapshots without keeping all
  their version history in RAM.
- Resolvers must reclaim obsolete conflict history without deleting
  its entries individually. Releasing a large amount of history
  should not impose a long cleanup pause on subsequent transactions.

## 2. Scope and implementation stages

The goal is to support both read-only and writing transactions beyond
five seconds. Ordinary short transactions should not bear the additional
storage and processing costs of old snapshots wherever those costs
can be isolated.

The work has three parts.

### 2.1 Tracking snapshots still in use

Clients will periodically report the oldest read version used by their
live transactions. GRV Proxies will combine these reports and publish
their minima to the Master.

Commit Proxies will separately report the oldest read version among
their accepted commits whose validation has not finished. The Master
will combine both types of proxy contribution into one retention floor.

Client leases and time-limited GRV Proxy delegations will prevent failed
clients or proxies from retaining history indefinitely. A Commit Proxy's
contribution will remain protected until its work finishes or is fenced
from completing.

With legacy-client support enabled, the ordinary retention windows will
remain in place, and reported read versions will only extend them.
With that support disabled, retention will follow the registered
transactions and commits still in flight.

Commit-path age checks will be updated consistently. The client's search
for whether a retried commit already succeeded will also need to cover
the permitted write-transaction lifetime.

The protocol is described in
[Floor tracking](01-floor-tracking.md).

### 2.2 Resolver conflict history in reusable arenas

The Resolver will keep its existing conflict-validation semantics and
mutable SkipList representation. Memory will be allocated through
at most two arenas: one receiving new writes and one holding older
conflict history.

Each arena will record its newest commit version. Once the retention
floor has passed that version, the entire arena can be recycled.
The goal is O(1) logical reclamation per arena, instead of work
proportional to its number of conflict entries. The allocator must
support that reuse without introducing a hidden per-entry cleanup.

Validation will search only arenas that could contain writes newer
than the transaction's read version. A transaction may require both
arenas; their lookup and memory costs must be measured.

Implementation will first connect the retention floor to the existing
Resolver, then introduce arena-based reclamation.

The structure and rotation rules are described in
[Resolver design](02-resolver.md).

### 2.3 Storage Server version history in spillable pages

The latest values will remain in the persisted single-version tree.
Older versions will be stored separately in shard-local history pages,
which may remain in memory or spill to disk.

Each page will record the newest commit that replaced one of its
historical values. Once no snapshot at or above the retention floor
needs those records, the page can be recycled as a unit.
Reclamation will require O(1) work per page rather than visiting
each historical value.

Reads satisfied by the main tree will avoid historical-page lookup.
Other reads will use resident history or fetch spilled pages as needed.

Historical pages will not become a second durable database.
A Storage Server restart will discard its local history. Reads whose
snapshots are no longer available will return `transaction_too_old`.

The layout, read paths and reclamation rules are described in
[Storage Server design](03-storage.md).

Together, these mechanisms will preserve the versions needed to read
older snapshots and the conflict history needed to validate longer
write transactions. They will not promise unlimited retention:
resource limits, expired coverage and failures may still cause a
transaction to become too old.

## 3. What the measurements tell us

### 3.1 Resolver

The original model predicted that removing entry-by-entry garbage
collection could approximately double Resolver throughput.
Measurements of the existing Resolver did not support that prediction.

The reclamation work targeted by the design accounted for about 8.4%
of conflict-detection time in the measured workload. Search accounted
for 61.5%. Increasing the benchmark's retained window from 50 to 200
and 500 versions left the reclamation share at approximately 9%.

These results do not establish a general throughput advantage for
two arenas. The reason to implement the design is whole-arena
reclamation when retained history becomes obsolete.

The prototype must measure the cost of searching two arenas, retained
memory, and arena reset and reuse. It must also measure commit latency
when a large amount of history becomes reclaimable.

### 3.2 Storage Servers

A `VersionedMap` tree node was measured at 88 bytes, allocated in a
96-byte block. This verifies one input to the memory model, but does
not establish total memory savings.

Those savings depend on the number of live nodes, key and value sizes,
allocator overhead, page-directory memory and spill behaviour.
They must be measured against total process memory.

The intended improvement is to let older history move to disk instead
of requiring the full retained window to remain in RAM. No numerical
RAM reduction or end-to-end CPU improvement is claimed yet.

## 4. Validation strategy

### 4.1 Component tests and reference models

Conflict validation and historical reads will be compared against
simple reference models. Tests will cover boundary versions, range
clears, arena rotation, page reuse and reclamation during suspended
reads.

Protocol tests will cover delayed, duplicated and reordered messages,
lease expiry, proxy replacement and recovery.

### 4.2 Deterministic simulation

FoundationDB simulation will exercise the integrated system under
different event orderings, failures and recoveries. Runs must remain
reproducible.

The full simulation suite must pass. Mixed-version tests will check
legacy-client support and transitions between retention modes.

### 4.3 Storage Server comparison

Test Storage Server pairing will compare the new implementation with
the existing one for snapshots both servers retain.

Older snapshots available only from the new implementation will be
checked against the reference model. A difference in retained history
must be distinguished from returning different data for the same
available snapshot.
