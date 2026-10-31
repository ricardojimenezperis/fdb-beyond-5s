# resolver v2 evaluation figures, AMD and Apple M3 (fig-v2-eval-amd-*, fig-v2-eval-m3-*)

These figures come only from runs already kept; no benchmark was run for them.
- `python3 make_figures.py --from-csv` draws `figures/*.svg` and `figures/*.png` (200 dpi) from `data/` alone.
- `python3 make_figures.py RAW_DIR` first extracts the plotted candidates from the raw run records into `data/*.csv`. The raw records are not distributed: they also hold development candidates outside this comparison. The CSVs carry every field of the plotted runs.
- `SHA256SUMS` lists every file of this directory. `../../figs/` holds copies of the figures for the evaluation document.

The script needs Python 3 and matplotlib 3.x. The figures were drawn with Python 3.12.1 and matplotlib 3.11.2; a second run from `data/` alone, in an empty directory, gave byte-identical SVG and PNG files. Other matplotlib versions can change the rendered bytes without changing the values.

## Candidates

| Label | What ran |
|---|---|
| FDB | FoundationDB's skip-list conflict history, selected by default knobs |
| btree-v1 | the sequential B+ tree reference |
| resolver v2 | resolver1 at development state `dc2802fc2f`, with 16 tasks, before the write-reservation fix |

Other candidates appear in the raw records. The script skips them and does not plot them.

## Datasets

Each dataset comes from a single session, with three rounds in which the candidates run interleaved. All runs of a workload made the same decisions, and the script checks this before drawing:
- points: `4787dbf4af9b9ca8`, 29,209 conflicts;
- ranges: `df42a6e5c89796ea`, 3,479,753 conflicts.

No run reclaimed history: there is no memory pressure.

### AMD: integrated benchmark through FDB (`fig-v2-eval-amd-fdb-integrated-bench-*`)

- **Machine:** AMD Ryzen 7 7700, Linux 6.8.0-138-generic.
- **Session:** 2026-10-29, 18:58–19:03.
- **Benchmark:** `fdbserver -r unittests -f noSim/fdbserver/conflictset/macBenchmark --test_workload=points|ranges`.
  - It runs resolver1's `blocksizebench` workload through the Resolver's batch path.
  - Each run: 3,000 batches; 12,000,000 reads in total (each read is its own transaction); 2,999,856 (points) or 2,967,779 (ranges) write ranges.
  - Reads are timed as the batch path's read check, writes as its merge.
- **Run conditions:**
  - One run at a time, pinned to CPU 7, in the `foundationdb/build:rockylinux9-latest` image.
  - Working directory, trace, stdout and stderr in `/dev/shm`; no container log.
  - Dirty pages synced and drained before each run.
  - Trees with `--knob_paged_resolver_pool_pages=16384`, and `BTREE_V2_TASKS=16` in the environment.
- **Builds:** Release, Clang 19.1.5. Each binary is a fork commit plus one commit that adds the benchmark (`MacBenchmark.inc` and its include in `ConflictSet.cpp`):

  | Candidate | Fork commit with the benchmark | `fdbserver` SHA-256 |
  |---|---|---|
  | FDB | `0504e6b0225e686259cbcdeabeaa7d737a544104` | `bca2572daba43630…` |
  | btree-v1 | `347e083108fd6486ce2c4526052053231cadc899` (parent `5a07ab1936`) | `b9ea6294c3b27071…` |
  | resolver v2 | `3ba52abba955998757fa939b731fc3d2b8fa25b1` (parent `22a8ae9fba`, resolver1 from `dc2802fc2f`) | `621ebcf409a7343d…` |

- **Code measured, and the publication branch:**
  - The resolver v2 binary measured here is resolver1 at `dc2802fc2f`, before the write-reservation fix. The measurement is identified by the commits and binary hashes in this table, not by the commits of the publication branch `resolver-v2`.
  - The publication branch, without that fix, was checked against this binary in a separate, untimed run: its smoke replay matched the batch lines exactly, with room and under memory pressure.
  - The publication branch then adds the write-reservation fix, which reserves room for a write's longest keys. On the standalone benchmark the fixed code makes the same decisions (the same decision hashes, with and without reclamation). That does not make these results measurements of the fixed code.
- **Input CSV:** `data/amd-fdb-integrated-bench-runs.csv`.

### Apple M3: standalone benchmark (`fig-v2-eval-m3-standalone-*`)

- **Machine:** Apple M3 Pro, macOS 27.0.1 (26A434).
- **Session:** 2026-10-29, about 19:25–19:28.
- **Benchmark:** `blocksizebench points|ranges 3000 1024`. It drives `TempConflictBTree` directly, with 2 KiB blocks and a 1 GiB budget. FDB cannot run this benchmark.
- **Runs:** interleaved by round and workload, one at a time.
- **Builds:** Apple clang 15.0.0, `clang++ -std=c++20 -O3 -DNDEBUG`.
  - btree-v1: resolver1 at `073fcbb9af53df8a90c737c5931fc48240586640`, binary SHA-256 `3b0202419b60fc33…`.
  - resolver v2: resolver1 at `dc2802fc2f6d76035e9d97bc621263e8b4f59cec`, with the benchmark's task count set to 16 and `checkReads<16>` instantiated. Binary SHA-256 `5203669bc704a51d…`.
- **Input CSV:** `data/m3-standalone-runs.csv`.

## Aggregation

- **Medians and points:** each figure shows every round as a dot and the median of the three rounds as a bar. There are no confidence intervals.
- **Ratios:** a ratio is computed within each round, against the same round's FDB (AMD) or btree-v1 (M3). The figure shows the median of the three per-round ratios. No ratio combines different machines or sessions.
- **Directions:**
  - Rates (reads or writes per second) are higher-is-better.
  - Times are lower-is-better.
  - Each panel says which. Ratio panels draw a reference line at 1.
- **Separate phases:**
  - Read and write rates are drawn separately.
  - The M3 standalone benchmark also reports read, write and total times; they are drawn as three panels.
  - The integrated benchmark reports no total time, so none is drawn.

## Limitations

- **Load not recorded:**
  - The 1-minute load before each run was not recorded in either session.
  - On AMD, each run was pinned to one CPU.
  - On the Mac, runs were not pinned and the machine was in normal use. In round 1, btree-v1's read rate on points is about 14% below rounds 2 and 3; the figures show that round as a separate dot.
- **Standalone btree-v1 is resolver1 `073fcbb9af`.** Its algorithm is btree-v1's, and its decisions are identical, but its source differs from the fork's btree-v1 `5a07ab1936` (node layout, among other changes). The standalone v1 ratios therefore do not describe exactly the binary in the AMD dataset.
- **No memory pressure:** in both benchmarks the trees run without reclamation, so the reclamation paths are not timed.
- **One session per machine:** results from AMD and Apple M3 are not combined, and no ratio crosses machines.
- **Datasets missing on these machines:**
  - On AMD, FDB's `conflictset` benchmark and `skiplisttest` were not run with resolver v2 (dc2802fc2f). The AMD runs of those benchmarks are of earlier development versions, which are not plotted.
  - The standalone benchmark was not run on AMD.
  - FDB is not built on the Mac, so there is no integrated dataset for Apple M3.

## Captions

- **fig-v2-eval-amd-fdb-integrated-bench-rates:**
  - Read and write rates when the standalone workload runs through FDB's Resolver batch path, AMD, one session; three rounds (dots) and their median (bar).
  - Reads, in millions per second: FDB 2.58, resolver v2 1.81, btree-v1 0.91 (points); resolver v2 3.21, FDB 3.07, btree-v1 2.03 (ranges).
  - Writes, in millions per second: FDB 1.53, btree-v1 0.89, resolver v2 0.81 (points); FDB 2.52, btree-v1 1.83, resolver v2 1.76 (ranges).
- **fig-v2-eval-amd-fdb-integrated-bench-vs-fdb:**
  - The same runs, relative to FDB within each round.
  - Reads: resolver v2 0.70 (points) and 1.05 (ranges); btree-v1 0.35 and 0.66.
  - Writes: resolver v2 0.53 and 0.70; btree-v1 0.58 and 0.73.
- **fig-v2-eval-m3-standalone-reads:**
  - Reads per second in the standalone benchmark, Apple M3, one session.
  - resolver v2 reads at 2.23 times btree-v1's rate on points (2.36 vs 1.06 M/s), and 1.50 times on ranges (3.30 vs 2.20 M/s).
- **fig-v2-eval-m3-standalone-writes:**
  - Writes per second in the same runs.
  - resolver v2 writes at 0.95 (points) and 0.92 (ranges) of btree-v1's rate.
- **fig-v2-eval-m3-standalone-times:**
  - Read, write and total times of the same runs, for 3,000 batches.
  - Points: read 5.09 s (resolver v2) vs 11.33 s (btree-v1); write 2.86 vs 2.72 s; total 7.95 vs 14.05 s.
  - Ranges: read 3.63 vs 5.45 s; write 1.69 vs 1.56 s; total 5.35 vs 7.00 s.
