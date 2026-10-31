# AMD evaluation package · release assets

The four largest files of this package are distributed as gzip-compressed assets of the release [`resolver-v2-evaluation`](https://github.com/ricardojimenezperis/fdb-beyond-5s/releases/tag/resolver-v2-evaluation) rather than tracked in Git. Everything else is as received, and `SHA256SUMS` still lists the four files by their original paths.

| Release asset | Restores |
|---|---|
| `amd-data-longtrace-amd-6gib-per-batch.csv.gz` | `data/longtrace-amd-6gib-per-batch.csv` |
| `amd-raw-longtrace-acct-v2-batches.csv.gz` | `raw/longtrace/acct-v2/batches.csv` |
| `amd-raw-longtrace-acct-v2-batches.csv.acct.csv.gz` | `raw/longtrace/acct-v2/batches.csv.acct.csv` |
| `amd-raw-longtrace-acct-v2-batches.csv.ages.csv.gz` | `raw/longtrace/acct-v2/batches.csv.ages.csv` |

Run from this directory:

```sh
base=https://github.com/ricardojimenezperis/fdb-beyond-5s/releases/download/resolver-v2-evaluation
for a in amd-data-longtrace-amd-6gib-per-batch.csv amd-raw-longtrace-acct-v2-batches.csv \
         amd-raw-longtrace-acct-v2-batches.csv.acct.csv amd-raw-longtrace-acct-v2-batches.csv.ages.csv; do
	curl -LO "$base/$a.gz"
done
sha256sum -c AMD-ASSETS.sha256
gunzip -c amd-data-longtrace-amd-6gib-per-batch.csv.gz > data/longtrace-amd-6gib-per-batch.csv
gunzip -c amd-raw-longtrace-acct-v2-batches.csv.gz > raw/longtrace/acct-v2/batches.csv
gunzip -c amd-raw-longtrace-acct-v2-batches.csv.acct.csv.gz > raw/longtrace/acct-v2/batches.csv.acct.csv
gunzip -c amd-raw-longtrace-acct-v2-batches.csv.ages.csv.gz > raw/longtrace/acct-v2/batches.csv.ages.csv
sha256sum -c SHA256SUMS
```

`AMD-ASSETS.sha256` checks the compressed downloads; `SHA256SUMS` then checks the restored files together with the rest of the package. The long-trace figures need `data/longtrace-amd-6gib-per-batch.csv`; the other figures and tables use only the files tracked here.
