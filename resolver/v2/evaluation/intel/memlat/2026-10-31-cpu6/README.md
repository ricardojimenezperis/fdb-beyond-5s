# Memory hierarchy latency, HP CPU 6 (2026-10-31)

- Code: `src/chase.c` (SHA-256 in `out/sha256.txt`), built with `gcc -O2 -fno-prefetch-loop-arrays -Wall -o chase src/chase.c`
  (compiler in `out/compiler.txt`); no software prefetch in the binary (`objdump -d chase | grep -c prefetch` = 0).
- Command: `taskset -c 6 ./chase 6 4096 536870912` (run under `/usr/bin/time -v`; the program also pins itself to CPU 6
  with sched_setaffinity and reports `running=6`).
- Method: one random cyclic permutation (Sattolo) over 64-byte lines of the working set, one pointer per line, so every
  access depends on the previous load. Working sets from 4 KiB to 512 MiB in steps of 2^(1/4) (69 sizes), 14 times the
  36 MiB L3. Per size: warm-up of 2 traversals plus 2^20 steps, then 5 timed repetitions of max(2^24, 4 x lines)
  accesses each, timed with CLOCK_MONOTONIC; ns/access per repetition and their median.
- Ordinary pages: anonymous mmap with `prctl(PR_SET_THP_DISABLE)` and `madvise(MADV_NOHUGEPAGE)`; system THP is
  `madvise` and `nr_hugepages` is 0. The program reads its own `/proc/self/smaps`: `AnonHugePages` of the buffer is
  0 kB after touching it and at the end.
- Results are kept in memory and printed after the last measurement; stdout went to /dev/shm and was copied here after
  exit.
- Conditions (`out/env.txt`, `out/topology.txt`, `out/freq-cpu6.txt`): kernel 7.0.0-38-generic before and after; no
  campaign, build or container running; the machine had just booted, with the desktop session (Claude desktop,
  gnome-shell) active on other CPUs; load 0.30 before, 1.33 after (the run itself). CPU 6 is a P-core (cpu_core 0-7),
  no SMT sibling (thread_siblings_list = 6). Governor powersave, driver intel_pstate (HWP active),
  EPP balance_performance, turbo on, min/max 0.8/5.8 GHz. Sampled once a second from CPU 0 (279 samples of
  scaling_cur_freq), CPU 6 was at 5.6-5.7 GHz in 232, 5.4-5.5 GHz in 44, and 5.3, 5.1 and 2.2 GHz once each (the
  2.2 GHz sample is the first, at start). Frequency is not fixed, so ns/access is the measured quantity; cycle
  equivalents are approximate.
- Caches of CPU 6 (sysfs): L1d 48 KiB 12-way, L1i 64 KiB, L2 3 MiB 12-way (private), L3 36 MiB 12-way shared by CPUs
  0-23; 64-byte lines.
- Outputs: `out/memlat-cpu6.csv` (bytes, accesses per repetition, 5 repetitions, median), `out/chase-output.txt` (raw),
  `out/memlat-cpu6.png`.
