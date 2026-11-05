# Resolver v3 evaluation on Intel Core Ultra 9 285K

v3.8 (two write groups of eight tasks) against FDB, v1 and v2. Only v3.8 was run in this campaign (2026-11-04); the
references are archived runs of the same work on the same machine. The [evaluation](../../06-evaluation.md#method) states what every ratio here crosses; the
[evidence index](../README.md) lists the files and how to regenerate the tables.

## Arms

| arm | source | build | memory | session |
|---|---|---|---|---|
| v3.8 | `d6bba393b9`, tree `433edd77…` (= public `55d5f98974`), clean checkout | Clang 19.1.5, Release, libc++, lld, `-DCMAKE_CXX_FLAGS=-falign-functions=64` | pool on HugeTLB (`RESOLVER1_PAGE_MODE=hugetlb`) | 2026-11-04, Linux 7.0.0-42 |
| FDB | `1dfa7ffe6a` with the evaluation harness | stock | ordinary | 2026-11-02 |
| v1 | `c44fd2247b` with the evaluation harness | stock | ordinary | 2026-11-02 |
| v2 | `491d3f5a6f` (head of `resolver-v2`) with the evaluation harness, 16 read tasks | stock | ordinary | 2026-11-02 |
| by5 FDB, 265 s window | native binary `52cf0920…`, FDB's skip list selected, as in the resolver v1 evaluation; accounting run from 2026-10-11 | stock | ordinary | 2026-10-23 |
| by5 v1 | `5a07ab1936` with the long-trace harness | stock | ordinary | 2026-10-23 |
| by5 v2 | `22a8ae9fba` with the long-trace harness, the measured v2 of the resolver v2 evaluation | stock | ordinary | 2026-10-30 |

The references of 2026-11-02 ran on Linux 7.0.0-38; their harness trees are in
`references/2026-11-02-short/BINARIES-harness3.txt`. Those files also hold rows of an earlier v3 development snapshot
(`8bcb81727a`), which is not used here. The by5 references are described with their binaries and patches in
[`../../../v2/evaluation/hp/`](../../../v2/evaluation/hp/MANIFEST.md) and in the resolver v1 evaluation.

v3.8's binaries (`BIN-SHA256SUMS`): `fdbserver` `d34a05ac…`, standalone `blocksizebench` `2de25d73…`, by5 native
`9102c0fd…` and accounting `5488fad9…`. `src-v38-resolver.sha256` lists the SHA-256 of the 71 files under
`fdbserver/resolver` of the measured tree; they match the public commit `55d5f98974` file for file. `build/` holds the
cmake line and the long-trace build manifests.

## Conditions

- One process at a time, pinned to one core (`taskset`, or `docker --cpuset-cpus` for the runs in the build image's
  container), the same core for every timed run of v3.8 and of the references; accounting runs are not timed.
- conflictset and macBenchmark ran in the build image's container, the standalone benchmark, skiplisttest and by5 on
  the host, as for the references.
- Work directories in memory; written pages drained between runs.
- One discarded warm-up per load, then three runs.
- The HugeTLB coverage of the whole pool was checked in a separate untimed run of every load (`backing/`), with the
  decisions of those runs.
- `runs/excluded-skiplisttest-head16/` is a first head16 set disturbed by a concurrent process on the host; it is kept
  and not used. The head16 figures come from the repeat in `runs/skiplisttest/head16-*`.

## Correctness

- The resolver1 battery: 228 of 228 with assertions and 228 of 228 with ASan/UBSan (`correctness/`).
- FDB's unit tests `/fdbserver/conflictset/` (6 of 6) and `/fdbserver/skiplist/`: pass.
- Decisions in every timed run, identical to the references:

| load | decisions |
|---|---|
| standalone, macBenchmark | points 29,209 conflicts, `4787dbf4af9b9ca8`; ranges 3,479,753, `df42a6e5c89796ea` |
| conflictset | points 962,526 committed, `41ccf929e519502f`; ranges 164,926, `224f4d48ebf2cb5d`; no read too old |
| skiplisttest | `30afc613d78499ea` |
| by5, whole segment | offered `11334b1aea22e641`, decisions `5001114085b550fb`; no read rejected for window or memory |
| by5, 200 s validation | offered `11c9f1cfb2e4d0fd`, decisions `6c863fc42fc1a441`, with the 98,300-page pool and with a 300-page pool (reclaiming); the accounting build's walk checks found no mismatch |

## Figures

Each figure is the whole timed workload as the benchmark reports it:

| load | figure |
|---|---|
| standalone (`blocksizebench`, 3,000 batches, 1 GiB, no reclamation) | `totalS`: preparation of the batch's endpoints, read check and write merge; the shared benchmark source times the same phases for every tree |
| conflictset (2,000 batches, 1,000,000 transactions) | measured batch time, `seconds` |
| macBenchmark (3,000 batches) | `totalS` of `MacBenchmarkPhases`: add, sort, read, write and combine |
| skiplisttest (500 batches of 40,000 ranges, 16 GiB pool) | `resolveS`: add + detect |
| by5 (172,800 batches, 5,400 s of the 7,200 s trace, cold start, 6 GiB budget) | sum of the per-batch totals |

## Results

From `tables.md` (`scripts/make_tables.py`). Lower is better; the bracket is the minimum and maximum over every pair of
a v3.8 run with a reference run.

| load | v3.8 runs (s) | v3.8 | FDB | v1 | v2 | v3.8 / FDB | v3.8 / v1 | v3.8 / v2 |
|---|---|---|---|---|---|---|---|---|
| standalone points | 8.767 8.871 8.878 | 8.871 | – | 14.076 | 10.719 | – | 0.630 [0.622, 0.640] | 0.828 [0.816, 0.831] |
| standalone ranges | 6.514 6.572 6.576 | 6.572 | – | 6.610 | 7.387 | – | 0.994 [0.974, 1.011] | 0.890 [0.876, 0.900] |
| conflictset points | 3.931 3.913 3.941 | 3.931 | 4.942 | 8.173 | 5.548 | 0.795 [0.787, 0.800] | 0.481 [0.476, 0.484] | 0.708 [0.703, 0.718] |
| conflictset ranges | 2.599 2.626 2.636 | 2.626 | 2.381 | 2.315 | 2.429 | 1.103 [1.069, 1.111] | 1.134 [1.100, 1.145] | 1.081 [1.069, 1.087] |
| macBenchmark points | 8.951 8.913 8.951 | 8.951 | 8.934 | 20.990 | 11.555 | 1.002 [0.996, 1.018] | 0.426 [0.415, 0.432] | 0.775 [0.764, 0.775] |
| macBenchmark ranges | 6.642 6.700 6.687 | 6.687 | 7.121 | 10.201 | 7.397 | 0.939 [0.932, 0.941] | 0.656 [0.650, 0.675] | 0.904 [0.896, 0.906] |
| skiplisttest head16 | 14.812 14.791 14.773 | 14.791 | 19.496 | 34.358 | 21.046 | 0.759 [0.755, 0.761] | 0.430 [0.430, 0.434] | 0.703 [0.702, 0.707] |
| skiplisttest head32 | 15.994 15.998 15.884 | 15.994 | 19.089 | 35.990 | 22.886 | 0.838 [0.826, 0.841] | 0.444 [0.431, 0.447] | 0.699 [0.692, 0.702] |
| by5 6 GiB, whole segment | 943.78 952.39 946.44 | 946.44 | 1,240.68 | 3,375.50 | 2,289.52 | 0.763 [0.761, 0.770] | 0.280 [0.279, 0.284] | 0.413 [0.411, 0.416] |

- **Slower:** conflictset ranges, against all three references (1.08 to 1.13).
- **Not established** (within the 2–10% that the same binaries have moved between sessions on this machine):
  macBenchmark points against FDB, macBenchmark ranges against FDB and v2, standalone ranges against v1, conflictset
  ranges against v2.
- skiplisttest head64, tail32, tail64, head128 and tail128 ran for v3.8 only (`analysis/short.txt`); the references
  have no run of that work, so they are not compared.

### by5, equal memory

| | v3.8 | FDB | v1 | v2 |
|---|---|---|---|---|
| minimum retention after the first discard, s | 582.938 | 265.000 (window) | 582.938 | 582.938 |
| first discard, s into the segment | 702.91 | 265.03 | 702.91 | 702.91 |
| peak counted history, bytes | 6,442,188,800 | 6,354,541,376 | 6,442,188,800 | 6,442,190,848 |
| peak RSS | 0.56 GiB, plus the 6.0 GiB pool on HugeTLB | 6.48 GiB | 6.55 GiB | 6.55 GiB |

v3.8 retains the same history as v1 and v2 in every 600 s window (`analysis/windows-600s.csv`). Retention has not
settled at the end of the segment: it still falls by 3.9 s per 600 s, as for v1 and v2. v3.8's three runs are within
0.9% of each other.

### Memory of v3.8 in the other loads

| load | peak RSS | pool (HugeTLB, not in RSS) |
|---|---|---|
| standalone | 0.66 GiB | 1 GiB |
| conflictset | 68 MiB | 1 GiB |
| macBenchmark | 429 MiB | 1 GiB |
| skiplisttest head16 / head32 | 1.21 / 1.82 GiB | 16 GiB |

The references kept their pools in ordinary memory, counted in RSS; conflictset recorded 195 / 87 MiB for FDB and
1,091 MiB for v1 and v2 at the end of the run (points / ranges for FDB).

## Group sweep (`sweep/`)

v3.8 unchanged, five standalone builds that differ only in `RESOLVER1_WRITE_GROUPS` and `RESOLVER1_WRITE_LG`
(`sweep/BIN-SHA256SUMS`; 2×8 is bit-identical to the evaluation's `blocksizebench`), HugeTLB pool checked in every
binary and load (`sweep/backing/`), one warm-up, then three rounds in rotated order (r1 2×8, 2×10, 2×12, 3×12, 3×20;
r2 3×12, 3×20, 2×8, 2×10, 2×12; r3 2×10, 2×12, 3×12, 3×20, 2×8). Decisions identical in every run.
`sweep/summary.csv` comes from `sweep/scripts/summary.py`.

| workload | config | totalS median (s) | / 2×8, rounds r1, r2, r3 | median | write / 2×8 (diagnostic) |
|---|---|---|---|---|---|
| points | 2×8 | 8.908 | – | – | – |
| points | 2×10 | 8.922 | 0.998, 1.002, 0.999 | 0.999 | 0.978 |
| points | 2×12 | 8.937 | 0.997, 1.004, 0.997 | 0.997 | 0.958 |
| points | 3×12 | 8.927 | 0.996, 1.003, 0.996 | 0.996 | 0.960 |
| points | 3×20 | 8.891 | 0.992, 0.999, 0.994 | 0.994 | 0.940 |
| ranges | 2×8 | 6.549 | – | – | – |
| ranges | 2×10 | 6.552 | 1.001, 0.999, 1.001 | 1.001 | 0.998 |
| ranges | 2×12 | 6.572 | 0.998, 1.000, 1.004 | 1.000 | 0.991 |
| ranges | 3×12 | 6.615 | 0.993, 1.013, 1.010 | 1.010 | 0.994 |
| ranges | 3×20 | 6.636 | 0.996, 1.001, 1.013 | 1.001 | 0.988 |

Larger groups write faster in points (up to 6.0% for 3×20), but the read phase rises by 0.4–0.9% and the preparation by
up to 0.7%, so the whole batch changes by −0.1% to −0.6%, within the round-to-round spread of 1.6–1.7% (0.3–2.5% in
ranges). In ranges no variant is faster. No configuration qualifies; 2×8 stays.

## Files

| path | what it holds |
|---|---|
| `tables.md`, `scripts/make_tables.py` | the results table, from `analysis/short.json` and `data/by5-runs.csv` |
| `analysis/short.json`, `short.txt` | every short-load run of v3.8 and the references; `scripts/tables.py` regenerates them byte for byte from `runs/` and `references/2026-11-02-short/` |
| `analysis/analysis.txt`, `analysis.json`, `windows-600s.csv` | by5 retention, memory and timing (`scripts/analyze-by5.py`) |
| `data/by5-runs.csv` | by5 whole-segment totals per run and their source |
| `runs/` | per-run output of every load; by5 runs have `batch-times.csv.gz` (batch, measured, read, write, cleanup and total of each batch, from the batch log) and `batch-logs.sha256` |
| `references/` | the archived reference runs: the 2026-11-02 short loads (per-run CSV and raw lines) and the by5 runs' `run.txt` and `out.txt` |
| `build/`, `correctness/`, `backing/` | build lines and manifests, test logs, HugeTLB coverage checks |
| `scripts/` | the campaign scripts as run (`short.sh`, `sl.sh`, `sl-head16-rerun.sh`, `long.sh`, `all-long.sh`) and the analysis scripts |
| `sweep/` | the group sweep |

**Changes from the campaign's own files.** Paths of the measuring machine are replaced by variables. `tables.py` has its
two input paths changed to this layout. `analyze-by5.py`, `analysis.*` and `windows-600s.csv` omit an internal control
arm that is not part of this evaluation; the `v2` rows dated 2026-10-23 in `analysis.txt` are an earlier development
build of v2 and are not used (the by5 v2 reference is `v26`, 2026-10-30). FDB trace files are not included.

**Not included.** The full by5 batch logs of v3.8 (identified by `batch-logs.sha256` in each run) and of the references
(`references/by5/BATCH-LOGS-CANONICAL.txt`); the 7,200 s trace, which the resolver v1 evaluation shows how to
regenerate; the binaries. The by5 totals of FDB and v1 are therefore taken from `analysis.txt`; they are the figures
already published in the resolver v1 evaluation, and v2's are in
[`../../../v2/evaluation/hp/data/longrun-resolver-v2-time-summary.csv`](../../../v2/evaluation/hp/data/longrun-resolver-v2-time-summary.csv).
Not run: stage 2 of the sweep, other loads with reclamation (except by5's 300-page validation), profiles.
