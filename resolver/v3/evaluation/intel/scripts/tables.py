import csv, re, statistics as st, json
from pathlib import Path
C = Path(__file__).resolve().parents[1]
A = C / 'references/2026-11-02-short'
out = {}


def kv(s):
    return dict(re.findall(r'(\w+)=([^\s]+)', s))


def med(v):
    return st.median(v)


def ref(path, metric, wl_key='workload', filt=None):
    r = {}
    for row in csv.DictReader(open(path)):
        if row.get('kind') == 'warm' or row.get('kind') == 'warm-up':
            continue
        if filt and not filt(row):
            continue
        r.setdefault((row['arm'] if 'arm' in row else row['version'], row[wl_key]), []).append((int(row['round']), float(row[metric])))
    return {k: [x for _, x in sorted(v)] for k, v in r.items()}


res = {}
# standalone
v = {}
for l in open(C / 'runs/mac-standalone.txt'):
    d = kv(l)
    if d['run'] == 'warm':
        continue
    v.setdefault(d['workload'], []).append((d['run'], float(d['totalS']), float(d['prepS']), float(d['readS']), float(d['writeS']), d['decisions'], int(d.get('maxrss_kib') or 0)))
rs = ref(A / 'standalone/standalone-runs.csv', 'totalS')
res['standalone'] = {w: {'v38': [x[1] for x in v[w]], 'v38_prep': [x[2] for x in v[w]], 'v38_read': [x[3] for x in v[w]], 'v38_write': [x[4] for x in v[w]],
                         'v38_dec': sorted({x[5] for x in v[w]}), 'v38_rss_kib': [x[6] for x in v[w]],
                         'v1': rs[('v1', w)], 'v2': rs[('v2', w)]} for w in ('points', 'ranges')}
# conflictset
v = {}
for d0 in sorted((C / 'runs/conflictset').iterdir()):
    m = re.match(r'p3-conflictset-(r\d)-(\w+)$', d0.name)
    if not m:
        continue
    s = (d0 / 'stdout.txt').read_text()
    b = kv(re.search(r'^ConflictSetBenchmark .*$', s, re.M).group(0))
    t = (d0 / 'time.txt').read_text()
    v.setdefault(m.group(2), []).append((m.group(1), float(b['seconds']), int(b['txPerSec']), b['committed'], b['decisions'], float(b['checkReadS']), float(b['mergeS']),
                                         int(re.search(r'Maximum resident set size \(kbytes\): (\d+)', t).group(1))))
rc = ref(A / 'conflictset/runs.csv', 'seconds', filt=lambda r: r['kind'] == 'timed')
res['conflictset'] = {w: {'v38': [x[1] for x in sorted(v[w])], 'v38_tx': [x[2] for x in sorted(v[w])], 'v38_committed': sorted({x[3] for x in v[w]}),
                          'v38_dec': sorted({x[4] for x in v[w]}), 'v38_read': [x[5] for x in sorted(v[w])], 'v38_merge': [x[6] for x in sorted(v[w])],
                          'v38_rss_kib': [x[7] for x in sorted(v[w])],
                          'fdb': rc[('fdb', w)], 'v1': rc[('v1', w)], 'v2': rc[('v2', w)]} for w in ('points', 'ranges')}
# macbench
v = {}
for d0 in sorted((C / 'runs/macbench').iterdir()):
    m = re.match(r'p3-macbench-(r\d)-(\w+)$', d0.name)
    if not m:
        continue
    s = (d0 / 'stdout.txt').read_text()
    p = kv(re.search(r'^MacBenchmarkPhases .*$', s, re.M).group(0))
    b = kv(re.search(r'^MacBenchmark index.*$', s, re.M).group(0))
    t = (d0 / 'time.txt').read_text()
    v.setdefault(m.group(2), []).append((m.group(1), float(p['totalS']), float(p['readS']), float(p['writeS']), float(p['sortS']), float(p['addS']), b['decisions'], b['conflicts'],
                                         int(re.search(r'Maximum resident set size \(kbytes\): (\d+)', t).group(1))))
rm = ref(A / 'macbenchmark/runs.csv', 'ph_totalS', filt=lambda r: r['kind'] == 'timed')
res['macbench'] = {w: {'v38': [x[1] for x in sorted(v[w])], 'v38_read': [x[2] for x in sorted(v[w])], 'v38_write': [x[3] for x in sorted(v[w])],
                       'v38_sort': [x[4] for x in sorted(v[w])], 'v38_add': [x[5] for x in sorted(v[w])], 'v38_dec': sorted({x[6] for x in v[w]}),
                       'v38_conf': sorted({x[7] for x in v[w]}), 'v38_rss_kib': [x[8] for x in sorted(v[w])],
                       'fdb': rm[('fdb', w)], 'v1': rm[('v1', w)], 'v2': rm[('v2', w)]} for w in ('points', 'ranges')}
# skiplisttest
v = {}
for d0 in sorted((C / 'runs/skiplisttest').iterdir()):
    m = re.match(r'(\w+?\d+)-(warm|r\d)$', d0.name)
    if not m or m.group(2) == 'warm':
        continue
    s = (d0 / 'out.txt').read_text()
    tl = kv(re.search(r'^totals: .*$', s, re.M).group(0))
    dec = re.search(r'decisionsHash=(\w+)', s).group(1)
    t = (d0 / 'time.txt').read_text()
    v.setdefault(m.group(1), []).append((m.group(2), float(tl['resolveS']), float(tl['detectS']), dec, int(re.search(r'Maximum resident set size \(kbytes\): (\d+)', t).group(1)),
                                         (d0 / 'run.txt').read_text().count('exit=0')))
res['skiplisttest'] = {}
for conf in v:
    e = {'v38': [x[1] for x in sorted(v[conf])], 'v38_detect': [x[2] for x in sorted(v[conf])], 'v38_dec': sorted({x[3] for x in v[conf]}), 'v38_rss_kib': [x[4] for x in sorted(v[conf])]}
    p = A / f'skiplisttest/{conf}/runs.csv'
    if p.exists():
        r = ref(p, 'resolveS', wl_key='conf', filt=lambda r: r['kind'] == 'timed')
        decs = {row['decisionsHash'] for row in csv.DictReader(open(p))}
        for a in ('fdb', 'v1', 'v2'):
            e[a] = r[(a, conf)]
        e['ref_dec'] = sorted(decs)
    res['skiplisttest'][conf] = e
(C / 'analysis/short.json').write_text(json.dumps(res, indent=1))
L = []
for load, unit in (('standalone', 'totalS'), ('conflictset', 'seconds'), ('macbench', 'totalS'), ('skiplisttest', 'resolveS')):
    L.append(f'== {load} ({unit}, s; lower is better)')
    for w, e in res[load].items():
        row = f'  {w:8s} v3.8 {med(e["v38"]):.3f} [{" ".join("%.3f" % x for x in e["v38"])}] dec {e.get("v38_dec")}'
        for a in ('fdb', 'v1', 'v2'):
            if a in e:
                row += f' | {a} {med(e[a]):.3f} [{" ".join("%.3f" % x for x in e[a])}] v3.8/{a} {med(e["v38"]) / med(e[a]):.3f}'
        L.append(row)
print('\n'.join(L))
(C / 'analysis/short.txt').write_text('\n'.join(L) + '\n')
