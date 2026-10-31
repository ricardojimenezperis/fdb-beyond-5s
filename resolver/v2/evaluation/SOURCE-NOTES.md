# Evaluation source notes

## Intel records and reconstruction

The Intel records, figures and scripts are preserved from the updated package
`2026-10-30-fig-v2-eval-hp.zip`. All 471 entries in its `SHA256SUMS` were
verified before assembly. The two compressed release assets retain their
independent checksums in `evaluation/hp/ASSETS.sha256`.

The new package changes the provenance manifest, commit mapping and supporting
notes, and adds reconstruction sources. The existing measurement CSVs, raw
records and figure files are unchanged from the preceding package. The evaluation
therefore keeps its existing numerical results and plots.

The evaluation recomputes absolute medians and paired ratios from the
short-benchmark CSVs. Only `kind=timed` standalone rows enter those calculations.
Original records, including warm-ups and the earlier standalone baseline, remain
in their original campaign structure.

`hp/MANIFEST.md` now gives measured commits, binary hashes, public component
equivalents and reconstruction instructions separately. The public v1 equivalent
covers only `resolver1`; the public FDB harness reference is not a measured binary
source identity. The final public v2 counterpart remains pending.

Seven patches under `hp/reconstruct/patches/` reconstruct the measured source
states from public commit `990d78d9831266ac082adb8b6d61d30a9a910189`.
The campaign owner reports checking every path, mode and blob outside `design/`
after applying each chain. That chain-to-tree comparison has not been repeated
while assembling this bundle.

The four long-trace build-source manifests were checked locally against the
included copies: all 82 source hashes match. Their `fdbserver` entries identify
the measured executables; those executable bytes are not included. The previous
standalone baseline also has preserved sources under `reconstruct/build-sources/`.
The manifests distinguish checks performed by the campaign owner from checks
reported by the development machine. Reconstructed source identity is not a
claim that a later build will reproduce an identical executable.

## AMD and Apple M3

The AMD and Apple M3 tables transcribe the campaign summaries supplied by their
owner. Their raw records and figure package were not attached to this evaluation
bundle, so those tables have not been independently recomputed here.

The AMD summary identifies the integrated benchmark and its three-round paired
time ratios. It records fixed candidate order, no warm-up, no CPU occupancy log,
and a total defined as the sum of timed phases.

The Apple M3 summary supplies three-round paired throughput ratios but does not
identify the standalone baseline commit. Its results are therefore labeled as a
comparison against the reported baseline, not assumed to use the Intel fork-v1
reference. The controlled AMD campaigns requested later have not supplied results
for this edition.

## Publication, 31 October 2026

`hp/` is published as received: its records, CSVs, manifests, script and `SHA256SUMS` are unchanged, and all 471 checksums verify. Historical names of development candidates in the campaign files document where the records come from; the evaluation's tables, figures and conclusions compare only FDB, v1 and v2. The two compressed per-batch datasets are assets of the release [`resolver-v2-evaluation`](https://github.com/ricardojimenezperis/fdb-beyond-5s/releases/tag/resolver-v2-evaluation), as the package intended; downloaded into `hp/data/`, they verify against `hp/ASSETS.sha256`. They were briefly committed in `hp/data/` and remain in the repository history.

The copies of the Intel figures in `../figs/` carry an embedded content-credential manifest added when they were first published. Apart from that metadata they match `hp/figures/`.

The public counterpart of the measured v2, left pending above and in `hp/MANIFEST.md`, is recorded in [PUBLIC-CORRESPONDENCE.md](PUBLIC-CORRESPONDENCE.md). That file also notes that the measured `resolver1` test file differs from the development snapshot, while all compiled files match.

The AMD and Apple M3 paragraphs above describe the earlier edition. Their run records, figures and script are now in [`amd-m3/`](amd-m3/NOTE.md), and the evaluation recalculates both campaigns from them. The AMD table matches the earlier summary. The Apple M3 baseline is identified as the standalone v1 at `073fcbb9af`.
