## Whole workload: v3.8 median and ratio to each archived reference

Ratio = v3.8 median / reference median; brackets: minimum and maximum over every pair of a v3.8 run with a reference run.

| load | v3.8 runs (s) | v3.8 median | FDB median | v1 median | v2 median | v3.8 / FDB | v3.8 / v1 | v3.8 / v2 |
|---|---|---|---|---|---|---|---|---|
| conflictset points | 4.363 4.373 4.368 4.378 4.368 | 4.368 | 4.190 | 7.310 | 5.445 | 1.043 [1.033, 1.047] | 0.598 [0.594, 0.600] | 0.802 [0.800, 0.805] |
| conflictset ranges | 2.589 2.590 2.599 2.608 2.606 | 2.599 | 2.026 | 1.939 | 2.415 | 1.283 [1.277, 1.290] | 1.340 [1.327, 1.355] | 1.076 [1.070, 1.089] |
| macBenchmark points | 8.822 8.787 8.784 8.809 8.792 | 8.792 | 6.746 | 16.867 | 10.421 | 1.303 [1.302, 1.308] | 0.521 [0.521, 0.525] | 0.844 [0.840, 0.847] |
| macBenchmark ranges | 5.080 5.038 5.063 5.047 5.056 | 5.056 | 5.141 | 7.677 | 5.470 | 0.983 [0.977, 0.991] | 0.659 [0.655, 0.665] | 0.924 [0.920, 0.931] |
| standalone points, read + write | 8.600 8.619 8.596 8.535 8.618 | 8.600 | – | 12.414 | 9.159 | – | 0.693 [0.687, 0.698] | 0.939 [0.931, 0.942] |
| standalone points, totalS | 10.371 10.393 10.360 10.298 10.374 | 10.371 | – | 12.414 | 9.159 | – | 0.835 [0.829, 0.841] | 1.132 [1.124, 1.136] |
| standalone ranges, read + write | 4.891 4.886 4.892 4.894 4.902 | 4.892 | – | 6.074 | 4.996 | – | 0.805 [0.787, 0.812] | 0.979 [0.978, 0.985] |
| standalone ranges, totalS | 6.681 6.655 6.673 6.662 6.671 | 6.671 | – | 6.074 | 4.996 | – | 1.098 [1.072, 1.107] | 1.335 [1.332, 1.342] |
| skiplisttest head16 | 15.670 15.675 15.653 | 15.670 | 16.215 | 28.025 | 19.438 | 0.966 [0.965, 0.969] | 0.559 [0.557, 0.560] | 0.806 [0.803, 0.807] |
| skiplisttest head32 | 16.520 16.317 16.405 | 16.405 | 16.531 | 29.526 | 20.800 | 0.992 [0.986, 1.002] | 0.556 [0.541, 0.561] | 0.789 [0.782, 0.798] |
| skiplisttest tail32 | 17.803 17.717 17.841 | 17.803 | 17.841 | 31.020 | 22.177 | 0.998 [0.989, 1.001] | 0.574 [0.570, 0.575] | 0.803 [0.795, 0.806] |
| skiplisttest head64 | 17.158 17.365 17.052 | 17.158 | 17.865 | 31.728 | 22.221 | 0.960 [0.949, 0.974] | 0.541 [0.536, 0.550] | 0.772 [0.764, 0.785] |
| skiplisttest tail64 | 22.112 22.031 22.130 | 22.112 | 23.466 | 37.284 | 27.478 | 0.942 [0.938, 0.943] | 0.593 [0.590, 0.595] | 0.805 [0.798, 0.807] |
| skiplisttest head128 | 19.244 18.755 19.284 | 19.244 | 19.915 | 35.543 | 26.617 | 0.966 [0.941, 0.969] | 0.541 [0.522, 0.543] | 0.723 [0.700, 0.732] |
| skiplisttest tail128 | 35.199 36.162 35.117 | 35.199 | 36.484 | 52.155 | 41.963 | 0.965 [0.959, 0.994] | 0.675 [0.661, 0.697] | 0.839 [0.830, 0.865] |
| by5 6 GiB, whole segment | 1002.096 1003.146 1004.810 | 1003.146 | 1043.874 | 2078.613 | – | 0.961 [0.956, 0.965] | 0.483 [0.481, 0.484] | – |

## Diagnostic only: read and write phase ratios (medians)

| load | against | read phase | write phase |
|---|---|---|---|
| conflictset points | FDB | 1.587 | 0.521 |
| conflictset points | v1 | 0.723 | 0.288 |
| conflictset points | v2 | 1.373 | 0.254 |
| conflictset ranges | FDB | 1.462 | 0.592 |
| conflictset ranges | v1 | 1.560 | 0.476 |
| conflictset ranges | v2 | 1.174 | 0.440 |
| macBenchmark points | FDB | 1.633 | 0.543 |
| macBenchmark points | v1 | 0.575 | 0.316 |
| macBenchmark points | v2 | 1.171 | 0.284 |
| macBenchmark ranges | FDB | 1.057 | 0.766 |
| macBenchmark ranges | v1 | 0.694 | 0.549 |
| macBenchmark ranges | v2 | 1.108 | 0.537 |
| standalone points | v1 | 0.745 | 0.490 |
| standalone points | v2 | 1.152 | 0.448 |
| standalone ranges | v1 | 0.847 | 0.660 |
| standalone ranges | v2 | 1.112 | 0.638 |
| skiplisttest head16 | FDB | 1.136 | 0.979 |
| skiplisttest head16 | v1 | 0.575 | 0.446 |
| skiplisttest head16 | v2 | 1.398 | 0.405 |
| skiplisttest head32 | FDB | 1.189 | 1.000 |
| skiplisttest head32 | v1 | 0.581 | 0.428 |
| skiplisttest head32 | v2 | 1.369 | 0.384 |
| skiplisttest tail32 | FDB | 1.201 | 0.999 |
| skiplisttest tail32 | v1 | 0.580 | 0.425 |
| skiplisttest tail32 | v2 | 1.377 | 0.384 |
| skiplisttest head64 | FDB | 1.148 | 0.949 |
| skiplisttest head64 | v1 | 0.550 | 0.423 |
| skiplisttest head64 | v2 | 1.346 | 0.378 |
| skiplisttest tail64 | FDB | 1.090 | 0.923 |
| skiplisttest tail64 | v1 | 0.548 | 0.423 |
| skiplisttest tail64 | v2 | 1.318 | 0.379 |
| skiplisttest head128 | FDB | 1.133 | 0.895 |
| skiplisttest head128 | v1 | 0.529 | 0.414 |
| skiplisttest head128 | v2 | 1.186 | 0.365 |
| skiplisttest tail128 | FDB | 1.121 | 0.838 |
| skiplisttest tail128 | v1 | 0.595 | 0.404 |
| skiplisttest tail128 | v2 | 1.232 | 0.365 |
| by5 6 GiB, whole segment | FDB | 2.267 | 0.432 |
| by5 6 GiB, whole segment | v1 | 0.710 | 0.227 |

## Group sweep, stage 1: totalS (prepS + readS + writeS), ratio to 2x8 in the same round

| workload | config | totalS r1 / r2 / r3 | median | / 2x8 r1 / r2 / r3 | median | all rounds below 1 |
|---|---|---|---|---|---|---|
| points | 2x8 | 10.170 / 10.204 / 10.174 | 10.174 | – | – | – |
| points | 2x10 | 10.205 / 10.158 / 10.215 | 10.205 | 1.0034 / 0.9955 / 1.0040 | 1.0034 | no |
| points | 2x12 | 10.214 / 10.190 / 10.259 | 10.214 | 1.0043 / 0.9986 / 1.0084 | 1.0043 | no |
| points | 3x12 | 10.205 / 10.218 / 10.158 | 10.205 | 1.0034 / 1.0014 / 0.9984 | 1.0014 | no |
| points | 3x20 | 10.207 / 10.194 / 10.192 | 10.194 | 1.0036 / 0.9990 / 1.0018 | 1.0018 | no |
| ranges | 2x8 | 6.656 / 6.630 / 6.626 | 6.630 | – | – | – |
| ranges | 2x10 | 6.613 / 6.631 / 6.605 | 6.613 | 0.9935 / 1.0002 / 0.9968 | 0.9968 | no |
| ranges | 2x12 | 6.688 / 6.670 / 6.651 | 6.670 | 1.0048 / 1.0060 / 1.0038 | 1.0048 | no |
| ranges | 3x12 | 6.651 / 6.654 / 6.619 | 6.651 | 0.9992 / 1.0036 / 0.9989 | 0.9992 | no |
| ranges | 3x20 | 6.601 / 6.638 / 6.620 | 6.620 | 0.9917 / 1.0012 / 0.9991 | 0.9991 | no |

CPU sibling busy, all v3.8 runs: max 4.545%
problems: none
