#!/usr/bin/env python3
"""Read FoundationDB counters out of simulation trace files, correctly.

Written because a measurement in this project was published from a wrong reading of the
trace format, and the script that produced it was not kept. Both halves of that mistake are
addressed here: the format is decoded explicitly, and this file is versioned next to the
numbers it produces.

The format. `Traceable<ICounter*>::toString` (fdbrpc/include/fdbrpc/Stats.h:72) prints a
counter as three space-separated tokens:

    Name="<rate> <roughness> <value>"

`value` -- the third -- is the cumulative count. `rate` is instantaneous and `roughness` is a
burstiness estimate that is -1 when nothing has been counted. A SpecialCounter has no rate
(`hasRate()` is false) and prints its bare value instead, which is why a one-token field is
accepted and a two-token field is not.

Aggregation. Counters are aggregated **per instance and then summed**. Taking a maximum across
instances pairs a numerator from one process with a denominator from another; that error also
happened in this project. Within an instance the default is the last sample that carries every
requested counter, so an identity is checked on values that were emitted together.
"""
import argparse, collections, glob, os, re, sys

FIELD = re.compile(r'\s([A-Za-z][A-Za-z0-9_]*)="([^"]*)"')
NUM = re.compile(r'^-?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$')


class CounterFormatError(ValueError):
    pass


def counter_value(field):
    """The cumulative value of a counter field. Rejects anything that is not a counter."""
    parts = field.split(' ')
    if len(parts) == 3:
        rate, roughness, value = parts
        for tok in (rate, roughness, value):
            if not NUM.match(tok):
                raise CounterFormatError('not numeric: %r' % field)
        if not re.match(r'^-?\d+$', value):
            raise CounterFormatError('value is not an integer: %r' % field)
        return int(value)
    if len(parts) == 1 and re.match(r'^-?\d+$', parts[0]):
        return int(parts[0])  # SpecialCounter: bare value, no rate
    raise CounterFormatError('expected 3 tokens (rate roughness value) or a bare value: %r' % field)


def samples(paths, event):
    """(id, time, {name: raw field}) for each trace line of the given event type."""
    for path in sorted(paths):
        with open(path, errors='ignore') as fh:
            for line in fh:
                if 'Type="%s"' % event not in line:
                    continue
                fields = dict(FIELD.findall(line))
                yield fields.get('ID', ''), float(fields.get('Time', 0)), fields


def finals(paths, event, counters, mode='common'):
    """Per instance, the chosen sample's value for each counter."""
    by_id = collections.defaultdict(list)
    for iid, t, fields in samples(paths, event):
        by_id[iid].append((t, fields))
    out = {}
    for iid, rows in by_id.items():
        rows.sort(key=lambda r: r[0])
        if mode == 'common':
            chosen = None
            for t, fields in rows:
                if all(c in fields for c in counters):
                    chosen = (t, fields)
            if chosen is None:
                continue
            out[iid] = (chosen[0], {c: counter_value(chosen[1][c]) for c in counters})
        elif mode == 'max':
            acc, last_t = {}, 0
            for t, fields in rows:
                for c in counters:
                    if c in fields:
                        acc[c] = max(acc.get(c, 0), counter_value(fields[c]))
                        last_t = max(last_t, t)
            if acc:
                out[iid] = (last_t, {c: acc.get(c, 0) for c in counters})
    return out


def series(paths, event, counters):
    by_id = collections.defaultdict(list)
    for iid, t, fields in samples(paths, event):
        if any(c in fields for c in counters):
            by_id[iid].append((t, {c: counter_value(fields[c]) for c in counters if c in fields}))
    for rows in by_id.values():
        rows.sort(key=lambda r: r[0])
    return by_id


def self_test():
    """Fixture: the three cases that matter, and the ones that must be rejected."""
    assert counter_value('0.999943 0.573471 9249') == 9249, 'third token is the value'
    assert counter_value('0 -1 0') == 0, 'roughness is -1 when nothing was counted'
    assert counter_value('285490843') == 285490843, 'SpecialCounter prints a bare value'
    for bad in ('1 2', '', 'a b c', '1 2 3.5', '1 2 3 4'):
        try:
            counter_value(bad)
        except CounterFormatError:
            pass
        else:
            raise AssertionError('should have been rejected: %r' % bad)
    # A first token that looks like a plausible count is exactly the trap this tool exists for.
    assert counter_value('176 0.5 1595') == 1595
    print('self-test: OK')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--trace-dir', action='append', default=[])
    p.add_argument('--event', default='ResolverMetrics')
    p.add_argument('--counter', action='append', default=[])
    p.add_argument('--mode', choices=('common', 'max'), default='common')
    p.add_argument('--identity', help='e.g. "A,B == C,D": checked per instance and on the sum')
    p.add_argument('--series', action='store_true', help='print the per-instance time series')
    a = p.parse_args()
    if a.self_test:
        self_test()
        return 0
    paths = [f for d in a.trace_dir for f in glob.glob(os.path.join(d, 'trace*.xml'))]
    if not paths:
        print('no trace files', file=sys.stderr)
        return 2
    if a.series:
        for iid, rows in sorted(series(paths, a.event, a.counter).items()):
            print('--- %s ---' % iid[:16])
            for t, vals in rows:
                print('  t=%-12.3f %s' % (t, ' '.join('%s=%d' % (k, v) for k, v in sorted(vals.items()))))
        return 0
    f = finals(paths, a.event, a.counter, a.mode)
    total = collections.Counter()
    print('%-18s %-12s %s' % ('instance', 'time', '  '.join(a.counter)))
    for iid, (t, vals) in sorted(f.items(), key=lambda kv: kv[1][0]):
        print('%-18s %-12.3f %s' % (iid[:16], t, '  '.join(str(vals[c]) for c in a.counter)))
        for c in a.counter:
            total[c] += vals[c]
    print('%-18s %-12s %s' % ('SUM (%d inst)' % len(f), '', '  '.join(str(total[c]) for c in a.counter)))
    if a.identity:
        lhs, rhs = [s.strip().split(',') for s in a.identity.split('==')]
        lhs = [s.strip() for s in lhs]
        rhs = [s.strip() for s in rhs]
        bad = 0
        for iid, (t, vals) in sorted(f.items()):
            l, r = sum(vals[c] for c in lhs), sum(vals[c] for c in rhs)
            if l != r:
                bad += 1
                print('  IDENTITY FAILS on %s: %d != %d' % (iid[:16], l, r))
        sl, sr = sum(total[c] for c in lhs), sum(total[c] for c in rhs)
        print('identity %s: %d/%d instances hold; sum %d %s %d'
              % (a.identity, len(f) - bad, len(f), sl, '==' if sl == sr else '!=', sr))
        return 1 if bad else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
