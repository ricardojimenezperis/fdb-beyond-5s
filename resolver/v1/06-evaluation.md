# Resolver v1 · Evaluation under a 6 GiB history budget

[Documentation index](README.md) · [Evidence register](07-evidence-and-validation.md)

Under the same **6 GiB counted-history budget**, resolver v1 retains substantially more history than FDB's original skip list, at a higher resolution cost. Over the measured workload, v1's minimum retention after its first discard is **582.938 seconds**, compared with FDB's calibrated **265-second window**: **2.200× as much history**. Its native full-trace time is **3,375.50 seconds versus 1,240.68 seconds**, with a reported per-round time ratio of **2.721×**.

Higher execution time was expected at this stage: v1 processes read searches and write insertions individually and does not yet group their traversals or prefetch upcoming nodes and keys to overlap memory accesses, as FDB does. This first version establishes the representation, update algorithms and reclamation policy; grouped processing and prefetching are planned optimizations. Their absence provides context for the timing gap, although the measurements do not isolate how much of that gap they explain.

Both implementations still show slow drift at the end: v1's retention decreases, while FDB's counted history memory increases with its window fixed at 265 seconds. These changes have become small enough to treat the final measured regime as practically stable. Both implementations process the same offered work, produce matching recorded decisions, and reject no reads for age or memory.

## 1. Experiment and configurations

The primary evidence is the equal-memory long-run campaign of 23 October 2026. Short benchmarks without memory pressure are outside this evaluation.

| Parameter | Setting |
|---|---|
| Harness | `by5resolvertest -s 1` |
| Trace | `pilot-2000M-l0-7200.trace`, digest prefix `0c0d3439…` |
| Source-trace batches | `[57,600, 230,400)` |
| Run length | 172,800 batches; 5,400 seconds of trace time |
| Initial state | Cold start |
| Offered rate | 65,536 transactions per second of trace time |
| Key profile | Profile 0, 16–64-byte keys |
| Main-run settings | `BY5_FICT_MODEL=0`, `BY5_VALIDATE=0`; process and split seed 1 |
| History budget | 6,442,450,944 bytes, including counted metadata |
| Native timing | Three sequential, isolated rounds on CPU 6 |
| Logging | Batch logs buffered in memory and written at exit |
| Compiler / kernel | Clang 19.1.5 / `7.0.0-38-generic` |

Trace time describes workload and version progression. Elapsed time measures the execution cost of that workload; the two are not interchangeable.

| Implementation | History configuration |
|---|---|
| FDB | Original skip list, fixed 265-second history window; counted memory is live skip-list storage |
| V1 | Sequential temporal conflict B+ tree, 2 KiB blocks; 98,300 pool pages of 64 KiB, totaling 6,442,188,800 bytes; ordinary memory, allocated and touched at construction; no fixed time window |

V1 uses structure revision `aff127286e68ea7933dc9ef502a7dbd760e1e43d`. The v1 campaign uses a dedicated binary built from integration revision `5a07ab1936aaa6c01b5f36191c5446dee0a3d674`, in which resolver v1 processes every batch independently of the legacy implementation selector.

The run sets `--knob_resolver_use_paged_conflict_set=0` to avoid constructing the unused legacy paged-history pool, and `--knob_paged_resolver_pool_pages=98300` to configure v1's shared pool. The value `0` does not select the B+ tree; the dedicated binary determines the implementation used. FDB's original skip list is measured with a separate binary.

The harness treats v1 as a capacity-limited history: reads below `reclaimCV` count as memory rejections. Accounting and native timing use separate builds. Accounting runs are untimed; native rounds run in isolation, with synchronization and pending writes drained between runs.

## 2. Calibrated FDB baseline

The completed calibration establishes **265 seconds as the largest tested FDB window that fits**:

| FDB window | Outcome |
|---|---|
| 265 seconds | Fits; peak counted history 6,354,541,376 bytes |
| 270 seconds | Exceeds 6 GiB at trace time 2,199 seconds |

The fitting boundary is within `[265,270)` seconds at the tested granularity. This completed calibration supplies the reference window for the comparison.

The campaign reuses the completed FDB accounting reference from the earlier calibration campaign. Its new accounting run was stopped around batch 105,500 after reaching a peak of 6,353,469,440 bytes, consistent with that reference. Native FDB timings below are the new completed rounds, not timings from the stopped accounting run.

## 3. Retention and counted memory

| Metric | FDB | V1 |
|---|---:|---:|
| First discard, trace seconds | 265.00 | 702.91 |
| Minimum retention after first discard, seconds | 265.000 | 582.938 |
| Minimum retention / FDB | 1.000× | **2.200×** |
| Minimum retention increase / FDB | — | **+120.0%** |
| Peak counted history, bytes | 6,354,541,376 | 6,442,188,800 |
| Fraction of the 6 GiB budget | 98.64% | 99.996% |
| Reads rejected for window or memory | 0 | 0 |

The block-level trajectory below comes from the campaign report. FDB's 265 seconds is its configured reference window, not a newly computed empirical median.

V1 retains at least 317.938 seconds more history than FDB's reference over its measured post-discard period. Its pool leaves 262,144 bytes of counted margin. These are harness history-accounting figures, not process RSS or a bound on all transient process allocations.

### Retention evolution

| Trace interval, seconds | FDB window, seconds | V1 minimum, seconds | V1 median, seconds |
|---|---:|---:|---:|
| 600–1,200 | 265 | 627.000 | 647.594 |
| 1,200–1,800 | 265 | 615.219 | 620.531 |
| 1,800–2,400 | 265 | 606.688 | 610.500 |
| 2,400–3,000 | 265 | 600.188 | 603.219 |
| 3,000–3,600 | 265 | 595.062 | 597.500 |
| 3,600–4,200 | 265 | 590.531 | 592.781 |
| 4,200–4,800 | 265 | 586.594 | 588.500 |
| 4,800–5,400 | 265 | 582.938 | 584.719 |

V1's retention declines in every block, with the decline slowing over time. The last two block medians differ by 3.781 seconds, approximately 0.64%, over 600 seconds of trace time. Its minimum occurs in the last seconds of the run.

FDB also continues to drift: its counted history memory grows slowly while its configured retention remains fixed at 265 seconds. The completed accounting reference reports growth of approximately 0.2 MB per 600-second block in the final intervals, about 0.003% of its roughly 6.35 GB of counted history.

**Both are considered practically stable over the final measured interval:** v1 still loses retention slowly, and FDB still consumes additional memory slowly, but the residual changes are small. The reported values describe this operating regime rather than an extrapolated infinite-duration limit.

## 4. Native execution time

Times are medians of three rounds, with min–max in parentheses. Segment batch numbers are relative to the selected run. Ratios are medians of per-round ratios, so they need not equal quotients of the rounded time medians.

| Segment | FDB, seconds (min–max) | V1, seconds (min–max) | V1 / FDB time | V1 speedup relative to FDB |
|---|---:|---:|---:|---:|
| Initial, batches 0–12,899 | 74.79 (74.32–74.86) | 165.57 (164.91–165.90) | 2.212× | 0.452× |
| Late, batches 76,800–172,799; trace 2,400–5,400 s | 707.35 (706.38–707.84) | 1,939.00 (1,912.71–1,941.40) | 2.741× | 0.365× |
| Full trace | 1,240.68 (1,237.19–1,240.72) | 3,375.50 (3,352.11–3,379.03) | **2.721×** | **0.368×** |

Speedup is defined as FDB time divided by v1 time, calculated as the inverse of the reported time ratio. A value below one means that v1 is slower. V1 requires **172.1% more full-trace time** and **174.1% more late-segment time**. The relative gap is larger after the histories have filled and reclamation is active than in the initial segment.

### Read/write time and batch latency

Full-trace medians:

| Metric | FDB | V1 |
|---|---:|---:|
| Write time | 599.46 s | 1,628.10 s |
| Read time | 320.53 s | 1,639.17 s |
| Per-batch p95 | 7.509 ms | 20.821 ms |
| Per-batch p99 | 7.652 ms | 21.438 ms |

The ratios of these reported component medians are approximately **2.72× for writes** and **5.11× for reads**. Reads therefore show the larger relative gap. The measurements identify which component takes more time, but do not establish the underlying cause without a profile. Total time also includes work beyond the read and write subtotals.

The full-trace round spread is below 0.3% for FDB and 0.8% for v1, much smaller than the measured gap. A short progress-watcher task ran on another core at the start of FDB's first round; that round agrees with the other two within 0.3%.

## 5. Validation and equivalent work

All full runs have identical recorded hashes:

| Quantity | Hash |
|---|---|
| Offered work | `11334b1aea22e641` |
| Decisions | `5001114085b550fb` |

No read is rejected for window or memory. Native rounds match the corresponding accounting data in all 23 non-time columns of the batch log, as reported by the campaign.

Before the long runs, a 200-second validation with `BY5_VALIDATE=1` checks the implementations, including v1 with a 300-page pool. The reported validation hashes are offered `11c9f1cfb2e4d0fd` and decisions `6c863fc42fc1a441`, with no rejection. V1 begins reclamation at batch 71 with that small pool, exercising the pressure path before timing.

These checks support equivalence for this workload and configuration. The evidence register describes the prepared-leaf regression, the campaign validation and additional coverage requirements.

## 6. Interpretation

The experiment demonstrates a concrete trade-off under equal counted memory: **v1 retains roughly 2.2 times FDB's calibrated history window while taking roughly 2.7 times as long to resolve the same offered workload**. Both implementations return matching recorded decisions without rejecting reads for age or memory.

Equal memory does not imply equal retained history. V1 searches and maintains a longer history, so this experiment measures the complete retention/time trade-off under a shared budget rather than isolating search cost at equal history size.

The design's retention objective is supported over the measured horizon, while resolution speed remains an optimization target, particularly the read path. Both implementations are treated as practically stable at the end of this experiment, with the small residual trends reported above. The measurements do not explain those trends or establish behavior over indefinitely longer runs, other seeds, budgets or workloads.

## 7. Evidence and reproducibility

Primary source: the campaign report of the equal-memory campaign dated 2026-10-23. FDB memory calibration is inherited from the completed earlier calibration campaign. The native time tables above are the completed same-campaign measurements.

Reported binary SHA-256 prefixes are FDB native `52cf0920…`, FDB accounting `abfcf338…`, v1 native `83777ae6…`, and v1 accounting `6ca0adcd…`. Full hashes and build manifests are in the campaign artifacts; these prefixes alone are not a reproducibility manifest.

The campaign artifacts are kept read-only with `SHA256SUMS`: binaries and build manifests, validation/accounting/native logs, scripts, environment information and the analysis output. The stopped FDB accounting run is explicitly marked.

Reproducing the experiment requires the campaign hardware and environment details, complete source and binary hashes, the input trace and its hash, core affinity, admission and expiry rules, logging mode, and memory-accounting definitions. Raw campaign artifacts are separate from this snapshot.
