# Design Overview

The baseline for existing FoundationDB behaviour is
`apple/foundationdb` main at `a443d3ee60`.

## 1. Introduction

FoundationDB's small, fixed read-version window—normally five seconds—
limits transaction duration. Extending that window requires retaining
more history: primarily old data versions, currently held in memory,
and secondarily the conflict history needed to validate commits.

This project addresses these constraints with a new history-storage
design. Storage Servers will retain old versions in spillable pages,
allowing history to grow beyond RAM capacity. Whole pages are reclaimed
in O(1), without visiting individual records. Resolvers will retain conflict
history in a B+ tree of disjoint intervals with O(1) logical key-page
reclamation. Short keys will be stored inline in the B+ tree; longer keys
will be represented by a prefix and a pointer to the full key in the key heap.

Administrative knobs configure the memory and disk capacity allocated
to old versions on each Storage Server and the memory capacity allocated
to conflict history on each Resolver. Operators can provision these
resources to support their desired transaction durations under the
expected workload.

Transaction duration is therefore governed by configured resources
and the rate of history generation, rather than a fixed five-second
window. Transactions can complete while the history they need remains
available; otherwise, affected operations return `transaction_too_old`.

### 1.1 How history is retained and recycled

Each Resolver will retain conflict history within a configured memory
budget. As capacity is needed, it will recycle old key pages and B+ nodes
and advance its validation boundary. 

Each Storage Server will keep current values in a persisted single-version
tree and historical versions separately in pages. Two circular arrays,
one in memory and one on disk, will hold those pages. Their capacities
will be configurable and shared by all shards on that Storage Server.
Pages will be recycled in allocation order as space is needed. Recycling
will advance the availability boundaries of the shards whose history is
discarded.

Commit Proxies will retain the history of Resolver assignments needed to
route conflict checks after load balancing. If a Commit Proxy discards
old assignments, it will advance its routing-history boundary and reject
commits requiring information it no longer holds.

Retention does not depend on a global minimum read version, client leases
or Master coordination as in the original design. 
Each component enforces the boundary of its own
available history. These boundaries need not coincide: a transaction may successfully read from a Storage Server but
later receive `transaction_too_old` when committing through a Resolver
that has already recycled the required conflict history.


### 1.2 Recovery and failures

The design will retain FoundationDB's existing failure detection and
recovery mechanisms. Newly recruited Resolvers will start with empty
conflict history and a validation boundary of
`recoveryTransactionVersion`. Commit Proxies will initialize their routing
state and admission boundary consistently with the new transaction-system
epoch.

A Storage Server that survives transaction-system recovery may still
serve older reads from its retained history. Recovery alone will not
reset its local history or move its availability boundaries backwards.

Historical pages, including spilled pages, will be deleted upon a Storage
Server restart. The existing storage engine and TLogs will recover the
current database state. The restarted server will establish its read
boundaries from the state actually available. Shard acquisition will
likewise establish availability from the transferred state.

A transaction may therefore need to restart after losing required history
through either recycling or component failure.


## 2. Scope

The project covers three areas:

- Local history boundaries and commit routing, including the removal
  of fixed-window checks.
- Paged conflict history and B+ tree reclamation in Resolvers.
- Spillable historical versions in Storage Servers.

These changes will preserve existing isolation semantics and must be
integrated before extended transactions are enabled. Reads served by
current state should avoid historical-version processing.

Component designs:

- [Commit Proxy design](01-commitproxy.md)
- [Resolver design](02-resolver.md)
- [Storage Server design](03-storage.md)

The paged Resolver is implemented and integrated for testing. Storage
Server history and system-wide support for extended transactions remain
implementation work. The descriptions above specify the intended design.

## 3. Validation and benchmarking

Component tests will compare reads and conflict decisions with independent
reference models, including reclamation boundaries and capacity exhaustion.
FoundationDB's deterministic simulation will cover concurrency, failures,
recovery and shard movement. Results will also be compared with unmodified
FoundationDB wherever both implementations retain the same history.

Benchmarks will compare throughput, median and tail latency, retained
history and resource use under identical workloads and resource budgets.
They will vary key sizes, read/write mix, contention and transaction
duration, covering both spare capacity and sustained recycling or spilling.
The impact on short transactions and completion rates of long transactions
will be measured separately.

Runs will be reproducible, with configurations and seeds recorded,
repeated timings and diagnostic instrumentation disabled for timing.
Results will be reported separately from the design.