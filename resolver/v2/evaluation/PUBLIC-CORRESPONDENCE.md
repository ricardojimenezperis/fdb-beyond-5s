# Resolver v2 · Public correspondence of the measured sources

This file completes the public-correspondence column that the Intel campaign [manifest](hp/MANIFEST.md) and [`commits.csv`](hp/commits.csv) leave pending for resolver v2. Those files are kept as received, so that their checksums in [`hp/SHA256SUMS`](hp/SHA256SUMS) still hold. As in the manifest, a public counterpart is an equivalent of the **`resolver1` sources only**; it identifies neither a measured executable nor its integration.

## Published commits

| Role | Commit |
|---|---|
| Published head, branch [`resolver-v2`](https://github.com/ricardojimenezperis/foundationdb/tree/resolver-v2) of the FoundationDB fork | `491d3f5a6fd02bea43b7b02502e35469afc2a53a` |
| Write-reservation fix, after every measurement | `36dcc1a9d53e7277a3299672021c49f62a16799e` |
| Last public commit before the fix: the public counterpart of the measured `resolver1` | `7c7638a92ca780eb6d8622b2dc84c1c9d128e7c9` |
| Base of the series, head of branch `resolver-v1` | `a8921c3babbaea00099cc8c4c8608e942e921120` |

The published head is **not** equivalent to the measured component. After `7c7638a92c` it adds the write reservation, which changes `tempconflictbtree.cpp`, `tempconflictbtree.h` and `fdbserver/resolver/BTreeV2.cpp`, together with new component tests and documentation. The measurements remain identified by their own commits and binary hashes, listed in the manifest.

## Completed column

| Measured state | Measured commit | Public commit with equivalent `resolver1` sources |
|---|---|---|
| resolver v2, conflictset and skiplisttest | `22a8ae9fba943b73cf149d21b05dfe6b07c6e013` | `7c7638a92ca780eb6d8622b2dc84c1c9d128e7c9`, with the differences below |
| resolver v2, integrated benchmark | `3ba52abba955998757fa939b731fc3d2b8fa25b1` | the same |
| resolver v2, long trace (accounting, native) | `22a8ae9fba` + harness patches | the same |
| resolver v2, standalone | `resolver1` of `22a8ae9fba` (= `dc2802fc2f6d76035e9d97bc621263e8b4f59cec`) | the same |

## How the correspondence was checked

The measured tree was rebuilt by applying the package's patches `990d78d983..5a07ab1936.patch` and `5a07ab1936..22a8ae9fba.patch` to public tag `resolver-v1` (`990d78d9831266ac082adb8b6d61d30a9a910189`). Its `resolver1` was compared with development commit `dc2802fc2f` and with public `7c7638a92c`.

- **Compiled sources.** Between `dc2802fc2f` and `7c7638a92c`, the compiled `resolver1` sources differ only as follows:
  - `bnodestore.h` and `tempconflictbtree.h` differ only in comments: their tokens are identical once comments are removed;
  - `tempconflictbtree.cpp` differs in comments and in one explicit instantiation, `checkReads<16>` instead of `checkReads<24>`, beside the unchanged `checkReads<32>`;
  - all other compiled sources are identical.

  The measured binaries ran with 16 tasks: `22a8ae9fba` instantiates 8, 16, 24 and 32 tasks and selects one at run time through the `BTREE_V2_TASKS` environment variable, which the campaigns set to 16. The public branch fixes the task count when compiling, 16 by default. The template code that both instantiate is the same.
- **Benchmark source.** `bench/blocksizebench.cpp` differs only in its task count, 32 in `dc2802fc2f` and 16 in `7c7638a92c`. The standalone campaigns ran it with 16 tasks.
- **Tests and documents.** The component tests and documents of `resolver1` differ; they are not compiled into `fdbserver`.
- **One file of the measured tree.** The rebuilt `22a8ae9fba` carries `tests/tempconflictbtreetests.cpp` as blob `08de8857635725be3cca758c92e772bf6a7ba8d5`, the version of development commit `7809eea63f`, while `dc2802fc2f` carries blob `343342fa1e8829488381d559c556edbeed8bb54c`. The other 38 files of `resolver1` are identical. The manifest states that all 39 files match; that holds for every compiled file but not for this test file, which no measured binary compiles. The manifest is left as received.
- **Integration.** The FDB integration is not claimed equal, as for v1. The measured commits also contain other conflict histories that knobs can select. The public branch resolves every batch with btree-v2, with the task count fixed when compiling.

## Behaviour of the public builds

These checks compare public builds with a measured binary. They do not make the public builds measured binaries, and none of these hashes appears in the evaluation.

- **Before the fix.** Every commit of the series up to `7c7638a92c` builds in FDB and passes its conflict-set and skip-list unit tests. Its earlier equivalent `22d9bb7a67` differs from `7c7638a92c` only in a `resolver1` test file. In smoke replay it matched, batch by batch, the AMD build of measured commit `3ba52abba9` (`fdbserver` SHA-256 `621ebcf409a7343d7d058d0742c963e3acc7eafc3eb641f54b38936fbf66acd5`), both with room and under memory pressure.
- **Published head.** The head's sources were built at `9017816c61`, which differs from `491d3f5a6f` only in the `resolver1` README (`fdbserver` SHA-256 `97a613c492cce70563d41b14a8452bbaa9c1aab95d9e2184a17657bbab02a068`). In the same six smoke cases it matched FDB's skip list with room and the reference replay under pressure, with no mismatch. It also matched the same measured binary batch by batch.

The write reservation changes no decision in these replays, nor in the standalone benchmark, whose decision hashes are the same with and without reclamation. The measurements nevertheless remain measurements of `dc2802fc2f`; see the [evidence register](../07-evidence-and-validation.md#write-reservation-after-the-measurements).
