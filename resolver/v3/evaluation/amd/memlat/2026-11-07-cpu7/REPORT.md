# Dependent-pointer latency on AMD Ryzen 7 7700, CPU 7 (2026-11-07)

The Intel pointer-chase calibration of 31 October 2026 was run unchanged on the AMD machine: the binary built here is
byte-identical to the Intel binary. All files are in this directory; `analysis/check.txt` holds every size and check.

## Results

The Intel run is one process that warms each size in place and then times five repetitions, so the five measurements
below are the repetitions of the primary sweep, `runs/m1-primary`, chosen before its data were seen. ns/access.

| Working set | AMD run 1 | run 2 | run 3 | run 4 | run 5 | AMD median ns/access |
|---|---:|---:|---:|---:|---:|---:|
| up to 45 KiB: inside AMD's 32 KiB L1d, shown at 27,520 B | 0.753 | 0.753 | 0.753 | 0.753 | 0.753 | 0.753 |
| up to 45 KiB: beyond AMD's L1d, shown at 46,336 B | 2.738 | 2.739 | 2.738 | 2.739 | 2.780 | 2.739 |
| 1 MiB | 6.287 | 6.290 | 6.277 | 6.282 | 6.275 | 6.282 |
| 16 MiB | 12.020 | 11.993 | 11.972 | 11.973 | 11.985 | 11.985 |
| 512 MiB | 103.828 | 103.821 | 103.801 | 103.860 | 103.839 | 103.828 |

| Dependent-pointer working set | Intel median ns/access | AMD median ns/access |
|---|---:|---:|
| up to 45 KiB | 0.70 | 0.75 up to 26.9 KiB; 2.74 at 45.3 KiB (2.76 at 38.0 KiB) |
| 1 MiB | 3.64 | 6.28 (five processes: 5.49 to 6.42) |
| 16 MiB | 19.2 | 12.0 |
| 512 MiB | 163.2 | 103.8 |

`512 MiB latency / L1-resident latency`, from unrounded medians at 27,520 B, a size in the L1 plateau of both:

- Intel: 163.232 / 0.704 = **231.9** (233.1 from the rounded 163.2 / 0.70).
- AMD: 103.828 / 0.753 = **137.9** (138.4 from the rounded 103.8 / 0.75; 137.7 to 139.5 over the five processes).

The two measurements characterize the different latency curves that the same scheduler must tolerate.

## Two limits on reading the table

**The "up to 45 KiB" row is L1-resident on Intel only.** Intel's L1d is 48 KiB and AMD's is 32 KiB. On AMD the medians
are 0.752-0.753 ns from 4,096 to 27,520 B, 0.796 ns at 32,704 B (repetitions 0.784-0.816), and 2.762 and 2.739 ns at
38,912 and 46,336 B. A single AMD value would misdescribe this row, so the row is split. The table sizes are also not
at the same place in each hierarchy: 1 MiB equals AMD's L2 and is a third of Intel's 3 MiB L2.

**The 1 MiB figure depends on the process.** Repetitions within a process agree, but medians differ between processes.
With the planned replicate m2, three more whole sweeps (m3 to m5), added after this appeared, make five processes:

| Size | m1 (primary) | m2 | m3 | m4 | m5 | Spread within m1 | Spread between the five |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 27,520 B | 0.753 | 0.753 | 0.753 | 0.753 | 0.753 | 0.00% | 0.00% |
| 46,336 B | 2.739 | 2.738 | 2.738 | 2.738 | 2.738 | 1.53% | 0.04% |
| 1 MiB | 6.282 | 6.212 | 5.493 | 5.631 | 6.422 | 0.24% | 14.95% |
| 16 MiB | 11.985 | 12.239 | 12.481 | 12.397 | 12.283 | 0.40% | 4.04% |
| 512 MiB | 103.828 | 105.022 | 103.920 | 103.719 | 104.576 | 0.06% | 1.25% |

The largest spreads between processes are at 741,440 B (23%) and 881,728 B (25%); the median over the 69 sizes is 0.92%.
A different physical page placement in each process would fit this, but placement was neither controlled nor recorded,
so the cause is not established. The Intel record has one process, so its spread between processes is unknown.

## Recovered Intel benchmark

| Item | Value |
|---|---|
| Location | `fdb-beyond-5s`, `resolver/v2/evaluation/intel/memlat/2026-10-31-cpu6/`, added in `2ba4c5d2b2`, read at `024a90db65` |
| Copy | `intel-reference/`, complete; its original `SHA256SUMS` passes (`build/intel-sha256-check.txt`) |
| `src/chase.c` SHA-256 | `2cc2d07fe131e64e9b06ae7f087b35fa9000d5284bf55ff68a578e6e4d3588d5`, checked before building |
| `chase` SHA-256 | `a8c5d3ff83f4ff5a1fc54319a8a5d4d1e3900fb4d86ac22b853c4d8357069673` |
| Compiler | gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| Build | `gcc -O2 -fno-prefetch-loop-arrays -Wall -o chase src/chase.c` |
| Intel run | `taskset -c 6 ./chase 6 4096 536870912`, under `/usr/bin/time -v` |
| Raw output | `intel-reference/2026-10-31-cpu6/out/chase-output.txt`, `memlat-cpu6.csv` |

Not recovered: the absolute path on the Intel machine and the wrapper that wrote its `env.txt` and frequency samples.
Neither is in the record. There is no separate build or run script; the commands above are what the Intel README
records. The source, binary and output are identified by hash.

## What ran

The build on this machine used the same compiler package and command, with no warnings, and `cmp` reports the binary
identical to Intel's, with the same Build ID (`build/build-output.txt`). The source was not altered.

```sh
timeout 1500 /usr/bin/time -v -o time.txt taskset -c 7 numactl --membind=0 ./chase 7 4096 536870912
```

The complete commands are in each `runs/*/run.txt`, and the drivers in `scripts/`.

- **Experiment, as on Intel:** 69 sizes from 4 KiB to 512 MiB, one pointer per 64-byte line in random order, in-process
  warm-up of 2 traversals plus 2^20 steps per size, five repetitions of max(2^24, 4 x lines) accesses, median of five.
  The sizes and access counts of all 69 rows equal Intel's. The table sizes are 1,048,512 B, 16,777,152 B and
  536,870,848 B, the sweep's nearest line-multiples.
- **Warm-up:** the source's warm-up is inside the process and prints nothing, so there is no warm-up output to retain
  and no separate warm-up execution, as on Intel. Executions d1 and d2 below are excluded from the results.
- **Controls:** CPU 7, sibling CPU 15 idle, node 0, one run at a time, no containers, stdout and `time` output in
  memory and copied after exit, `File system outputs: 0`. Before each run: `sync`, `drop_caches`, compaction, 10 s.

| Run | Purpose | CPU 15 busy | Exit | Elapsed |
|---|---|---:|---:|---:|
| `d1-smaps` | excluded: 512 MiB only, mapping snapshots from outside | 0.000% | 0 | 19 s |
| `d2-chain` | excluded: all sizes with the chain validator | 0.000% | 0 | 3:00.90 |
| `m1-primary` | the results | 0.035% | 0 | 2:51.79 |
| `m2`, `m3`, `m4`, `m5` | between-process spread | 0.017%, 0.017%, 0.000%, 0.006% | 0 | 2:51.42 to 2:53.03 |

## Environmental differences from Intel and from the published AMD campaign

- **Host, not container.** The Intel binary was built and run on the host, so this was too; the published AMD campaign
  used `docker run --cpuset-cpus=7 --cpuset-mems=0`. Its effect was not measured. An earlier different program on this
  core, in that container, gave close figures in another session (6.197, 12.318 and 103.544 ns at 1 MiB, 16 MiB and
  512 MiB); this is a sanity check only, its records are not published, and it is not used.
- **`numactl --membind=0`** was added to state the node. This machine has one node and Intel's command had no binding.
- **Kernel** 6.8.0-138 here, 7.0.0-38 on Intel. **SMT** on with the sibling idle here, absent on Intel's core.
- **Frequency is not fixed on either machine.** Here: `amd-pstate-epp`, governor `powersave` as in the published
  campaign, EPP `performance`, maximum 5.389 GHz, left unchanged. In `d2-chain`, 180 samples at 1 Hz from CPU 3 had a
  median of 5.14 GHz and a range of 5.139-5.340 GHz, leaving out the first sample of 0.4 GHz. Measured runs were not
  sampled, while Intel's measured run was. ns/access is the measured quantity.
- **Machine state.** Here an idle server with memory compacted; on Intel a desktop just booted, with its session active
  on other CPUs and no compaction recorded.
- **`-falign-functions=64` was not used,** because the flags had to equal Intel's.

## Validation

| Check | Result |
|---|---|
| 4 KiB pages while the 512 MiB case is resident | `KernelPageSize: 4 kB`, `MMUPageSize: 4 kB`, `AnonHugePages: 0 kB`, `Shared_Hugetlb: 0 kB`, `Private_Hugetlb: 0 kB`, `Rss: 524288 kB`, VmFlags with `nh` and no `ht`, at 3 s, 9 s and 14 s (`runs/d1-smaps/snapshot-t*/`), and at the start of the 512 MiB case of the 69-size sweep (`runs/d2-chain/smaps-512mib-case.txt`) |
| Same, in every measured process | the program's own `anon_huge_kb_after_touch=0` and `anon_huge_kb_at_end=0`; smaps was not read during measured runs, so as not to disturb them; `nr_hugepages` 0 before and after |
| Node and placement | `numa_maps`: `bind:0 anon=131072 N0=131072 kernelpagesize_kB=4`; `Cpus_allowed_list: 7`, `Mems_allowed_list: 0`, `THP_enabled: 0`; every run prints `running=7` |
| Every element once per traversal | the source has no built-in check, so `validation/chainwalk.c`, preloaded on the same binary, walks the chain before the first and after the last timed repetition of each size: 69 of 69 sizes close a single cycle of exactly the size's lines, within the size, unchanged across the repetitions |
| The validator can fail | source mutant `rho`: 17 of 17 sizes flagged; `two-cycles`: 9 of 17 flagged, all with even `n`; for the other 8, `n` is odd and the mutation still forms one valid cycle; the exact source: 0 of 17 (`validation/`) |
| Final sink | `sink` printed in all runs; m1 `9600887812531392`, Intel `9608394884785344`. It adds 69 final pointers, so it depends on the mapping's base address. All differences from Intel are multiples of 69 x 4096, and `sink - 69*buf` = 1234707648 in `d2-chain` is consistent with Intel's sink, so the final positions agree across machines (`analysis/check.txt`) |
| Exit | all seven executions exit 0 with empty stderr |
| No prefetch | `objdump -d chase \| grep -c prefetch` = 0; the measured loop is two dependent `mov (%r12),%r12` per iteration, with nothing else touching memory (`build/measured-loop.txt`) |

Not covered: the validator run is a separate process from the measured ones, though the binary and chain construction
are deterministic. Its timings are excluded; between 311,680 B and 19,951,552 B, 21 of its medians are more than 1%
below the five measured sweeps, and the cause is not established. The final-sink agreement is a modular check, not a
replay of Intel's run.

## Regenerating

```sh
sha256sum -c SHA256SUMS
python3 scripts/check.py    # prints analysis/check.txt; ends with "problems: none"
```

`runs/*/` holds every individual measurement, `env/` the system records, including `uname`, `lscpu`, caches, affinity
and frequency policy.

## Public copy

The public copy replaces private hostnames and absolute filesystem prefixes in command transcripts and makes the
campaign scripts root-relative. Source code, binaries, measured output and numerical analysis are unchanged.

The same replacement applies to the file names in the process maps (`smaps`, `numa_maps`) and to the root-filesystem
UUID in `env/cmdline.txt`. `$CAMPAIGN` stands for this directory. The files under `intel-reference/` are exact copies
of the published Intel campaign, protected by its own `SHA256SUMS`.
