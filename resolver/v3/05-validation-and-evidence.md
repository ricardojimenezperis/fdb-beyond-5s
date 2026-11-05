# Resolver v3 · Validation and evidence

[Index](README.md)

## Source identities

| Role | Commit / tree | Meaning |
|---|---|---|
| Published v3.8 | 55d5f98974b352f3a124439ae86beb0f8b7ca76d | Tag resolver-v3; default 2×8 |
| Originally measured v3.8 | d6bba393b92ca4dcf058a024dfcf890733d695c3 | Same complete tree as the published commit |
| Shared tree | 433edd77accbc70b47bfeaa4301e6da743139992 | Source identity independent of commit-history rewrite |
| v2 public parent | 491d3f5a6fd02bea43b7b02502e35469afc2a53a | Base of curated v3 history |

The candidate bundle was recovered against the FDB base and checked with git fsck --full --strict. The 17 published version trees match their original trees. Four original auxiliary commits were folded into v3.1 without changing that version's final tree. v3.9 and v3.10 have v3.8 as their common parent. The [original-to-published mapping](mapping.tsv) records the corresponding trees; measurements on originals must retain that provenance.

### The resolver-v3 tag and branch

The annotated tag `resolver-v3` points to 55d5f98974 and is not moved. Its tree keeps `fdbserver/resolver/resolver1/README.md` as inherited from v2: that README is titled resolver v2 and describes the btree-v2 integration. The `resolver-v3` branch continues past the tag with a documentation-only commit that updates that README to v3.8; it changes no source, test or build file. The source identity of v3.8 remains commit 55d5f98974 and tree 433edd77accbc70b47bfeaa4301e6da743139992, the tree of the measured commit d6bba393b9.

## Component and integration checks

- On Apple arm64, the published v3.8 source compiled with Apple clang 15. All 228 component tests passed. With 3000 batches, the standalone benchmark returned points conflicts 29209, decisions 4787dbf4af9b9ca8; ranges conflicts 3479753, decisions df42a6e5c89796ea.
- On Linux, fdbserver built from published v3.8 without warnings. The FDB conflictset and skiplist unit tests passed.
- The FDB smoke replays compared the published v3.8, the evaluated v3.8 and FDB reference under roomy and pressure conditions. Twelve cases reported zero mismatches, and the two v3.8 binaries produced identical batch lines.

These checks validate the curated published source and its native integration. They do not establish a performance advantage. The Apple arm64 component run did not include native FDB compilation, sanitizer or coverage tests for this curated commit; the Linux integration run supplied the native build and replay checks above.

On Intel, the campaign of the [evaluation](06-evaluation.md) ran the 228 component tests on the tree-equivalent measured commit, with assertions and with ASan/UBSan, 228 of 228 passing, together with FDB's `/fdbserver/conflictset/` and `/fdbserver/skiplist/` unit tests.

## Performance evidence boundaries

The archived FDB/v1/v2 results and later v3.8 runs cross sessions and use different HugeTLB and code alignment conditions, so small differences cannot be promoted to controlled speedups. Stage-specific read or write ratios are not whole-batch results. A long-trace retention result must be reported separately from timing and with its memory-budget definition.

The phase-gated benchmark, an optional footprint-walker parameter and alignment options used in some later experiments are not all part of the published v3.8 tree. A report should name the measured commit, extra patch and build flags, the public tree-equivalent commit, processor, workload, memory backing and session boundary.

The design documents of this snapshot make no speed claim; the [evaluation](06-evaluation.md) presents comparisons with FDB, v1 and v2 across sessions on Intel and AMD, each with its margin of uncertainty, without turning them into a general claim that v3 is faster.
