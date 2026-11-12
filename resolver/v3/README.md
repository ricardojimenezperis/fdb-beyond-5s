# Beyond-5s · Final Resolver v3 engineering snapshot

**Published branch:** [`resolver-v3`](https://github.com/ricardojimenezperis/foundationdb/tree/resolver-v3), head `68c9bc35013a79c495e86507425f12bad41be299`.

**Final release:** [`resolver-v3-final`](https://github.com/ricardojimenezperis/foundationdb/releases/tag/resolver-v3-final).

**Production code:** `f866c98d8585a77280adbf44c72650f16e975f86`, tree `03766c182616a192e38fb713ed3b9124ec428b39`.

**Tests:** `18260afe5f80e00ae2b5cd4952f49522c07cbd0e`, `2a68fc9b90670bea84c7a458e85d582c7f4eb816` and `02d354f928143a45bb51cada4f06da11da628589`; none changes production code. The suite has 240 tests and passes 240/240 in 48 configurations. [MC/DC coverage](https://github.com/ricardojimenezperis/foundationdb/blob/resolver-v3/fdbserver/resolver/resolver1/tests/MCDCCoverage.md): 153 of 210 conditions (72.86%).

**Configuration:** two groups of eight write tasks, with the conflict-history pool in HugeTLB for every published performance figure.

## Start here

Final Resolver v3 routes read and write begins together through the temporal conflict B+ tree. Read tasks check conflicts and record where accepted writes will later start. A single-threaded scheduler overlaps the searches and prefetches of up to 16 write tasks, while the oldest task alone applies writes in key order. The history representation, fixed block-pool budget and age-based reclamation continue from v1/v2.

The final production commit adds the invariant that every saved write rank is associated with its leaf generation and mutation epoch. A task repeats its search when reclamation, block reuse or another write invalidates that position. Node and key-block generations occupy disjoint spaces, so a recycled key block cannot validate a stale leaf reference.

"Interleaved" describes memory-latency overlap within one core, not simultaneous tree mutation. The default two-group policy has eight tasks per group. A three-group option is present but was not selected.

## Reading path

| Question | Document |
|---|---|
| What changed from v2? | [Engineering overview](01-engineering-overview.md) |
| Where does it fit in FDB? | [Project architecture](02-project-architecture.md) |
| How do routing, tasks and safety work? | [Detailed design](03-resolver-v3-design.md) |
| How did v3.8 become the final v3? | [Decisions and evolution](04-decisions-and-evolution.md) |
| Which sources and checks support it? | [Validation and evidence](05-validation-and-evidence.md) |
| How does final v3 compare with FDB, v1 and v2? | [Evaluation](06-evaluation.md) |

The [v2 snapshot](../v2/README.md) remains immutable. The [current Resolver design](../../design/02-resolver.md) describes final v3. The complete consolidated report is also published with the code at [`evaluation/REPORT.md`](https://github.com/ricardojimenezperis/foundationdb/blob/resolver-v3/fdbserver/resolver/resolver1/evaluation/REPORT.md).

## Version identities

V3.8, commit `55d5f98974b352f3a124439ae86beb0f8b7ca76d` and tag `resolver-v3`, is the historical milestone from which the final fix and the experimental branches descend. Validation after that milestone found that reclamation could invalidate a saved write rank while leaving it apparently reusable. The final production commit `f866c98d` corrects that defect; `18260afe` supplies its regression test, `2a68fc9b` adds mixed-batch tests and `02d354f9` adds MC/DC coverage tests. The release tag `resolver-v3-final` stays at documentation merge `027ddeb8`, whose tree contains the final code, tests, catalogs and evaluation. The branch continues with the AMD long-trace result (`1233986b`), the MC/DC tests (`02d354f9`) and their report and evidence (`68c9bc35`), none of which changes production code.

The [mapping](mapping.tsv) records the curated v3.0–v3.17 history and the final fix. The v3.9 fast-path experiment and the v3.10–v3.17 experimental line still branch directly from v3.8; neither replaces final v3.
