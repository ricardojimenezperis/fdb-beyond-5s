import csv, json, re, statistics, sys
from pathlib import Path

C = Path(sys.argv[1])
OLD = Path('$CAMPAIGNS/2026-10-23-equal-memory-hp')
FDB = Path('$CAMPAIGNS/2026-10-11-fdb-v6-long-retention-hp/wave1/fdb-w265')
C26 = Path('$CAMPAIGNS/2026-10-30-equal-memory-v26-hp')
ARMS = {'fdb265': FDB, 'v1': OLD / 'runs/acct-v1', 'v26': C26 / 'runs/acct-v26', 'v38': C / 'runs/by5/acct-v38'}
TIME = {'add_s', 'sort_s', 'read_s', 'intrabatch_s', 'combine_s', 'write_s', 'cleanup_s', 'total_s', 'cumulative_peak_rss_kib'}
CUTS = {'initial': (0, 12900), 'late': (76800, 172800), 'whole': (0, 172800)}
L = []
P = lambda s='': L.append(s)
out = {}


def rows(p):
    return list(csv.DictReader(open(p)))


def hashes(p):
    s = open(p).read()
    return {k: (re.search(k + r'=(\w+)', s) or [None, None])[1] for k in ('offeredHash', 'decisionsHash')}


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


data = {}
for a, d in ARMS.items():
    rs = rows(d / 'batches.csv')
    ac = rows(d / 'batches.csv.acct.csv')
    t = [int(x['cv']) / 1e6 for x in rs]
    ret = [int(x['retained_us']) / 1e6 for x in rs]
    if a == 'fdb265':
        ret = [min(x, 265.0) for x in t]
    data[a] = dict(rs=rs, t=t, ret=ret, hb=[int(x['history_bytes']) for x in ac],
                   rw=[int(x['rejected_window']) for x in rs], rm=[int(x['rejected_memory']) for x in rs])

P('== Same segment, load and memory contract')
for a, d in ARMS.items():
    rs = data[a]['rs']
    run = (d / 'run.txt').read_text().split('\n')[0]
    P(f'  {a}: {len(rs)} batches, first cv {rs[0]["cv"]}, last cv {rs[-1]["cv"]}, hashes {hashes(d / "out.txt")}')
    P(f'      run: {run[:400]}')
same_offer = len({hashes(d / 'out.txt')['offeredHash'] for d in ARMS.values()}) == 1
same_cv = len({tuple(x['cv'] for x in data[a]['rs']) for a in ARMS}) == 1
P(f'  same offered hash: {same_offer}; same batch cv sequence: {same_cv}')

P()
P('== Rejections and their legitimacy')
for a in ARMS:
    rw, rm = sum(data[a]['rw']), sum(data[a]['rm'])
    P(f'  {a}: rejected for window {rw}, for memory {rm}')
v26 = data['v38']['rs']
bad = [i for i, x in enumerate(v26) if int(x['rejected_memory']) and int(x['capacity_floor_before']) <= 0]
P(f'  v26 memory rejections in batches whose boundary (capacity_floor_before) is 0: {len(bad)} (the harness rejects for memory only reads below the tree boundary at the start of the batch)')
diffcols = [c for c in v26[0] if c in data['v1']['rs'][0] and c not in TIME and c != 'measured']
first_diff = {c: next((i for i, (x, y) in enumerate(zip(data['v1']['rs'], v26)) if x[c] != y[c]), None) for c in ('accepted', 'conflicts', 'rejected_memory')}
P(f'  first batch where v38 differs from v1: {first_diff}')

P()
P('== Retention')
ret_out = {}
for a in ARMS:
    t, ret = data[a]['t'], data[a]['ret']
    first = next(i for i in range(len(ret)) if ret[i] < t[i] - 1e-6)
    m = min(range(first, len(ret)), key=lambda i: ret[i])
    ret_out[a] = dict(first_batch=first, first_t=t[first], minimum=ret[m], minimum_t=t[m])
    P(f'  {a}: first discard batch {first} (t {t[first]:.2f} s); minimum after it {ret[m]:.3f} s at t {t[m]:.1f} s')
out['retention'] = ret_out

P()
P('== 600 s windows: retention min / p5 / median (s), peak history_bytes, rejections (window+memory)')
win = {}
for a in ARMS:
    t, ret, hb = data[a]['t'], data[a]['ret'], data[a]['hb']
    first = ret_out[a]['first_batch']
    win[a] = []
    for b0 in range(0, 172800, 19200):
        seg = [ret[i] for i in range(max(b0, first), b0 + 19200)]
        rej = sum(data[a]['rw'][b0:b0 + 19200]) + sum(data[a]['rm'][b0:b0 + 19200])
        w = dict(window=f'{b0 // 32}-{(b0 + 19200) // 32}', min=min(seg) if seg else None, p5=pct(seg, .05) if seg else None,
                 median=statistics.median(seg) if seg else None, peak_history_bytes=max(hb[b0:b0 + 19200]), rejections=rej)
        win[a].append(w)
for i in range(9):
    line = f'  {win["v38"][i]["window"]:>10}'
    for a in ARMS:
        w = win[a][i]
        line += f' | {a}: ' + (f'{w["min"]:.3f}/{w["p5"]:.3f}/{w["median"]:.3f}' if w['min'] is not None else '-') + f' {w["peak_history_bytes"] / 2**30:.3f}GiB rej {w["rejections"]}'
    P(line)
out['windows'] = win

P()
P('== Final slope of v38 retention (per 600 s)')
t, ret = data['v38']['t'], data['v38']['ret']
mins = [w['min'] for w in win['v38']]
meds = [w['median'] for w in win['v38']]
P(f'  per-window minimum: ' + ' '.join(f'{x:.2f}' for x in mins if x is not None))
P(f'  change of the window minimum over the last three windows: ' + ' '.join(f'{mins[i] - mins[i - 1]:+.2f}' for i in (6, 7, 8)))
P(f'  change of the window median over the last three windows: ' + ' '.join(f'{meds[i] - meds[i - 1]:+.2f}' for i in (6, 7, 8)))
xs = [t[i] for i in range(124800, 172800, 32)]
ys = [ret[i] for i in range(124800, 172800, 32)]
mx, my = statistics.mean(xs), statistics.mean(ys)
slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
P(f'  least-squares slope of retention over the last 1,500 s of the run (one point per second): {slope * 600:+.3f} s per 600 s')
out['final_slope_s_per_600s'] = slope * 600

P()
P('== Native time (orientative against the archived rounds of 2026-10-23; different session)')
tim = {}
for a, base in (('v38', C / 'runs/by5'), ('v26', C26 / 'runs'), ('fdb265', OLD / 'runs'), ('v1', OLD / 'runs'), ('v2', OLD / 'runs')):
    for r in (1, 2, 3):
        p = base / f'round{r}-{a}/batches.csv'
        if not p.exists():
            continue
        rs = rows(p)
        tot, w, rd = ([float(x[c]) for x in rs] for c in ('total_s', 'write_s', 'read_s'))
        for cut, (lo, hi) in CUTS.items():
            tim.setdefault((a, cut), []).append((sum(tot[lo:hi]), sum(w[lo:hi]), sum(rd[lo:hi])))
for cut in CUTS:
    P(f'  -- {cut}')
    for a in ('fdb265', 'v1', 'v2', 'v26', 'v38'):
        v = tim.get((a, cut))
        if v:
            P(f'    {a:6s} total median {statistics.median(x[0] for x in v):9.2f} s (min {min(x[0] for x in v):.2f}, max {max(x[0] for x in v):.2f}) rounds {" ".join("%.2f" % x[0] for x in v)}, write {statistics.median(x[1] for x in v):.2f}, read {statistics.median(x[2] for x in v):.2f}  [{"2026-11-04" if a == "v38" else "2026-10-30" if a == "v26" else "2026-10-23"}]')

with open(C / 'analysis/windows-600s.csv', 'w') as f:
    w = csv.writer(f)
    w.writerow(['arm', 'window_s', 'retention_min_s', 'retention_p5_s', 'retention_median_s', 'peak_history_bytes', 'rejections'])
    for a in ARMS:
        for x in win[a]:
            w.writerow([a, x['window'], x['min'], x['p5'], x['median'], x['peak_history_bytes'], x['rejections']])
(C / 'analysis/analysis.json').write_text(json.dumps(out, indent=1))
(C / 'analysis/analysis.txt').write_text('\n'.join(L) + '\n')
print('\n'.join(L))
