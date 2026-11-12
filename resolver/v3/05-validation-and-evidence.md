# Final Resolver v3 · Validation and evidence

[Index](README.md) · [MC/DC report](https://github.com/ricardojimenezperis/foundationdb/blob/resolver-v3/fdbserver/resolver/resolver1/tests/MCDCCoverage.md) · [Consolidated report](https://github.com/ricardojimenezperis/foundationdb/blob/resolver-v3/fdbserver/resolver/resolver1/evaluation/REPORT.md) · [Release assets](https://github.com/ricardojimenezperis/foundationdb/releases/tag/resolver-v3-final)

## Source identities

| Role | Commit / tree | Meaning |
|---|---|---|
| Historical v3.8 | `55d5f98974b352f3a124439ae86beb0f8b7ca76d` / `433edd77accbc70b47bfeaa4301e6da743139992` | Tag `resolver-v3`; base of final v3 and the experimental branches |
| Final production code | `f866c98d8585a77280adbf44c72650f16e975f86` / `03766c182616a192e38fb713ed3b9124ec428b39` | Stale-write-rank fix; no later commit changes production |
| Regression test | `18260afe5f80e00ae2b5cd4952f49522c07cbd0e` / `793a3e13cbf282dde84ea7673e423e35e46bbb23` | Fix regression, catalogs and evidence |
| Mixed-batch tests | `2a68fc9b90670bea84c7a458e85d582c7f4eb816` / `c2b47f360d39af8e5728867b37dcd99af5f746ab` | Mixed read/write batches; tests only |
| Final release | `027ddeb8b79e7092999135de176d82e23b1c8da0` / `75a1002a97baf145efd89a2d851c7d792bcfa258` | Tag `resolver-v3-final`; final code, tests, catalogs and consolidated evaluation |
| AMD long-trace result | `1233986b4365db2d5f9784ad382d667e4f2cc593` / `f9cdf90cb54bba9b294820c3f6b89847a2441ad8` | Documentation only |
| MC/DC tests | `02d354f928143a45bb51cada4f06da11da628589` / `c23e51ad3c71a012360f483824afc107dd1de0c7` | Eight tests for MC/DC coverage; tests only |
| Published branch head | `68c9bc35013a79c495e86507425f12bad41be299` / `b7a66946e04eba577fbdcd056d8f08c6fb801ad0` | MC/DC report, evidence and updated component catalogs |

The older tag `resolver-v3` remains at v3.8. It is an immutable historical identity, not the selected final release.

## Component validation

- The suite has **240 tests**: 19 PagePool, 41 KeyStore, 48 BNodeStore and 132 TempConflictBTree. The release `resolver-v3-final` had 232; `02d354f9` adds 8 tests for MC/DC coverage.
- The macOS and Linux matrices each run 24 configurations: two write-task shapes (2×8 and 2×4), four block sizes (1, 2, 4 and 8 KiB), and three builds.
- Every configuration passes 240/240. macOS covers assertions, `-O2 -DNDEBUG`, and UBSan with Guard Malloc. Linux covers assertions, `-O2 -DNDEBUG`, and ASan/UBSan. Neither matrix reports a warning or sanitizer failure.
- [MC/DC coverage](https://github.com/ricardojimenezperis/foundationdb/blob/resolver-v3/fdbserver/resolver/resolver1/tests/MCDCCoverage.md), measured with clang 20.1.8 on the default 2×8 build: 153 of 210 conditions show an independence pair (72.86%). The report classifies each of the other 57 conditions and keeps assertion conditions in the denominator. The `RESOLVER1_WRITE_LG=0` build is measured separately and is not added to these figures.
- The mixed-batch tests combine admitted, conflicting and too-old transactions and cover empty, adjacent and overlapping writes.

## The v3.8 defect and final fix

V3.8 could reuse a saved write rank after pressure reclamation freed its prepared leaf. In a two-page pool, its regression case loses 60 accepted writes with 2×8 tasks and aborts at CV 21 with 2×4; the sweep finds 17 bad sizes under 2×8 and all 400 abort under 2×4. The same outcomes appear with assertions, optimized `NDEBUG` and sanitizer builds, except that three aborts become losses under `NDEBUG`.

`f866c98d` associates every saved rank with its leaf generation and mutation epoch and separates node and key-block generation spaces. The fixed code passes the two-page case and all 400 sweep sizes under both task shapes. Its decisions change only where v3.8 was wrong.

## FoundationDB integration

On Intel, `fdbserver` built from `18260afe` passes:

- ASan/UBSan conflict-set tests, FDB replay and small-pool cells down to two pages;
- 46/46 recovery runs covering Resolver, commit-proxy and TLog kills, 1↔2 Resolvers, 1→2 proxies and combined cases;
- 378/380 runs in both v3 and unmodified-FDB simulation arms, with the same two upstream `IncrementTest` failures and zero decision mismatches across 4,830,024 transactions;
- the common workload against 11 mutants, detecting six and explicitly recording the five not detected.

The long-trace runs on Intel and AMD agree with FDB in every one of 172,800 batches. Across all sixteen runs they share offered hash `11334b1aea22e641` and decisions hash `5001114085b550fb`, with no rejection for the window or memory.

## Evidence boundary

The complete report and the release assets are authoritative for final v3. Every public performance figure uses final v3 with its complete history pool in HugeTLB; FDB, v1 and v2 use their native ordinary-memory implementations. The comparison represents those deployed configurations and does not isolate page mode.

The evidence retained under [`evaluation/`](evaluation/README.md) belongs to the earlier v3.8 campaign. It remains reproducible historical evidence, but its 228-test count, cross-session timing ratios and v3.8 conclusions are superseded by the final report. Historical files are not rewritten to look as though they measured the final code.
