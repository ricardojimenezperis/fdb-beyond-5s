# Resolver v3 · Evaluation

Resolver v3.8 makes write application cheap enough that read checking becomes the larger cost again. Against resolver
v2 it is faster in most workloads on both measured machines, but slower in FDB's conflictset benchmark with range
transactions. Against FoundationDB's original conflict set (FDB), the result depends on the machine and the workload:
on the Intel machine v3.8 is faster in conflictset points, skiplisttest and the long trace, and comparable in the
integrated custom benchmark; on the AMD machine it is slower in conflictset ranges and in the integrated custom
benchmark with points, and comparable elsewhere, the long trace included. On the long trace v3.8 retains the same
history as v1 and v2 under the same 6 GiB budget, more than twice FDB's configured window.

The benchmarks keep their paths separate: FDB's `conflictset/benchmark`, the custom benchmark integrated into FDB
(`macBenchmark`), that benchmark's standalone executable (`blocksizebench`), `skiplisttest`, and the long trace (by5).
Every ratio of resolver times compares candidates on one machine, and no resolver time from the Intel machine is
compared with one from the AMD machine. The one cross-machine table is the
[dependent-memory latency](#dependent-memory-latency) diagnostic, which runs one byte-identical binary on both.

## Measured code

| | |
|---|---|
| public code | [`resolver-v3`](https://github.com/ricardojimenezperis/foundationdb/tree/resolver-v3), commit `55d5f98974b352f3a124439ae86beb0f8b7ca76d` |
| measured commit | `d6bba393b92ca4dcf058a024dfcf890733d695c3`, the original v3.8 commit |
| tree of both | `433edd77accbc70b47bfeaa4301e6da743139992` |
| configuration | the defaults: two write groups of eight tasks (`RESOLVER1_WRITE_GROUPS=2`, `RESOLVER1_WRITE_LG=8`), 16 read tasks (`BTREE_V3_TASKS=16`), 2 KiB blocks |

The public history was rebuilt from the original commits with identical trees; the [mapping](mapping.tsv) gives the
correspondence. The measured and published commits have the same tree, so the measured sources are the published
ones. On the Intel machine the SHA-256 of all 71 files under `fdbserver/resolver` were recorded at measurement time,
and they match the public commit file for file. The [validation and evidence](05-validation-and-evidence.md) document
covers the snapshot's tests.

## Method

**Only v3.8 was measured for this evaluation.** FDB, v1 and v2 are archived runs on the same machine, with the same
work and the same decisions. Every comparison below except the AMD standalone benchmark times the same phases of that
work in both arms: conflictset and macBenchmark with the same harness code, skiplisttest with the same measured region,
and by5 as the sum of the same per-batch totals. The AMD standalone benchmark times different phases, and is reported
without a conclusion. The archived runs come from earlier sessions:

| | v3.8 | FDB, v1, v2 |
|---|---|---|
| session | 4 November 2026 | 23 October to 2 November 2026 |
| conflict-history pool | HugeTLB 2 MiB pages (`RESOLVER1_PAGE_MODE=hugetlb`), a v3 option | ordinary memory |
| function alignment | `-falign-functions=64` over the whole binary | stock flags, no alignment |
| compiler | Clang 19.1.5, Release, libc++, lld | the same |

FDB's code and flags were not changed. The function alignment is applied to v3.8 because, in its development
campaigns, function placement alone moved read and write times by 1–3%. HugeTLB coverage of the whole pool was checked
in separate untimed runs of the short loads on Intel, and by sampling during every timed run on AMD.

**Every ratio crosses sessions and memory backing.** None is a paired comparison within one session. On the AMD
machine a v3.8 reference has moved by up to ±7% between sessions; on the Intel machine the same binaries have moved by
2–10% between sessions. A ratio within ±7% of 1 on AMD, or within ±10% on Intel, is therefore not an established
difference, in either direction, and the text below calls it comparable.

**Runs.** One timed process at a time, pinned to one core, the same core for v3.8 and for the references on each
machine; on AMD the core's SMT sibling stayed idle (the Intel processor has no SMT). One warm-up per load, kept and
excluded. Then three runs on Intel, and five runs on AMD (three in skiplisttest and by5). Working files stay in memory
during timed runs.

**Correctness before timing.** Every timed v3.8 run made the same decisions as the references:

| load | decisions |
|---|---|
| conflictset | points 962,526 committed, `41ccf929e519502f`; ranges 164,926, `224f4d48ebf2cb5d`; no read too old |
| macBenchmark and standalone | points 29,209 conflicts, `4787dbf4af9b9ca8`; ranges 3,479,753, `df42a6e5c89796ea` |
| skiplisttest | 7,176,921 committed, `30afc613d78499ea` |
| by5 | offered `11334b1aea22e641`, decisions `5001114085b550fb`; no read rejected for window or memory |

On Intel the resolver1 battery passed 228 of 228 tests with assertions and with ASan/UBSan, and FDB's unit tests
`/fdbserver/conflictset/` and `/fdbserver/skiplist/` passed. On AMD the same unit tests and the smoke replay passed on
a build of the public commit.

**Figures.** Every figure is a whole-workload time as the benchmark reports it. Where the archived harness of a
machine reports less, the comparison uses what both arms report:

| load | Intel | AMD |
|---|---|---|
| conflictset, 2,000 batches, 1,000,000 transactions | measured batch time | measured batch time |
| macBenchmark, 3,000 batches | all timed phases: add, sort, read, write, combine | read + write + combine |
| standalone, 3,000 batches, 1 GiB | preparation + read + write, the same phases for every tree | not the same work in both arms (see below) |
| skiplisttest, 500 batches of 40,000 ranges, 16 GiB | add + detect | detect only |
| by5, 172,800 batches, 5,400 s of trace, 6 GiB | sum of the per-batch totals | sum of the per-batch totals |

Lower is better. Each ratio is v3.8's median over the reference's median; the evidence tables add the minimum and
maximum over every pair of runs. **Phase times are diagnostics.** v3.8 does part of the work of writes during the read
pass, so its read phase is longer and its write phase much shorter than v2's; a phase ratio is not a gain of the whole
and is not quoted as one.

### Dependent-memory latency

A dependent-pointer traversal characterizes the serial data-access latency that recursive partitioning removes and
cooperative scheduling attempts to overlap. The same byte-identical binary was run on both machines: one dependent
pointer per 64-byte cache line, randomized into one chain, ordinary 4 KiB pages, no software prefetch and five timed
repetitions per working-set size. Because each loaded pointer determines the address of the next load, the experiment
exposes no memory-level parallelism between iterations.

| Working set | Intel median, ns/access | AMD median, ns/access | AMD / Intel |
|---|---:|---:|---:|
| 26.9 KiB | 0.704 | 0.753 | 1.07 |
| 45.3 KiB | 0.707 | 2.739 | 3.87 |
| 1 MiB | 3.637 | 6.282 | 1.73 |
| 16 MiB | 19.210 | 11.985 | 0.62 |
| 512 MiB | 163.232 | 103.828 | 0.64 |

The 26.9 KiB working set fits in both L1 data caches and gives similar latency. At 45.3 KiB, Intel remains on the
plateau of its 48 KiB L1, while AMD has exceeded its 32 KiB L1. At 1 MiB, the working set fits within Intel's 3 MiB
private L2 but lies at the capacity boundary of AMD's 1 MiB L2. The ordering reverses at larger sizes: AMD has the lower
dependent-access latency at 16 MiB and 512 MiB. From the common L1-resident point to 512 MiB, latency grows by 231.9× on
Intel and 137.9× on AMD.

AMD is therefore not a uniformly slower memory system. Its smaller private caches make the medium working-set region
more expensive, while its last-level-cache and large-working-set latency are lower in this experiment. This difference
is consistent with the Resolver's cross-machine behaviour. Cooperative scheduling has a fixed cost in task state,
dispatch, branches and instructions, and increases the set of leaves, keys and task records that remain live between a
prefetch and its use. Intel provides more private-cache capacity for that active set and has substantially more
far-memory latency for recursive partitioning and cooperative prefetching to remove or hide. AMD provides less
private-cache headroom, but a miss that reaches the shared cache or DRAM is cheaper, leaving less latency against which
to amortize the scheduler's fixed work.

That combination provides a plausible architectural explanation for the measured pattern: v3.8 obtains its clearest
gains on Intel, while on AMD it is comparable on the long trace and most range and key-layout workloads but slower in
the integrated point workload. It does not by itself explain the `conflictset` range result. That workload commits only
16.5% of its transactions, so FoundationDB's early termination after a conflict remains an independent algorithmic
advantage.

The pointer chase is a diagnostic, not a causal attribution. It permits only one outstanding dependent access and uses
ordinary 4 KiB pages, whereas v3.8 interleaves several tasks and maps its tree pool with HugeTLB. A complete attribution
would require paired Resolver and FoundationDB measurements of cache residency, load waits, prefetch timeliness, branch
behaviour and CPU-frontend starvation in the same sessions.

The AMD 1 MiB result is less stable across process launches than the other selected points. The preselected primary
process measured 6.282 ns/access; the medians of five complete processes ranged from 5.493 to 6.422 ns/access.
Physical-page placement is consistent with that variation but was not controlled or recorded, so its cause is not
established. The table reports the preselected primary process. Intel has only one recorded process, so its
corresponding between-process variation is unknown.

Evidence: [Intel dependent-pointer campaign](../v2/evaluation/intel/memlat/README.md) and [AMD dependent-pointer
campaign](evaluation/amd/memlat/2026-11-07-cpu7/REPORT.md).

## Intel Core Ultra 9 285K

| load | v3.8, s | FDB, s | v1, s | v2, s | v3.8 / FDB | v3.8 / v1 | v3.8 / v2 |
|---|---:|---:|---:|---:|---:|---:|---:|
| conflictset points | 3.931 | 4.942 | 8.173 | 5.548 | 0.795 | 0.481 | 0.708 |
| conflictset ranges | 2.626 | 2.381 | 2.315 | 2.429 | **1.103** | **1.134** | **1.081** |
| macBenchmark points | 8.951 | 8.934 | 20.990 | 11.555 | **1.002** | 0.426 | 0.775 |
| macBenchmark ranges | 6.687 | 7.121 | 10.201 | 7.397 | 0.939 | 0.656 | 0.904 |
| standalone points | 8.871 | – | 14.076 | 10.719 | – | 0.630 | 0.828 |
| standalone ranges | 6.572 | – | 6.610 | 7.387 | – | 0.994 | 0.890 |
| skiplisttest head16 | 14.791 | 19.496 | 34.358 | 21.046 | 0.759 | 0.430 | 0.703 |
| skiplisttest head32 | 15.994 | 19.089 | 35.990 | 22.886 | 0.838 | 0.444 | 0.699 |
| by5, whole segment | 946.44 | 1,240.68 | 3,375.50 | 2,289.52 | 0.763 | 0.280 | 0.413 |

Bold ratios are above 1: v3.8's median is slower. Whether a ratio is an established difference depends on the session
margin, ±10% on this machine.

- **Against v2:** faster outside the session margin in conflictset points, macBenchmark points, standalone points and
  ranges, skiplisttest and by5; comparable in macBenchmark ranges; slower in conflictset ranges (1.08, within the
  margin but in every pair of runs).
- **Against FDB:** faster in conflictset points (0.80), skiplisttest (0.76 and 0.84) and by5 (0.76); comparable in
  macBenchmark (1.00 and 0.94); slower in conflictset ranges (1.10).
- **Against v1:** faster everywhere except standalone ranges (comparable) and conflictset ranges (slower, 1.13).
- skiplisttest has no reference runs for its other five key layouts on this machine; v3.8's runs of them are in the
  evidence and are not compared.

## AMD Ryzen 7 7700

| load | v3.8, s | FDB, s | v1, s | v2, s | v3.8 / FDB | v3.8 / v1 | v3.8 / v2 |
|---|---:|---:|---:|---:|---:|---:|---:|
| conflictset points | 4.368 | 4.190 | 7.310 | 5.445 | **1.043** | 0.598 | 0.802 |
| conflictset ranges | 2.599 | 2.026 | 1.939 | 2.415 | **1.283** | **1.340** | **1.076** |
| macBenchmark points | 8.792 | 6.746 | 16.867 | 10.421 | **1.303** | 0.521 | 0.844 |
| macBenchmark ranges | 5.056 | 5.141 | 7.677 | 5.470 | 0.983 | 0.659 | 0.924 |
| skiplisttest head16 | 15.670 | 16.215 | 28.025 | 19.438 | 0.966 | 0.559 | 0.806 |
| skiplisttest head32 | 16.405 | 16.531 | 29.526 | 20.800 | 0.992 | 0.556 | 0.789 |
| skiplisttest tail32 | 17.803 | 17.841 | 31.020 | 22.177 | 0.998 | 0.574 | 0.803 |
| skiplisttest head64 | 17.158 | 17.865 | 31.728 | 22.221 | 0.960 | 0.541 | 0.772 |
| skiplisttest tail64 | 22.112 | 23.466 | 37.284 | 27.478 | 0.942 | 0.593 | 0.805 |
| skiplisttest head128 | 19.244 | 19.915 | 35.543 | 26.617 | 0.966 | 0.541 | 0.723 |
| skiplisttest tail128 | 35.199 | 36.484 | 52.155 | 41.963 | 0.965 | 0.675 | 0.839 |
| by5, whole segment | 1,003.146 | 1,043.874 | 2,078.613 | – | 0.961 | 0.483 | – |

Bold ratios are above 1: v3.8's median is slower. Whether a ratio is an established difference depends on the session
margin, ±7% on this machine.

- **Against v2:** faster outside the session margin in conflictset points, macBenchmark and every skiplisttest layout;
  slower in conflictset ranges (1.08). v2 has no by5 timing runs on this machine.
- **Against FDB:** slower in conflictset ranges (1.28) and macBenchmark points (1.30); comparable in conflictset
  points (1.04), macBenchmark ranges, every skiplisttest layout (0.94–1.00) and by5 (0.96).
- **Against v1:** faster everywhere except conflictset ranges (slower, 1.34).

The dependent-memory characterization above helps interpret, but does not independently prove, this architectural split.
Intel combines larger private caches with a larger far-memory penalty, giving the cooperative traversal both more cache
headroom and more latency to hide. AMD's smaller private caches and lower large-working-set latency reduce both
quantities. The unusually abort-heavy `conflictset` range workload additionally favours FoundationDB's early termination
path.

**The standalone benchmark is not compared on this machine.** The archived v1 and v2 runs used each version's own
benchmark, which times only the read check and the write merge. v3.8's benchmark also times the preparation of the
batch: building, sorting and indexing the endpoints of reads and writes. Measured with read + write only, v3.8 is 0.94
(points) and 0.98 (ranges) of v2; with its preparation included, 1.13 and 1.34. The first leaves untimed v3.8's sort of
the write endpoints; the second times work the references do not. Neither compares the same work, and no conclusion is
drawn from them. The Intel standalone rows compare the same work.

## Long trace: equal memory

The trace is `pilot-2000M-l0-7200.trace`, segment `[57,600, 230,400)`: 172,800 batches and 5,400 s of trace time from a
cold start, with a 6 GiB counted-history budget. The trees use pools of 98,300 pages (6,442,188,800 bytes); FDB uses
the largest window that fits, 265 s.

| | Intel | AMD |
|---|---|---|
| v3.8 first discard | batch 22,492, 702.91 s | batch 22,492, 702.91 s |
| v3.8 minimum retention after it | 582.938 s | 582.938 s |
| the same as v1 (and v2 on Intel) | in every 600 s window | first discard, minimum and steady-state median (592.78 s) |
| FDB | 265 s window | 265 s window |
| v3.8 peak RSS, pool excluded | 0.56 GiB | 564 MiB |

v3.8's pool is on HugeTLB and outside RSS; it is the same 6,442,188,800 bytes as v1's. FDB's counted history peaked at
6,354,541,376 bytes. On Intel, v3.8's retention is still falling at the end of the segment, by 3.9 s per 600 s, as for v1
and v2: the trace does not establish a stationary retention limit.

The final minimum is 2.20× FDB's window, at a native time of 0.76× FDB on Intel and comparable to FDB on AMD (0.96).
Against v1 the time is 0.28× (Intel) and 0.48× (AMD), and against v2 0.41× (Intel), for the same retained history.

## Write-group configuration

v3.8's write scheduler takes the number of write groups and the tasks per group at build time. Both machines swept
2×8, 2×10, 2×12, 3×12 and 3×20, with v3.8's code unchanged, in the standalone benchmark: one warm-up, then three rounds
in rotated order, ratios within each round. A variant qualified only with a whole-batch gain larger than the spread
between rounds, repeated in every round, and no regression in the other workload; a tie kept 2×8.

| machine | whole batch, variant / 2×8 (median of rounds) | spread of rounds | result |
|---|---|---|---|
| Intel | points 0.994–0.999; ranges 1.000–1.010 | up to 2.5% | none qualifies |
| AMD | points 1.001–1.004; ranges 0.997–1.005 | 0.33% (points), 0.45% (ranges) | none qualifies |

On Intel larger groups shorten the write phase in points by up to 6.0% (3×20), but the read phase and the preparation
grow, and the whole batch changes by 0.1–0.6%, within the spread. On AMD every whole-batch ratio is within ±0.5% of 1.
**Two groups of eight tasks stay the default on both machines**, and one configuration serves both.

## Limits of the comparisons

- The references are archived: different sessions, ordinary memory and no function alignment, against v3.8 with
  HugeTLB and alignment. The effect of HugeTLB and alignment on v3.8 is not separated from the effect of the code.
- Ratios within the session margin are not established, whichever side of 1 they fall.
- All timings are of the resolver's batch path and harness on one core, not of cluster throughput.
- The short benchmarks do not exercise reclamation; the long trace and a 300-page validation run do.
- On AMD the standalone benchmark does not compare the same work, and v2 has no long-trace timing.
- On Intel five of the seven skiplisttest layouts have no reference.
- The sweep did not reach the integrated benchmarks; no variant qualified for them.

## Evidence

The [evidence index](evaluation/README.md) lists every file, how the tables were checked and how to regenerate them.
Each machine has its own notes: [Intel](evaluation/intel/README.md) and [AMD](evaluation/amd/README.md), with every run,
the bracket of each ratio over all pairs of runs ([Intel tables](evaluation/intel/tables.md),
[AMD tables](evaluation/amd/tables.md)), the phase diagnostics, memory, binary hashes and the sweeps.
