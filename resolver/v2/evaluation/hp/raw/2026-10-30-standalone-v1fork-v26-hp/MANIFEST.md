# 2026-10-30-standalone-v1fork-v26-hp

Standalone benchmark (resolver1 `blocksizebench`) on HP, run again with the fork's sequential btree-v1 as reference and
resolver v2 final. It replaces, for the public figures, the 2026-10-29 standalone runs, whose reference was resolver1
`073fcbb9af53df8a90c737c5931fc48240586640`. Those runs stay as a historical campaign in 2026-10-29-btree-v26-hp and
their samples are not mixed with these.

## Machine
- HP, Intel Core Ultra 9 285K, kernel 7.0.0-38-generic at start and end (out/env.txt), CPU 6 (taskset and docker
  --cpuset-cpus=6), started at load load=0.41 0.52 0.36.
- Runs 2026-10-30 21:23:44 to 21:26:33 UTC.

## Sources
- btree-v1: fdbserver/resolver/resolver1 of fork commit 5a07ab1936aaa6c01b5f36191c5446dee0a3d674 (git archive,
  src/v1-5a07ab1936; byte-identical to the commit, 0 mismatches). Its block size is fixed at 2 KiB in common.h.
- resolver v2: fdbserver/resolver/resolver1 and ResolverBatchTypes.h of fork commit
  22a8ae9fba943b73cf149d21b05dfe6b07c6e013 (src/v2.6-22a8ae9fba). All resolver1 files are byte-identical to resolver1
  commit dc2802fc2f6d76035e9d97bc621263e8b4f59cec (0 mismatches). RESOLVER1_BLOCK_SIZE default 2048.
- Harness: src/blocksizebench.cpp, one file for both, derived from dc2802fc2f's bench/blocksizebench.cpp. The
  generator (seed 1234), batches, key layout, budget, timers and output are the same for both builds. Only the read
  call differs, by macro:
  - RESOLVER1_SEQUENTIAL_READS (btree-v1): tree.checkReads(span of ReadConflictRange, status), as in 073fcbb9af's
    benchmark;
  - otherwise (resolver v2): the read endpoints are built and stably sorted before the timer starts, then
    tree.checkReads<BENCH_TASKS>(points, ...) is timed, as in dc2802fc2f's benchmark; BENCH_TASKS=16.
  The sort in the v2 path is untimed work the v1 path does not do.

## Build (scripts/build.sh, in foundationdb/build:rockylinux9-latest)
- Compiler: clang version 19.1.5, CXX='/usr/local/bin/clang++ -stdlib=libc++', flags -std=c++20 -O3 -DNDEBUG.
- btree-v1: -DRESOLVER1_SEQUENTIAL_READS, sources pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp.
- resolver v2: -DBENCH_TASKS=16, sources pagepool.cpp keystore.cpp bnodestore.cpp and src/tcb16-v26.cpp
  (tempconflictbtree.cpp plus the checkReads<16> instantiation).
- Binaries (SHA-256):
  - 967260079484e2a5c6c4d636a65c333468bf42d23ba967dc11b4d0792224c574  bin/blocksizebench-v1-5a07ab1936
  - 063d0ea3b1301aef83253537d46d77667e8c1f9bc58189d6ff295c9939f4c7fe  bin/blocksizebench-v2.6-g16

## Runs (scripts/run.sh)
- Each run: blocksizebench-<v> <points|ranges> 3000 1024 (3,000 batches of 1,000 writes and 4,000 reads, 12-byte
  keys, budget 1,024 MiB, 2 KiB blocks, nothing reclaimed).
- One warm-up run per candidate and workload (kind=warm), then three rounds (kind=timed): rounds 1 and 3 run
  btree-v1 then resolver v2, round 2 the reverse; points then ranges in each round.
- sync and drained dirty/writeback pages before each run; each run's single output line is held in the shell and
  appended to out/runs.txt after the run exits.

## Results
- out/runs.txt, one line per run; all 16 exit 0. Decisions: points 4787dbf4af9b9ca8 (29,209 conflicts), ranges
  df42a6e5c89796ea (3,479,753 conflicts) for every run, reclaimCV=0; the same hashes as the 2026-10-29 runs.
