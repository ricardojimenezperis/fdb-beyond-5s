# Resolver v3 evaluation · Evidence

The evidence behind the [evaluation](../06-evaluation.md) of resolver v3.8 on an **Intel Core Ultra 9 285K** and an
**AMD Ryzen 7 7700**: run records, extracted data, manifests, binary and source hashes, decision checks, the scripts
that produce every table, and the write-group sweeps of both machines. Each machine has its own directory; no file
combines them.

The measured commit is `d6bba393b92ca4dcf058a024dfcf890733d695c3`; the published commit `55d5f98974` (tag
[`resolver-v3`](https://github.com/ricardojimenezperis/foundationdb/tree/resolver-v3)) has the same tree,
`433edd77accbc70b47bfeaa4301e6da743139992` ([mapping](../mapping.tsv)). Run records name the measured commit.

## Contents

| path | what it holds |
|---|---|
| [`intel/README.md`](intel/README.md) | Intel notes: arms and sources, conditions, correctness, figure definitions, results, memory, the sweep, changes from the campaign's files |
| [`intel/tables.md`](intel/tables.md) | Intel results with every run and the bracket of each ratio |
| `intel/runs/` | per-run output of v3.8 in every load; by5 runs with per-batch times and the SHA-256 of their full batch logs |
| `intel/references/` | the archived FDB, v1 and v2 runs: short loads of 2 November 2026 (per-run CSV and raw lines), by5 `run.txt` and `out.txt` |
| `intel/analysis/` | `short.json`/`short.txt` (every short-load run), by5 retention, memory and timing (`analysis.*`, `windows-600s.csv`) |
| `intel/data/by5-runs.csv` | by5 whole-segment totals per run, with the source of each |
| `intel/BIN-SHA256SUMS`, `intel/src-v38-resolver.sha256`, `intel/build/` | binaries, measured sources, build lines and manifests |
| `intel/correctness/`, `intel/backing/` | test logs; HugeTLB coverage checks with their decisions |
| `intel/scripts/` | campaign scripts as run, and `tables.py`, `analyze-by5.py`, `make_tables.py` |
| [`intel/sweep/`](intel/sweep/summary.csv) | the Intel write-group sweep: per-run output, `summary.csv`, binaries, coverage checks, scripts |
| [`amd/README.md`](amd/README.md) | AMD notes: arms and sources, conditions, correctness, figure definitions, results, memory, phases, the sweep, changes from the campaign's files |
| [`amd/tables.md`](amd/tables.md) | AMD results with every run, the bracket of each ratio, the phase diagnostics and the sweep |
| `amd/raw/` | per run of v3.8, by5 and the sweep: `run.txt` (core, sibling load, HugeTLB coverage, command), `stdout.txt`, `stderr.txt`, `rusage.txt`; manifests, session logs, reservation records |
| `amd/data/` | one row per run, extracted from `raw/`; archived by5 totals; by5 per-batch times and analysis |
| `amd/BIN-SHA256SUMS`, `amd/correctness/` | binaries; unit tests and smoke replay of the public commit |
| `amd/scripts/` | campaign scripts as run, and `extract.py`, `make_tables.py` |
| `SHA256SUMS` | every file under this directory |

The AMD references are the published data of the resolver v2 evaluation,
[`../../v2/evaluation/amd/data/`](../../v2/evaluation/amd/data/), read in place, and the long-trace runs of the
resolver v1 evaluation (asset `resolver-v1-evidence.tar.xz` of the
[`resolver-v1`](https://github.com/ricardojimenezperis/foundationdb/releases/tag/resolver-v1) release). The Intel
long-trace references for v2 are in [`../../v2/evaluation/hp/`](../../v2/evaluation/hp/MANIFEST.md).

## Regenerating the tables

Python 3 and the standard library only. From this directory:

```sh
sha256sum -c SHA256SUMS
python3 amd/scripts/extract.py                      # amd/data/*.csv from amd/raw/
python3 amd/scripts/make_tables.py > amd/tables.md  # also reads ../../v2/evaluation/amd/data/
python3 intel/scripts/tables.py > /dev/null         # intel/analysis/short.json and short.txt
python3 intel/scripts/make_tables.py > intel/tables.md
python3 intel/sweep/scripts/summary.py intel/sweep > intel/sweep/summary.csv
sha256sum -c SHA256SUMS
```

In a clean copy of the repository with Python 3.12.3, every file these commands write was byte-identical to the
published one, and both checksum passes succeeded. `amd/scripts/make_tables.py` and `intel/scripts/make_tables.py` end
with a `problems:` line that lists every failed check: on AMD a run that did not exit cleanly, a decision that differs
from a reference, or a sweep run without its whole pool in HugeTLB; on Intel a decision that differs from a reference
or a by5 total that the per-batch times do not reproduce. It reads `none` for both.

`intel/analysis/analysis.*` and `windows-600s.csv` come from `intel/scripts/analyze-by5.py`, which needs the full
by5 batch logs; they are not included (see below), so those files are not regenerated here. The by5 totals of v3.8 are
checked from the per-batch times by `intel/scripts/make_tables.py`; those of FDB and v1 are the figures published in
the resolver v1 evaluation, and v2's are in the resolver v2 evaluation.

## What the records are

- Paths of the measuring machines are replaced by variables (`$HOME`, `$CAMPAIGNS`, `$EVAL`, …) and binary paths by
  the binary's name; binaries are identified by SHA-256.
- Campaign scripts are kept as run. `intel/scripts/tables.py` has its two input paths changed to this layout.
- `amd/raw/v38/MANIFEST` omits the hash of one helper script that the campaign did not use.
- Files of an internal control arm that is not part of this evaluation are omitted, and its rows are removed from the
  Intel by5 analysis files and from `analyze-by5.py`. FDB trace files are not included.

## Not included

- The full by5 batch logs, identified by SHA-256 in each run's `batch-logs.sha256` and, for the Intel references, in
  `intel/references/by5/BATCH-LOGS-CANONICAL.txt`. The per-batch times of every v3.8 timed run are included.
- The 7,200 s trace, which the resolver v1 evaluation shows how to regenerate.
- The binaries.
