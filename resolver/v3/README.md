# Beyond-5s · Resolver v3 engineering snapshot

**Selected code:** [resolver-v3](https://github.com/ricardojimenezperis/foundationdb/tree/resolver-v3) at commit 55d5f98974b352f3a124439ae86beb0f8b7ca76d (v3.8, two groups of eight write tasks).  
**Measured original:** d6bba393b92ca4dcf058a024dfcf890733d695c3, with the same tree 433edd77accbc70b47bfeaa4301e6da743139992.  
**Lineage:** eight published version commits from resolver-v2 to v3.8; v3.9 and v3.10–v3.17 are separate experimental branches.  
**Configuration:** two groups of eight write tasks are the selected default. This design snapshot makes no aggregate speed claim against FDB or earlier resolver versions.

## Start here

Resolver v3.8 routes read and write begins together through the temporal conflict B+ tree. Read tasks check conflicts and record where accepted writes will later start. A single-threaded scheduler then overlaps the searches and prefetches of up to 16 write tasks, while the oldest task applies writes in key order. The history representation, budgeted block pool and age-based reclamation continue from v1/v2.

"Interleaved" describes memory latency overlap within one core, not simultaneous tree mutation. The default two-group policy has eight tasks per group. A three-group option is present but was not selected.

## Reading path

| Question | Document |
|---|---|
| What changed from v2? | [Engineering overview](01-engineering-overview.md) |
| Where does it fit in FDB? | [Project architecture](02-project-architecture.md) |
| How do routing, tasks and safety work? | [Detailed design](03-resolver-v3-design.md) |
| Why v3.8 and what happened to later experiments? | [Decisions and evolution](04-decisions-and-evolution.md) |
| Which sources and checks support this snapshot? | [Validation and evidence](05-validation-and-evidence.md) |

The [v2 snapshot](../v2/README.md) remains immutable. The [current Resolver design](../../design/02-resolver.md) points to the chosen v3 implementation. This snapshot is source-specific: performance numbers that used a phase-gated benchmark, a walkers patch or special alignment must state those build conditions explicitly.

## Version identities

The public history preserves one commit per version. Four auxiliary integration/benchmark commits in the development history were folded into the published v3.1 commit while preserving its final tree. The code was not rewritten; commit identities changed because the history was curated. The [mapping](mapping.tsv) records all 17 original and published commits and their trees.

The v3.9 fast-path experiment branches directly from v3.8. The v3.10–v3.17 experimental line also branches from v3.8 and is retained for review, but neither line replaces the selected v3.8.
