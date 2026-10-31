### conflictset: median (rounds 1, 2, 3)

| workload | candidate | checkReadS | mergeS | measured batch time (s) | tx/s | committed | tooOld | decisions |
|---|---|---|---|---|---|---|---|---|
| points | FDB | 2.086 (2.086, 2.084, 2.090) | 1.284 (1.284, 1.281, 1.307) | 4.190 (4.190, 4.182, 4.224) | 238676 (238676, 239100, 236761) | 962526 | 0 | 41ccf929e519502f |
| points | btree-v1 | 4.580 (4.604, 4.580, 4.574) | 2.323 (2.332, 2.323, 2.322) | 7.310 (7.347, 7.310, 7.298) | 136795 (136104, 136795, 137031) | 962526 | 0 | 41ccf929e519502f |
| points | resolver v2 | 2.411 (2.409, 2.418, 2.411) | 2.633 (2.628, 2.636, 2.633) | 5.445 (5.440, 5.455, 5.445) | 183655 (183828, 183304, 183655) | 962526 | 0 | 41ccf929e519502f |
| ranges | FDB | 1.469 (1.469, 1.469, 1.466) | 0.159 (0.159, 0.159, 0.158) | 2.026 (2.028, 2.026, 2.022) | 493592 (493046, 493592, 494441) | 164926 | 0 | 224f4d48ebf2cb5d |
| ranges | btree-v1 | 1.377 (1.365, 1.377, 1.384) | 0.197 (0.195, 0.198, 0.197) | 1.939 (1.925, 1.939, 1.951) | 515620 (519416, 515620, 512524) | 164926 | 0 | 224f4d48ebf2cb5d |
| ranges | resolver v2 | 1.829 (1.815, 1.833, 1.829) | 0.213 (0.213, 0.216, 0.213) | 2.415 (2.396, 2.420, 2.415) | 414002 (417432, 413203, 414002) | 164926 | 0 | 224f4d48ebf2cb5d |

### macBenchmark: median (rounds 1, 2, 3); the last time column is the sum of timed phases

| workload | candidate | readS | writeS | combineS | sum of timed phases (s) | conflicts | decisions |
|---|---|---|---|---|---|---|---|
| points | FDB | 4.724 (4.731, 4.714, 4.724) | 1.992 (1.984, 2.000, 1.992) | 0.031 (0.031, 0.031, 0.031) | 6.746 (6.746, 6.745, 6.747) | 29209 | 4787dbf4af9b9ca8 |
| points | btree-v1 | 13.417 (13.418, 13.417, 13.371) | 3.417 (3.422, 3.417, 3.409) | 0.034 (0.034, 0.033, 0.034) | 16.867 (16.874, 16.867, 16.814) | 29209 | 4787dbf4af9b9ca8 |
| points | resolver v2 | 6.589 (6.597, 6.579, 6.589) | 3.812 (3.833, 3.812, 3.794) | 0.030 (0.030, 0.030, 0.030) | 10.421 (10.460, 10.421, 10.413) | 29209 | 4787dbf4af9b9ca8 |
| ranges | FDB | 3.922 (3.932, 3.922, 3.911) | 1.187 (1.193, 1.187, 1.182) | 0.032 (0.032, 0.032, 0.032) | 5.141 (5.157, 5.141, 5.125) | 3479753 | df42a6e5c89796ea |
| ranges | btree-v1 | 5.976 (5.950, 6.002, 5.976) | 1.657 (1.650, 1.657, 1.667) | 0.034 (0.034, 0.034, 0.034) | 7.677 (7.634, 7.693, 7.677) | 3479753 | df42a6e5c89796ea |
| ranges | resolver v2 | 3.744 (3.739, 3.744, 3.744) | 1.694 (1.687, 1.701, 1.694) | 0.032 (0.032, 0.031, 0.032) | 5.470 (5.458, 5.476, 5.470) | 3479753 | df42a6e5c89796ea |

### standalone blocksizebench, 2 KiB blocks: median (rounds 1, 2, 3); totalS is the sum of timed phases

| workload | candidate | readS | writeS | totalS | conflicts | decisions |
|---|---|---|---|---|---|---|
| points | btree-v1 | 9.877 (9.877, 9.882, 9.827) | 2.537 (2.537, 2.538, 2.527) | 12.414 (12.414, 12.420, 12.354) | 29209 | 4787dbf4af9b9ca8 |
| points | resolver v2 | 6.390 (6.392, 6.383, 6.390) | 2.772 (2.772, 2.776, 2.759) | 9.159 (9.164, 9.159, 9.150) | 29209 | 4787dbf4af9b9ca8 |
| ranges | btree-v1 | 4.738 (4.701, 4.738, 4.872) | 1.336 (1.335, 1.336, 1.336) | 6.074 (6.035, 6.074, 6.208) | 3479753 | df42a6e5c89796ea |
| ranges | resolver v2 | 3.608 (3.621, 3.598, 3.608) | 1.382 (1.378, 1.382, 1.388) | 4.996 (4.998, 4.979, 4.996) | 3479753 | df42a6e5c89796ea |

### skiplisttest: median (rounds 1, 2, 3)

| config | candidate | Detect only (s) | checkReadS | mergeS | committed | decisions |
|---|---|---|---|---|---|---|
| head16 | FDB | 16.215 (16.177, 16.215, 16.215) | 8.136 (8.123, 8.136, 8.145) | 4.395 (4.370, 4.399, 4.395) | 7176921 | 30afc613d78499ea |
| head16 | btree-v1 | 28.025 (27.993, 28.025, 28.083) | 16.063 (16.042, 16.063, 16.080) | 9.653 (9.653, 9.646, 9.656) | 7176921 | 30afc613d78499ea |
| head16 | resolver v2 | 19.438 (19.483, 19.438, 19.433) | 6.609 (6.615, 6.609, 6.600) | 10.624 (10.631, 10.596, 10.624) | 7176921 | 30afc613d78499ea |
| head32 | FDB | 16.531 (16.531, 16.556, 16.482) | 8.240 (8.240, 8.258, 8.202) | 4.377 (4.377, 4.380, 4.369) | 7176921 | 30afc613d78499ea |
| head32 | btree-v1 | 29.526 (30.180, 29.526, 29.433) | 16.854 (17.299, 16.854, 16.847) | 10.220 (10.482, 10.220, 10.190) | 7176921 | 30afc613d78499ea |
| head32 | resolver v2 | 20.800 (20.872, 20.800, 20.713) | 7.156 (7.198, 7.123, 7.156) | 11.386 (11.395, 11.386, 11.290) | 7176921 | 30afc613d78499ea |
| tail32 | FDB | 17.841 (17.831, 17.841, 17.912) | 8.225 (8.207, 8.225, 8.315) | 4.378 (4.378, 4.386, 4.372) | 7176921 | 30afc613d78499ea |
| tail32 | btree-v1 | 31.020 (31.002, 31.066, 31.020) | 17.032 (17.041, 17.032, 16.978) | 10.298 (10.269, 10.301, 10.298) | 7176921 | 30afc613d78499ea |
| tail32 | resolver v2 | 22.177 (22.135, 22.177, 22.289) | 7.171 (7.147, 7.171, 7.197) | 11.402 (11.402, 11.381, 11.432) | 7176921 | 30afc613d78499ea |
| head64 | FDB | 17.865 (17.865, 17.977, 17.834) | 8.655 (8.592, 8.770, 8.655) | 4.872 (4.926, 4.870, 4.872) | 7176921 | 30afc613d78499ea |
| head64 | btree-v1 | 31.728 (31.728, 31.834, 31.560) | 18.070 (18.070, 18.109, 17.913) | 10.928 (10.928, 10.957, 10.917) | 7176921 | 30afc613d78499ea |
| head64 | resolver v2 | 22.221 (22.133, 22.221, 22.321) | 7.385 (7.343, 7.385, 7.410) | 12.230 (12.230, 12.226, 12.287) | 7176921 | 30afc613d78499ea |
| tail64 | FDB | 23.466 (23.466, 23.464, 23.495) | 9.439 (9.428, 9.439, 9.520) | 5.162 (5.170, 5.162, 5.162) | 7176921 | 30afc613d78499ea |
| tail64 | btree-v1 | 37.284 (37.284, 37.361, 37.189) | 18.779 (18.779, 18.975, 18.700) | 11.254 (11.269, 11.201, 11.254) | 7176921 | 30afc613d78499ea |
| tail64 | resolver v2 | 27.478 (27.478, 27.599, 27.413) | 7.806 (7.806, 7.829, 7.785) | 12.575 (12.575, 12.659, 12.563) | 7176921 | 30afc613d78499ea |
| head128 | FDB | 19.915 (19.904, 19.932, 19.915) | 9.223 (9.223, 9.252, 9.208) | 5.738 (5.738, 5.768, 5.711) | 7176921 | 30afc613d78499ea |
| head128 | btree-v1 | 35.543 (35.933, 35.505, 35.543) | 19.775 (19.866, 19.629, 19.775) | 12.406 (12.765, 12.406, 12.254) | 7176921 | 30afc613d78499ea |
| head128 | resolver v2 | 26.617 (26.792, 26.328, 26.617) | 8.817 (8.817, 8.679, 8.845) | 14.067 (14.067, 14.000, 14.163) | 7176921 | 30afc613d78499ea |
| tail128 | FDB | 36.484 (36.484, 36.363, 36.624) | 12.057 (12.089, 12.034, 12.057) | 6.276 (6.276, 6.274, 6.278) | 7176921 | 30afc613d78499ea |
| tail128 | btree-v1 | 52.155 (53.101, 52.155, 51.852) | 22.726 (23.358, 22.726, 22.234) | 13.024 (12.945, 13.024, 13.107) | 7176921 | 30afc613d78499ea |
| tail128 | resolver v2 | 41.963 (41.963, 41.820, 42.331) | 10.973 (10.973, 10.964, 11.117) | 14.402 (14.334, 14.402, 14.517) | 7176921 | 30afc613d78499ea |

### time ratios paired by round: median, minimum, maximum of the three rounds

| block group phase ratio | median | min | max |
|---|---|---|---|
| conflictset points Measured batch time btree-v1/FDB | 1.7479 | 1.7278 | 1.7536 |
| conflictset points Measured batch time resolver v2/FDB | 1.2984 | 1.2892 | 1.3044 |
| conflictset points Measured batch time resolver v2/btree-v1 | 0.7461 | 0.7404 | 0.7463 |
| conflictset points Read check btree-v1/FDB | 2.1974 | 2.1885 | 2.2067 |
| conflictset points Read check resolver v2/FDB | 1.1549 | 1.1536 | 1.1600 |
| conflictset points Read check resolver v2/btree-v1 | 0.5271 | 0.5233 | 0.5279 |
| conflictset points Write merge btree-v1/FDB | 1.8133 | 1.7764 | 1.8166 |
| conflictset points Write merge resolver v2/FDB | 2.0478 | 2.0145 | 2.0578 |
| conflictset points Write merge resolver v2/btree-v1 | 1.1340 | 1.1273 | 1.1348 |
| conflictset ranges Measured batch time btree-v1/FDB | 0.9573 | 0.9492 | 0.9647 |
| conflictset ranges Measured batch time resolver v2/FDB | 1.1943 | 1.1811 | 1.1945 |
| conflictset ranges Measured batch time resolver v2/btree-v1 | 1.2443 | 1.2380 | 1.2479 |
| conflictset ranges Read check btree-v1/FDB | 0.9375 | 0.9297 | 0.9441 |
| conflictset ranges Read check resolver v2/FDB | 1.2480 | 1.2360 | 1.2481 |
| conflictset ranges Read check resolver v2/btree-v1 | 1.3294 | 1.3219 | 1.3313 |
| conflictset ranges Write merge btree-v1/FDB | 1.2421 | 1.2303 | 1.2437 |
| conflictset ranges Write merge resolver v2/FDB | 1.3466 | 1.3445 | 1.3585 |
| conflictset ranges Write merge resolver v2/btree-v1 | 1.0928 | 1.0827 | 1.0937 |
| macbench points Read check btree-v1/FDB | 2.8362 | 2.8304 | 2.8462 |
| macbench points Read check resolver v2/FDB | 1.3948 | 1.3944 | 1.3956 |
| macbench points Read check resolver v2/btree-v1 | 0.4917 | 0.4903 | 0.4928 |
| macbench points Sum of timed phases btree-v1/FDB | 2.5007 | 2.4921 | 2.5013 |
| macbench points Sum of timed phases resolver v2/FDB | 1.5450 | 1.5434 | 1.5505 |
| macbench points Sum of timed phases resolver v2/btree-v1 | 0.6193 | 0.6178 | 0.6199 |
| macbench points Write merge btree-v1/FDB | 1.7113 | 1.7085 | 1.7248 |
| macbench points Write merge resolver v2/FDB | 1.9060 | 1.9046 | 1.9320 |
| macbench points Write merge resolver v2/btree-v1 | 1.1156 | 1.1129 | 1.1201 |
| macbench ranges Read check btree-v1/FDB | 1.5280 | 1.5132 | 1.5303 |
| macbench ranges Read check resolver v2/FDB | 0.9546 | 0.9509 | 0.9573 |
| macbench ranges Read check resolver v2/btree-v1 | 0.6265 | 0.6238 | 0.6284 |
| macbench ranges Sum of timed phases btree-v1/FDB | 1.4964 | 1.4803 | 1.4980 |
| macbench ranges Sum of timed phases resolver v2/FDB | 1.0652 | 1.0584 | 1.0673 |
| macbench ranges Sum of timed phases resolver v2/btree-v1 | 0.7125 | 0.7118 | 0.7150 |
| macbench ranges Write merge btree-v1/FDB | 1.3960 | 1.3831 | 1.4103 |
| macbench ranges Write merge resolver v2/FDB | 1.4330 | 1.4141 | 1.4332 |
| macbench ranges Write merge resolver v2/btree-v1 | 1.0224 | 1.0162 | 1.0266 |
| skiplisttest head128 Detect only btree-v1/FDB | 1.7847 | 1.7813 | 1.8053 |
| skiplisttest head128 Detect only resolver v2/FDB | 1.3365 | 1.3209 | 1.3461 |
| skiplisttest head128 Detect only resolver v2/btree-v1 | 0.7456 | 0.7415 | 0.7489 |
| skiplisttest head128 Read check btree-v1/FDB | 2.1475 | 2.1216 | 2.1539 |
| skiplisttest head128 Read check resolver v2/FDB | 0.9560 | 0.9381 | 0.9606 |
| skiplisttest head128 Read check resolver v2/btree-v1 | 0.4438 | 0.4422 | 0.4473 |
| skiplisttest head128 Write merge btree-v1/FDB | 2.1509 | 2.1459 | 2.2247 |
| skiplisttest head128 Write merge resolver v2/FDB | 2.4516 | 2.4272 | 2.4802 |
| skiplisttest head128 Write merge resolver v2/btree-v1 | 1.1285 | 1.1020 | 1.1558 |
| skiplisttest head16 Detect only btree-v1/FDB | 1.7304 | 1.7283 | 1.7319 |
| skiplisttest head16 Detect only resolver v2/FDB | 1.1988 | 1.1985 | 1.2044 |
| skiplisttest head16 Detect only resolver v2/btree-v1 | 0.6936 | 0.6920 | 0.6960 |
| skiplisttest head16 Read check btree-v1/FDB | 1.9743 | 1.9742 | 1.9750 |
| skiplisttest head16 Read check resolver v2/FDB | 0.8122 | 0.8103 | 0.8143 |
| skiplisttest head16 Read check resolver v2/btree-v1 | 0.4114 | 0.4104 | 0.4123 |
| skiplisttest head16 Write merge btree-v1/FDB | 2.1970 | 2.1928 | 2.2091 |
| skiplisttest head16 Write merge resolver v2/FDB | 2.4173 | 2.4089 | 2.4329 |
| skiplisttest head16 Write merge resolver v2/btree-v1 | 1.1003 | 1.0985 | 1.1013 |
| skiplisttest head32 Detect only btree-v1/FDB | 1.7858 | 1.7834 | 1.8257 |
| skiplisttest head32 Detect only resolver v2/FDB | 1.2567 | 1.2563 | 1.2626 |
| skiplisttest head32 Detect only resolver v2/btree-v1 | 0.7037 | 0.6916 | 0.7045 |
| skiplisttest head32 Read check btree-v1/FDB | 2.0539 | 2.0408 | 2.0994 |
| skiplisttest head32 Read check resolver v2/FDB | 0.8725 | 0.8625 | 0.8735 |
| skiplisttest head32 Read check resolver v2/btree-v1 | 0.4226 | 0.4161 | 0.4248 |
| skiplisttest head32 Write merge btree-v1/FDB | 2.3333 | 2.3326 | 2.3950 |
| skiplisttest head32 Write merge resolver v2/FDB | 2.5996 | 2.5842 | 2.6036 |
| skiplisttest head32 Write merge resolver v2/btree-v1 | 1.1079 | 1.0871 | 1.1141 |
| skiplisttest head64 Detect only btree-v1/FDB | 1.7708 | 1.7697 | 1.7760 |
| skiplisttest head64 Detect only resolver v2/FDB | 1.2389 | 1.2361 | 1.2516 |
| skiplisttest head64 Detect only resolver v2/btree-v1 | 0.6980 | 0.6976 | 0.7073 |
| skiplisttest head64 Read check btree-v1/FDB | 2.0696 | 2.0650 | 2.1031 |
| skiplisttest head64 Read check resolver v2/FDB | 0.8546 | 0.8421 | 0.8561 |
| skiplisttest head64 Read check resolver v2/btree-v1 | 0.4078 | 0.4064 | 0.4136 |
| skiplisttest head64 Write merge btree-v1/FDB | 2.2409 | 2.2185 | 2.2497 |
| skiplisttest head64 Write merge resolver v2/FDB | 2.5103 | 2.4828 | 2.5220 |
| skiplisttest head64 Write merge resolver v2/btree-v1 | 1.1192 | 1.1159 | 1.1254 |
| skiplisttest tail128 Detect only btree-v1/FDB | 1.4343 | 1.4158 | 1.4555 |
| skiplisttest tail128 Detect only resolver v2/FDB | 1.1502 | 1.1501 | 1.1558 |
| skiplisttest tail128 Detect only resolver v2/btree-v1 | 0.8018 | 0.7902 | 0.8164 |
| skiplisttest tail128 Read check btree-v1/FDB | 1.8885 | 1.8441 | 1.9322 |
| skiplisttest tail128 Read check resolver v2/FDB | 0.9111 | 0.9077 | 0.9221 |
| skiplisttest tail128 Read check resolver v2/btree-v1 | 0.4825 | 0.4698 | 0.5000 |
| skiplisttest tail128 Write merge btree-v1/FDB | 2.0758 | 2.0629 | 2.0877 |
| skiplisttest tail128 Write merge resolver v2/FDB | 2.2954 | 2.2841 | 2.3122 |
| skiplisttest tail128 Write merge resolver v2/btree-v1 | 1.1072 | 1.1058 | 1.1075 |
| skiplisttest tail32 Detect only btree-v1/FDB | 1.7387 | 1.7318 | 1.7413 |
| skiplisttest tail32 Detect only resolver v2/FDB | 1.2430 | 1.2414 | 1.2444 |
| skiplisttest tail32 Detect only resolver v2/btree-v1 | 0.7140 | 0.7139 | 0.7185 |
| skiplisttest tail32 Read check btree-v1/FDB | 2.0707 | 2.0418 | 2.0765 |
| skiplisttest tail32 Read check resolver v2/FDB | 0.8709 | 0.8655 | 0.8719 |
| skiplisttest tail32 Read check resolver v2/btree-v1 | 0.4211 | 0.4194 | 0.4239 |
| skiplisttest tail32 Write merge btree-v1/FDB | 2.3486 | 2.3457 | 2.3553 |
| skiplisttest tail32 Write merge resolver v2/FDB | 2.6043 | 2.5949 | 2.6148 |
| skiplisttest tail32 Write merge resolver v2/btree-v1 | 1.1101 | 1.1049 | 1.1102 |
| skiplisttest tail64 Detect only btree-v1/FDB | 1.5889 | 1.5828 | 1.5923 |
| skiplisttest tail64 Detect only resolver v2/FDB | 1.1710 | 1.1668 | 1.1762 |
| skiplisttest tail64 Detect only resolver v2/btree-v1 | 0.7371 | 0.7370 | 0.7387 |
| skiplisttest tail64 Read check btree-v1/FDB | 1.9918 | 1.9643 | 2.0102 |
| skiplisttest tail64 Read check resolver v2/FDB | 0.8279 | 0.8178 | 0.8294 |
| skiplisttest tail64 Read check resolver v2/btree-v1 | 0.4157 | 0.4126 | 0.4163 |
| skiplisttest tail64 Write merge btree-v1/FDB | 2.1797 | 2.1699 | 2.1802 |
| skiplisttest tail64 Write merge resolver v2/FDB | 2.4339 | 2.4322 | 2.4524 |
| skiplisttest tail64 Write merge resolver v2/btree-v1 | 1.1164 | 1.1159 | 1.1302 |
| standalone points Read check resolver v2/btree-v1 | 0.6472 | 0.6459 | 0.6502 |
| standalone points Sum of timed phases resolver v2/btree-v1 | 0.7382 | 0.7374 | 0.7407 |
| standalone points Write insertion resolver v2/btree-v1 | 1.0926 | 1.0918 | 1.0938 |
| standalone ranges Read check resolver v2/btree-v1 | 0.7594 | 0.7406 | 0.7703 |
| standalone ranges Sum of timed phases resolver v2/btree-v1 | 0.8197 | 0.8048 | 0.8282 |
| standalone ranges Write insertion resolver v2/btree-v1 | 1.0344 | 1.0322 | 1.0389 |

