# Storage Server design

*Design draft. Implementation status is summarized in §6.*

This document defines how Storage Servers retain and serve snapshots using
bounded memory and disk space for history. Snapshot availability depends on
the history still retained by the shards a read accesses.

The design baseline for existing FoundationDB internals is
`apple/foundationdb` main at `a443d3ee60`.

## 1. Current behaviour and the five-second limit

A Storage Server stores committed key-value data and serves reads at a
transaction's read version. To reconstruct recent snapshots, it combines the
durable state in its storage engine with newer mutations held in an in-memory
`VersionedMap`. This tree preserves multiple versions: mutations create new
tree versions while sharing unchanged nodes with earlier versions. Point and
range reads select the tree root corresponding to the requested version and
merge its contents with the durable storage-engine state.

FoundationDB retains recent history within a fixed version window, configured
by `MAX_READ_TRANSACTION_LIFE_VERSIONS` and normally corresponding to five
seconds. As versions advance, `oldestVersion` moves forward and older history
is reclaimed. A read requesting a version below `oldestVersion` is rejected
with `transaction_too_old`.

Removing an old version is not a tree compaction.
`forgetVersionsBeforeAsync` removes obsolete roots and schedules nodes that
are no longer referenced for deferred destruction. The cleanup actor frees
at most 100 nodes before yielding. The Storage Server does not call
`VersionedMap::compact()`.

## 2. Core design

Each shard keeps the latest value of each key in a persisted single-version
tree and older versions in version chains stored in shard-local append-only
pages. Recycling a history page takes O(1) logical work, without visiting its
individual records. History pages can spill to disk, allowing long-lived
snapshots without keeping their complete history in RAM.

Each historical value records the interval of read versions for which it is
visible. Replacing or deleting a value closes that interval at the replacement's
commit version. The historical representation also records the information
needed to establish that a key was absent.

References between history records use `(pageID, offset, length)`. Each page
allocation, including reuse of recycled storage, receives a new `pageID` from
a monotonically increasing process-wide counter.

Pages are reclaimed in global allocation order. The Storage Server records
`lastReclaimedPageID`, so a reference is obsolete when
`pageID <= lastReclaimedPageID`. To keep historical-version dereferencing
cheap, valid references locate their memory or disk slots through page-ID
arithmetic and placement state, without a directory lookup. References remain
unchanged when pages spill to disk.

### 2.1 Shared history-page pool

History pages are stored in two circular arrays per Storage Server: one in
memory and one on disk. Two knobs configure their respective numbers of
fixed-size page slots. Together, the arrays form a common history-page pool
shared by all shards.

New pages enter the memory array. Older closed pages spill to the disk array
while preserving their page IDs. Slots are reused cyclically once their
previous contents have been safely released. Each shard obtains pages as
needed, and reusable slots can hold new pages belonging to any shard.

A page belongs to one shard while it contains history. Each shard maintains
a linked list of its historical pages, allowing them to be enumerated during
shard movement without scanning other shards' pages. Spilling changes a page's
placement while preserving its identity and shard ownership.

The slot mapping must distinguish resident, spilling and spilled pages and
remain valid across ring wraparound. Its exact encoding and placement rules
remain to be defined. A bit-mask implementation must specify the corresponding
restrictions on ring sizes. Page identities must remain stable throughout
these placement changes.

The configured capacity is available for retaining history. A full pool is
normal: when another page is needed, the oldest history is recycled. Pages
are not discarded merely because time has passed. Reclamation follows global
allocation order and advances only the availability boundaries of shards
whose history is discarded (§3.1).

Accounting distinguishes occupied slots, reusable slots and slots awaiting
completion of I/O before reuse. A page being spilled temporarily occupies
resources in both arrays; both slots count against their respective limits.
Temporary I/O buffers and other metadata are measured separately.

### 2.2 History lifetime and failure recovery

Historical pages are deleted upon a Storage Server restart, including those
spilled to disk. The existing storage engine and TLogs remain responsible
for recovering the current database state.

After a restart, the Storage Server begins retaining new history from its
recovered state. For each shard, it establishes the earliest snapshot that
this state and subsequent mutations can reconstruct. Reads below that boundary
return `transaction_too_old`.

A transferred or newly acquired shard likewise establishes its available
history from the state actually received. Its page list allows history to be
enumerated, but transferring that history also requires translating references
into the recipient's page-ID namespace. Until a history-transfer mechanism is
implemented, availability at the recipient is determined by the acquired base
state and subsequent mutations.

A transactional recovery does not by itself discard the history of a surviving
Storage Server. Reads from transactions started before recovery may succeed
while the required snapshots remain available. Commit validation independently
depends on the history available at the Commit Proxies and Resolvers.

Possible future extensions include routing reads to replicas that still retain
the requested snapshot and reconstructing history from TLogs. Reconstruction
requires both the necessary log records and a suitable base state; recent
mutations alone do not recover overwritten values.

### 2.3 Read boundaries

For each shard `i`, the Storage Server maintains:

- `oldestAvailableRVᵢ`: the lowest read version guaranteed to be reconstructible
  for that shard. It never retreats during the shard's local lifetime.
  Reclamation advances it before discarding the corresponding history.
  Recruitment or shard acquisition initializes it from the state actually
  available.
- `oldestResidentRVᵢ`: a conservative boundary at or above
  `oldestAvailableRVᵢ` such that any historical pages needed by a read at or
  above it are resident in memory. Spilling updates this boundary consistently
  with page placement.

These boundaries are per shard. The baseline implementation instead uses one
`oldestVersion` for the whole Storage Server.

For read versions already reached by the Storage Server:

1. `rv < oldestAvailableRVᵢ`: the read returns `transaction_too_old`.
2. `oldestAvailableRVᵢ <= rv < oldestResidentRVᵢ`: the snapshot is available,
   but reading it may require fetching historical pages from disk.
3. `rv >= oldestResidentRVᵢ`: historical pages needed by the read are resident
   in memory. Main-tree access may still require storage I/O.

A request spanning several shards must satisfy the availability check for all
shards it accesses. Existing checks for versions not yet reached by the Storage
Server still apply.

### 2.4 Read-path requirements

A read satisfied by the single-version main tree does not traverse history
chains. It may still require storage-engine I/O if the relevant data is not
cached.

When a read requires historical versions, resident pages are resolved
synchronously using their IDs and placement state, without allocating memory
or performing history-related storage I/O. Reads requiring spilled history
use asynchronous I/O and follow §3.3.

Benchmarks measure main-tree reads, resident-history reads and spilled-history
reads separately.

### 2.5 Versioned range clears

A `clearRange` records a logical marker containing the cleared range and its
commit version. It does not visit and rewrite every historical value in that
range.

Reads before the clear's commit version must still find the earlier values.
Reads at or after that version see the range as empty, except for keys written
again afterwards.

When a later write modifies an affected part of the tree, the clear marker is
pushed down as necessary to preserve these rules. Ordering of sets and clears
within one commit version must also preserve the final state produced by that
mutation sequence.

A subtree may be unlinked without inspecting its individual records only when
its metadata certifies that it contains no writes newer than the clear and any
values needed by retained snapshots remain accessible through the historical
representation.

Unlinking a subtree does not reclaim its retained history. Reclamation must
account for both historical values and clear markers needed to establish
visibility or absence. The page certificate in §3 must cover these dependencies.

## 3. Page reclamation

For a page containing historical values, `page.maxCommitVersion` is the highest
commit version that replaced a value stored on that page. It is the end of the
latest visibility interval represented by those values, not the commit version
that originally created them.

A closed page containing only those historical values can be discarded once
its shard's availability boundary `b` satisfies:

```cpp
page.maxCommitVersion <= b
```

At equality, the replacement is already visible, so the older value is
unnecessary. The latest value remains in the main tree regardless of its age.
A page containing clear markers or other history metadata must also certify
that none of those records is required for snapshots at or above `b`. Metadata
needed by the current state must remain represented outside reclaimed history.

An append page is closed before reclamation so its certificate cannot change
while it is recycled. Per-page metadata is maintained as records are added;
reclamation does not scan individual records.

### 3.1 Reclamation on allocation

When another history page is needed, the Storage Server uses a reusable memory
slot. If necessary, it spills an older closed resident page to make room. If the
disk ring is full, it retires its oldest page before reusing that slot. With no
disk capacity configured, it retires the oldest resident page directly.

Every retirement follows global page-allocation order. Before discarding a
page, the server advances its owner's `oldestAvailableRVᵢ` to at least the
boundary required by that page's certificate. For a page containing only
historical values, this is the maximum of the existing boundary and
`page.maxCommitVersion`. The boundary cannot exceed the version through which
the shard's mutations have been applied.

The server then retires the page, advances `lastReclaimedPageID` and removes
it from its shard's page list. Its backing slot becomes reusable after the
conditions in §3.3 are met. Retiring several pages repeats this transition in
allocation order. Each affected shard's boundary advances as required by its
own discarded pages.

The O(1) target covers logical retirement of one page: checking its certificate,
updating boundaries and list metadata, and recording retirement. Maintaining
metadata, completing outstanding I/O and physically releasing storage are
separate costs.

If the oldest page is still open, it must be closed and its certificate made
valid before retirement. Allocation and mutation application must be ordered
so this requirement makes progress even under a small configured capacity.

### 3.2 Links to reclaimed pages

Links to reclaimed pages remain unchanged. For a read at or above the shard's
`oldestAvailableRVᵢ`, traversal must find the visible version or establish
absence before following an obsolete link. This requirement also applies to
range-clear markers and enumeration of keys absent from the current tree.

Recycling a page therefore requires no traversal or repair of incoming links.
The `lastReclaimedPageID` check rejects obsolete references before slot access.
Reusing a physical slot assigns a new page identity.

### 3.3 Reads and I/O overlapping reclamation

Before accessing historical pages, a read checks its version against the
shard's `oldestAvailableRVᵢ`. If it is below that boundary, the read returns
`transaction_too_old`.

Each uninterrupted segment accessing resident history completes without
suspension, so reclamation cannot interleave on the same cooperative execution
thread. A read retains no direct pointers to reclaimable page memory across
a suspension. On resumption it checks the boundary again and resolves any
page it will access using its identity and current placement.

Spilled reads use a buffer owned by the read operation:

1. Check `rv >= oldestAvailableRVᵢ` and resolve the page identity.
2. Read the page into the operation's buffer.
3. After resuming, check `rv >= oldestAvailableRVᵢ` before interpreting the
   bytes. Otherwise, discard the result and return `transaction_too_old`.

The boundary advances before the corresponding history is discarded. A read
requiring a discarded page therefore fails the availability check after
resuming.

Page backing storage becomes reusable only when outstanding I/O can no longer
interfere with its new use. Spill writes, reads, cancellation and completion
must obey this rule. Logically retired pages awaiting I/O completion remain
charged to capacity and are not counted as reusable. Late completions cannot
restore a retired page's placement or overwrite a reused slot.

### 3.4 Page identity

Each page allocation or reuse receives a new identifier from a process-wide
64-bit counter. Resetting the history pool does not reset this counter.
Counter exhaustion fails rather than wrapping.

If the entire history pool is reset while the process remains running,
operations referencing discarded history are cancelled or drained before reset
completes. The affected shards' availability boundaries reflect the history
lost, and `lastReclaimedPageID` covers the retired prefix.

The counter need not be persisted. A process restart discards historical pages
and outstanding operations, so references from the previous process are no
longer usable. Shard transfer must respect the namespace rule in §2.2.

## 4. Capacity and resource control

### 4.1 Capacity configuration

The memory and disk page-count knobs bound their respective circular arrays.
The implementation validates that the configuration supports page construction,
spill transitions and the largest indivisible allocation required by the
representation. The slot mapping may impose further size restrictions (§2.1).

A full history pool retains as much history as the configured capacity allows.
Further allocations recycle the oldest pages. A transaction may then receive
`transaction_too_old` when reading an affected shard. Other shards and replicas
retain their own availability boundaries.

Pool occupancy alone does not introduce a new Ratekeeper limit. Existing
admission controls continue to handle storage queues, durability lag, disk
space and other operational pressure. Spill I/O and temporary buffers consume
real resources and must be included in capacity and performance measurements.

### 4.2 Allocation progress

The allocation path must make progress under the configured capacity while
respecting outstanding I/O. It may wait for I/O needed to free a slot; waiting
for an old transaction to finish is not part of reclamation.

Large values, range clears and mutation batches may consume multiple pages
and require repeated recycling. The representation must reserve enough space
to complete each indivisible update and preserve a reconstructible current
state throughout. Reclamation cannot discard a page still needed to finish
constructing that state.

The minimum supported capacity and any allocation staging are derived from
these requirements and validated during implementation. Configurations too
small to complete an indivisible update are rejected.

### 4.3 Implementation sequence

Implement the page representation, version-chain and range-clear read semantics,
per-shard availability boundaries, ring addressing and spill I/O. Integrate
capacity accounting, allocation-triggered reclamation and safe slot reuse.

These are implementation stages. Capacity-based retention is enabled after the
complete read, mutation, reclamation and recovery paths have been validated
together. Commit Proxies and Resolvers must also use their local history
boundaries, as described in [Commit Proxy design](01-commitproxy.md) and
[Resolver design](02-resolver.md).

## 5. Validation and measurements

### 5.1 Snapshot correctness

Deterministic simulation compares point and range reads against a reference
model covering writes, deletes, range clears, repeated overwrites and mixed
mutations within one commit version. Successful reads must return the correct
snapshot; reads below an accessed shard's availability boundary return
`transaction_too_old`.

Tests cover historical keys absent from the current tree, overlapping clear
markers, subtree unlinking, resident and spilled version chains, and snapshots
exactly at, immediately below and immediately above a replacement or reclamation
boundary. They verify the page certificate for both values and history metadata.

TSS comparisons with an unmodified Storage Server check snapshots available
on both servers. Older snapshots are validated against the reference model.
The implementation must also pass FoundationDB's simulation suite.

### 5.2 Pool capacity and reclamation

Tests exercise unequal shard demand, shared-pool allocation, both ring capacities,
wraparound, spill transitions and reuse of slots by another shard. They measure
actual peak allocation alongside occupied, reusable and I/O-pending slot counts.

Reclamation tests verify global allocation order, `lastReclaimedPageID`, per-shard
page-list removal and availability-boundary advances. Reads at or above the new
boundary remain correct; reads below it return `transaction_too_old`, including
reads already in progress. Unaffected shards retain their boundaries.

Tests distinguish recycling already obsolete history from recycling that
advances a shard's boundary. Idle periods must not expire history solely because
a fixed amount of time has passed.

Small-capacity tests exercise large records, range clears, allocations crossing
page boundaries and repeated reclamation within a mutation batch. They must
demonstrate progress, correct current state and no reuse of storage still
accessible by outstanding I/O.

### 5.3 I/O, identity and failures

Tests delay spill reads and writes across reclamation and slot reuse. They
verify boundary rechecks, operation-owned buffers, stable page references across
spill, obsolete-ID detection and late completion handling. They also cover
history-pool reset and identifier exhaustion.

Recovery tests distinguish a surviving Storage Server retaining its history
from a server restart that deletes it. Shard-acquisition tests verify that
availability reflects the state actually received and that references cannot
resolve into an unrelated page at the recipient.

Integration tests combine long transactions, shard movement, replicas with
different retained histories and local capacity exhaustion. Reads started before
a transactional recovery are tested according to actual history availability.
The feature is exercised both enabled and disabled.

### 5.4 Performance

Measurements cover:

- Main-tree, resident-history and spilled-history read latency and throughput.
- Page consumption under overwrites, range clears and different shard loads.
- The retained version span and elapsed history duration under those workloads.
- RAM, spill-space, per-page metadata and temporary-buffer overhead.
- Logical page retirement, I/O drain and physical slot-reuse latency.
- Frequency and size of availability-boundary advances per shard.
- `transaction_too_old` rates under sustained writes and long-running reads.
- Spill bandwidth, write amplification and interaction with existing admission
  controls.

Measurements guide the default memory and disk page counts and sizing
recommendations. The O(1) target concerns logical reclamation per page; no
general read or write throughput improvement is assumed.

## 6. Status

The baseline implementation provides the existing storage engine,
`VersionedMap` read path and ordinary resource controls.

The single-version tree integration, paged history, versioned range clears,
circular pools, spill path, per-shard boundaries, capacity enforcement and
their end-to-end validation remain pending.
