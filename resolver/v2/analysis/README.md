# Resolver v2 · Synthetic partition-comparison model

This directory holds a **synthetic comparison model**, not a benchmark. It counts the key comparisons needed to locate 10,000 eligible read begins in a declared tree, once with an independent search per read and once with the recursive partitioning of resolver v2. It does not measure execution time, and the tree is an assumption, not the occupancy observed in any campaign. The measured results are in the [evaluation](../06-evaluation.md); the [design](../03-resolver-v2-design.md#example-10000-reads-and-a-6-gib-history-pool) explains the example.

| File | Content |
|---|---|
| [`partition-comparison-example.py`](partition-comparison-example.py) | The model: tree geometry, seed and counting; Python 3, no dependencies |
| [`partition-comparison-example.json`](partition-comparison-example.json) | Its output: geometry, counts by depth, visited nodes and the costs it excludes |
| [`make_partition_figures.py`](make_partition_figures.py) | Draws `../figs/fig-v2-model-partition-totals` and `../figs/fig-v2-model-partition-by-depth` from the JSON; needs matplotlib |
| `SHA256SUMS` | Checksums of the files above and of the four figure files |

```sh
python3 partition-comparison-example.py > regenerated.json
cmp regenerated.json partition-comparison-example.json
python3 make_partition_figures.py
```

The model and its JSON are published as received, with their original checksums: `03594dc4947dcad7…` and `28d13810eef2fcdd…`. Running the script with Python 3 reproduces the JSON byte for byte, with these totals:

| Case | Independent searches | Recursive partitioning |
|---|---:|---:|
| Uniform reads, internal routing only | 204,408 | 90,496 |
| Uniform reads, including the initial leaf search | 259,091 | 144,956 |
| Clustered reads, including the initial leaf search | 261,795 | 23,643 |

The figures were drawn with Python 3.12.1 and matplotlib 3.11.2; a second run in an empty directory gave byte-identical SVG and PNG files.

## Analytical expectation and long-trace tail

Two later calculations complement the model. Neither is a new performance measurement of resolver v2.

| File | Content |
|---|---|
| [`partition-comparison-expectation.md`](partition-comparison-expectation.md) | Method and reproduction of the analytical expectation |
| [`partition-comparison-expectation.py`](partition-comparison-expectation.py) | Expected comparison counts for the uniform example under independent uniform read begins, without sampling; needs NumPy and SciPy |
| [`partition-comparison-expectation.json`](partition-comparison-expectation.json) | Its output |
| [`requirements-expectation.txt`](requirements-expectation.txt) | Dependency versions of the reference run |
| [`longtrace-tail-change.py`](longtrace-tail-change.py) | Observed change over the last 600 trace seconds of the Intel long trace, from the release asset `longrun-per-batch.csv.gz`; Python 3, no dependencies |
| [`longtrace-tail-change.json`](longtrace-tail-change.json) | Its output |
| `SHA256SUMS-article-addendum` | Checksums of these six files |

The expectation gives 204,286 and 90,463 comparisons for internal routing, and 258,920 and 144,877 including the initial leaf search, against the counted 204,408, 90,496, 259,091 and 144,956 of the generated batch. Floating-point results can differ in their last digits between platforms.

Over trace seconds [4,800, 5,400], the endpoint change in retained history is −3.65625 seconds (−0.6233%) for both trees, and FDB's counted history memory grows by 222,464 bytes (+0.0035%). These are endpoint differences, not fitted rates; the [evaluation](../06-evaluation.md#retention-and-memory) reports a least-squares slope over the last 1,500 seconds.

```sh
python3 longtrace-tail-change.py ../evaluation/hp/data/longrun-per-batch.csv.gz
shasum -a 256 -c SHA256SUMS-article-addendum
```
