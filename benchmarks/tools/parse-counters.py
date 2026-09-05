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


class TraceFormatError(ValueError):
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


def samples(run_dir, event):
    """(instance_id, time, {name: raw field}) for each trace line of the given event type.

    The key an instance is grouped under is (run_dir, ID). Two runs of the same seed produce the
    same instance IDs, so keying on the ID alone silently fuses them into one apparent run; only
    the rotated trace files of a single directory belong together. A line of the requested event
    type without an ID or a Time is an error rather than a default, since '' and 0 merge
    malformed events into one bucket instead of reporting them.
    """
    paths = sorted(glob.glob(os.path.join(run_dir, 'trace*.xml')))
    for path in paths:
        with open(path, errors='ignore') as fh:
            for line in fh:
                if 'Type="%s"' % event not in line:
                    continue
                fields = dict(FIELD.findall(line))
                if 'ID' not in fields:
                    raise TraceFormatError('%s: %s event with no ID' % (path, event))
                if 'Time' not in fields:
                    raise TraceFormatError('%s: %s event with no Time' % (path, event))
                yield (run_dir, fields['ID']), float(fields['Time']), fields


def finals(run_dirs, event, counters, mode='common'):
    """Per instance, the chosen sample's value for each counter.

    'common' takes the last sample carrying every requested counter, so an identity is checked
    on values that were emitted together. 'max' takes each counter's maximum, which is only
    sound for monotonic counters and is rejected unless every requested counter was seen at
    least once -- a counter that never appeared is unknown, not zero.
    """
    by_key = collections.defaultdict(list)
    for run_dir in run_dirs:
        for key, t, fields in samples(run_dir, event):
            by_key[key].append((t, fields))
    out = {}
    for key, rows in by_key.items():
        rows.sort(key=lambda r: r[0])
        if mode == 'common':
            chosen = None
            for t, fields in rows:
                if all(c in fields for c in counters):
                    chosen = (t, fields)
            if chosen is None:
                continue
            out[key] = (chosen[0], {c: counter_value(chosen[1][c]) for c in counters})
        elif mode == 'max':
            acc, last_t = {}, None
            for t, fields in rows:
                for c in counters:
                    if c in fields:
                        v = counter_value(fields[c])
                        # Not acc.get(c, 0): a first value of -1 would be raised to 0, and a
                        # counter never seen would be fabricated as 0.
                        acc[c] = v if c not in acc else max(acc[c], v)
                        last_t = t if last_t is None else max(last_t, t)
            if not acc:
                continue
            missing = [c for c in counters if c not in acc]
            if missing:
                raise TraceFormatError('instance %s/%s never reported %s; absent is not zero'
                                       % (os.path.basename(key[0]), key[1][:16], ','.join(missing)))
            out[key] = (last_t, {c: acc[c] for c in counters})
    return out


def series(run_dirs, event, counters):
    by_key = collections.defaultdict(list)
    for run_dir in run_dirs:
        for key, t, fields in samples(run_dir, event):
            if any(c in fields for c in counters):
                by_key[key].append((t, {c: counter_value(fields[c]) for c in counters if c in fields}))
    for rows in by_key.values():
        rows.sort(key=lambda r: r[0])
    return by_key


def self_test():
    """Fixture: the cases that matter, and the ones that must be rejected."""
    import shutil, tempfile
    assert counter_value('0.999943 0.573471 9249') == 9249, 'third token is the value'
    assert counter_value('0 -1 0') == 0, 'roughness is -1 when nothing was counted'
    assert counter_value('285490843') == 285490843, 'SpecialCounter prints a bare value'
    assert counter_value('0 -1 -1') == -1, 'a negative value is a value, not a floor of zero'
    for bad in ('1 2', '', 'a b c', '1 2 3.5', '1 2 3 4'):
        try:
            counter_value(bad)
        except CounterFormatError:
            pass
        else:
            raise AssertionError('should have been rejected: %r' % bad)
    # A first token that looks like a plausible count is exactly the trap this tool exists for.
    assert counter_value('176 0.5 1595') == 1595

    root = tempfile.mkdtemp(prefix='parse-counters-selftest-')
    try:
        def write(run, lines):
            d = os.path.join(root, run)
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, 'trace.0.0.0.0.0.1.aaaaaa.0.1.xml'), 'w') as fh:
                fh.write('\n'.join(lines) + '\n')
            return d

        ev = ('<Event Severity="10" Time="%s" Type="M" ID="%s" A="%s" B="%s" />')
        # Two runs, same instance ID: they must not be fused.
        a = write('runA', [ev % ('1.0', 'aaaa', '0 -1 10', '0 -1 1'),
                           ev % ('2.0', 'aaaa', '0 -1 20', '0 -1 2')])
        b = write('runB', [ev % ('1.0', 'aaaa', '0 -1 300', '0 -1 3')])
        f = finals([a, b], 'M', ['A', 'B'])
        assert len(f) == 2, 'same ID in two run directories must stay two instances, got %d' % len(f)
        assert sum(v['A'] for _, v in f.values()) == 320, 'runs fused'

        # A negative first value must survive --mode max.
        c = write('runC', [ev % ('1.0', 'bbbb', '0 -1 -1', '0 -1 0'),
                           ev % ('2.0', 'bbbb', '0 -1 -1', '0 -1 0')])
        assert list(finals([c], 'M', ['A'], mode='max').values())[0][1]['A'] == -1, '-1 raised to 0'

        # A counter that never appears is unknown, not zero.
        try:
            finals([c], 'M', ['A', 'Zzz'], mode='max')
        except TraceFormatError:
            pass
        else:
            raise AssertionError('a never-reported counter must not be fabricated as zero')

        # A malformed event must fail rather than merge under '' / 0.
        d = write('runD', ['<Event Severity="10" Time="1.0" Type="M" A="0 -1 5" />'])
        try:
            finals([d], 'M', ['A'])
        except TraceFormatError:
            pass
        else:
            raise AssertionError('an event with no ID must be rejected')
    finally:
        shutil.rmtree(root, ignore_errors=True)
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
    runs = [d for d in a.trace_dir if glob.glob(os.path.join(d, 'trace*.xml'))]
    if not runs:
        print('no trace files', file=sys.stderr)
        return 2
    if a.series:
        for key, rows in sorted(series(runs, a.event, a.counter).items()):
            print('--- %s/%s ---' % (os.path.basename(key[0]), key[1][:16]))
            for t, vals in rows:
                print('  t=%-12.3f %s' % (t, ' '.join('%s=%d' % (k, v) for k, v in sorted(vals.items()))))
        return 0
    f = finals(runs, a.event, a.counter, a.mode)
    total = collections.Counter()
    print('%-18s %-12s %s' % ('instance', 'time', '  '.join(a.counter)))
    for key, (t, vals) in sorted(f.items(), key=lambda kv: kv[1][0]):
        print('%-18s %-12.3f %s' % (key[1][:16], t, '  '.join(str(vals[c]) for c in a.counter)))
        for c in a.counter:
            total[c] += vals[c]
    print('%-18s %-12s %s' % ('SUM (%d inst)' % len(f), '', '  '.join(str(total[c]) for c in a.counter)))
    if a.identity:
        lhs, rhs = [s.strip().split(',') for s in a.identity.split('==')]
        lhs = [s.strip() for s in lhs]
        rhs = [s.strip() for s in rhs]
        bad = 0
        for key, (t, vals) in sorted(f.items()):
            l, r = sum(vals[c] for c in lhs), sum(vals[c] for c in rhs)
            if l != r:
                bad += 1
                print('  IDENTITY FAILS on %s/%s: %d != %d' % (os.path.basename(key[0]), key[1][:16], l, r))
        sl, sr = sum(total[c] for c in lhs), sum(total[c] for c in rhs)
        print('identity %s: %d/%d instances hold; sum %d %s %d'
              % (a.identity, len(f) - bad, len(f), sl, '==' if sl == sr else '!=', sr))
        return 1 if bad else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
