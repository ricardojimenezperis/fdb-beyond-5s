# Provenance manifest: fig-v2-eval-hp

Every artifact behind the figures, with full commit hashes, full SHA-256 and the commands that built and ran it.
- `commits.csv`: every commit.
- `binaries.csv`: every binary and input.
- `raw/`: the original records and scripts, under the same relative paths they had in the campaigns.
- `SHA256SUMS`: every file of this directory except the two release assets, whose checksums are given below.

## Measured states, public counterparts and reconstruction

Each figure was produced by the binaries listed under "Binaries", identified by commit and SHA-256. Those identities
are permanent. A public commit listed here is an equivalent of the **resolver1 sources only**
(`fdbserver/resolver/resolver1`). It does not stand for the measured executable, nor for its batch-path integration,
harness or configuration.

| measured state | measured commit | binary SHA-256 | public commit with equivalent resolver1 sources | reconstruction from the public base |
|---|---|---|---|---|
| FDB, conflictset and skiplisttest | `2656860f23132d530ce5c1ac003ee82e71299731` | `a21538f3…797de` | none: FDB's skip list is upstream code | `990d78d983..2656860f23.patch` |
| FDB, integrated benchmark | `0504e6b0225e686259cbcdeabeaa7d737a544104` | `c3990be3…e4ff3` | none | above, then `2656860f23..0504e6b022.patch` |
| FDB 265 s, long-trace accounting | `d4dde9cfb531d495dcf8638a20129c7fbbb98312` | `d519f029…f6294` | none | `990d78d983..2656860f23.patch`, `2656860f23..d4dde9cfb5.patch`, then the compiled sources in `build-sources/…/Xfl6` |
| btree-v1, conflictset and skiplisttest | `5a07ab1936aaa6c01b5f36191c5446dee0a3d674` | `6fd487fd…c708d` | `c44fd2247be51c26ca48c56dcc0587fb7b54ea04` | `990d78d983..5a07ab1936.patch` |
| btree-v1, integrated benchmark | `347e083108fd6486ce2c4526052053231cadc899` | `699cb35b…e6f24` | `c44fd2247b` (resolver1 only) | above, then `5a07ab1936..347e083108.patch` |
| btree-v1, long-trace accounting | `5a07ab1936` + harness patches | `6ca0adcd…0c490c2b` | `c44fd2247b` (resolver1 only) | `990d78d983..5a07ab1936.patch`, then `build-sources/…/v1-acct` |
| btree-v1, standalone | resolver1 of `5a07ab1936` + shared harness | `96726007…24c574` | `c44fd2247b` (resolver1 only) | `990d78d983..5a07ab1936.patch` for resolver1, plus `raw/2026-10-30-standalone-v1fork-v26-hp/src/` and `scripts/build.sh` |
| resolver v2, conflictset and skiplisttest | `22a8ae9fba943b73cf149d21b05dfe6b07c6e013` | `02227040…40232f8` | pending | `990d78d983..5a07ab1936.patch`, `5a07ab1936..22a8ae9fba.patch` |
| resolver v2, integrated benchmark | `3ba52abba955998757fa939b731fc3d2b8fa25b1` | `4e61dc12…a956` | pending | above, then `22a8ae9fba..3ba52abba9.patch` |
| resolver v2, long trace (accounting, native) | `22a8ae9fba` + harness patches | `8961252e…4ddc5f`, `2c1c5fb3…bac6ce` | pending | the two patches to `22a8ae9fba`, then `build-sources/…/v26-acct` and `…/v26-nativemem` |
| resolver v2, standalone | resolver1 of `22a8ae9fba` (= `dc2802fc2f6d76035e9d97bc621263e8b4f59cec`) + shared harness | `063d0ea3…f4c7fe` | pending | the two patches to `22a8ae9fba` for resolver1, plus the shared harness and `build.sh` |
| historic standalone baseline (not plotted) | resolver1 `073fcbb9af53df8a90c737c5931fc48240586640` | `833214ce…e19c` | none | `build-sources/2026-10-29-btree-v26-hp/mac-standalone/` (sources and bench files), with `raw/2026-10-29-btree-v26-hp/mac-standalone/run-hp.sh` |

The full SHA-256 of every binary is in the next section and in `binaries.csv`.

### Public base and reconstruction
- **Public base:** tag `resolver-v1` of github.com/ricardojimenezperis/foundationdb, commit
  `990d78d9831266ac082adb8b6d61d30a9a910189`. A tag is a fixed reference, unlike a branch head.
- **Patches:** `reconstruct/patches/<from>..<to>.patch` hold full-index binary tree diffs (`git diff --binary
  --full-index <from> <to> -- . ':!design'`). They exclude `design/`, which holds documents and is not used by any
  build.
- **Chains:**
  - FDB: base → `2656860f23` → `0504e6b022`, and base → `2656860f23` → `d4dde9cfb5`;
  - btree-v1 and resolver v2: base → `5a07ab1936` → `347e083108`, and base → `5a07ab1936` → `22a8ae9fba` →
    `3ba52abba9`.
- **Procedure:**

  ```bash
  git clone https://github.com/ricardojimenezperis/foundationdb fdb && cd fdb
  git checkout --detach 990d78d9831266ac082adb8b6d61d30a9a910189
  git apply --index --binary ../reconstruct/patches/990d78d983..5a07ab1936.patch
  git apply --index --binary ../reconstruct/patches/5a07ab1936..22a8ae9fba.patch
  ```

  This yields resolver v2's measured tree. Then build with the command in "Builds".
- **Relinked long-trace binaries:** their exact compiled sources are in `reconstruct/build-sources/<campaign>/bin/<name>/`:
  - the patched harness, `ConflictSet.cpp`, the B+ sources and `fdbserver.cpp`;
  - each directory's `MANIFEST`, which records the tree, the flags and the SHA-256 of every source.

  Compile them with the full build's own compile and link lines, as `raw/2026-10-30-equal-memory-v26-hp/scripts/hp-build-tree.sh`
  does.
- **Standalone binaries:** the resolver1 directory of the reconstructed tree, with the shared harness, built by
  `raw/2026-10-30-standalone-v1fork-v26-hp/scripts/build.sh`.

### Differences between measured binaries and public commits
- **btree-v1 / `c44fd2247b`:** the equivalence covers `fdbserver/resolver/resolver1` only. The measured binaries also
  contain parts that are not claimed equal to the public branch's integration ("Resolve with btree-v1 through direct
  calls"):
  - the fork's batch-path integration (`ConflictSet.cpp` and the knobs that select the history);
  - the by5 harness;
  - in some binaries, `MacBenchmark.inc` or the long-trace harness patches.
- **FDB / `1dfa7ffe6a5210c66e36a628552d10a7046649d0`:** a public reference of the harness, on branch `resolver-v1`,
  with FDB's skip list. It is not the commit that produced any measured binary.
- **resolver v2:** the public branch `resolver-v2` (on `a8921c3bab`) is not final, and its hashes may change. The
  commit goes in `commits.csv` once it is final, beside the measured identities, which stay.
- **Configuration that is not in any commit:**
  - `BTREE_V2_TASKS=16` and the pool-size knobs in each run command;
  - long-trace harness patches `by5-acct-v107.py`, `by5-membuf.py`, `by5-tempbtree*.py`;
  - the `Xfl6` compile flags `-DPAGED_BTREE_INLINE_KEY_BYTES=64 -DPAGED_BTREE_NODE_CAPACITY=23 -DPAGED_RESOLVER_SEARCH_STATS=1`;
  - no `-D` flags elsewhere;
  - the standalone harness macros `RESOLVER1_SEQUENTIAL_READS` and `BENCH_TASKS=16`.

### Evidence: checked on HP, and reported by the development machine
- **Checked on HP:**
  - **btree-v1:** the `fdbserver/resolver/resolver1` tree of public `c44fd2247b` is git tree
    `2fe37ad8e201312a2bcadacef23cb5b0e3d18f3c`, the same as in the measured `5a07ab1936`. Checked from a read-only
    fetch of branch `resolver-v1`, whose head `a8921c3babbaea00099cc8c4c8608e942e921120` carries a later resolver1
    tree, `7238015bfb35e203cc654983fb1e961f1bd17a67`.
  - **resolver v2:** the resolver1 of `22a8ae9fba` is byte-identical, 39 of 39 files, to the copy of resolver1
    `dc2802fc2f` received as a `git archive` from the development machine.
  - **Patches:** each patch chain, applied to `990d78d983`, reproduces the measured commit's tree outside `design/`.
    Path, mode and blob id were checked for every file, for all seven measured commits.
  - **Compiled sources:** each copy in `build-sources/` matches the SHA-256 that its binary's `MANIFEST` records.
  - **Standalone sources:** the sources of the 2026-10-30 runs are byte-identical to `5a07ab1936` and
    `22a8ae9fba`/`dc2802fc2f`.
- **Reported by the development machine, not verified on HP:**
  - the smoke replay of `c44fd2247b` matches `5a07ab1936` batch by batch;
  - the public `resolver-v2` is equivalent to `dc2802fc2f`, apart from comments, the `checkReads<16>` instantiation,
    the benchmark's `Tasks = 16` and test fixes;
  - its smoke replay matches `3ba52abba9` batch by batch with `BTREE_V2_TASKS=16`;
  - the full hashes of `073fcbb9af` and `dc2802fc2f` (HP holds only their abbreviated forms, which match);
  - `dc2802fc2f` is the tip of private branch `btree-v2`, and `073fcbb9af` an ancestor of it;
  - `22a8ae9fba`, `3ba52abba9`, `0504e6b022`, `347e083108` and `073fcbb9af` will not be published;
  - `d4dde9cfb5` is not in the development machine's repository.

## Binaries (`fdbserver` unless stated)

| dataset | candidate | SHA-256 | built from |
|---|---|---|---|
| conflictset, skiplisttest | FDB | `a21538f3b269cb5d158a3fd9657c72e0c9b460b996631b4425269048990797de` | `2656860f23` |
| conflictset, skiplisttest | btree-v1 | `6fd487fd673580d06448dd57d8489e49096ef32ffc4764bac710f5ca786454f9` | `5a07ab1936` |
| conflictset, skiplisttest | resolver v2 | `022270403ad2d635b7f937c5b22dccb5478391edeb7a17382e74bc55e40232f8` | `22a8ae9fba` |
| integrated benchmark | FDB | `c3990be3e98068b6950592c1c78bb2019363cd275dc162d55bc08b79dc3e4ff3` | `0504e6b022` |
| integrated benchmark | btree-v1 | `699cb35b7731600cabd3d4129b0b1da4377ff9727588bb1d7bc628df77be6f24` | `347e083108` |
| integrated benchmark | resolver v2 | `4e61dc1274587ed3d0aa9815e99dafb1c0995178c6e63c93c297699b5781a956` | `3ba52abba9` |
| standalone | btree-v1 (`blocksizebench-v1-5a07ab1936`) | `967260079484e2a5c6c4d636a65c333468bf42d23ba967dc11b4d0792224c574` | `5a07ab1936` resolver1 + shared harness |
| standalone | resolver v2 (`blocksizebench-v2.6-g16`) | `063d0ea3b1301aef83253537d46d77667e8c1f9bc58189d6ff295c9939f4c7fe` | `22a8ae9fba` resolver1 (= `dc2802fc2f`) + shared harness |
| long trace | resolver v2, accounting | `8961252e4132778ad04b23b76292af5eaa0c9a934970ae58420ad1418e4ddc5f` | `22a8ae9fba` + harness patches `by5-acct-v107.py`, `by5-tempbtree-v26.py` |
| long trace | resolver v2, native | `2c1c5fb37bbe430cd50aef829cfe3fd657d2a0e92fc342e7564e308d65bac6ce` | `22a8ae9fba` + harness patches `by5-membuf.py`, `by5-tempbtree-v26.py` |
| long trace | btree-v1, accounting | `6ca0adcd6ff2ab530565187d5007689a4ad4d2fea5440f85e6f12f389c490c2b` | `5a07ab1936` + harness patches `by5-acct-v107.py`, `by5-tempbtree.py` |
| long trace | FDB 265 s, accounting | `d519f029c09ff6372f66439b3a2c3b1031953aac356d6927ce59b8b2e3df6294` | `d4dde9cfb5` |
| historic standalone (not plotted) | baseline `073fcbb9af` | `833214cedb2e18b870b4bf362ceddb7148caf4bb5091813ba51cb4959e3ce19c` | resolver1 `073fcbb9af` with its own benchmark |
| historic standalone (not plotted) | resolver v2 | `3e0708e599049d57ba21c0d566249665b80b2d72fe547dc49b67e332a3e3ef20` | resolver1 `dc2802fc2f` with its own benchmark |

Input trace `pilot-2000M-l0-7200.trace`: 15,099,494,528 bytes, SHA-256
`0c0d343969fe2e91ef95c09b765b74d078bc9ed48ccf2df15d9dd9db3fd4b6c2`.

## Builds
- **Image:** every build ran in the image `foundationdb/build:rockylinux9-latest`, id
  `sha256:591bb3f7b9a8c83ca8ddac97f0d05239532d0dd03eef411094e9fc23766879f7`. The compiler is Clang 19.1.5.
- **`fdbserver` full builds** (`raw/builds/*/build.sh`):
  - `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/local/bin/clang -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++ -DUSE_LIBCXX=1 -DUSE_LD=LLD <worktree>; ninja fdbserver`;
  - one clean, detached worktree per commit;
  - no `-D` layout flags.
- **Long-trace binaries:**
  - The harness sources of the commit are copied and patched by the listed Python scripts. They are then compiled
    and relinked with the full build's own compile and link lines (`raw/2026-10-30-equal-memory-v26-hp/scripts/hp-build-tree.sh`).
  - Each binary's MANIFEST records the commit, `status 0 modified`, and `flags=[]`.
  - The patches change only the harness:
    - `by5-acct-v107.py`: memory accounting;
    - `by5-membuf.py`: batch log kept in memory and written at exit;
    - `by5-tempbtree*.py`: the tree counted as a capacity history.
- **Standalone binaries** (`raw/2026-10-30-standalone-v1fork-v26-hp/scripts/build.sh`):
  - `CXX='/usr/local/bin/clang++ -stdlib=libc++'`, `-std=c++20 -O3 -DNDEBUG`;
  - btree-v1 with `-DRESOLVER1_SEQUENTIAL_READS`, resolver v2 with `-DBENCH_TASKS=16`;
  - both built from one harness, `src/blocksizebench.cpp`.
- **Historic standalone binaries** (`raw/2026-10-29-btree-v26-hp/mac-standalone/run-hp.sh`): each built from its own
  commit's benchmark. resolver v2 has its task count set to 16 by `sed`.

## Runs and conditions

**Common conditions:**
- HP, Intel Core Ultra 9 285K (hybrid, no SMT), kernel 7.0.0-38-generic in every session.
- Every timed run on CPU 6 (`docker --cpuset-cpus=6` and/or `taskset -c 6`), one at a time.
- `sync` and dirty/writeback drained before each run, and no disk writes during a run: outputs went to /dev/shm or
  were held in memory and written at exit.
- Each run's load averages are kept in its record.

**conflictset:**
- Run 2026-10-29 19:38–19:41 CET by `raw/2026-10-29-btree-v26-hp/scripts/fdbbench5-hp.sh`, arms
  `fdb v107 v1 v2.5:16 v2.6:16`, three rounds. v107 and v2.5 are present in `runs.txt` but not plotted.
- Command: `fdbserver -r unittests -f noSim/fdbserver/conflictset/benchmark --test_workload=<points|ranges>`, with
  `--knob_paged_resolver_pool_pages=16384` for the trees and `BTREE_V2_TASKS=16` for v2.
- Benchmark parameters: seed 1, 2,000 batches, 1,000 warm-up, 1,000,000 transactions.

**Integrated benchmark:**
- Run 2026-10-29 19:41–19:47 by `macbench5-hp.sh`, same arms and rounds.
- Command: `fdbserver -r unittests -f noSim/fdbserver/conflictset/macBenchmark --test_workload=<points|ranges>`.
- Parameters: 3,000 batches, generator seed 1234; trees with pool 16,384 pages.

**skiplisttest:**
- Run 2026-10-29 19:47–21:23 by `run1-sl.sh`. Each configuration's three rounds follow a warm-up run, and the
  warm-up is not used.
- Command: `fdbserver -r skiplisttest -C dummy.cluster -s 1`, with `--knob_paged_resolver_pool_pages=262144` for the
  trees.
- Environment: `BTREE_V2_TASKS=16 SKIPLISTTEST_CPU=6 SKIPLISTTEST_KEY_BYTES=<n> SKIPLISTTEST_KEY_LAYOUT=<head|tail> SKIPLISTTEST_PER_BATCH=40000`.
- Load: 500 batches, span 10.

**Standalone:**
- Run 2026-10-30 21:23:44–21:26:33 UTC by `raw/2026-10-30-standalone-v1fork-v26-hp/scripts/run.sh`, in the image,
  `--cpuset-cpus=6`.
- Command: `blocksizebench-<v> <points|ranges> 3000 1024`.
- Order: one warm-up run per candidate and workload, then three rounds. Rounds 1 and 3 run btree-v1 first, round 2
  resolver v2 first.
- Load at start: 0.41 0.52 0.36.

**Long trace:**
- `raw/2026-10-30-equal-memory-v26-hp/scripts/run-eq.sh`:
  `fdbserver -r by5resolvertest -C dummy.cluster -s 1 --knob_resolver_use_paged_conflict_set=0 --knob_paged_resolver_pool_pages=98300`.
- Environment: `BY5_SEGMENT_START=57600 BY5_SEGMENT_BATCHES=172800 BY5_PREHISTORY_BATCHES=57600 BY5_PRIME=0 BY5_FICT_MODEL=0 BY5_VALIDATE=0 BY5_PROCESS_SEED=1 BY5_FICT_SEED=1 BY5_FICT_LEAF=0`.
- resolver v2: accounting run 2026-10-30 00:21–01:02 CET, and native rounds 01:02–03:02, all on CPU 6.
- btree-v1 accounting: 2026-10-23 11:58–13:23 CEST, CPU 10, untimed.
- FDB 265 s: run with a 265 s window (the run record shows `fdb:265000000`), 2026-10-10 19:31 to 2026-10-11
  01:59, CPU 5, untimed.
- The `run.txt` of each run is in `raw/`.

## Original records and their checksums

Raw records preserve the original execution paths and machine-load readings.
These paths document the measurement environment; reproducing the figures
does not require that directory layout.

- **Short benchmarks:** `raw/2026-10-29-btree-v26-hp/` and `raw/2026-10-30-standalone-v1fork-v26-hp/` hold every
  record the CSV files were extracted from:
  - `runs.txt` and `env.txt`;
  - each plotted run's stdout and stderr (conflictset, integrated benchmark);
  - `out.txt`, `run.txt` and `time.txt` (skiplisttest);
  - the scripts.

  Every copied file was checked against its campaign's SHA256SUMS, with no difference. FDB's trace XML files are not
  copied.
- **Row tracing:** each row of a short-benchmark CSV names its source: `source` is file and line, and `run_stdout`,
  `source_out` and `source_run` are run directories. These paths are relative to the campaign root and to `raw/`
  alike.
- **Long trace:** the original per-batch logs are not distributed. They are kept on HP. Row `batch = i` of
  `longrun-per-batch.csv.gz` comes from line `i` of:

| file | SHA-256 |
|---|---|
| `2026-10-30-equal-memory-v26-hp/runs/acct-v26/batches.csv` | `f73298e9b24410d68eb9a661f38e87f5883496eb019dea04b9f8c82f2724a5e8` |
| `2026-10-30-equal-memory-v26-hp/runs/acct-v26/batches.csv.acct.csv` | `482ffd8062c708ba246298b133774545affa7e6e16c84ac109a0ca9a6f792373` |
| `2026-10-23-equal-memory-hp/runs/acct-v1/batches.csv` | `a304b487146e51f3097e94593d14703443b9603e71472e88355a367a9e247c01` |
| `2026-10-23-equal-memory-hp/runs/acct-v1/batches.csv.acct.csv` | `88e7e7f83991fc2e59521e64e0e105886848d436fbe3b6ab38f2556ab7ddc27a` |
| `2026-10-11-fdb-v6-long-retention-hp/wave1/fdb-w265/batches.csv` | `b0d526e14d80dff850c13e956f6e226e9217c986515fae6bcc0a20973e54bba3` |
| `2026-10-11-fdb-v6-long-retention-hp/wave1/fdb-w265/batches.csv.acct.csv` | `bbfcf1c50ba2925671377c86ed59224aba7a18c34a3fe5e61a5d096e7dc27ee2` |

Rows `(round r, batch i)` of `longrun-resolver-v2-time-per-batch.csv.gz` come from line `i` of
`2026-10-30-equal-memory-v26-hp/runs/round<r>-v26/batches.csv`:

| round | SHA-256 |
|---:|---|
| 1 | `9f02c0275305ff36b1f46d7c22a589841bb9c511f4ef76a97da1308413aa65ab` |
| 2 | `0c139ae729f0adedcc72d51b3ffec35a45d65746d4f854faa1033e1ddffb28b7` |
| 3 | `e83e6a6dc0914e977ee9f2b6dfe6d458dc23d38dcaf0425fbc77d6a83c10c1f7` |

## Release assets (not for Git)

| asset | bytes | SHA-256 |
|---|---:|---|
| `longrun-per-batch.csv.gz` | 1,672,941 | `f1effa8256b2e136587706946ebccdd2a9a95f0925f3b8a3ae5a46a15492abe6` |
| `longrun-resolver-v2-time-per-batch.csv.gz` | 16,218,539 | `10974fb6e5080c2469587a9bb5bca03d192c7632e4e49f60566d61b19beeca54` |

- **Reproducible:** both are gzip files written with mtime 0 and no stored name, so `--campaign-root` regenerates
  them byte for byte.
- **Use:** download them into `data/` and check them with `sha256sum -c ASSETS.sha256`.
