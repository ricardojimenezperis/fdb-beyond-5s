# Resolver v3 · Engineering overview

[Index](README.md)

V2 interleaved read checks but still applied each accepted write through a scalar path. V3 routes the batch's read and write begins together, records write destinations during the read descent, and interleaves write **searches** through a non-preemptive scheduler. The oldest write alone mutates the B+ tree.

| Area | Selected final-v3 behavior |
|---|---|
| Input | Tagged read/write begins extracted from sorted native endpoints |
| Read navigation | Recursive partitioning of mixed begins among child separators |
| Read check | V2 grouped scheduler, predecessor check and fused forward leaf scan |
| Write destinations | Buffered leaf results emitted in begin order, with redirection through topology changes |
| Acceptance | Filter by transaction status, union overlapping accepted writes by endpoint index |
| Write scheduling | Two groups of eight tasks; speculative searches and prefetches, single oldest writer |
| Space and splits | Reserve per write, validate destination, reuse split candidate while its epoch is current |
| Cleanup | Deferred read-phase empty-node removal and existing lazy expiration/reclamation |

The final production commit adds two safety invariants to the v3.8 machinery. Each saved write rank carries the generation of its leaf and the leaf's mutation epoch; reclamation, reuse or an intervening write forces a new search when either no longer matches. Node generations and key-block generations are disjoint, preventing a recycled key block from satisfying a stale node-generation check.

A write task executes one continuation until a dependency is hinted, it parks, or it finishes. The next task may then use the core while memory requests mature. This is **one-thread scheduling**, with no simultaneous writes to different leaves. That distinction matters because the v3.11–v3.17 line later explored concurrent progression of independent leaf components and increased complexity without becoming the chosen release.

The architecture keeps a precise ordering boundary: transaction decisions finish before addWrites filters accepted intervals; write mutations happen in increasing accepted-begin order. The read phase may compact expired entries but cannot reclaim key blocks while tasks retain pointers. Write reservation can reclaim history, so saved destinations must be validated or redirected before use.

Two groups of eight tasks are the selected default. The stale-write-rank fix does not change that scheduling policy.
