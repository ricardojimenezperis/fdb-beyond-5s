# Resolver v2 · Evidence and validation

[Index](README.md)

Resolver v2 was checked at three levels: component tests against reference conflict checks, targeted tests that reclaim and reuse key blocks, and replay through the FDB integration. These checks exercise different failure modes. Matching benchmark decisions is useful integration evidence, while a small reproducer can establish whether a particular routing or lifetime defect is detected.

## Source identity

The measured component is development commit `dc2802fc2f6d76035e9d97bc621263e8b4f59cec`. Its FDB integration is `22a8ae9fba943b73cf149d21b05dfe6b07c6e013`; `3ba52abba955998757fa939b731fc3d2b8fa25b1` adds the integrated benchmark. Of the 39 files of `resolver1`, 38 match the development snapshot; the test file `tests/tempconflictbtreetests.cpp` corresponds to an earlier development state and is not part of the measured FDB executables. The campaign manifest, which reports 39 matching files, is kept as received; the [public correspondence](evaluation/PUBLIC-CORRESPONDENCE.md#how-the-correspondence-was-checked) gives the correction.

The sequential reference is fork commit `5a07ab1936aaa6c01b5f36191c5446dee0a3d674`. Public commit `c44fd2247be51c26ca48c56dcc0587fb7b54ea04` has the same `resolver1` directory. This correspondence covers the component sources; it does not identify the measured executable or establish that its surrounding integration is identical.

The Intel [campaign manifest](evaluation/hp/MANIFEST.md) records each measured state, binary SHA-256, compiler configuration and execution command. It also provides patch chains from public commit `990d78d9831266ac082adb8b6d61d30a9a910189` for source states whose commits are not published. The campaign owner checked that the seven chains reproduce every path, mode and blob outside `design/`, which contains documentation. Exact compiled sources are preserved for the relinked long-trace builds. All 471 package checksums and the 82 source-file hashes in those build manifests were verified when assembling the evaluation materials. The [public correspondence](evaluation/PUBLIC-CORRESPONDENCE.md) records the published head `491d3f5a6fd02bea43b7b02502e35469afc2a53a`, the write-reservation fix `36dcc1a9d5` and `7c7638a92c`, the last public commit before it, whose `resolver1` sources correspond to the measured component. The AMD campaign of 30–31 October measured other revisions; its [note](evaluation/amd/NOTE.md) and [`public-correspondence.csv`](evaluation/amd/public-correspondence.csv) give their own correspondence with the published history. The [AMD and Apple M3 note](evaluation/amd-m3/NOTE.md) records the commits and binary hashes of those campaigns, and its `SHA256SUMS` covers every file of that package.

## Write reservation after the measurements

The public branch contains one change to the component that none of the measurements include. Before storing a write, the tree reclaims history until the pool can hold the write's range and two exteriors, each a record of two keys, plus two nodes. The measured code always reserved 35 blocks, which with 2 KiB blocks covers keys of up to 11,132 bytes. FDB's client truncates write-conflict keys to 10,001 bytes, or 30,001 bytes for system keys; a write inside a stored range of 30,001-byte keys can need up to 92 blocks. With the pool full, such a write could fail to store a key, and the failed store was dereferenced.

The corrected tree reserves `max(35, 3 * ceil(2L / P) + 2)` blocks, where `L` is the longest key stored so far or in the incoming write and `P` is the payload of a key block. If the pool cannot hold one write even after reclaiming everything, `addWrites` throws `std::length_error` before storing anything. `AddWritesReclaimsRoomForLongKeys` crashes without the change; `AddWritesThrowsWhenThePoolCannotHoldAWrite` checks the exception and that the tree keeps working afterwards.

The reserve does not decrease after a long key is reclaimed. `AddWritesKeepsTheReserveOfALongKeyAfterReclaimingIt` measures the cost of that conservative choice: with 2 KiB blocks in a 384-block pool, one write of 30,001-byte keys followed by 20,000 short writes leaves 7,588 of 20,001 commit versions above the reclamation cutoff, against 9,096 for the short writes alone. The 57 extra blocks are about 15% of that small pool and about 0.01% of the 1 GiB pool of the FDB integration. With keys of 10,001 bytes the reserve stays at 35 and the cutoff is unchanged.

The change keeps the decisions of the benchmark workloads, whose keys never exceed the old reserve: the standalone benchmark produces the same decision hashes with the corrected code, with and without reclamation. Those results remain measurements of `dc2802fc2f`, identified by their own commits and binary hashes; matching decisions do not make them measurements of the corrected code.

## Component and differential tests

The component suite of the public source series has 228 tests: 15 for the page pool, 41 for the key store, 47 for the node store and 125 for the temporal tree. It passes with 1, 2, 4 and 8 KiB blocks, which also checks the `RESOLVER1_BLOCK_SIZE` override. With 2 KiB blocks, gcov reports 99.20% of 1,128 lines executed in the tree's implementation, 100.00% of 826 branches executed and 84.38% taken at least once. These are execution metrics, not a measure of condition coverage.

The grouped-read tests cover half-open boundaries, point and empty ranges, empty keys, duplicate reads, long reads across leaves and key blocks, transaction status and conflict reporting. They also exercise suspended tasks, different internal fanouts, expired entries, trees with every entry expired, empty leaves, searches leftwards past expired leaves and into a previous subtree, and empty-child routing. Comparisons against brute-force scans and `hasConflict` check the resulting conflict decisions rather than only the traversal's internal state. A further test checks multi-interval `addWrites` calls against a reference model through the public read interface.

Each of the eleven commits of the public branch [`resolver-v2`](https://github.com/ricardojimenezperis/foundationdb/tree/resolver-v2), head `491d3f5a6fd02bea43b7b02502e35469afc2a53a`, builds in FDB (Release, Clang 19.1.5, AMD) and passes FDB's conflict-set and skip-list unit tests; see [Integration and replay](#integration-and-replay) for the replay checks.

Earlier differential testing checked 3.6 million reads with G=2, 3 and 16, including duplicate, point and long ranges, several reads per transaction, and reporting of every conflicting read. Two deliberately introduced defects were detected. Clang 19 runs with ASan and UBSan passed the then-current unit suite, differential test and `checkbench`. Those sanitizer runs preceded later additions to the suite, so they do not establish sanitizer coverage of every test in the final public series.

## Reclamation and cursor safety

Advancing `reclaimCV_` while leaving old key bytes allocated does not reproduce a stale-reference failure. The separator defect requires the referenced key block to be reclaimed, returned to the pool and potentially overwritten.

`CheckReadsPastSeparatorsInReusedKeyBlock`, in the component suite, builds trees whose expired subtree is a middle child and, in a second tree, the first child. The separators leading to it point into the oldest key block, which the test reclaims and confirms the pool hands out again. Reads from the expired subtree into live history, inside it, into it from before, elsewhere and newer than every entry must match the reference. Reverting either half of the separator repair, the refresh of an expired separator or the marking of every child when none is live, makes a validity assertion fail.

Separate standalone tests constructed 72 directed trees with G=1, 2, 3, 16, 24 and 32. They reclaimed the old key block, allocated its address again and overwrote its contents before running grouped reads. Repeated reads checked deferred cleanup, and a subsequent write checked that the repaired tree remained usable. Another 900 batches repeatedly wrote and reclaimed history in a small pool. Together these tests checked 107,136 read decisions against a reference model and passed with ASan and UBSan. They used compatible batch-record substitutes, rather than the complete FDB adapter.

The underlying invariants are explained in the [design](03-resolver-v2-design.md). A leaf is compacted before its first saved rank in a batch, and its empty form remains linked until traversal ends. An expired separator is repaired before its key bytes are accessed. A task routed into an empty subtree must survive when its read can extend into live history to the right.

## Integration and replay

Six smoke-replay cases, of 20,000 to 30,000 transactions each, were run on the head of the public branch, write reservation included. With sufficient history, resolver v2 matched FDB's skip list; under memory pressure, it matched the reference replay; neither comparison found a mismatch. The branch also matched the measured v2 binary, built from `dc2802fc2f` before the write reservation, batch by batch, both with room and under pressure. On an earlier state of the branch, a build with G=8 produced the same decisions as G=16.

The preserved Intel records provide additional checks on the measured executables. The integrated custom benchmark and the corrected standalone benchmark produce decision hashes `4787dbf4af9b9ca8` for points and `df42a6e5c89796ea` for ranges; the AMD integrated and Apple M3 standalone runs produce the same hashes. Skiplisttest records a matching decision hash for each configuration. FDB's conflictset benchmark prints accepted-transaction counts instead: 962,526 for points and 164,926 for ranges, with no too-old rejections. Equal counts are weaker evidence than matching transaction decisions and are described accordingly.

The long trace records offered-work hash `11334b1aea22e641` and decision hash `5001114085b550fb`, with no age or memory rejections. Its validation report also records zero mismatches over a 200-second segment with both the full 98,300-page pool and a 300-page pool; reclamation begins at batch 71 in the smaller configuration. The [evaluation](06-evaluation.md) distinguishes these correctness checks from native timing and memory accounting, which use separate builds.

## Interpreting the evidence

Decision hashes summarize the tested workloads; they are not a proof for all inputs. Likewise, line and branch coverage describe execution of instrumented code, not the completeness of its invariants. The targeted reclamation tests and planted defects give more specific evidence about the lifetime and routing failures they reproduce.

The 10,000-read comparison example in the design is a [synthetic comparison model](analysis/README.md), published with its script, its recorded output and their original checksums; rerunning the script reproduces that output byte for byte. It uses a declared geometry and seed to count routing and initial leaf-search comparisons. It neither loads the measured history nor simulates caches, scheduling or scan work. Its counts explain the effect of recursive partitioning; the campaigns establish measured performance.

The validation described here concerns a single-threaded resolver component and its test integration. It does not establish concurrent-access safety, crash recovery, or end-to-end support for transactions older than FDB's ordinary window. The broader transaction-retention protocol is described in the [project architecture](02-project-architecture.md).
