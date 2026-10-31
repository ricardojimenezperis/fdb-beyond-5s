# resolver v2 evaluation figures, HP (fig-v2-eval-hp-*)

Ten figures, each as SVG and PNG (200 dpi), built from measurements kept on HP. Full provenance (commits, SHA-256,
build and run commands, conditions) is in `MANIFEST.md`, `commits.csv` and `binaries.csv`. Measured states whose
commits are not published can be rebuilt from public tag `resolver-v1` with the patches and compiled sources in
`reconstruct/` (see `MANIFEST.md`). A public commit listed there is equivalent in its resolver1 sources only, not in
the measured executable.

## How to regenerate

Requirements: Python 3 and matplotlib 3.x.

```bash
python3 make_figures.py --from-csv
```

draws all ten figures from the CSV files in `data/` alone. It needs the two release assets
`data/longrun-per-batch.csv.gz` and `data/longrun-resolver-v2-time-per-batch.csv.gz`, which are not in Git:

- download both into `data/`;
- check them with `sha256sum -c ASSETS.sha256` (run in `data/`);
- the script stops with a message naming any asset that is missing.

```bash
python3 make_figures.py --campaign-root <dir>
```

first extracts every CSV file in `data/` from the original campaign directories under `<dir>`, then draws.
- **Campaign directories:** `2026-10-29-btree-v26-hp`, `2026-10-30-standalone-v1fork-v26-hp`,
  `2026-10-30-equal-memory-v26-hp`, `2026-10-23-equal-memory-hp` and `2026-10-11-fdb-v6-long-retention-hp`.
- **Short benchmarks from this directory:** `--campaign-root raw` re-extracts their CSV files, since `raw/` mirrors
  their paths. The long-trace logs are not in `raw/`: in that case the script says so and leaves the long-trace CSV
  files in `data/` as distributed.
- **Byte-identical output:** the gzip assets are written with mtime 0 and no stored name, so extraction reproduces
  them exactly.
- **Derived files:** both modes rewrite `data/longrun-resolver-v2-time-summary.csv` and
  `data/longrun-resolver-v2-time-600s.csv` from the per-batch times.

## Candidates

| label | code | in |
|---|---|---|
| FDB | FoundationDB's skip-list conflict history, selected by default knobs | all datasets |
| btree-v1 | the sequential B+ reference, fork commit `5a07ab1936aaa6c01b5f36191c5446dee0a3d674` | all datasets |
| resolver v2 | btree-v2.6, resolver1 development commit `dc2802fc2f6d76035e9d97bc621263e8b4f59cec`, measured from fork commit `22a8ae9fba943b73cf149d21b05dfe6b07c6e013` | all datasets |

- **FDB's source commit:**
  - fork commit `2656860f23132d530ce5c1ac003ee82e71299731` in conflictset and skiplisttest (binary
    `a21538f3b269cb5d158a3fd9657c72e0c9b460b996631b4425269048990797de`);
  - `0504e6b0225e686259cbcdeabeaa7d737a544104` in the integrated benchmark (`2656860f23` + the benchmark);
  - `d4dde9cfb531d495dcf8638a20129c7fbbb98312` for the long-trace accounting run.

  In all three it runs with the paged history disabled.
- **resolver v2's two commits are the same code:** `22a8ae9fba`'s resolver1 is byte-identical to `dc2802fc2f`.
- **Not plotted:** v107, btree-v2-g8, v2.4 and v2.5.

## Datasets and the evidence of equal work
1. **conflictset**, 2026-10-29, three interleaved rounds.
   - Every run of a workload committed the same number of transactions (962,526 points, 164,926 ranges), with 0 too
     old.
   - This benchmark prints no decisions hash. The evidence is a count match only: it does not show that the same
     transactions were accepted.
2. **Integrated benchmark** (`macBenchmark`: resolver1's standalone workload through FDB's Resolver batch path),
   2026-10-29, three interleaved rounds.
   - Every run has the same decisions hash: `4787dbf4af9b9ca8` with 29,209 conflicts (points), and
     `df42a6e5c89796ea` with 3,479,753 conflicts (ranges).
3. **Standalone benchmark**, 2026-10-30 (`2026-10-30-standalone-v1fork-v26-hp`): one warm-up run, then three rounds
   with the order alternated.
   - btree-v1 is the fork's resolver1 at `5a07ab1936`; resolver v2 is `dc2802fc2f` with G=16.
   - One harness source drives both. Generator (seed 1234), 3,000 batches of 1,000 writes and 4,000 reads, budget
     1,024 MiB, 2 KiB blocks, timers and output are all the same. Only the read call differs, each tree's own API:
     - btree-v1: `checkReads(span, status)`;
     - resolver v2: `checkReads<16>(points, ...)`, after an untimed stable sort of the read endpoints.
   - Same compiler and flags for both.
   - Every run, warm-up included, has the decisions hashes of dataset 2, `reclaimCV=0` and exit 0.
4. **skiplisttest**, 2026-10-29, seven key configurations, three interleaved rounds after a warm-up run that is not
   used.
   - One decisions hash per configuration, the same for all candidates; every exit code is 0.
5. **Long trace**, 5,400 s segment of `pilot-2000M-l0-7200.trace`, history budget 6,442,450,944 B.
   - Runs: resolver v2 on 2026-10-30, btree-v1 on 2026-10-23 (archived), FDB with a 265 s window on 2026-10-10/11
     (archived).
   - Checked before overlaying btree-v1 and resolver v2, for all three:
     - the same commit-version sequence (172,800 batches);
     - offered hash `11334b1aea22e641` and decisions hash `5001114085b550fb`;
     - no rejection.
   - btree-v1 and resolver v2 also have the same pool (98,300 pages of ordinary memory), knobs and harness
     configuration.

**Historic standalone runs:** the 2026-10-29 standalone runs, with reference resolver1 `073fcbb9af`, are kept as
history in `data/standalone-historic-073fcbb9af-runs.csv` and `raw/2026-10-29-btree-v26-hp/mac-standalone/`. They
are not plotted and not mixed with the 2026-10-30 runs.

## Aggregation
- **Short benchmarks:** the median of three rounds is the bar, and each round is a dot. There are no confidence
  intervals.
- **Ratios:** computed within each round against the same round's reference, and the median is taken over the three
  per-round ratios. No ratio mixes sessions or machines.
- **Directions:** throughput and rates are higher-is-better; time and time ratios are lower-is-better. Each panel says
  which, and ratio panels have a reference line at 1.
- **Retention:** commit version minus the tree's boundary after the batch (`retained_us`).
  - Figures plot the last batch of each trace second.
  - The tail slope is an ordinary least-squares fit over all batches above 3,900 s of trace time.
- **Memory:** `history_bytes` from the accounting runs, plotted as the maximum per trace second.

## Long-trace time breakdown (resolver v2)

**Categories:**
- read = `read_s`, write = `write_s`;
- rest = `total_s − read_s − write_s`, shown in two parts:
  - **other timed phases:** add, sort, intra-batch, combine and cleanup; 102.7–106.7 s per round;
  - **unattributed:** total minus all seven timed phases; 1.28–1.29 s per round, 0.06%.

Rest is defined as the difference, so read + write + rest equals the total by construction. That identity says
nothing about overlap; the instrumentation does.

**What each column measures** (harness `fdbserver/resolver/By5ResolverTest.inc`, batch path `ConflictBatch` in
`fdbserver/resolver/ConflictSet.cpp`, commit `22a8ae9fba`):
- `total_s`: wall time from before the batch's first `addTransaction` to the return of `detectConflicts`.
- `add_s`: the `addTransaction` loop, the start of that same interval.
- The others are differences, across the batch, of counters that `detectConflicts` accumulates. Each covers one
  call, timed as `t = timer(); call; counter += timer() - t`:
  - `sort_s`: `sortPoints`;
  - `read_s`: `checkReadConflictRanges`;
  - `intrabatch_s`: `checkIntraBatchConflicts`;
  - `combine_s`: `combineWriteConflictRanges`;
  - `write_s`: `mergeWriteConflictRanges`;
  - `cleanup_s`: the end-of-batch boundary update.

**Why read and write cannot overlap:**
- The calls run in this order, one after another, on one thread.
- `BTREE_V2_TASKS` interleaves software tasks inside `checkReads`. It creates no thread: no thread or async
  construct exists in resolver1 or in the batch path.
- So `checkReadConflictRanges` has returned before `mergeWriteConflictRanges` starts, and the seven timed intervals
  are disjoint parts of `total_s`.

**Where the unattributed time comes from:** time between those intervals inside `detectConflicts`. That includes
allocating and clearing the status array, collecting accepted and too-old transactions, and the timer calls
themselves.

## Raw records

Raw records preserve the original execution paths and machine-load readings.
These paths document the measurement environment; reproducing the figures
does not require that directory layout.

## Limitations
- **Two sessions:**
  - Short benchmarks are from 2026-10-29, except the standalone runs (2026-10-30). The long trace's resolver v2 runs
    are from 2026-10-30.
  - No figure compares times across sessions. In the long trace only resolver v2 has native times from its own
    session, so no time comparison with btree-v1 or FDB is shown there.
- **Session load:** the 2026-10-29 session started right after builds. Its 5- and 15-minute load averages were still
  19 and 31, decaying. The 1-minute load before each run was 1.06–1.83 (conflictset and integrated) and 0.93–1.24
  (skiplisttest).
- **conflictset:** the equal-work evidence is a count match, not a decision identity (see dataset 1).
- **Standalone harness:** one source drives both trees, but the read call is each tree's own API. resolver v2's path
  also sorts the read endpoints, untimed, before its timed reads.
- **Many code changes separate btree-v1 (`5a07ab1936`) from resolver v2 (`dc2802fc2f`).** The ratios do not
  attribute the difference to any single one, grouped reads included.
- **FDB's long-trace retention is not a measured curve.** It is the configured 265 s window, drawn as a line.
- **Retention is not steady.** It is still falling at the end of the segment, by −3.89 s per 600 s over the last
  1,500 s, for both btree-v1 and resolver v2. Where it would settle is not measured.
- **Only the long trace reclaims.** The short benchmarks run without memory pressure.
- **One machine:** HP only.

## Captions
- **fig-v2-eval-hp-conflictset:**
  - Throughput relative to FDB in FDB's conflictset benchmark, HP, one session, three interleaved rounds (dots) and
    their median (bar).
  - Points: resolver v2 0.89 and btree-v1 0.64 of FDB's transactions per second.
  - Ranges: resolver v2 0.97 and btree-v1 1.08.
  - The table gives the absolute rates.
- **fig-v2-eval-hp-conflictset-phases:**
  - Read and write time relative to FDB in the same runs.
  - Reads: resolver v2 0.91 (points) and 1.02 (ranges); btree-v1 2.11 and 0.89.
  - Writes: resolver v2 1.91 and 1.43; btree-v1 1.56 and 1.24.
- **fig-v2-eval-hp-fdb-integrated-bench:**
  - Read and write throughput relative to FDB when the standalone workload runs through FDB's Resolver batch path,
    HP, one session.
  - Reads: resolver v2 0.90 (points) and 1.14 (ranges); btree-v1 0.33 and 0.66.
  - Writes: resolver v2 0.52 and 0.65; btree-v1 0.60 and 0.71.
- **fig-v2-eval-hp-standalone-reads:**
  - Reads per second in resolver1's standalone benchmark, HP, one session, three rounds with alternating order. Both
    builds use one harness, with each tree's own read call.
  - resolver v2 (`dc2802fc2f`, G=16) reads 2.11 times the rate of btree-v1 (`5a07ab1936`) on points (2.25 vs 1.07
    M/s) and 1.46 times on ranges (3.47 vs 2.38 M/s).
  - The two code states differ in several changes.
- **fig-v2-eval-hp-standalone-writes:**
  - Writes per second in the same runs.
  - resolver v2 writes at 0.88 (points) and 0.91 (ranges) of btree-v1's rate.
- **fig-v2-eval-hp-skiplisttest:** time ratios of resolver v2 in skiplisttest over seven key configurations, HP, one
  session.
  - Against FDB: total 1.07–1.28, read 0.63–0.80, write 2.22–2.80.
  - Against btree-v1: total 0.62–0.77, read 0.28–0.30, write 1.08–1.12.
- **fig-v2-eval-hp-longrun-retention:**
  - Retained history over a 5,400 s segment of the long trace, under a 6,442,450,944 B history budget.
  - resolver v2 (2026-10-30) and the archived btree-v1 run (2026-10-23) overlap, with the same segment, load,
    decisions and memory contract. They peak at 702.9 s and stand at 582.9 s at the end.
  - FDB's configured 265 s window is drawn for reference.
- **fig-v2-eval-hp-longrun-retention-tail:**
  - The last 3,000 s of the same runs.
  - Retention is still decreasing at the end of the segment: −3.89 s per 600 s over the last 1,500 s, for both.
- **fig-v2-eval-hp-longrun-memory:**
  - Counted history memory over the same segment.
  - The trees count their whole pool from the first batch: peaks of 6,442,188,800 B (btree-v1) and 6,442,190,848 B
    (resolver v2).
  - FDB with a 265 s window (archived, 2026-10-10/11) rises to 6,354,541,376 B.
- **fig-v2-eval-hp-longrun-time-breakdown:**
  - resolver v2's native time on the long trace, three rounds in one session (2026-10-30).
  - Round 1: write 72.5%, read 23.0% and the other timed phases 4.5% of its 2,290 s; 1.28 s is unattributed.
  - From the second 600 s window on, each window takes 260–264 s.
