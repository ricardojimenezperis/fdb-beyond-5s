## Whole workload: v3.8 median and ratio to each archived reference

Ratio = v3.8 median / reference median; brackets: minimum and maximum over every pair of a v3.8 run with a reference run.

| load | figure | v3.8 runs (s) | v3.8 median | FDB median | v1 median | v2 median | v3.8 / FDB | v3.8 / v1 | v3.8 / v2 |
|---|---|---|---|---|---|---|---|---|---|
| standalone points | totalS | 8.767 8.871 8.878 | 8.871 | – | 14.076 | 10.719 | – | 0.630 [0.622, 0.640] | 0.828 [0.816, 0.831] |
| standalone ranges | totalS | 6.514 6.572 6.576 | 6.572 | – | 6.610 | 7.387 | – | 0.994 [0.974, 1.011] | 0.890 [0.876, 0.900] |
| conflictset points | seconds | 3.931 3.913 3.941 | 3.931 | 4.942 | 8.173 | 5.548 | 0.795 [0.787, 0.800] | 0.481 [0.476, 0.484] | 0.708 [0.703, 0.718] |
| conflictset ranges | seconds | 2.599 2.626 2.636 | 2.626 | 2.381 | 2.315 | 2.429 | 1.103 [1.069, 1.111] | 1.134 [1.100, 1.145] | 1.081 [1.069, 1.087] |
| macBenchmark points | totalS | 8.951 8.913 8.951 | 8.951 | 8.934 | 20.990 | 11.555 | 1.002 [0.996, 1.018] | 0.426 [0.415, 0.432] | 0.775 [0.764, 0.775] |
| macBenchmark ranges | totalS | 6.642 6.700 6.687 | 6.687 | 7.121 | 10.201 | 7.397 | 0.939 [0.932, 0.941] | 0.656 [0.650, 0.675] | 0.904 [0.896, 0.906] |
| skiplisttest head16 | resolveS | 14.812 14.791 14.773 | 14.791 | 19.496 | 34.358 | 21.046 | 0.759 [0.755, 0.761] | 0.430 [0.430, 0.434] | 0.703 [0.702, 0.707] |
| skiplisttest head32 | resolveS | 15.994 15.998 15.884 | 15.994 | 19.089 | 35.990 | 22.886 | 0.838 [0.826, 0.841] | 0.444 [0.431, 0.447] | 0.699 [0.692, 0.702] |
| by5 6 GiB, whole segment | sum of batch totals | 943.780 952.390 946.440 | 946.440 | 1240.680 | 3375.500 | 2289.520 | 0.763 [0.761, 0.770] | 0.280 [0.279, 0.284] | 0.413 [0.411, 0.416] |

v3.8 only, no reference run of the same work: skiplisttest head128 18.874 s; skiplisttest head64 16.878 s; skiplisttest tail128 51.611 s; skiplisttest tail32 18.905 s; skiplisttest tail64 28.144 s
problems: none
