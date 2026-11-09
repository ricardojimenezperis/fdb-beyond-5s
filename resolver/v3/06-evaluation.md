# Final Resolver v3 · Evaluation

[Index](README.md) · [Complete consolidated report](https://github.com/ricardojimenezperis/foundationdb/blob/resolver-v3/fdbserver/resolver/resolver1/evaluation/REPORT.md) · [Release assets](https://github.com/ricardojimenezperis/foundationdb/releases/tag/resolver-v3-final)

This document presents the final Resolver v3 figures. It replaces the v3.8 timing conclusions previously published here. The underlying v3.8 records remain under [`evaluation/`](evaluation/README.md) as historical evidence.

## Evaluated code and method

The performance binaries were built from `18260afe5f80e00ae2b5cd4952f49522c07cbd0e`; their production code is exactly `f866c98d8585a77280adbf44c72650f16e975f86`. The later `2a68fc9b90670bea84c7a458e85d582c7f4eb816` changes tests only.

Every v3 performance run uses `RESOLVER1_PAGE_MODE=hugetlb`, with the whole history pool verified in HugeTLB pages. FDB, btree-v1 and v2 use their native ordinary-memory implementations. These are comparisons of the published deployment configurations, not experiments isolating page mode.

Short benchmarks use one excluded warm-up per arm and five rotated, paired rounds on each machine. Ratios are the median of the five per-round ratios, followed by their minimum–maximum range. Lower is better. Every timed run produces the published decisions. No resolver time is compared across machines.

## Short benchmarks on AMD Ryzen 7 7700

| load | v3/v2 | v3/FDB | v3/v1 |
|---|---|---|---|
| conflictset points | 0.7990 (0.7929–0.8026) | 1.0515 (1.0446–1.0572) | 0.5961 (0.5948–0.5978) |
| conflictset ranges | 1.0566 (1.0495–1.0657) | 1.2867 (1.2820–1.2906) | 1.3283 (1.2937–1.3533) |
| macBenchmark points | 0.8416 (0.8348–0.8490) | 1.3237 (1.3115–1.3312) | 0.5255 (0.5201–0.5333) |
| macBenchmark ranges | 0.9253 (0.9172–0.9303) | 0.9899 (0.9848–0.9941) | 0.6594 (0.6519–0.6663) |
| standalone points | 0.8643 (0.8536–0.8788) | N/A | not comparable: 0.8275 (0.8196–0.8370) |
| standalone ranges | 0.8673 (0.8563–0.8777) | N/A | not comparable: 1.0874 (1.0781–1.1038) |
| skiplisttest head16 | 0.8076 (0.8062–0.8162) | 0.9707 (0.9682–0.9819) | 0.5582 (0.5534–0.5641) |
| skiplisttest head32 | 0.7957 (0.7899–0.8039) | 1.0055 (0.9993–1.0133) | 0.5551 (0.5512–0.5628) |
| skiplisttest tail32 | 0.8098 (0.8032–0.8158) | 1.0099 (0.9938–1.0126) | 0.5730 (0.5682–0.5748) |
| skiplisttest head64 | 0.7673 (0.7647–0.7758) | 0.9648 (0.9562–0.9723) | 0.5377 (0.5318–0.5410) |
| skiplisttest tail64 | 0.8112 (0.8060–0.8149) | 0.9531 (0.9415–0.9614) | 0.5922 (0.5874–0.5988) |
| skiplisttest head128 | 0.7407 (0.7271–0.7519) | 0.9869 (0.9628–0.9908) | 0.5353 (0.5185–0.5471) |
| skiplisttest tail128 | 0.8317 (0.8115–0.8510) | 0.9750 (0.9712–0.9953) | 0.6673 (0.6638–0.6863) |

Against v2, final v3 takes less time in every load except conflictset ranges, where it takes 5.7% more. Against FDB, skiplisttest lies between 0.95 and 1.01 and macBenchmark ranges is 0.99; v3 takes more time in conflictset points (1.05), conflictset ranges (1.29) and macBenchmark points (1.32). Byte-identical AMD binaries have differed by about ±1%, so ratios inside that band are not attributed to code.

The standalone executable has no FDB arm. Its v3/v1 column is shown but not comparable because v3's `totalS` includes a preparation phase that v1's executable does not have.

## Short benchmarks on Intel Core Ultra 9 285K

| load | v3/v2 | v3/FDB | v3/v1 |
|---|---|---|---|
| conflictset points | 0.7198 (0.7193–0.7208) | 0.8078 (0.8056–0.8103) | 0.4977 (0.4965–0.4982) |
| conflictset ranges | 1.0918 (1.0851–1.0935) | 1.1238 (1.1210–1.1263) | 1.1996 (1.1927–1.2045) |
| macBenchmark points | 0.7372 (0.7354–0.7376) | 0.9989 (0.9905–1.0030) | 0.3786 (0.3781–0.3796) |
| macBenchmark ranges | 0.8849 (0.8774–0.8928) | 0.9154 (0.9078–0.9189) | 0.6019 (0.6001–0.6048) |
| standalone points | 0.8201 (0.8167–0.8209) | N/A | not comparable: 0.6368 (0.6344–0.6378) |
| standalone ranges | 0.8903 (0.8895–0.8908) | N/A | not comparable: 1.0068 (1.0028–1.0100) |
| skiplisttest head16 | 0.6892 (0.6858–0.6916) | 0.7354 (0.7341–0.7374) | 0.4216 (0.4201–0.4217) |
| skiplisttest head32 | 0.6754 (0.6738–0.6765) | 0.8155 (0.8145–0.8177) | 0.4274 (0.4271–0.4285) |
| skiplisttest tail32 | 0.7129 (0.7110–0.7147) | 0.8411 (0.8389–0.8431) | 0.4705 (0.4697–0.4713) |
| skiplisttest head64 | 0.6559 (0.6518–0.6568) | 0.8037 (0.7991–0.8054) | 0.4129 (0.4051–0.4133) |
| skiplisttest tail64 | 0.7582 (0.7559–0.7605) | 0.8708 (0.8701–0.8739) | 0.5270 (0.5256–0.5289) |
| skiplisttest head128 | 0.6426 (0.6404–0.6450) | 0.8255 (0.8244–0.8300) | 0.4033 (0.4016–0.4054) |
| skiplisttest tail128 | 0.8441 (0.8371–0.8482) | 0.9103 (0.9075–0.9151) | 0.6392 (0.6345–0.6426) |

Against v2, final v3 takes less time in every load except conflictset ranges, where it takes 9.2% more. Against FDB, it takes less time in conflictset points (0.81), macBenchmark ranges (0.92) and all seven skiplist layouts (0.74–0.91); macBenchmark points is level at 0.9989; conflictset ranges takes more time at 1.12. This session has no A/A control, so the min–max ranges are not confidence intervals and no sub-1% difference is attributed to the code.

## Long trace and retention

The trace contains 172,800 batches over 5,400 seconds with a 6 GiB history budget. FDB retains a configured 265-second window. Final v3 first advances its boundary at batch 22,492 (about 703 seconds) and retains 582.9–584.7 seconds in the measured tail. Its boundary and retained history are identical batch by batch on Intel and AMD.

On Intel, two paired runs give:

| pair | order | FDB `totalS` | v3 `totalS` | v3/FDB |
|---|---|---:|---:|---:|
| 1 | FDB → v3 | 69.612 s | 53.710 s | 0.7716 |
| 2 | v3 → FDB | 69.389 s | 53.646 s | 0.7731 |

The median is **0.7723**. The two pairs agree within 0.2%, but two pairs do not justify a ±1% precision claim.

AMD repeats the complete logical and retention result with one measured pair: FDB 60.500 s and v3 57.061 s. A single pair cannot distinguish a reproducible timing difference from run noise, so **no AMD long-trace performance ratio is inferred**. The times remain published as observations, not as a performance conclusion.

## Cost of the stale-write-rank fix

The paired AMD A/B compares `f866c98d` with v3.8 under HugeTLB across 16 AB/BA pairs per block. Every paired median lies between −0.49% and +0.60%. A byte-identical A/A control produces differences of the same size. No regression above 1% is detected, and sub-1% differences are not attributed to the fix.

## Write-task groups

The published configuration remains 2×8. Common HugeTLB campaigns on AMD and Intel compare 2×4 and 2×6 with 2×8, with an A/A arm. No candidate meets the predeclared qualification rule. The complete ratios and intervals are in the [consolidated report](https://github.com/ricardojimenezperis/foundationdb/blob/resolver-v3/fdbserver/resolver/resolver1/evaluation/REPORT.md#36-write-task-groups-amd-and-intel).

## Scope

- Page mode is not isolated from the data structure; the measurements compare published deployment configurations.
- The cause of the conflictset-ranges ratios was not profiled.
- Cold-cache behavior is not measured.
- Correctness, recovery, simulation, sanitizer and mutant results are summarized in [Validation and evidence](05-validation-and-evidence.md) and specified in the consolidated report.
