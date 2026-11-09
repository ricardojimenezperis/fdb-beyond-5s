# Resolver v3 · Decisions and evolution

[Index](README.md)

This record describes the selected line and the experimental branches. A published commit identifies a code tree; it is not evidence that every intermediate experiment outperformed its parent. The source history contains one commit per named version. No v3.4 version was produced.

| Version | Published commit | Main change | Outcome |
|---|---|---|---|
| v3.0 | cd395e859c | Route read and write begins through one descent | Establish mixed routing; full FDB integration follows |
| v3.1 | 04aa590b5d | Apply writes through a cursor and integrate BTreeV3 into FDB | Also folds the original auxiliary integration and benchmark commits |
| v3.2 | bbb9ee98f0 | Keep prepared destinations valid across leaf releases and splits | Restore correctness under changing topology |
| v3.3 | f6c925d005 | Check the right neighbour only when the prepared destination requires it | Avoid unconditional routing work |
| v3.5 | ab2b0b7e53 | Stream accepted-write union and prefetch destinations | Cut repeated key and leaf work |
| v3.6 | 50e2006e48 | Interleave writes through two alternating groups of staged tasks | Establish grouped search and prefetch |
| v3.7 | 099ee465a1 | Advance a task to its next prefetch in one visit | Reduce scheduler and state-machine overhead |
| v3.8 | 55d5f98974 | Prepare an interval once; shorten key hints and chain local steps | Historical milestone and base of the experimental branches |
| **final v3** | **f866c98d85** | Validate every saved write rank with its leaf generation and mutation epoch; separate node and key-block generation spaces | **Selected production code; two groups × eight tasks** |

The published v3.8 tree equals the originally measured d6bba393b9 tree (433edd77accb). The new commit hash reflects the curated history. After publication, a two-page regression exposed stale ranks after reclamation: v3.8 could lose accepted writes or abort. `f866c98d` fixes that defect, `18260afe` adds its regression test, and `2a68fc9b` adds mixed read/write batch tests. Detailed identities accompany the snapshot in the mapping and final report.

## Why 2×8

V3.8 implements an optional three-group policy, but defaults to two groups of eight. Final v3 retains that default. A phase-level improvement for another group size would not by itself establish a whole-batch improvement or justify a different default.

## Experimental branches

Both branches below start from the historical v3.8 milestone, preserving the actual ancestry. They do not contain the final stale-write-rank fix.

| Branch | Versions | Idea and disposition |
|---|---|---|
| resolver-v3-fastpath | v3.9 | Split write-search continuations into fast and slow paths; retained as an experiment |
| resolver-v3-experiments | v3.10–v3.17 | Explore runnable rings, leaf-independent progression, footprint components, state-based dispatch, scheduling order and covered-leaf retirement; retained for inspection, not selected as the v3 release |

The second line first replaced fixed write groups with a runnable ring. Its parked searches occupied slots while only a few tasks remained runnable; increasing pending capacity did not fix that relationship. Subsequent versions tried allowing independent leaves to advance. V3.11 failed structural split correctness and v3.12 repaired its admission condition. V3.13 built transitive components from leaf footprints; the preparation pass cost memory latency. V3.14 interleaved that preparation. V3.15 moved to state-based dispatch, and v3.16 changed state order to improve prefetch lead on Intel; architectural and code-layout effects differed on AMD. V3.17 retired covered interior leaves as a whole, but in the measured ranges workload these leaves were uncommon and its isolated code effect was small.

The experimental line adds machinery to footprint preparation and write scheduling. Final v3 retains v3.8's allocation-age reclamation, lazy leaf cleanup and `cleanPending`, plus the saved-rank validation fix described above.
