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
