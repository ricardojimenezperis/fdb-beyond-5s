# Resolver v3 evaluation on AMD Ryzen 7 7700

v3.8 (two write groups of eight tasks) in the configurations of the published AMD evaluation of resolver v2, and in
the 6 GiB long trace (by5), against the archived FDB, v1 and v2 runs. Only v3.8 was run in this campaign
(2026-11-04). The [evaluation](../../06-evaluation.md#method) states what every ratio here crosses; the
[evidence index](../README.md) lists the files and how to regenerate the tables.

## Arms

| arm | source | build | memory | session |
|---|---|---|---|---|
| v3.8 | `d6bba393b9`, tree `433edd77…` (= public `55d5f98974`), unchanged; defaults `BTREE_V3_TASKS=16`, `RESOLVER1_WRITE_GROUPS=2`, `RESOLVER1_WRITE_LG=8` | Clang 19.1.5, Release, libc++, lld, `-falign-functions=64` in `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` | pool on HugeTLB (`RESOLVER1_PAGE_MODE=hugetlb`) | 2026-11-04 |
| FDB | `1dfa7ffe6a` with the evaluation harness | stock | ordinary | 2026-10-31 |
| v1 | `a8921c3bab` (head of `resolver-v1`) with the evaluation harness | stock | ordinary | 2026-10-31 |
| v2 | `22d9bb7a67` with the evaluation harness, `BTREE_V2_TASKS=16`; its public equivalent `7c7638a92c` differs in tests only | stock | ordinary | 2026-10-31 |
| by5 FDB, 265 s window | FDB's skip list selected in a private series commit, as in the resolver v1 evaluation | stock | ordinary | 2026-10-23 |
| by5 v1 | private commit `5a07ab1936`, as in the resolver v1 evaluation | stock | ordinary | 2026-10-23 |

The archived runs of 2026-10-31 are the published data of the resolver v2 evaluation,
[`../../../v2/evaluation/amd/data/`](../../../v2/evaluation/amd/data/), which `scripts/make_tables.py` reads in place.
The archived by5 runs are those of the resolver v1 evaluation (asset `resolver-v1-evidence.tar.xz` of the
`resolver-v1` release, `6gib-amd/`); `data/archived-by5-runs.csv` transcribes their per-run totals from its
`analysis-v1-fdb265.json` (SHA-256 `9e67f86a0ebf2e39975de907970e2e981221afea7c22461435470773b5f4b72e`).

v3.8's evaluation harness is in its own tree: `EvalBench.inc` and `MacBenchmark.inc` are identical to the archived
harness, and skiplisttest has the same measured region plus a `totals:` line. Binaries are in `BIN-SHA256SUMS`:
`fdbserver` `b5f83bfc…`, standalone `blocksizebench` `415091af…` (built from the tree's `resolver1/` and
`bench/blocksizebench.cpp`, byte-identical copies), by5 native `290c6c6e…` and accounting `83beb3e6…` (the tree's
`by5eval/build.sh` over the aligned build). Build scripts: `scripts/build-v38.sh`, `scripts/sweep-build.sh`.

## Conditions

- One process at a time in `docker run --cpuset-cpus=7 --cpuset-mems=0` on CPU 7, with its SMT sibling idle (median
  0.09% busy over the 64 runs). One run, conflictset ranges r1, had its sibling 4.5% busy from a polling process on
  the host; its time is the lowest of its five, so it is kept. Output directories in memory. Linux 6.8.0.
- HugeTLB: before each block, `drop_caches`, compaction, then reservation of 512 × 2 MiB pages (conflictset,
  macBenchmark, standalone, 1 GiB pool), 8,192 (skiplisttest, 16 GiB) or 3,300 (by5, pool of 6,442,188,800 bytes).
  Memory compacted again before each run. A sampler outside CPU 7 recorded the free huge pages during each run: the
  whole pool was in HugeTLB in every run (by5 used 3,072 of 3,300 pages). Settings restored at the end
  (`restored-settings.txt`).
- One warm-up per load, kept and excluded; then five runs of conflictset, macBenchmark and standalone, and three of
  each skiplisttest configuration and of by5.
- Every run's decisions were checked against the archived ones before the next run; a mismatch would have stopped
  the campaign. There was none.

## Correctness

| load | decisions, every run, identical to the archived arms |
|---|---|
| conflictset | points 962,526 committed, 0 too old, `41ccf929e519502f`; ranges 164,926, `224f4d48ebf2cb5d` |
| macBenchmark, standalone | points 29,209 conflicts, `4787dbf4af9b9ca8`; ranges 3,479,753, `df42a6e5c89796ea` |
| skiplisttest, seven configurations | 7,176,921 committed, `30afc613d78499ea` |
| by5 | offered `11334b1aea22e641`, decisions `5001114085b550fb`; no read rejected for window or memory |

`correctness/` holds the validation of a Release build of the public commit `55d5f98974`: FDB's unit tests
`/fdbserver/conflictset/` (6 of 6) and `/fdbserver/skiplist/` pass, and the smoke replay (`smoke-compare.txt`, six cases)
decides as FDB's skip list with room, and under pressure explains every too-old transaction by the window or the
boundary, with no mismatch. The sweep's ten builds (five with assertions) were checked at 60 and 3,000 batches
(`raw/sweep/correctness.txt`).

## Figures

| load | figure |
|---|---|
| conflictset (2,000 batches, 500 transactions each, pool 16,384 pages) | measured batch time, `seconds` |
| macBenchmark (3,000 batches) | read + write + combine, the timed phases the archived harness reports |
| standalone (`blocksizebench`, 3,000 batches, 1 GiB) | see below |
| skiplisttest (40,000 ranges per batch, span 10, 16 GiB pool) | "Detect only" (`detectS`), the archived figure |
| by5 (172,800 batches, 5,400 s of the 7,200 s trace, cold start, 6 GiB budget) | sum of the per-batch totals |

**The standalone figure is not the same work in both arms.** The archived v1 and v2 used each version's own benchmark,
which times only the read check and the write merge (`totalS` = read + write). v3.8's benchmark times three phases:
preparation (building and sorting the endpoints of reads and writes, and indexing them), read and write. Two bounds are
shown: v3.8's read + write against the archived totals (v3.8's sort of write endpoints untimed), and v3.8's `totalS`
against them (v3.8's preparation timed, the references' not). Neither is a like-for-like comparison.

## Results

From `tables.md` (`scripts/make_tables.py`). Lower is better; the bracket is the minimum and maximum over every pair of
a v3.8 run with an archived run.

| load | v3.8 runs (s) | v3.8 | FDB | v1 | v2 | v3.8 / FDB | v3.8 / v1 | v3.8 / v2 |
|---|---|---|---|---|---|---|---|---|
| conflictset points | 4.363 4.373 4.368 4.378 4.368 | 4.368 | 4.190 | 7.310 | 5.445 | 1.043 [1.033, 1.047] | 0.598 [0.594, 0.600] | 0.802 [0.800, 0.805] |
| conflictset ranges | 2.589 2.590 2.599 2.608 2.606 | 2.599 | 2.026 | 1.939 | 2.415 | 1.283 [1.277, 1.290] | 1.340 [1.327, 1.355] | 1.076 [1.070, 1.089] |
| macBenchmark points | 8.822 8.787 8.784 8.809 8.792 | 8.792 | 6.746 | 16.867 | 10.421 | 1.303 [1.302, 1.308] | 0.521 [0.521, 0.525] | 0.844 [0.840, 0.847] |
| macBenchmark ranges | 5.080 5.038 5.063 5.047 5.056 | 5.056 | 5.141 | 7.677 | 5.470 | 0.983 [0.977, 0.991] | 0.659 [0.655, 0.665] | 0.924 [0.920, 0.931] |
| standalone points, read + write | 8.600 8.619 8.596 8.535 8.618 | 8.600 | – | 12.414 | 9.159 | – | 0.693 [0.687, 0.698] | 0.939 [0.931, 0.942] |
| standalone points, `totalS` | 10.371 10.393 10.360 10.298 10.374 | 10.371 | – | 12.414 | 9.159 | – | 0.835 [0.829, 0.841] | 1.132 [1.124, 1.136] |
| standalone ranges, read + write | 4.891 4.886 4.892 4.894 4.902 | 4.892 | – | 6.074 | 4.996 | – | 0.805 [0.787, 0.812] | 0.979 [0.978, 0.985] |
| standalone ranges, `totalS` | 6.681 6.655 6.673 6.662 6.671 | 6.671 | – | 6.074 | 4.996 | – | 1.098 [1.072, 1.107] | 1.335 [1.332, 1.342] |
| skiplisttest head16 | 15.670 15.675 15.653 | 15.670 | 16.215 | 28.025 | 19.438 | 0.966 [0.965, 0.969] | 0.559 [0.557, 0.560] | 0.806 [0.803, 0.807] |
| skiplisttest head32 | 16.520 16.317 16.405 | 16.405 | 16.531 | 29.526 | 20.800 | 0.992 [0.986, 1.002] | 0.556 [0.541, 0.561] | 0.789 [0.782, 0.798] |
| skiplisttest tail32 | 17.803 17.717 17.841 | 17.803 | 17.841 | 31.020 | 22.177 | 0.998 [0.989, 1.001] | 0.574 [0.570, 0.575] | 0.803 [0.795, 0.806] |
| skiplisttest head64 | 17.158 17.365 17.052 | 17.158 | 17.865 | 31.728 | 22.221 | 0.960 [0.949, 0.974] | 0.541 [0.536, 0.550] | 0.772 [0.764, 0.785] |
| skiplisttest tail64 | 22.112 22.031 22.130 | 22.112 | 23.466 | 37.284 | 27.478 | 0.942 [0.938, 0.943] | 0.593 [0.590, 0.595] | 0.805 [0.798, 0.807] |
| skiplisttest head128 | 19.244 18.755 19.284 | 19.244 | 19.915 | 35.543 | 26.617 | 0.966 [0.941, 0.969] | 0.541 [0.522, 0.543] | 0.723 [0.700, 0.732] |
| skiplisttest tail128 | 35.199 36.162 35.117 | 35.199 | 36.484 | 52.155 | 41.963 | 0.965 [0.959, 0.994] | 0.675 [0.661, 0.697] | 0.839 [0.830, 0.865] |
| by5 6 GiB, whole segment | 1002.096 1003.146 1004.810 | 1003.146 | 1043.874 | 2078.613 | – | 0.961 [0.956, 0.965] | 0.483 [0.481, 0.484] | – |

- **Slower:** conflictset ranges against all three references; macBenchmark points against FDB (1.30).
- **Not established** (within ±7% of 1, the distance a v3.8 reference has moved between sessions on this machine):
  every ratio against FDB in skiplisttest and by5, conflictset points and macBenchmark ranges against FDB. These can be
  read as comparable to FDB.
- v2 has no by5 timing runs on this machine, only its accounting run.

### by5, equal memory

- v3.8 keeps its history in its 6,442,188,800-byte pool, the same budget as v1. Its retention is identical to v1's:
  first discard at batch 22,492 (702.9 s into the segment); over the steady state (2,400–5,400 s) a minimum of
  582.94 s and a median of 592.78 s (`data/by5/analysis-fdb265-v38.json`, from the tree's `by5eval/analyze.py`; its
  arms are relabelled `fdb265` and `v38`).
- FDB keeps its 265 s window, its history peaking at 6,354,541,376 bytes.
- Batch time p99 over the whole segment: v3.8 6.245 ms, FDB 6.386 ms (medians of the three runs).

### Memory of v3.8

| load | peak RSS (HugeTLB excluded) | HugeTLB pool |
|---|---|---|
| conflictset | 57 MiB | 1,024 MiB |
| macBenchmark | 418 MiB | 1,024 MiB |
| standalone | 678 MiB | 1,024 MiB |
| skiplisttest | 1,228–5,659 MiB, by key length | 16,384 MiB |
| by5 | 564 MiB | 6,144 MiB |

The archived arms report `rssAfterMiB` and `residentMiB` from the harness with their pools counted in RSS: v2 17.6–22.0
GiB in skiplisttest, FDB 1.4–6.0 GiB.

### Diagnostic only: phases

`tables.md` gives the read- and write-phase ratios of every load. v3.8 does part of the work of writes in its read
pass, so its write phase is far shorter than v2's (0.25–0.64) and its read phase longer (1.11–1.40). These ratios
explain where the time goes; they are not gains of the whole and are not quoted as such. In by5, v3.8 spends 701.5 s
reading, 229.7 s writing and 0.004 s in cleanup (medians); FDB 309.5 s, 532.2 s and 124.1 s; v1 988.1 s and 1,011.8 s.

## Group sweep

v3.8 unchanged; five standalone builds differing only in `-DRESOLVER1_WRITE_GROUPS` and `-DRESOLVER1_WRITE_LG`
(`BIN-SHA256SUMS`; 2×8 is byte-identical to the evaluation's binary). Conditions as above, 512 huge pages, the whole
pool in HugeTLB in every run. `blocksizebench <points|ranges> 3000 1024`; one warm-up per configuration, then three
rounds: r1 2×8, 2×10, 2×12, 3×12, 3×20; r2 reversed; r3 2×8, 3×20, 2×10, 3×12, 2×12. Ratios within each round.
Decisions identical in all 40 correctness runs and in every timed run.

| workload | config | totalS r1 / r2 / r3 (s) | median | / 2×8 r1 / r2 / r3 | median |
|---|---|---|---|---|---|
| points | 2×8 | 10.170 / 10.204 / 10.174 | 10.174 | – | – |
| points | 2×10 | 10.205 / 10.158 / 10.215 | 10.205 | 1.0034 / 0.9955 / 1.0040 | 1.0034 |
| points | 2×12 | 10.214 / 10.190 / 10.259 | 10.214 | 1.0043 / 0.9986 / 1.0084 | 1.0043 |
| points | 3×12 | 10.205 / 10.218 / 10.158 | 10.205 | 1.0034 / 1.0014 / 0.9984 | 1.0014 |
| points | 3×20 | 10.207 / 10.194 / 10.192 | 10.194 | 1.0036 / 0.9990 / 1.0018 | 1.0018 |
| ranges | 2×8 | 6.656 / 6.630 / 6.626 | 6.630 | – | – |
| ranges | 2×10 | 6.613 / 6.631 / 6.605 | 6.613 | 0.9935 / 1.0002 / 0.9968 | 0.9968 |
| ranges | 2×12 | 6.688 / 6.670 / 6.651 | 6.670 | 1.0048 / 1.0060 / 1.0038 | 1.0048 |
| ranges | 3×12 | 6.651 / 6.654 / 6.619 | 6.651 | 0.9992 / 1.0036 / 0.9989 | 0.9992 |
| ranges | 3×20 | 6.601 / 6.638 / 6.620 | 6.620 | 0.9917 / 1.0012 / 0.9991 | 0.9991 |

Every whole-batch ratio is within ±0.5% of 1, and every variant has at least one round at or above 1, against a 2×8
spread between rounds of 0.33% (points) and 0.45% (ranges). 2×12 is slightly slower in ranges in all three rounds. The
three-group write phases are 1.0–1.4% shorter (3×20 in points 1.3% longer), which is 0.1–0.2% of the whole batch and
does not show in it. No variant qualifies for the integrated benchmarks; 2×8 stays.

## Dependent-memory latency

Byte-identical replay of the Intel dependent-pointer benchmark on the Ryzen 7 7700. It characterizes serial load-to-use
latency across the cache and memory hierarchy, verifies ordinary 4 KiB backing and publishes the raw runs, chain
validation and between-process variation. It is diagnostic evidence for interpreting the Intel/AMD split, not a direct
measurement of the HugeTLB-backed v3.8 pool. The [evaluation](../../06-evaluation.md#dependent-memory-latency) gives its
results and their reading, the [campaign report](memlat/2026-11-07-cpu7/REPORT.md) every run and check, and the [Intel
campaign](../../../v2/evaluation/intel/memlat/README.md) the reference it replays.

| | |
|---|---|
| source | `src/chase.c`, SHA-256 `2cc2d07fe131e64e9b06ae7f087b35fa9000d5284bf55ff68a578e6e4d3588d5`, unchanged from the Intel campaign |
| binary | `chase`, SHA-256 `a8c5d3ff83f4ff5a1fc54319a8a5d4d1e3900fb4d86ac22b853c4d8357069673` on both machines |
| compiler | `gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` |
| build | `gcc -O2 -fno-prefetch-loop-arrays -Wall -o chase src/chase.c`; no `-falign-functions=64`, because the experiment reproduced the Intel build exactly |
| placement | CPU 7, with its SMT sibling, CPU 15, idle; memory on node 0 |
| pages | ordinary 4 KiB; the 512 MiB mapping was verified as 4 KiB with no THP or HugeTLB |
| prefetch | no prefetch instruction in the binary |
| repetitions | 69 working-set sizes, each with five in-process timed repetitions; the table uses the preselected primary process |
| sweeps | five whole-process sweeps, which expose the variation at 1 MiB (medians 5.493 to 6.422 ns/access) |
| validation | all executions exited successfully; a chain validator on the same binary found, at all 69 sizes, a single cycle visiting every cache-line element |

To verify its checksums and regenerate its analysis, from this directory:

```sh
cd memlat/2026-11-07-cpu7
sha256sum -c SHA256SUMS
python3 scripts/check.py > analysis/check.txt
```

## Files

| path | what it holds |
|---|---|
| `tables.md`, `scripts/make_tables.py` | every table above, from `data/` and the archived CSVs of the resolver v2 evaluation |
| `data/v38-*-runs.csv`, `data/sweep-stage1-runs.csv` | one row per run, extracted from `raw/` by `scripts/extract.py` |
| `data/archived-by5-runs.csv` | the archived by5 per-run totals (see Arms) |
| `data/by5/round*-v3-batch-times.csv.gz` | per batch of each by5 timed run: batch, measured, read, write, cleanup and total, cut from the batch log |
| `data/by5/analysis-fdb265-v38.json` | retention, memory and timing of by5, archived FDB and v3.8 |
| `raw/v38/`, `raw/by5-v38/`, `raw/sweep/` | per run: `run.txt` (core, sibling load, HugeTLB, command), `stdout.txt`, `stderr.txt`, `rusage.txt`; session logs, manifests, reservation records; `batch-logs.sha256` for by5 |
| `BIN-SHA256SUMS`, `correctness/` | binaries and validation |
| `memlat/2026-11-07-cpu7/` | the dependent-memory campaign, self-contained, with its own `SHA256SUMS` |
| `scripts/` | the campaign scripts as run (`build-v38.sh`, `campaign-v38.sh`, `by5-v38.sh`, `runone.sh`, `rusage.py`, `prepare_memory.py`, `sweep-*.sh`) and the two analysis scripts |

**Changes from the campaign's own files.** Paths of the measuring machine are replaced by variables, and binary paths by
the binary's name. `raw/v38/MANIFEST` omits the hash of one helper script that this campaign did not use. FDB trace
files are not included.

**Not included.** The full by5 batch logs (identified in each run's `batch-logs.sha256`), the 7,200 s trace and the
resolver binaries. Not run: the integrated stage of the sweep, profiles.
