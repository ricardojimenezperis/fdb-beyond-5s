# Storage Server design

This document defines how Storage Servers will retain and serve snapshots
beyond FoundationDB's ordinary five-second window without keeping the complete
version history in the current in-memory `VersionedMap`.

The protocol that determines the oldest read version still required is
specified in `01-floor-tracking.md`.

References to current FoundationDB internals were verified against
`apple/foundationdb` main at `a443d3ee60`.

## 1. Current behaviour and the five-second limit

A Storage Server stores committed key-value data and serves reads at a
transaction's read version. To reconstruct recent snapshots, it combines the
durable state in its storage engine with newer mutations held in an in-memory
`VersionedMap`.

`VersionedMap` is a persistent tree. Mutations create new tree versions while
sharing unchanged nodes with earlier versions. Point and range reads select the
tree root corresponding to the requested version and merge its contents with
the durable storage-engine state (`storageserver.cpp:2431–2436`,
`storageserver.cpp:3174`).

Keeping this history indefinitely would make memory consumption grow with the
write rate and the age of the oldest readable snapshot. FoundationDB therefore
advances `oldestVersion` using
`MAX_READ_TRANSACTION_LIFE_VERSIONS`. A request below `oldestVersion` can no
longer be reconstructed and returns `transaction_too_old`.

Removing an old version is not a tree compaction.
`forgetVersionsBeforeAsync` removes obsolete roots and schedules nodes that are
no longer referenced for deferred destruction (`VersionedMap.h:788–836`,
`storageserver.cpp:10888`). The cleanup actor frees at most 100 nodes before
yielding. The Storage Server does not call `VersionedMap::compact()`.

In the current VersionedMap, removing old roots triggers deferred destruction
of tree nodes that are no longer referenced. This requires per-node work.
The proposed design will instead reclaim historical storage by recycling
whole pages.

This design will keep the latest values in a persisted single-version tree
and store older versions separately in reclaimable pages. Those pages may
spill to disk, allowing long-lived snapshots without keeping their complete
history in RAM.

## 2. Core design

Each shard will keep the latest value of each key in the persisted single-version tree. 
Older versions will be stored separately in version chains inside shard-local append-only pages, 
which may reside in memory or spill to disk.

References between history records will use stable `(pageID, offset, length)` identifiers 
rather than raw memory addresses. A Page Directory will resolve each `pageID` to a resident page 
or its location in the spill area.

### History lifetime and failure recovery

Historical pages will not be durable. Spilling them to disk will extend the available history beyond RAM capacity, 
but will not guarantee that they survive a Storage Server restart. 
The existing storage engine and TLogs will remain responsible for recovering the current database state.

After a restart, the Storage Server will discard its previous local history 
and begin retaining new history from its recovered state. 
It will return `transaction_too_old` for reads requiring versions it no longer holds. 
Consequently, a long transaction may have to restart after a Storage Server failure.

Two possible future extensions are:

* Route the read to a replica that still retains the requested snapshot.
* Reconstruct history from TLogs. This would require 
retaining the necessary log records according to the retention floor, 
together with a suitable base state from which to replay them. 
Retaining recent mutations alone does not guarantee that overwritten values can be reconstructed.

Neither extension will make the historical pages themselves a second durable store
that would result in duplicating the cost of durability.

### History reclamation and read boundaries

History pages will be reclaimed as complete units once none of their records
is needed to serve snapshots at or above the retention floor. Historical
values will not be deleted individually.

For each shard, the Storage Server will maintain two boundaries:

- `MRVᵢ`: the oldest read version it can still serve.
- `MRV_RAMᵢ`: the oldest read version for which any required historical pages
  are guaranteed to be resident in memory.

For read versions already reached by the Storage Server, these boundaries
define three cases:

1. `rv < MRVᵢ`: the snapshot is no longer available. The read returns
   `transaction_too_old`.
2. `MRVᵢ ≤ rv < MRV_RAMᵢ`: the snapshot is available, but reading it may
   require fetching historical pages from disk.
3. `rv ≥ MRV_RAMᵢ`: any historical pages needed by the read are resident
   in memory, so accessing those pages requires no storage I/O.

### 2.1 Read-path requirements

A read satisfied by the single-version main tree will not consult the history
Page Directory or traverse version chains. It may still require storage-engine
I/O if the relevant data is not cached.

When a read requires historical versions, resident pages will be resolved
synchronously through the Page Directory, without allocating memory or
performing history-related storage I/O. A valid read below `MRV_RAMᵢ` may
require asynchronous access to spilled pages.

Benchmarks will measure main-tree reads, resident-history reads and
spilled-history reads separately.

### 2.2 Versioned range clears

A `clearRange` will record a logical marker containing the cleared range and
its commit version. It will not visit and rewrite every historical value in
that range.

Reads before the clear's commit version must still find the earlier values.
Reads at or after that version must see the range as empty, except for keys
written again afterwards.

When a later write modifies an affected part of the tree, the clear marker
will be pushed down as necessary to preserve these rules.

A subtree may be unlinked without inspecting its individual records only when
its metadata certifies that it contains no writes newer than the clear and
any values still needed by older snapshots remain accessible through the
historical representation.

Unlinking a cleared subtree from the current tree will not reclaim its
retained history. Historical pages will remain available until the retention
floor allows them to be reclaimed.

## 3. Lazy garbage collection

Garbage collection will process history pages in their creation order.
Each page will track its highest commit version: the newest commit that
replaced a value stored on that page. When this version is at or below the
retention floor, i.e., page.maxCommitVersion <= retentionFloor, 
the page will be recycled. Processing will stop at the first
page whose highest commit version exceeds the floor.

This makes reclamation O(1) per page: one comparison followed by whole-page
reuse, instead of O(number of records) work to GC.

### 3.1 Links to reclaimed pages

Links to reclaimed pages will remain unchanged. A read at or above the
retention floor will find its visible version, or establish that the key is
absent, before following any such link.

Recycling a page therefore requires no traversal or repair of incoming links.
Reads already in progress will recheck the local retention floor after
each suspension and before accessing historical records, as described below.

### 3.2 Reads overlapping reclamation

Before accessing historical pages, a read will check its read version
against the shard's local minimum retained version, `MRVᵢ`. If `rv < MRVᵢ`,
it will return `transaction_too_old`.

Access to history pages already in memory will complete without suspension,
so GC cannot interleave on the same cooperative execution thread.

Spilled reads will use a buffer owned by the read operation:

1. Check that `rv >= MRVᵢ`.
2. Read the page into the operation's buffer.
3. After resuming, check again that `rv >= MRVᵢ` before interpreting
   any bytes. Otherwise, discard the buffer and return
   `transaction_too_old`.

The local minimum retained version must advance before the corresponding
pages can be recycled. Because it never retreats, a read whose required
page was reclaimed during the I/O will fail the second check.

Reads will not retain direct pointers to reclaimable page memory across
a suspension. Any later page access will resolve the page again after
checking `MRVᵢ`.

### 3.3 Page identity

Each page allocation or reuse will receive a new identifier from a
process-wide 64-bit counter. Resetting the Page Directory will not reset
this counter. Counter exhaustion must fail rather than wrap.

The counter need not be persisted. A process restart discards the directory,
historical pages and all outstanding operations, so identifiers from the
previous process can no longer be referenced.


## 4. Retention limits and resource control

The Master will distribute the retention floor through the dedicated channel
described in `01-floor-tracking.md`, using the DBInfo broadcast mechanism
without modifying DBInfo or triggering its change handlers.

While legacy-client support is enabled, Storage Servers will preserve at
least the ordinary five-second window. The reported floor may extend that
window but will not shorten it. With legacy-client support disabled, the
reported floor will determine the retention required by active transactions
and accepted commits.

Each Storage Server will advance its local minimum retained version before
reclaiming the corresponding history. This boundary will never retreat:
a later update cannot restore history that has already been discarded.

Ratekeeper already monitors Storage Server free space and limits incoming
write traffic when space becomes scarce. The new design will also account
for the memory and disk space occupied by historical pages.

An administrative limit will bound how far into the past retention may
extend. The Master will apply that limit when processing proxy reports.
A client report older than the permitted boundary will not install or
renew a lease for that snapshot. Refusing a renewal will not shorten an
existing lease.

Clients will continue receiving the ordinary GRV reply, without a lease
grant or a retention result. A read below the shard's actual minimum
retained version, `MRVᵢ`, will return `transaction_too_old`.

The aggregated floor identifies the oldest version to retain, not the
individual transaction holding it. It therefore cannot be used to select
a particular client for cancellation.


## 5. Validation

Deterministic simulation will compare reads against a reference model,
covering writes, range clears, spilling, page reuse and server restarts.
Every read at an available version must return the correct snapshot;
reads below the local retention floor must return `transaction_too_old`.

The changes must also pass FoundationDB's simulation suite.

TSS comparisons with an unmodified Storage Server will check snapshots
available on both servers. Reads beyond the unmodified server's retention
window will be validated against the reference model instead.

## 6. Status

This document defines the proposed Storage Server design. Implementation
and validation are pending.

The initial implementation will discard local historical versions after
a restart. Routing to another replica that retains them and reconstructing
history from TLogs remain possible future extensions.
