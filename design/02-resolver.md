# Resolver design

The baseline for the existing FoundationDB internals described here is
`apple/foundationdb` main at `a443d3ee60`.

This document defines capacity-driven conflict history in a shared page pool.
`PagedBTreeConflictSet` stores disjoint intervals in a B+ tree. Short separator
keys are stored inline; longer keys are represented by prefixes and pointers
to immutable heap records containing the full keys. The main feature is O(1)
logical reclamation of individual key-heap pages. Much of the design
and optimization effort focuses on cache efficiency, which is crucial to
competitive performance.

## 1. What the Resolver does

The Resolver checks read-conflict ranges against accepted writes. For a
transaction at read version `r`, an overlapping write with a CV strictly greater
than `r` causes a conflict. Overlapping writes alone do not cause a conflict.
Explicit client conflict ranges retain their semantics regardless of mutation
type.

For any key, only its latest write CV is needed for this check: if an older
write is newer than `r`, the latest write is also newer than `r`. Overwriting a
region can therefore replace its older conflict entries without losing a conflict
at any admitted RV. 

The validation boundary, stored in `validationMinRV`, is the oldest read
version for which the Resolver retains sufficient conflict history. It never
decreases and must advance before discarded history becomes inaccessible. Transactions with
read-conflict ranges and an RV below it receive `transaction_too_old`; an RV
equal to it remains admissible. Writes with a CV at or below it cannot conflict
with an admitted read. Transactions without read-conflict ranges remain exempt
from this age check.
On recovery, each new Resolver initializes `validationMinRV` to
`recoveryTransactionVersion`.

Retention is capacity-driven, without client leases or a global minimum-RV
protocol. Idle time alone does not discard history. Commit Proxy routing and
Storage Server availability have their own boundaries.

For a transaction spanning several Resolvers, writes accepted locally may remain
recorded even if another Resolver rejects the transaction, as in current FDB.
The conservative false conflicts that can result from this behaviour are preserved.

## 2. Paged temporal conflict history

### 2.1 One B+ tree of disjoint intervals

`PagedBTreeConflictSet` owns the shared `PagePool`, `TemporalHeap`, B+ node
allocator, index and validation boundary. Point and range writes share one
index. A point denotes `[key, keyAfter(key))`, with compact successor encoding.
Indexed intervals are disjoint; gaps contain no retained relevant write. Expired
entries can remain physically present, but expiry must be checked before key
access or conflict evaluation.

| Field | Meaning |
|---|---|
| Begin reference and inline prefix | Inclusive start, possibly resolved from a heap record |
| End reference and inline prefix | Independent exclusive end; may have another source record |
| Interval CV | Version of the represented write |
| Key-order permutation | Logical order of occupied physical entry slots |
| Child link and separator metadata | Internal B+ node routing |
| Node `maxCV` | Conservative upper bound for retained writes under the node |

The Resolver already combines accepted writes
before insertion. The tree receives that ordered input and does not repeat the
union or add a separate adjacent-equal-CV coalescing pass.

### 2.2 Shared capacity and allocation units

One configured page pool supplies both the key heap and B+ tree.
`PAGED_RESOLVER_POOL_PAGES` sets its total capacity. Pages are 64 KiB;
B+ pages contain multiple nodes. Only the history selected by
`RESOLVER_USE_PAGED_CONFLICT_SET` is constructed.

The administrator can set explicit key and index page budgets. In automatic
mode, each side has a minimum reserved share, and the remaining pages are
assigned on demand. Once assigned, a page belongs permanently to that side.
After all unassigned pages have been consumed, the split is fixed.

Key pages are reclaimed as whole units and reused by the key heap. B+ nodes
are reclaimed individually and reused through the node free list. B+ pages
are never reclaimed, even if all their nodes are free, and neither side
transfers assigned pages to the other. A workload change can therefore
exhaust one side while the other has free capacity; explicit budgets allow
the administrator to choose a split appropriate to the workload.

The CV list tracks live key pages and individual B+ nodes. It is separate
from the allocator free lists. A free node can satisfy a node allocation
immediately; a key-page allocation requires a free key page.

The pool occupies one aligned memory region. Pool capacity, physical
residency and auxiliary metadata outside the pool are distinct quantities.
Reusing a physical key-page slot gives it a fresh logical page ID.

### 2.3 Immutable temporal heap and endpoint references

The heap remains append-only on its newest page, with no individual-record free
list. A new write appends its key bytes and current CV. Existing record bytes are
never overwritten when indexed intervals are shortened or split.

`TemporalHeap` maintains its page chain, monotonically increasing logical page
IDs, live page mapping, `lastReclaimedPageID` and `reclaimedThroughCV`. Each
`HistoryPage` has an ID, `usedBytes`, links and a conservative `newestCV` maximum.
A `HeapRef` resolves a page ID, offset and length; logical IDs are fresh even when
the same physical page is reused. Lookup indexes `slots_[pageID % slots_.size()]`
and validates that the ID is live. The modulus is the table capacity, not the
current number of occupied pages. There is no search through a heap directory.

Records and their full endpoint keys fit inside one 64 KiB page. Record-size
checks include internally generated endpoints. The page maximum covers the CVs
of all records on the page. Reclamation advances the boundary to that maximum
before removing the mapping. Updating a maximum never lowers it.

For old `[a,z)@100` overwritten by `[d,m)@200`, the index becomes:

| Indexed interval | Begin bytes | End bytes | Interval CV |
|---|---|---|---:|
| `[a,d)` | `a` in the old record | `d` in the new record | 100 |
| `[d,m)` | `d` in the new record | `m` in the new record | 200 |
| `[m,z)` | `m` in the new record | `z` in the old record | 100 |

With ordinary heap-backed storage, only the incoming write adds a heap record. 
Splitting an old interval may require
an additional index entry, but no copied fragment record or additional key bytes
in the heap. Old keys remain where they were until their page is reclaimed.
There is no in-place key-capacity decision and no copying old fragments to recent
pages merely to change their limits.

**Endpoint lifetime invariant.** Each interval's endpoint bytes originate in a
record whose CV is at least the interval CV. Initially the versions are equal.
Trimming inherits an existing endpoint or takes an endpoint from the newer write,
while preserving the fragment's CV. Repeated trimming preserves the invariant.
Before any endpoint page is reclaimed, the boundary reaches at least its source
record CV and therefore the referencing interval CV. The interval is already
logically expired before its endpoint bytes can disappear.

Consequently, endpoint reuse does not pin old pages or raise their maxima to the
new write CV. A live interval cannot lose one of its endpoint pages under this
contract. The interval CV is stored in the index, so expiry can be checked before
resolving either reference. No reference counting or endpoint-repair scan is
required for this lifetime rule.

New heap records follow incoming write CV order. Creating an index entry
for an old fragment does not append an old-CV heap record. New records are
appended only to the newest page; free space in older pages is not backfilled.

With inline-only storage, keys that fit the inline representation need no
heap record. The endpoint-lifetime invariant applies to heap-backed
references; inline key bytes are preserved when entries move between nodes.

### 2.4 Node structure and local reclamation

Each node contains a fixed-capacity array of entry slots, an occupancy
bitmap and a permutation mapping key order to physical slots. 

Leaf entries represent disjoint intervals. Each entry stores its commit
version, independent begin/end references and inline key bytes. Internal
entries identify child nodes and store their separators and conservative
maximum commit versions.

The permutation orders entries by key without moving the entries
themselves. Searching accesses slots through this permutation. Inserting
an entry writes it into a free physical slot and inserts its slot number
at the appropriate position in the permutation. Removing an entry removes
its slot number from the permutation and clears its occupancy bit.

Physical slots are managed circularly. An insertion cursor selects the
next free slot, wrapping at the end of the array. A reclamation head
identifies the first occupied slot to examine for local cleanup. Neither
cursor represents key order.

Before reserving another node, an insertion attempts to reclaim space
locally using the current `validationMinRV`. In a leaf, it removes expired
entries from the reclamation head and advances past the freed slots. In
an internal node, it can remove a child whose conservative `maxCV` is at
or below that boundary. Cleanup stops at the first head entry that cannot
be reclaimed. This is a local head pass, not a scan of every slot.

Local reclamation frees entry slots without advancing the validation
boundary. Removing entries does not lower the node's conservative
`maxCV`. Reclaiming an entire node is a separate operation governed by
the CV list.

Nodes also store parent links; leaves are doubly linked in key order.
The CV-list links are stored in external vectors rather than inside
the nodes.

With 16 inline key bytes and a capacity of 23 entries, a node occupies 1,792 bytes,
and a 64-KiB page holds 36 nodes. Pages assigned to the B+ tree remain
assigned to it permanently. Freed nodes are reused through the node
free list; their backing pages are not returned to the key heap.

### 2.5 Replacing covered intervals

Incoming writes are processed in nondecreasing CV order. For a new
interval `[begin,end)@cv`:

1. Locate the first possible overlap, including the interval starting
   before `begin` if its end exceeds `begin`.
2. Preserve a left fragment if that interval starts before `begin`.
   Its new end references the incoming record's begin; its CV is unchanged.
3. Remove fully covered intervals from the map. Their immutable heap
   records remain until their pages are retired.
4. Preserve a right fragment if the last affected interval extends beyond
   `end`. Its new begin references the incoming record's end; its CV is
   unchanged.
5. Insert the incoming interval once, covering the entire new range,
   including any gaps between old intervals.

At most two outer fragments survive. Both may come from the same old
interval. Touching endpoints alone do not overlap.

For example, `[a,f)@100`, `[h,k)@110`, `[m,z)@120`, overwritten by
`[d,p)@200`, becomes `[a,d)@100`, `[d,p)@200`, `[p,z)@120`.

Each insertion reserves only the nodes required at its current position.
An insertion that fits needs none; a split requires a sibling, and
splitting the root also requires a new root. Reservations are computed
when needed, rather than filled speculatively before every write.

Allocation may reclaim history and invalidate the saved insertion
position. The operation then rechecks pending entries against
`validationMinRV`, relocates and recomputes its reservation. A pending
right fragment is retained by value and inserted if still live, even
when the incoming interval has already been published. Unused
reservations are returned.

### 2.6 Conflict lookup and `maxCV`

For a read range `[a,b)` at RV `r`, first reject versions below
`validationMinRV`. An empty range cannot conflict. Locate the possible
interval containing `a`, then examine intervals starting before `b`.
An interval conflicts exactly when:

- `begin < b`;
- `end > a`;
- `intervalCV > r`.

Because intervals are disjoint, at most one interval starting before
`a` can overlap the read. Subtrees whose `maxCV` is at most `r` can
be skipped.

`maxCV` is a conservative high-water mark. Removing entries or splitting
a node does not require lowering it. Every internal bound must remain
at least as large as the CVs represented below it. An overestimate may
reduce pruning or delay reclamation, but cannot hide a conflict.

Key comparisons use inline bytes first. Only an unresolved comparison
accesses the full key in the heap. The direct-suffix path resolves the
entry and full-key address once and resumes comparison after the bytes
already compared.

An internal node can store a fixed reference prefix of up to 12 bytes,
computed from its first two live separators. Separators sharing that
reference store their next K bytes inline; exceptions store their first
K full-key bytes instead. The reference remains unchanged for the node's
lifetime, and split siblings inherit it. Comparisons account for the
reference before using the inline suffix. With K = 16 and a 12-byte
reference, an internal separator can cover 28 key bytes without accessing
the heap.

Batch lookup can also traverse a single sorted sequence of begin and end
references. Each reference identifies its original query, whose `endIndex`
locates its end in the sorted sequence. Separators and query endpoints
advance together. Traversal frames identify a node and its endpoint
interval, avoiding copies into per-child query vectors. A shared vector
tracks queries that remain open across child boundaries, including
children containing neither endpoint.

### 2.7 Safe endpoint access and searches in flight

An interval's CV is checked before either endpoint is dereferenced.
Its endpoints may refer to different pages and source records. As
described in §2.3, each endpoint source is at least as new as the interval,
so an expired source cannot be required by a live interval.

Read processing does not allocate or reclaim history, and the validation
boundary remains fixed during the read phase. Interleaved searches can
therefore retain their traversal state for that phase.

Write allocation can reclaim history. Before nodes are recycled, saved
batch hints are invalidated. Pending insertions must relocate and
revalidate their CVs and endpoint references before continuing. Cleanup
must not dereference an expired key merely to locate its node.

### 2.8 The CV list

A doubly linked list orders key pages and B+ nodes by conservative maximum
commit version. Its links and bounds are stored in external vectors, with
one entry per key page and one per B+ node. Nodes also retain their
`maxCV` for tree traversal.

Writes arrive in nondecreasing CV order. When an object's bound increases,
its list entry is updated and moved to the tail. An unchanged bound causes
no movement. Removing old entries does not lower the bound.

On a split, both halves inherit the original conservative bound. The new
sibling is placed beside the original in temporal order. Merges and
transfers preserve a bound covering all retained contents and the
corresponding list position. An object is unlinked before its storage
is reused.

The list identifies reclamation candidates directly, without a global
search through leaves. Reclaiming a node may still require tree
maintenance: removing parent entries, updating separators and links,
and collapsing the root where appropriate.

A reclaimed key page returns to the key-page free list. A reclaimed
B+ node returns to the node free list. Their memory remains within its
assigned budget; reclaiming nodes does not release their backing pages
to the key heap.

### 2.9 Recycling at capacity

Key pages and B+ pages have permanent ownership. The administrator can
configure their budgets explicitly. In automatic mode, each side has
a minimum reserved share, and remaining pages are assigned on demand
until the pool is exhausted. Assigned pages are never transferred
between the two uses.

For an insertion, allocation proceeds as follows:

1. Attempt local head reclamation at the current `validationMinRV`.
   If the entry now fits, no additional node is needed.
2. Obtain any required nodes from the node free list, allocating a
   B+ page if its budget permits.
3. If storage is still unavailable, process candidates from the CV list,
   advancing `validationMinRV` to their conservative bounds before
   invalidating history.
4. Retire key pages as whole units and reclaim B+ nodes with the required
   tree maintenance.
5. Stop when the requested resource is available, then revalidate and
   replan the pending insertion.

Key-record allocation similarly reuses free key pages or acquires pages
within its budget before advancing reclamation. A key-page request is
satisfied by a key page; freeing B+ nodes cannot satisfy it.

Retiring a key page does not traverse incoming endpoint references.
O(1) applies to logical retirement of one key page, not to B+ tree
maintenance or an entire allocation that processes several candidates.

If the boundary reaches an incoming write's CV, that write cannot
conflict with a subsequently admitted read and need not remain stored.
Expired pending fragments are likewise discarded; still-live fragments
survive replanning. This does not abort the transaction or alter
completed validation decisions. Subsequent checks use the advanced
boundary.

Normal capacity pressure advances the boundary and recycles storage.
It introduces no new per-transaction out-of-space abort.

### 2.10 Resolver batch processing

The integration preserves FDB's batch pipeline:

1. Check read-conflict ranges against retained history, including local
   age checks.
2. Resolve conflicts within the batch in transaction order using
   `MiniConflictSet`.
3. Combine accepted writes and insert them at the batch commit version.
4. Perform cleanup at the batch boundary.

Conflict-range reporting, transaction order and the exemption for
transactions without read-conflict ranges remain unchanged.
`transaction_too_old` remains distinct from a detected conflict.
Extended retention does not change serializability semantics.

## 3. Integration and recovery

### 3.1 Commit Proxy routing

Commit Proxies retain the assignment history needed to route conflict
checks across Resolver reassignments. Routing, pruning and recovery
rules are specified in [Commit Proxy design](01-commitproxy.md).

### 3.2 Recovery

A new Resolver starts with an empty index and key heap and inherits no
history or page identities from the previous instance. Its validation
boundary is initialized as described in §1, before processing recovery
batches or ordinary requests.

FoundationDB's existing recovery determines the outcome of unfinished
work. The explicit validation boundary replaces reliance on a fixed-size
recovery version jump for rejecting older read versions.

### 3.3 Component lifetime and integration

The page pool outlives the heap and node allocator. Tree teardown completes
before referenced heap storage is released, and CV-list entries are
detached before their storage is reused.

Extended retention is enabled only after capacity enforcement, validation,
routing and recovery have been integrated and validated together.


## 4. Validation methodology

Component tests compare conflict decisions with an independent
write-history model and verify interval replacement, endpoint lifetime,
tree structure and reclamation under memory pressure. Integration tests
and FoundationDB's deterministic simulation exercise batch processing,
recovery and failures.

Performance comparisons use FoundationDB's `skiplisttest` to measure
Resolver batch processing against the original conflict set. Comparisons
require equivalent retained history, identical conflict decisions and
no additional `transaction_too_old` rejections. Repeated runs measure
processing time and per-batch p99 latency on one pinned physical core,
with its SMT sibling idle and diagnostic counters disabled.

Profiling uses Callgrind (Valgrind) for instruction counts, Linux perf
for CPU sampling and hardware counters, and AMD IBS for sampled
memory-access latency and data sources. Read and write phases are
analysed separately, and profiling runs are separate from timing runs.
Performance results will be published separately.

## 5. Implementation status

The paged B+ conflict set and capacity-driven reclamation are implemented
and integrated for testing. Inline-only keys, sorted-endpoint traversal
and node reference prefixes are optional and disabled by default.
Full FDB validation of node reference prefixes is in progress.

System-wide routing, recovery and removal of fixed-window limits remain
prerequisites for enabling extended transactions.
