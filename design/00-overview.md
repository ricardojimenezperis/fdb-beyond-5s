# Design Overview

*Status: design baseline complete; implementation is in progress.*

Claims about current FDB internals have been cross-checked against
`apple/foundationdb` main at `a443d3ee60`. The audit is recorded in
`design-crosscheck.md`, with supporting terrain maps in
`conflictset-map.md` and `versionedmap-notes.md`. References below use
`file:line`. Statements that still require empirical verification are marked
**[measure]** and have a corresponding plan in `questions.md`.

## 1. Why FoundationDB has a five-second transaction window

FoundationDB keeps the recent versions needed by MVCC in memory on the Storage
Servers. Retaining every old version indefinitely would make memory consumption
grow without bound, so old versions are discarded after a short window. Once a
transaction's read version is older than the versions still available, the
Storage Servers can no longer reconstruct its original snapshot.

Resolvers separately keep an in-memory history of recent conflicts. A write
transaction needs this history from its read version onwards so that the
Resolver can determine whether a transaction committed after that version wrote
into one of its read conflict ranges. With today's short transaction window,
both the size of this history and the cost of removing expired entries are
manageable.

Long transactions change that cleanup pattern. While a write transaction with
an old read version can still commit, the Resolver must preserve the conflict
history needed to validate it. When the oldest such transaction ends, the oldest
required version may move forward across a large interval, making many conflict
entries removable at once. Removing those entries one by one creates substantial
CPU overhead and can become a latency bottleneck, even though the same mechanism
works well with a short and steadily moving window.

FoundationDB also checks a transaction's age before allowing it to proceed
through the commit path. These checks do not retain or delete data. They reject
a transaction with `transaction_too_old` once its read version falls outside
the configured window, ensuring that Storage Servers and Resolvers are not
asked to use history they may already have discarded
(`CommitProxyServer.cpp:854–856`,
`CommitProxyServer.cpp:1741–1746`).

1. **Storage Servers (the primary constraint).**

   Storage Servers retain recent MVCC versions in an in-memory `VersionedMap`.
   The oldest snapshot they can serve is determined by `oldestVersion`, and the
   normal retention window is set by `MAX_READ_TRANSACTION_LIFE_VERSIONS`
   (`storageserver.cpp:10444–10447`, `:10889`).

   Both the tree nodes and retained values occupy RAM, so memory consumption
   grows approximately with:

   `write rate × retained time × workload-dependent amplification`

   T0.1 confirms a 96-byte allocation size for each tree node, but the complete
   steady-state amplification—including keys, values, structural sharing,
   allocator overhead and auxiliary structures—has not yet been measured
   against RSS (**[measure]**, `questions.md` Q10).

2. **Resolvers.**

   Resolvers retain the recent conflict history needed to validate write
   transactions. Unlike Storage Servers, they store key-range boundaries and
   versions, but not user values (`ConflictSet.cpp:241–311`,
   `:1035–1050`). Their memory footprint is therefore smaller than that of
   retained MVCC versions.

   The current cleanup removes expired conflict entries one by one
   (`ConflictSet.cpp:544–576`). This is manageable with the ordinary short
   window, but it can become CPU-intensive when the oldest version that must be
   preserved moves forward across a large interval and many entries become
   removable together.

   Storage Servers and Resolvers use different limits:
   `MAX_READ_TRANSACTION_LIFE_VERSIONS` controls the read window, while
   `MAX_WRITE_TRANSACTION_LIFE_VERSIONS` controls the conflict-history window.
   They are separate knobs but currently share the same five-second default
   (`Knobs.h:38–51`, `ServerKnobs.cpp:152–153`).

   The Resolver phase prepares conflict-history reclamation for a window that
   may remain fixed for a long time and then advance substantially. Existing
   measurements do not show a throughput improvement, so this phase is not
   presented as a performance optimization. It changes the structure and
   cleanup behavior needed to support longer write transactions in a later
   phase; v1 does not yet extend the commit window.

### How a transaction becomes too old

A transaction can encounter the limit in three places:

- A Storage Server rejects a read when it no longer retains the transaction's
  snapshot (`storageserver.cpp:2100–2102`, `:2131–2132`).
- A Resolver rejects a commit when it no longer has enough conflict history to
  validate the transaction (`ConflictSet.cpp:805`,
  `CommitProxyServer.cpp:2043–2044`).
- A Commit Proxy rejects a batch that has spent too long waiting before entering
  the commit path (`CommitProxyServer.cpp:854–856`).

The Storage phase addresses old snapshot reads. The Resolver phase prepares
conflict-history retention and reclamation for future long-running write
transactions. The Commit Proxy age checks must eventually be updated to match
the transaction windows that those components actually support.

2. **Resolvers.**

   Resolvers keep the recent conflict history required to validate write
   transactions. Today, expired conflict entries are removed one by one. This
   works with the ordinary five-second window, but becomes expensive when a
   long-running transaction keeps a large amount of history alive and that
   history later becomes removable all at once.

   The new structure has two design goals:

   1. Garbage collection on the normal transaction path must be O(1). Advancing
      the ordinary retention boundary must not trigger a scan proportional to
      the amount of conflict history being released.

   2. Long transactions must pay the additional cost they create. Ordinary
      short transactions must not become slower merely because another
      transaction keeps an old read version alive.

   Conflict history is stored in reusable pages. Garbage collection does not
   remove conflict entries one by one. It compares the youngest timestamp in
   the oldest retained page with the oldest read version that must still be
   supported. If that timestamp is older, every conflict in the page is
   obsolete and the whole page can be reused. Each garbage-collection step
   therefore requires one comparison and O(1) work.

   A long transaction spanning `n` retained pages performs one comparison per
   page during validation. Its cost is therefore proportional to the number of
   pages it spans. In the current Resolver, the same transaction must instead
   traverse the individual conflict entries contained in those pages. The
   paged structure makes the long transaction pay for the additional history it
   needs without adding that cost to short transactions.

## 3. Scope and implementation stages

The goal of this project is to remove FoundationDB's fixed five-second
transaction limit for both read-only and writing transactions, without making
ordinary short transactions pay for the additional history retained or
processed on behalf of long ones.

The work is divided into stages so that each mechanism can be implemented and
validated independently.

1. **Tracking the oldest snapshot still in use.**

   Clients periodically report the oldest read version used by their live
   transactions. Each GRV Proxy computes the oldest version reported by its
   clients, and the sequencer maintains one such value per proxy. Lease
   expiration ensures that a client or proxy that stops reporting cannot retain
   history indefinitely.

   The existing commit-path age checks must also stop enforcing a fixed
   five-second limit. They retain no history themselves; they must be made
   consistent with the versions that Storage Servers and Resolvers still
   preserve.

2. **Paged Resolver conflict history.**

   Resolver conflict history is stored in reusable pages. Garbage collection
   reclaims a page with one comparison and O(1) work instead of deleting its
   conflict entries individually. A long write transaction spanning `n` pages
   examines those `n` pages during validation, so it pays for the additional
   history it uses.

3. **Paged Storage Server version history.**

   Storage Servers retain recent versions in memory and move older version
   history into pages that can be reclaimed independently. This prevents a long
   snapshot from forcing the complete MVCC history to remain in the current
   in-memory tree.

Together, these three mechanisms support both read-only and writing
transactions beyond five seconds. Storage Servers preserve the versions needed
to read an old snapshot, while Resolvers preserve the conflict history needed
to validate a write transaction from its read version.


## 4. What the measurements tell us

The measurements answer two practical questions:

1. What does paged conflict history improve in the Resolver?
2. How much RAM could paged version history save on Storage Servers?

### Resolver

The original performance model predicted that eliminating entry-by-entry
garbage collection would approximately double Resolver throughput. We measured
the existing Resolver to determine whether garbage collection actually
accounted for enough CPU time to support that prediction.

It did not. The work eliminated by page-level reclamation accounted for about
8.4% of conflict-detection time. Conflict lookup remained the dominant cost, at
61.5%. In a benchmark retaining 50, 200 and 500 versions, the reclaimable share
remained approximately 9% rather than increasing with the window.

These results withdraw the predicted 2× throughput improvement. The paged
Resolver is not justified as a general throughput optimization.

The relevant result appears when accumulated conflict history is released.
Entry-by-entry reclamation took 56.4 ms in the measured case, whereas reclaiming
the paged representation took 0.2 ms. This is why the design remains useful: it
turns garbage collection into O(1) page reuse and prevents a short transaction
from paying a large cleanup pause caused by an earlier long transaction.

A long transaction may still examine several retained pages during validation.
That work is proportional to the history used by that transaction and is paid
by the long transaction itself.

### Storage Servers

We measured the allocation size of a `VersionedMap` tree node to verify one
input to the Storage Server memory model. The node occupies 88 bytes and is
allocated in a 96-byte block.

That result is not enough to predict total memory savings. We have not yet
measured the steady-state number of live nodes created per mutation, retained
key and value bytes, allocator overhead, auxiliary structures, or total RSS.

What is established is the scaling problem: retaining versions in the current
in-memory structure makes RAM consumption grow with the write rate and the
length of the retained window. Paged version history keeps the current state in
the main tree and allows older version pages to leave RAM while remaining
available from storage. This separates RAM consumption from the total length of
the retained history. The actual reduction in RAM must still be measured
against RSS.

Detailed benchmark methods, profiler breakdowns and limitations are recorded in
`../benchmarks/measurement-results.md` and
`../benchmarks/measurement-plan.md`.


## 5. Validation strategy
1. **Deterministic simulation first.**

   Each stage is first tested in FoundationDB's deterministic simulator. The
   tests cover normal execution, concurrent operations, message reordering,
   failures and recovery. A failing execution can be reproduced from its seed,
   making simulation the primary tool for finding and diagnosing distributed
   correctness errors.

2. **Full simulation suite.**

   After the focused tests pass, the complete FoundationDB simulation suite
   must pass without regressions. This verifies that the new mechanisms remain
   correct when exercised with the rest of the database under different
   workloads, failures and recovery scenarios.

3. **TSS comparison.**

   During the Storage Server stage, a Test Storage Server runs the new version
   storage alongside the existing implementation and compares their replies.

   Both servers should normally use the same retention window so that every
   reply is directly comparable. Tests with different windows must compare only
   versions retained by both servers; a read that succeeds only on the server
   with the longer window is an expected availability difference, not a data
   mismatch.


