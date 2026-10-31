# 2026-10-30-equal-memory-v26-hp

btree-v2.6 in the equal-memory long run of 2026-10-23-equal-memory-hp, compared with that campaign's archived references.

## Machine and kernel
- HP, Intel Core Ultra 9 285K; timed and accounting runs on CPU 6; validation on CPUs 8-10.
- Kernel 7.0.0-38-generic at start (2026-10-30T00:19:06+01:00) and end (2026-10-30T03:02:35+01:00).
- nr_hugepages=0 throughout (the archived campaign had 3,300 reserved during its runs; only v107 uses them).
- GPU PCIe link capped at Gen1 (gpu-pcie-gen1.service).

## Source and binaries
- btree-v2.6: 22a8ae9fba943b73cf149d21b05dfe6b07c6e013, full Release build (Clang 19.1.5), fdbserver 022270403ad2d635b7f937c5b22dccb5478391edeb7a17382e74bc55e40232f8, RESOLVER1_BLOCK_SIZE default 2048.
- Relinks of the patched harness over that build (scripts/hp-build-tree.sh, compile and link lines in cmds-v26/):
  - bin/v26-acct (by5-acct-v107.py, by5-tempbtree-v26.py): fdbserver 8961252e4132778ad04b23b76292af5eaa0c9a934970ae58420ad1418e4ddc5f
  - bin/v26-nativemem (by5-membuf.py, by5-tempbtree-v26.py): fdbserver 2c1c5fb37bbe430cd50aef829cfe3fd657d2a0e92fc342e7564e308d65bac6ce
- by5-tempbtree-v26.py differs from the archived by5-tempbtree.py only in the member it reads for memory outside the pool: pending_ (btree-v2.6's list of leaves to clean) instead of pendingCleaning_.

## Workload and memory contract (identical to the archived btree arms)
- Trace pilot-2000M-l0-7200.trace, SHA-256 0c0d343969fe2e91ef95c09b765b74d078bc9ed48ccf2df15d9dd9db3fd4b6c2.
- BY5_SEGMENT_START=57600 BY5_SEGMENT_BATCHES=172800 BY5_PREHISTORY_BATCHES=57600 BY5_PRIME=0 BY5_FICT_MODEL=0 BY5_VALIDATE=0 BY5_PROCESS_SEED=1 BY5_FICT_SEED=1 BY5_FICT_LEAF=0.
- Knobs: --knob_resolver_use_paged_conflict_set=0 --knob_paged_resolver_pool_pages=98300 (6,442,188,800 B of ordinary memory, no HugeTLB, no window).
- Budget 6,442,450,944 B of history_bytes = pool + capacity of pending_.

## References (archived, not rerun)
- fdb265: 2026-10-11-fdb-v6-long-retention-hp/wave1/fdb-w265 (accounting), FDB skip list, window 265 s; native rounds from 2026-10-23-equal-memory-hp.
- v107: 2026-10-23-equal-memory-hp, pool 97,696 pages, backing both (HugeTLB), fictitious 20/20/23, priority 1.
- v1 and v2-g8: 2026-10-23-equal-memory-hp, ordinary memory, pool 98,300 pages; v2-g8 built with 1 KiB blocks.

## Runs
- runs/validation/{v26,v26-p300,v26-acct}: 6,400 batches, BY5_VALIDATE=1; v26-p300 with a 300-page pool.
- runs/acct-v26: accounting run, whole segment.
- runs/round{1,2,3}-v26: native runs, sequential, CPU 6, batch logs buffered in memory and written at exit, sync and writeback drained between runs.

## Analysis
- scripts/analyze-v26.py -> analysis/analysis.txt, analysis/analysis.json
- scripts/deliverables.py -> analysis/per-batch-v26.csv, analysis/windows-600s.csv, analysis/retention-memory-v26.png
- SHA256SUMS covers every file of the campaign.
