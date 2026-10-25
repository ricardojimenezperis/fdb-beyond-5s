# Resolver v1 · Evidence and validation register

[Documentation index](README.md) · [Evaluation](06-evaluation.md)

## Revision identity

| Item | Identity |
|---|---|
| Resolver campaign baseline | `aff127286e68ea7933dc9ef502a7dbd760e1e43d` |
| FDB integration | `5a07ab1936aaa6c01b5f36191c5446dee0a3d674` |
| Superseded first resolver campaign revision | `1ec5d3d4d8907e6883568a168bfaab5d1fcca044` |
| Equal-memory campaign | 23 October 2026 |
| Published code | Tag [`resolver-v1`](https://github.com/ricardojimenezperis/foundationdb/tree/resolver-v1) of the FoundationDB fork |

## Published code

The code is published in a [FoundationDB fork](https://github.com/ricardojimenezperis/foundationdb), on the branch and tag `resolver-v1`, over upstream FoundationDB `746d04926c6b30b7fb5ba3b4dfa3bfca19f56e9c`:

| Part | Location |
|---|---|
| Resolver v1 source and standalone tests | [`fdbserver/resolver/resolver1/`](https://github.com/ricardojimenezperis/foundationdb/tree/resolver-v1/fdbserver/resolver/resolver1), with its original commit history up to the campaign revision `aff127286e68ea7933dc9ef502a7dbd760e1e43d` |
| FoundationDB integration (btree-v1) | Commit `c44fd2247be51c26ca48c56dcc0587fb7b54ea04` on that branch; the integration revision `5a07ab1936aaa6c01b5f36191c5446dee0a3d674` of the evaluated binary sat on a private base with other conflict histories, which are not part of this publication |
| Harness, trace generator and FDB baseline binary | Commit `1dfa7ffe6a5210c66e36a628552d10a7046649d0`, the parent of the integration |
| Evaluation scripts and reproduction | [`fdbserver/resolver/by5eval/README.md`](https://github.com/ricardojimenezperis/foundationdb/blob/resolver-v1/fdbserver/resolver/by5eval/README.md) |
| Evidence archive | [Release `resolver-v1`](https://github.com/ricardojimenezperis/foundationdb/releases/tag/resolver-v1): `resolver-v1-evidence.tar.xz`, 65,365,216 bytes, SHA-256 `6a88e7002d959be09a1286b2a2e7f07976eaf78b4cc3fe8ca72cdff9ed9318fe`, described in [`EVIDENCE.md`](https://github.com/ricardojimenezperis/foundationdb/blob/resolver-v1/fdbserver/resolver/by5eval/EVIDENCE.md) |

The source code itself is not included in this snapshot; the table below names the files each part of the design describes, as they appear in the resolver campaign revision.

## Source coverage

| Source | Mechanisms documented |
|---|---|
| `tempconflictbtree.h` / `tempconflictbtree.cpp` | Preconditions, descent, sequential reads and writes, exteriors, proactive splits, propagation, deletion and reclamation |
| `bnodestore.h` / `bnodestore.cpp` | Layout, capacities, free slots, ordering, binary search and age list |
| `keystore.h` / `keystore.cpp` | References, point encoding, append, chaining, generation checks and comparisons |
| `pagepool.h` / `pagepool.cpp` | Shared block pool, page allocation and allocated-byte accounting |
| `common.h` | 64 KiB pages, 2 KiB blocks and signed 64-bit versions |
| Resolver source `README.md` | Campaign revision identity and prepared-leaf regression history |

The resolver source README is distinct from this snapshot's index.

## Evaluation evidence

The primary timing and retention reference is the campaign report of 23 October 2026. The FDB memory calibration comes from the completed earlier calibration campaign.

| Claim | Evidence |
|---|---|
| Calibrated FDB window | 265 s fits the 6 GiB budget; 270 s exceeds it at trace time 2,199 s |
| V1 retention | Minimum 582.938 s after first discard; median 584.72 s over the last 600 s of trace time |
| Counted memory | V1 pool: 6,442,188,800 B; FDB peak: 6,354,541,376 B |
| Native full-trace execution | Three isolated rounds; medians 3,375.50 s for v1 and 1,240.68 s for FDB |
| Equivalent offered work | Full-run hash `11334b1aea22e641` |
| Matching decisions | Full-run hash `5001114085b550fb` |
| Admission | No reads rejected for window or memory |
| Accounting/native consistency | Campaign reports equality of all 23 non-time batch-log columns |

These results describe the specified workload, configurations and measured interval. They do not establish equivalence for all inputs or a steady-state limit over arbitrarily long runs. Final retention and memory trends are quantified in the evaluation.

## Regression and integration validation

**Prepared-leaf regression.** The first campaign revision failed the `scattered-points` smoke case with seed 1001 and 100,000 keys after roughly 1,500 batches. The corrective revision preserves the leaf prepared by `findKey` as the insertion destination. `ScatteredPointsRegression` is the named reproducer in the source revision history.

**Pre-campaign differential validation.** The campaign records a 200-second validation with `BY5_VALIDATE=1`, including v1 with a 300-page pool. Reclamation starts at batch 71, exercising memory pressure before timing. Reported hashes are offered `11c9f1cfb2e4d0fd` and decisions `6c863fc42fc1a441`, with no rejection.

**Scope.** The campaign establishes workload-specific differential evidence. Detailed regression execution and sanitizer logs are not included in this snapshot, and no sanitizer pass count is claimed. Native conflict-range reporting and admission boundary behavior require their own adapter-level coverage beyond matching this trace's recorded decisions.

## Additional validation targets

| Concern | Cases to cover |
|---|---|
| Interval semantics | Endpoint equality, empty-range caller handling, adjacent ranges, point encoding and prefix ordering |
| Write overlays | Two exteriors, full replacement, partial overlap, several crossed leaves and equal begin keys |
| Versions | Equal CV/RV, exterior CV preservation and monotone reclamation cutoff |
| Structure | Prepared-leaf regression, root creation/collapse, empty child removal, exact/inexact split cut ranks |
| Metadata | Parent-slot correspondence, split counter transitions, leaf-chain integrity and conservative maximum CVs |
| Key lifetime | Reused blocks, expired descriptors, separator refresh, multi-block records and copied stored endpoints |
| Allocation | Rollback, tiny pools, maximum endpoint lengths and sufficiency of the 35-block reserve |
| Integration | Complete conflict reports, already-conflicting transactions, too-old reads and differential decisions |
| Runtime safety | Revision-specific regression suite under ASan/UBSan with recorded commands and outcomes |

These are coverage requirements, not assertions that the repository lacks the corresponding tests.

## Technical limits

The shared pool accounts for page storage; temporary search and append vectors and allocator overhead are outside that figure. Counted history is not process RSS.

The write path requests a 35-block reserve before modifying the tree. Reserve sufficiency for the supported endpoint lengths and update patterns remains a validation target.

Reads can mutate physical structure through cleanup without adding logical write history. Stored key access assumes no concurrent reclamation. The local pressure cutoff does not implement a distributed minRV protection protocol.

## Campaign artifacts

The campaign artifacts are kept read-only with `SHA256SUMS`: binaries and build manifests, validation/accounting/native logs, environment settings, scripts and the analysis output. The interrupted duplicate FDB accounting run is marked separately; completed FDB accounting evidence supplies the memory reference.

Reported binary SHA-256 prefixes are FDB native `52cf0920…`, FDB accounting `abfcf338…`, v1 native `83777ae6…`, and v1 accounting `6ca0adcd…`. Full binary hashes, the input trace and raw logs remain in the campaign artifacts; they are not included here. This snapshot describes and identifies the experiment but is not a standalone benchmark reproduction bundle.
