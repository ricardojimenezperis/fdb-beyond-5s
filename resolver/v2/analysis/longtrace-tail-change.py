"""Observed changes over the last 600 trace seconds of the Intel records.

Python 3 standard library only. Usage:
    python3 longtrace-tail-change.py /path/to/longrun-per-batch.csv.gz

The input is the existing release asset, checked against its published SHA-256.
No regression or extrapolation is used: changes are endpoint differences.
"""

import csv
import gzip
import hashlib
import json
from pathlib import Path
import sys

INPUT_SHA256 = 'f1effa8256b2e136587706946ebccdd2a9a95f0925f3b8a3ae5a46a15492abe6'
WINDOW_SECONDS = 600.0


def main():
    if len(sys.argv) != 2:
        raise SystemExit('Usage: longtrace-tail-change.py /path/to/longrun-per-batch.csv.gz')
    path = Path(sys.argv[1])
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != INPUT_SHA256:
        raise SystemExit(f'Input checksum mismatch: {digest}')
    with gzip.open(path, 'rt', newline='') as stream:
        rows = list(csv.DictReader(stream))
    last = rows[-1]
    end = float(last['trace_time_s'])
    start = end - WINDOW_SECONDS
    matches = [row for row in rows if float(row['trace_time_s']) == start]
    if len(matches) != 1:
        raise SystemExit('The input must contain exactly one row at the interval start')
    first = matches[0]
    changes = {}
    for label, column in [
        ('btree_v1_retention', 'retention_btree_v1_s'),
        ('resolver_v2_retention', 'retention_resolver_v2_s'),
        ('fdb265_history_memory', 'history_bytes_fdb265'),
    ]:
        before = float(first[column])
        after = float(last[column])
        record = {
            'start': before,
            'end': after,
            'change': after - before,
            'change_percent_of_start': 100 * (after - before) / before,
            'unit': 'bytes' if 'memory' in label else 'seconds',
        }
        if 'memory' in label:
            record['change_decimal_MB'] = (after - before) / 1_000_000
        changes[label] = record
    print(json.dumps({
        'kind': 'observed endpoint changes, not fitted rates',
        'input_asset': path.name,
        'input_sha256': digest,
        'trace_start_s': start,
        'trace_end_s': end,
        'interval_trace_seconds': WINDOW_SECONDS,
        'first_batch': int(first['batch']),
        'last_batch': int(last['batch']),
        'changes': changes,
    }, indent=2))


if __name__ == '__main__':
    main()
