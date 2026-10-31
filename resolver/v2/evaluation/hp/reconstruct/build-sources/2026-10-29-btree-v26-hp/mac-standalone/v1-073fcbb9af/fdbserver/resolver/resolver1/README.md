# resolver1

## Evaluation campaign versions

| Campaign id | Implementation | Commit | Status |
|---|---|---|---|
| `btree-v1` | `TempConflictBTree` (`tempconflictbtree.h`, `tempconflictbtree.cpp`), with `BNodeStore`, `KeyStore` and `PagePool` | `aff127286e68ea7933dc9ef502a7dbd760e1e43d` | Current |
| `btree-v1`, first commit | Same | `1ec5d3d4d8907e6883568a168bfaab5d1fcca044` | Replaced: overflows a leaf (see below) |

`btree-v1` names the tree at its commit for the evaluation campaign, whose
candidates are `fdb`, `v107` and `btree-v1`. It is not a tag, and classes and
files keep their names.

The first commit failed the smoke test against v107: with scattered point
writes over 100,000 keys, after about 1,500 batches, a write whose key fell
past the last entry of a leaf was inserted into the next leaf, which had not
been prepared for it, until that leaf overflowed (seed 1001, case
`scattered-points`). `aff127286e` inserts every write into the leaf `findKey`
prepared; `ScatteredPointsRegression` reproduces the failure.

## Author

Ricardo Jimenez-Peris

- Website: <https://ricardojimenezperis.com>
- GitHub: <https://github.com/ricardojimenezperis>
