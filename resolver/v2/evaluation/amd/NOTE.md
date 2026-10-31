# Resolver v2 evaluation on AMD: FDB, btree-v1 and resolver v2

Campaign of 2026-10-30/31 on Hetzner, an AMD Ryzen 7 7700 (8 cores, 16 threads, SMT pairs 0↔8 … 7↔15, 32 MB L3
shared, one NUMA node), kernel 6.8.0-138-generic. Every timed run ran alone on CPU 7 with its SMT sibling, CPU 15,
idle. All results in this package are new unless they are marked as archived. The only archived data is the btree-v1
long-trace accounting run of 2026-10-23, which is used only for retention and memory, never for timing.

## 1. Candidates and sources

| candidate | measured revision (full commit) | what it is |
|---|---|---|
| FDB | 1dfa7ffe6a5210c66e36a628552d10a7046649d0 | the series' harness commit over upstream ef6d39c877: FDB's original skip list |
| btree-v1 | a8921c3babbaea00099cc8c4c8608e942e921120 | resolver-v1 (btree-v1, sequential reads), last commit before v2 |
| resolver v2 | 22d9bb7a670037fe40070a0164fd6885dc981276 | v2 (reads interleaved by `checkReads<G>`, G = `BTREE_V2_TASKS` = 16) |

These are the revisions actually built and measured, and every hash in this package refers to them. Each binary
always resolves with its own history: `newConflictSet()` differs per commit, so every candidate has its own
`fdbserver`. v107 is not a candidate and none of its binaries is used here (section 9).

Throughout this note, "resolver v2" means the measured revision 22d9bb7a67, not the published head of the
`resolver-v2` branch. Section 1.1 gives the correspondence.

### 1.1 Correspondence with the published history

The published history of the series was rewritten after this campaign. Its `resolver-v2` branch has head
491d3f5a6fd02bea43b7b02502e35469afc2a53a, 11 commits after a8921c3bab. 22d9bb7a67 is not in it.

| measured revision | published equivalent | difference |
|---|---|---|
| FDB 1dfa7ffe6a5210c66e36a628552d10a7046649d0 | the same commit, an ancestor of 491d3f5a6f | none |
| btree-v1 a8921c3babbaea00099cc8c4c8608e942e921120 | the same commit, an ancestor of 491d3f5a6f | none |
| resolver v2 22d9bb7a670037fe40070a0164fd6885dc981276 | 7c7638a92ca780eb6d8622b2dc84c1c9d128e7c9 | tests only: `fdbserver/resolver/resolver1/tests/tempconflictbtreetests.cpp` (+279, −1); no other file differs |

The published head then adds, after 7c7638a92c:
- 36dcc1a9d53e7277a3299672021c49f62a16799e, the write-reservation fix. It changes production code
  (`tempconflictbtree.{h,cpp}`, and an include in `BTreeV2.cpp`). Before each write the tree now reserves
  `max(35, 3 * ceil(2 * L / payload) + 2)` blocks, L being the longest key stored so far or the write's own if
  longer. The measured revision always reserved 35. If the pool cannot hold the write, it now throws
  `std::length_error` before storing anything, where the measured revision did not check the stores.
- 491d3f5a6fd02bea43b7b02502e35469afc2a53a, documentation and test evidence only.

For the loads of this campaign the longest key is 128 bytes in skiplisttest, 11 in conflictset (10, or 11 with
`keyAfter`), 12 in macBenchmark and blocksizebench, and 64 in the long trace. With 2 KiB blocks
`3 * ceil(2 * L / payload) + 2` is 5 blocks for all of them, so every write reserves the 35-block minimum, the same
quantity the measured revision reserved, and reclamation is requested with the same argument. The cost of executing
the new computation on every write (the key-length bookkeeping, the maximum and the division) has not been measured.
The results here are those of the measured revision. They are not a measurement of 491d3f5a6f.

Tree configuration: 2 KiB blocks (`RESOLVER1_BLOCK_SIZE` default 2048 in v2, `BlockSize = 2 * KiB` in v1), G = 16 for
v2, ordinary memory (`nr_hugepages=0` in every run), and the same pool for v1 and v2 in each block.

## 2. Builds

- Image `foundationdb/build:rockylinux9-latest`, sha256:591bb3f7b9a8c83ca8ddac97f0d05239532d0dd03eef411094e9fc23766879f7,
  clang 19.1.5.
- `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/local/bin/clang -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++ -DUSE_LIBCXX=1 -DUSE_LD=LLD <worktree>; ninja -j4 fdbserver`,
  v2 configured with `-DBTREE_V2_TASKS=16`. One detached worktree per commit, built before the session.
- Two harness builds per commit (section 3). `fdbserver` sha256:

| candidate | harness 1 (conflictset, macBenchmark) | harness 2 (skiplisttest) |
|---|---|---|
| FDB | b6e0a382ffececb79e433444849a06f5556b79287b4465327a138d56fe0e0d35 | 461d9d83864aaab2ac4b77daa2047ce111a9a19260d577e7c39e8c23c30913db |
| btree-v1 | c88bfa97cbac8fd1cb0c36ca8f5d34e391d64dcaa032765cca06a24a1b35f93c | a89118ac36b421f206859a85034cadabcd5091ad047be1e92bbde5a36422345b |
| resolver v2 | 7d74a7ea6ce6ca9c7cd5e1a6028ce532b1860435f17967111daf9af43788839d | f0246f71d86e46a5b395c00a558e7267266fd841fa569458fae0833a7ef185c8 |

- Standalone (`blocksizebench`, section 5.3): `clang++ -std=c++20 -O3 -DNDEBUG -stdlib=libc++ -Iresolver1 resolver1/pagepool.cpp resolver1/keystore.cpp resolver1/bnodestore.cpp resolver1/tempconflictbtree.cpp resolver1/bench/blocksizebench.cpp -o blocksizebench`,
  same image and compiler. v1 sha256 605c8bea90467ef392782319db5a46873169fd6a09ab9cc29affd2143b66a435, v2
  27f1e87d311e21386f11fabe5c92077eb7f2aa989eb7ebb67b3c5fc23d42b504.
- Long trace (section 5.5): `pub-v2/bin/v2-acct/fdbserver`, sha256
  0ab0357bcee5229876a06e9982bbcaf7e7272b6e927d3ecd8da57d32f2c370a6, built by `by5eval/build.sh acct` of the v2 tree
  at 22d9bb7a67 (its `by5-acct.py`, sha256 c6159d2d…, adds accounting only). Inputs in `longtrace-build/`.

## 3. Harness adaptations (no algorithm change)

The three commits do not carry the same benchmarks, so a common harness was added to each, identical in its added
lines (sha256 of the added lines 92d4bff2…, all three):

- `harness/EvalBench.inc`: the `noSim/fdbserver/conflictset/benchmark` unit test, with its original generation and
  defaults (1,000 warm-up batches, 2,000 measured, 500 transactions per batch, 4 reads and 2 writes each, key space
  10^7, mt19937_64 seeded by `--test_seed`). It resolves through each binary's own `ConflictSet` batch path and prints
  one `ConflictSetBenchmark` line. The decisions hash (FNV over each transaction's status) is computed after the batch
  timer stops. It needs `#include "fdbserver/core/Knobs.h"` to build in the FDB tree.
- `harness/MacBenchmark.inc`: the `noSim/fdbserver/conflictset/macBenchmark` unit test, copied unchanged from the
  btree-v2 tree; its generator is `MacBenchmarkRandom(1234)`.
- `harness-{fdb,v1,v2}.patch`: harness 1, the two includes above.
- `harness2-{fdb,v1,v2}.patch`: harness 1 plus one move in `skipListTest`: `ConflictSet* cs = newConflictSet();` goes
  after the test data is generated. Reason: in v1 and v2, `newConflictSet()` emits
  `TraceEvent("ResolverConflictHistory")`, and flow's trace sampling draws from `deterministicRandom()`. FDB's does
  not. Data generated after the constructor therefore differed between FDB and the trees, and the per-transaction
  decisions of FDB did not match. With the move, all three resolve the same data. A short check (head16 at 5,000
  ranges per batch) gave the same decisions hash 72bb0b788731a012 for all three before the battery continued
  (`raw/diag-harness2-short/`). conflictset and macBenchmark use their own generators and are not affected.
- Standalone: v1's commit has no `blocksizebench.cpp`. The v1 build uses
  `standalone-bench/v1/blocksizebench.cpp` (sha256 1b70ae9d…), the benchmark file as first written in the btree series
  (private commit eb3d86afa). It reads through v1's sequential `checkReads(span, status)`, which a8921c3bab has. Every
  `resolver1/` source compiled for v1 is byte-identical to a8921c3bab's. v2 uses its own tree's
  `resolver1/bench/blocksizebench.cpp` (sha256 8967ce1a…), which calls `checkReads<16>` on sorted points. Both use
  the repository layout (`resolver1/` beside `ResolverBatchTypes.h`). v1 is the fork's reference, not 073fcbb9af.
- Long trace: `scripts/longtrace-run.sh` is `by5eval/run.sh` of the v1/v2 trees plus a `v2)` arm identical to the
  `v1)` arm (pool `POOL_PAGES`, window 0).

## 4. Conditions and protocol

- `scripts/runone.sh` runs each process in `docker run --cpuset-cpus=7 --cpuset-mems=0`, with `/data` read-only and
  its output directory in `/dev/shm` (memory). After the process exits, the directory is copied to disk and synced,
  before the next run. Each `run.txt` records start, end, exit, CPU and sibling, the topology line of both
  (`7,7,0,0 15,7,0,0`), running containers before the run, load before, dirty memory before, CPU 7's and CPU 15's
  busy fraction over the run, `nr_hugepages`, kernel and the full command.
- Over the 148 timed runs: exit 0 in all; no other container running before any run; CPU 15 busy 0 to 0.81% (the
  highest in macBenchmark); CPU 7 busy 89 to 99.5% (container start and exit included).
- Writes: checked with strace outside the timed runs (CPUs 5/13, short loads; `scripts/diag-writes.sh`,
  `raw/diag-writes/`). The benchmarks write only their standard output and the trace file in `/out`, both in
  `/dev/shm`; no fsync.
- Warm-up: one run per candidate and workload or configuration before the rounds, kept and excluded from the results.
- Rounds: r1 FDB, v1, v2; r2 v1, v2, FDB; r3 v2, FDB, v1. Standalone, two candidates: v1, v2 / v2, v1 / v1, v2.
- Ratios are computed within each round (same round, same workload or configuration). Medians are shown with the
  three points; no best round and no best G is chosen.

## 5. Results by block

All tables come from `make_tables.py` over `data/` (`derived/tables.md` has every phase and ratio). Values are
median (round 1, round 2, round 3).

### 5.1 conflictset benchmark: 2026-10-31 00:15:38–00:18:06 (2 min 28 s, 24 runs)

Seed 1, 1,000 warm-up batches, 2,000 measured, 1,000,000 transactions, pool 16,384 pages for v1 and v2.

| workload | candidate | checkReadS | mergeS | measured batch time (s) | tx/s | committed | tooOld |
|---|---|---|---|---|---|---|---|
| points | FDB | 2.086 (2.086, 2.084, 2.090) | 1.284 | 4.190 (4.190, 4.182, 4.224) | 238,676 | 962,526 | 0 |
| points | btree-v1 | 4.580 (4.604, 4.580, 4.574) | 2.323 | 7.310 (7.347, 7.310, 7.298) | 136,795 | 962,526 | 0 |
| points | resolver v2 | 2.411 (2.409, 2.418, 2.411) | 2.633 | 5.445 (5.440, 5.455, 5.445) | 183,655 | 962,526 | 0 |
| ranges | FDB | 1.469 (1.469, 1.469, 1.466) | 0.159 | 2.026 (2.028, 2.026, 2.022) | 493,592 | 164,926 | 0 |
| ranges | btree-v1 | 1.377 (1.365, 1.377, 1.384) | 0.197 | 1.939 (1.925, 1.939, 1.951) | 515,620 | 164,926 | 0 |
| ranges | resolver v2 | 1.829 (1.815, 1.833, 1.829) | 0.213 | 2.415 (2.396, 2.420, 2.415) | 414,002 | 164,926 | 0 |

Decisions hash identical for the three: points 41ccf929e519502f, ranges 224f4d48ebf2cb5d.
Ratios of measured batch time, median [min, max] of the rounds: points v2/FDB 1.298 [1.289, 1.304], v2/v1 0.746
[0.740, 0.746]; ranges v2/FDB 1.194 [1.181, 1.195], v2/v1 1.244 [1.238, 1.248].

### 5.2 macBenchmark: 2026-10-31 00:18:34–00:23:07 (4 min 33 s, 24 runs)

3,000 batches, pool 16,384 pages for v1 and v2. "Sum of timed phases" = readS + writeS + combineS; it is not the
wall time of the batch path.

| workload | candidate | readS | writeS | sum of timed phases (s) | conflicts |
|---|---|---|---|---|---|
| points | FDB | 4.724 (4.731, 4.714, 4.724) | 1.992 | 6.746 (6.746, 6.745, 6.747) | 29,209 |
| points | btree-v1 | 13.417 (13.418, 13.417, 13.371) | 3.417 | 16.867 (16.874, 16.867, 16.814) | 29,209 |
| points | resolver v2 | 6.589 (6.597, 6.579, 6.589) | 3.812 | 10.421 (10.460, 10.421, 10.413) | 29,209 |
| ranges | FDB | 3.922 (3.932, 3.922, 3.911) | 1.187 | 5.141 (5.157, 5.141, 5.125) | 3,479,753 |
| ranges | btree-v1 | 5.976 (5.950, 6.002, 5.976) | 1.657 | 7.677 (7.634, 7.693, 7.677) | 3,479,753 |
| ranges | resolver v2 | 3.744 (3.739, 3.744, 3.744) | 1.694 | 5.470 (5.458, 5.476, 5.470) | 3,479,753 |

Decisions identical: points 4787dbf4af9b9ca8, ranges df42a6e5c89796ea. Sum of timed phases, median [min, max]:
points v2/FDB 1.545 [1.543, 1.551], v2/v1 0.619 [0.618, 0.620]; ranges v2/FDB 1.065 [1.058, 1.067], v2/v1 0.713
[0.712, 0.715]. Read check: ranges v2/FDB 0.955 [0.951, 0.957].

### 5.3 standalone blocksizebench, 2 KiB blocks: 2026-10-31 00:23:23–00:26:22 (3 min, 16 runs)

`blocksizebench <workload> 3000 1024`; v1 and v2 only (FDB has no standalone tree).

| workload | candidate | readS | writeS | totalS = sum of timed phases (s) | conflicts |
|---|---|---|---|---|---|
| points | btree-v1 | 9.877 (9.877, 9.882, 9.827) | 2.537 | 12.414 (12.414, 12.420, 12.354) | 29,209 |
| points | resolver v2 | 6.390 (6.392, 6.383, 6.390) | 2.772 | 9.159 (9.164, 9.159, 9.150) | 29,209 |
| ranges | btree-v1 | 4.738 (4.701, 4.738, 4.872) | 1.336 | 6.074 (6.035, 6.074, 6.208) | 3,479,753 |
| ranges | resolver v2 | 3.608 (3.621, 3.598, 3.608) | 1.382 | 4.996 (4.998, 4.979, 4.996) | 3,479,753 |

Decisions identical to macBenchmark's (same generator): points 4787dbf4af9b9ca8, ranges df42a6e5c89796ea.
v2/v1 totalS: points 0.738 [0.737, 0.741], ranges 0.820 [0.805, 0.828]; read check 0.647 and 0.759; write
insertion 1.093 and 1.034.

### 5.4 skiplisttest: 2026-10-31 01:04:25–01:55:49 (51 min 24 s, 84 runs)

500 batches of 40,000 ranges (10,000,000 transactions), span 10, seed 1, pool 262,144 pages for v1 and v2; seven
configurations. head16 took 5 min 52 s, which projected about 41 min for the seven; the long-key configurations
took longer (tail128 10 min 53 s).

| config | FDB Detect only (s) | btree-v1 | resolver v2 | v2/FDB | v2/v1 | v1/FDB |
|---|---|---|---|---|---|---|
| head16 | 16.215 | 28.025 | 19.438 | 1.199 [1.199, 1.204] | 0.694 [0.692, 0.696] | 1.730 |
| head32 | 16.531 | 29.526 | 20.800 | 1.257 [1.256, 1.263] | 0.704 [0.692, 0.705] | 1.786 |
| tail32 | 17.841 | 31.020 | 22.177 | 1.243 [1.241, 1.244] | 0.714 [0.714, 0.719] | 1.739 |
| head64 | 17.865 | 31.728 | 22.221 | 1.239 [1.236, 1.252] | 0.698 [0.698, 0.707] | 1.771 |
| tail64 | 23.466 | 37.284 | 27.478 | 1.171 [1.167, 1.176] | 0.737 [0.737, 0.739] | 1.589 |
| head128 | 19.915 | 35.543 | 26.617 | 1.337 [1.321, 1.346] | 0.746 [0.742, 0.749] | 1.785 |
| tail128 | 36.484 | 52.155 | 41.963 | 1.150 [1.150, 1.156] | 0.802 [0.790, 0.816] | 1.434 |

By phase, v2 against FDB: read check 0.81 to 0.96 (v2 faster in every configuration), write merge 2.30 to 2.60
(v2 slower). v2 against v1: read check 0.41 to 0.48, write merge 1.10 to 1.13. Per-round points are in
`derived/time-ratios.csv` and the phase tables in `derived/tables.md`. In every configuration the three candidates
committed 7,176,921 transactions with the same decisions hash, 30afc613d78499ea.

### 5.5 Long trace: validations 2026-10-30 22:50–22:53; accounting pass 2026-10-31 01:56:06–02:29:59 (33 min 53 s)

The 7,200 s trace `pilot-2000M-l0-7200.trace` (15,099,494,528 bytes, sha256
0c0d343969fe2e91ef95c09b765b74d078bc9ed48ccf2df15d9dd9db3fd4b6c2), cold fictitious start at batch 57,600, 2,048
transactions per batch, 31.25 ms per batch.

- **Short validations with checking** (`VALIDATE=1`, 9,600 batches, cores 5 and 4 concurrently, load average 42 at start; correctness only):
  normal budget (98,300 pages) and reduced budget (1,000 pages, 65,536,000 bytes, which reached its cap: retention
  at most 7.5 s against 300 s). Both: `modelTraceDiffEarly=0 modelTraceDiffLate=0`, `walkMismatches=0`, no rejection,
  the same decisions hash bb77645a24d12b24, and the same 376 decisions diverging from the trace's expected conflicts.
  The 376 come from the cold start (history before the segment is fictitious) and appear identically in the archived
  FDB and btree-v1 runs of the same start.
- **Accounting pass** (`VALIDATE=0`, CPU 7, segment [57600, 230400), 172,800 batches, pool 98,300 pages =
  6,442,188,800 bytes within the 6,442,450,944-byte budget, ordinary memory): exit 0, `walkMismatches=0` over 1,728
  walks, `divergent=0`, no window or memory rejection, peak history 6,442,188,800 bytes, peak RSS 6,344,316 KiB.
- **Equivalence with the archived btree-v1 run** (2026-10-23, binary a9db5854…, CPU 3), checked batch by batch
  before overlaying: same 172,800 batches; versions, offered hash, decisions hash, accepted, conflicts, window and
  memory rejections and history bytes identical in every batch; final offeredHash 11334b1aea22e641 and decisionsHash
  5001114085b550fb in both. The capacity floor, and so the retention, differs in 518 batches (first 27,070, last
  172,247) by exactly one 31.25 ms step, in both directions.
- **Retention**: both reach 702.875 s and discard for the first time at batch 22,492 (702.9 s into the segment); the
  minimum observed after that, 582.94 s, is at the end of the measured segment. No steady state is reached within it.

The accounting pass writes its per-batch logs to disk during the run (the by5 harness's ofstream). Its times are not
used and are not compared with any run.

## 6. Figures and captions

All figures are `figures/*.svg` and `*.png` (200 dpi), produced by `make_figs.py` from `data/` only.

- `fig-v2-eval-amd-conflictset-time`: FDB conflictset benchmark, seed 1, 2,000 measured batches of 500 transactions,
  pool 16,384 pages. Bars are medians of three rounds and dots the rounds; read check, write merge and measured batch
  time, points and ranges.
- `fig-v2-eval-amd-conflictset-throughput`: the same runs, transactions per second.
- `fig-v2-eval-amd-conflictset-time-ratios`: time ratios paired within each round (v2/FDB, v2/v1, v1/FDB); below 1
  the numerator is faster.
- `fig-v2-eval-amd-macbench-time`, `-throughput`, `-time-ratios`: macBenchmark, 3,000 batches; the total is the sum of
  timed phases (read check, write merge, combine), not wall time.
- `fig-v2-eval-amd-standalone-time`, `-throughput`, `-time-ratios`: blocksizebench, 2 KiB blocks, 3,000 batches,
  1,024 per batch; btree-v1 and v2 only.
- `fig-v2-eval-amd-skiplisttest-time`: skiplisttest "Detect only" time per key configuration, 500 batches of 40,000
  ranges, medians of three rounds with the rounds as dots.
- `fig-v2-eval-amd-skiplisttest-time-ratios`: paired ratios per configuration for Detect only, read check and write
  merge.
- `fig-v2-eval-amd-longtrace-retention`: history retained (minimum per second) over the 5,400 s measured segment,
  pool of 98,300 pages; resolver v2 new (2026-10-31), btree-v1 archived (2026-10-23). The curves coincide except for
  the 518 one-step differences above.
- `fig-v2-eval-amd-longtrace-memory`: history bytes per second for the same two runs.

## 7. Package contents

- `data/`: per-run CSVs (`conflictset-runs.csv` 24 rows, `macbench-runs.csv` 24, `standalone-runs.csv` 16,
  `skiplisttest-runs.csv` 84, warm-ups included with an empty `round`), `longtrace-amd-6gib-per-batch.csv`
  (345,600 rows; column `source` marks new and archived), `campaign-manifests.json`.
- `derived/`: `time-ratios.csv`, `summary.json`, `tables.md`.
- `public-correspondence.csv`: section 1.1 as a table.
- `figures/`: 13 figures, SVG and PNG.
- `raw/`: every run's `run.txt`, `stdout.txt`, `stderr.txt`, the MANIFEST and progress log of each block, the
  long-trace `run.txt`, `out.txt` and per-batch CSVs, the strace outputs, and the harness 2 short check.
  `raw/ORIGINAL-SHA256SUMS` lists the files before absolute paths were replaced by `$ROOT`, `$BUILDS` and `$HOME`,
  the only change made to them.
- `harness/`, `scripts/`, `standalone-bench/`, `longtrace-build/`: section 3 inputs and the scripts as run.
- `extract_data.py` (needs `FDB_ROOT`; rebuilds `data/` from the campaign directory), `make_figs.py` and
  `make_tables.py` (read only `data/`). Regenerating figures and `derived/` in a clean directory from `data/` gave
  byte-identical files (Python 3.12.3, matplotlib 3.6.3).

## 8. Commands

- conflictset: `scripts/integrated.sh conflictset <out>`, each run
  `fdbserver -r unittests -f noSim/fdbserver/conflictset/benchmark --test_workload=<points|ranges> --test_seed=1 --test_batches=2000 --test_warmupBatches=1000 --test_transactionsPerBatch=500 [--knob_paged_resolver_pool_pages=16384] -L /out`
  (the pool knob for v1 and v2).
- macBenchmark: `scripts/integrated.sh macbench <out>`, each run
  `fdbserver -r unittests -f noSim/fdbserver/conflictset/macBenchmark --test_workload=<w> --test_batches=3000 [--knob_paged_resolver_pool_pages=16384] -L /out`.
- standalone: `scripts/standalone.sh <out>`, each run `blocksizebench <w> 3000 1024`.
- skiplisttest: `scripts/skiplist.sh <out>`, each run
  `fdbserver -r skiplisttest -C fdb.cluster -s 1 [--knob_paged_resolver_pool_pages=262144]` with
  `SKIPLISTTEST_CPU=7 SKIPLISTTEST_KEY_BYTES=<16|32|64|128> SKIPLISTTEST_KEY_LAYOUT=<head|tail> SKIPLISTTEST_PER_BATCH=40000 SKIPLISTTEST_RANGE_SPAN=10`.
- long trace: `TRACE=<trace> POOL_PAGES=98300 SEGB=172800 VALIDATE=0 scripts/longtrace-run.sh v2 <v2-acct fdbserver> <out> 7`;
  validations with `SEGB=9600 VALIDATE=1` and `POOL_PAGES` 98300 (CPU 5) or 1000 (CPU 4).

## 9. Excluded from this evaluation

Kept on Hetzner as diagnostics, outside this package and outside the public evaluation:
- `diagnostic-not-public/skiplist-fdb-arm-v107-binary/`: a first skiplisttest battery whose FDB arm ran the v107
  binary with its default knobs. Rejected: the FDB arm must be built from FDB's own source. Replaced by section 5.4.
- `diagnostic-not-public/skiplist-fdbbase-head16-different-load/`: head16 with FDB from 1dfa7ffe6a before the harness 2
  move, when FDB resolved different data from the trees.
- `diag-decisions/`: the decision dumps that located the difference.

## 10. Limitations

- One machine, one session. No figure here is compared with HP or with earlier Hetzner sessions.
- These are batch-path and harness timings on one core, not cluster throughput.
- The standalone benchmark exists only for the trees; FDB is absent from that block.
- v1's standalone benchmark file is not part of v1's commit (section 3).
- The long-trace comparison is of retention, memory and decisions only; the v1 run is archived and ran on CPU 3, and
  no time of either run is used.
- The results are those of the measured revision 22d9bb7a67 (published equivalent 7c7638a92c), not of the published
  head 491d3f5a6f. The fix 36dcc1a9d5 keeps the 35-block reserve for these key lengths; the cost of its added
  computation per write has not been measured (section 1.1).
- The 6 GiB segment ends before retention stabilizes; 582.94 s is the minimum observed during the measured segment,
  not a steady-state value.
