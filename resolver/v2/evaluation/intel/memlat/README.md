# Memory-access latency on the Intel benchmark core

A separate calibration on 31 October 2026 measures random dependent loads on
CPU 6 of the Intel Core Ultra 9 285K used for the resolver timing campaigns.
The core is a performance core without SMT. Sysfs reports a 48 KiB L1 data
cache, a private 3 MiB L2 and a shared 36 MiB L3.

## Method

The program follows a random permutation forming one cycle, with one dependent
pointer per 64-byte cache line. It sweeps 69 working-set sizes from 4 KiB to
512 MiB, warms each size, and records five repetitions and their median.

```sh
taskset -c 6 ./chase 6 4096 536870912
```

The binary `chase` was built from `src/chase.c` with gcc 13.3.0
(`gcc -O2 -fno-prefetch-loop-arrays -Wall`) and has no prefetch instructions.
Its SHA-256 is `a8c5d3ff83f4ff5a1fc54319a8a5d4d1e3900fb4d86ac22b853c4d8357069673`;
that of `src/chase.c` is `2cc2d07fe131e64e9b06ae7f087b35fa9000d5284bf55ff68a578e6e4d3588d5`.
The buffer uses ordinary 4 KiB pages, with THP and
hugepages disabled and `AnonHugePages = 0 kB` for the buffer. Frequency is not
fixed: powersave/HWP with turbo is active, and almost all recorded samples are
5.4-5.7 GHz. Results are measured nanoseconds per dependent access. No benchmark
campaign runs concurrently; the desktop remains active on other CPUs.

## Results

| Working set | Median latency, ns/access |
|---|---:|
| 4-45 KiB | 0.704-0.707 |
| 54-128 KiB | 1.58 |
| 430-512 KiB | 3.00 |
| 1 MiB | 3.64 |
| 2 MiB | 4.20 |
| 4 MiB | 11.3 |
| 8 MiB | 14.9 |
| 16 MiB | 19.2 |
| 32 MiB | 68.8 |
| 64 MiB | 131 |
| 128 MiB | 154 |
| 512 MiB | 163.2 |

These are working-set measurements including address translation, rather than
isolated cache-hit latencies. The 54-128 KiB plateau is not a cache level
reported by sysfs; its cause was not established. At 32 MiB the repetitions
span about 11 ns, while the 512 MiB repetitions span 0.40 ns.

## Original records

The preserved campaign is [2026-10-31-cpu6](2026-10-31-cpu6/).
It contains `src/chase.c`, the binary identity, the CSV of all 69 sizes and
repetitions, raw output, topology, environment, frequency samples and a figure.
Its original [SHA256SUMS](2026-10-31-cpu6/SHA256SUMS) protects the
campaign files. The records are copied without changing their contents.
