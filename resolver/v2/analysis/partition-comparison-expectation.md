# Analytical expectation for the partitioning example

The article compares one generated batch with the expected comparison counts
under independent uniform sampling. Both calculations use 10,000 read begins,
children per internal level `[2, 28, 28, 28, 28]`, and 41 intervals per leaf.
The existing `partition-comparison-example.py` and JSON remain unchanged.

The expectation script uses no random samples. For every internal partition it
averages the actual binary-search depths over the binomial population and split
rank distributions. Empty read slices cost no comparisons. The population sum
discards tails outside its 1e-13 and 1-1e-13 quantiles without renormalizing.
The output is a numerical evaluation of an analytical expectation.

At a leaf, independent traversal searches once per read. Grouped traversal
searches once per occupied leaf for that leaf's smallest read begin. Queries in
the sampled model lie immediately after stored begins, so the possible leaf
insertion ranks are 1 through 41. The expectation uses the same convention.

## Reproduce

The reference run uses Python 3.12.14, NumPy 2.3.5 and SciPy 1.17.0.

```sh
python3 -m venv /tmp/resolver-v2-expectation
/tmp/resolver-v2-expectation/bin/pip install -r requirements-expectation.txt
/tmp/resolver-v2-expectation/bin/python partition-comparison-expectation.py > /tmp/partition-comparison-expectation.json
```

Floating-point results may differ in their last digits between platforms. The
rounded counts used in the article are:

| Comparisons | Expected, rounded | Generated batch, counted |
|---|---:|---:|
| Independent internal routing | 204,286 | 204,408 |
| Grouped internal routing | 90,463 | 90,496 |
| Independent routing and initial leaf searches | 258,920 | 259,091 |
| Grouped routing and initial leaf searches | 144,877 | 144,956 |

The analytical expectation is not a timing model. It does not include scans,
predecessor checks, cleanup, scheduling, input preparation or key-byte costs.

## Final trace interval

`longtrace-tail-change.py` is a separate calculation over the existing Intel
release dataset. It reports endpoint changes from trace time 4,800 to 5,400 s,
with percentages relative to the value at the start of that interval.

```sh
python3 longtrace-tail-change.py ../evaluation/hp/data/longrun-per-batch.csv.gz > /tmp/longtrace-tail-change.json
```

It verifies the published SHA-256 of that input asset before calculating:
retention changes by -3.65625 s (-0.62330%), and FDB counted memory changes by
+222,464 bytes (+0.00350113%). These are observed changes, not fitted rates.
