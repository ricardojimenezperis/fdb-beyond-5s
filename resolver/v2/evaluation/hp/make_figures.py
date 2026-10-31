import argparse, csv, gzip, io, re, statistics, sys
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

OUT = Path(__file__).resolve().parent
DATA, FIG = OUT / 'data', OUT / 'figures'
SHORT_REL = '2026-10-29-btree-v26-hp'
LONG_REL = '2026-10-30-equal-memory-v26-hp'
SA_REL = '2026-10-30-standalone-v1fork-v26-hp'
LONG_V1_REL = '2026-10-23-equal-memory-hp/runs/acct-v1'
LONG_FDB_REL = '2026-10-11-fdb-v6-long-retention-hp/wave1/fdb-w265'
GZ = ('longrun-per-batch.csv', 'longrun-resolver-v2-time-per-batch.csv')
HIST = 'standalone baseline (073fcbb9af)'
SA_V1, SA_V2 = 'btree-v1 (5a07ab1936)', 'resolver v2 (dc2802fc2f)'

GRAY, BLUE, ORANGE = '#7a7974', '#2a78d6', '#eb6834'
INK, INK2, GRID = '#0b0b0b', '#52514e', '#e4e3df'
COLOR = {'FDB': GRAY, 'btree-v1': BLUE, 'resolver v2': ORANGE}
NAMES = ['FDB', 'btree-v1', 'resolver v2']
CONFS = ['head16', 'head32', 'tail32', 'head64', 'tail64', 'head128', 'tail128']

plt.rcParams.update({
    'font.size': 12, 'axes.titlesize': 13, 'axes.labelsize': 12, 'xtick.labelsize': 11, 'ytick.labelsize': 11,
    'legend.fontsize': 11, 'axes.edgecolor': INK2, 'axes.labelcolor': INK, 'xtick.color': INK2, 'ytick.color': INK2,
    'text.color': INK, 'axes.spines.top': False, 'axes.spines.right': False, 'axes.grid': True, 'grid.color': GRID,
    'grid.linewidth': 0.8, 'axes.axisbelow': True, 'figure.facecolor': 'white', 'axes.facecolor': 'white',
    'svg.fonttype': 'none', 'font.family': 'DejaVu Sans', 'svg.hashsalt': 'fig-v2-eval-hp'})


def kv(line):
    return dict(m.groups() for m in re.finditer(r'(\w+)=(\S+)', line))


def save(fig, name):
    fig.savefig(FIG / f'{name}.svg', bbox_inches='tight', metadata={'Date': None})
    fig.savefig(FIG / f'{name}.png', dpi=200, bbox_inches='tight')
    plt.close(fig)


def wcsv(name, header, rows):
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator='\n')
    w.writerow(header)
    w.writerows(rows)
    if name in GZ:
        with open(DATA / f'{name}.gz', 'wb') as f, gzip.GzipFile(fileobj=f, mode='wb', mtime=0, filename='') as g:
            g.write(buf.getvalue().encode())
    else:
        (DATA / name).write_text(buf.getvalue())


def rcsv(name):
    if name in GZ:
        p = DATA / f'{name}.gz'
        if not p.exists():
            sys.exit(f'missing {p}: download the release asset {p.name} (see NOTE.md) into {DATA}')
        return list(csv.DictReader(io.TextIOWrapper(gzip.open(p), encoding='utf-8')))
    return list(csv.DictReader(open(DATA / name)))


def med(xs):
    return statistics.median(xs)


def strip(ax, x, values, color, med_value=None, width=0.28):
    offs = [-0.07, 0.0, 0.07][:len(values)]
    ax.scatter([x + o for o in offs], values, s=46, color=color, edgecolor='white', linewidth=1.0, zorder=3)
    m = med(values) if med_value is None else med_value
    ax.plot([x - width / 2, x + width / 2], [m, m], color=color, lw=3, solid_capstyle='round', zorder=4)
    return m


def ratio_legend(ax, loc='upper left'):
    h = [Line2D([], [], marker='o', ls='', color=INK2, markersize=7, label='per round (same-round pair)'),
         Line2D([], [], color=INK2, lw=3, label='median of three rounds')]
    ax.legend(handles=h, loc=loc, frameon=False)


def lines(root, rel):
    for n, l in enumerate(open(root / rel), 1):
        yield f'{rel}:{n}', l


def extract(root):
    SHORT, LONG, LONG_V1, LONG_FDB = (root / r for r in (SHORT_REL, LONG_REL, LONG_V1_REL, LONG_FDB_REL))
    arm = {'fdb': 'FDB', 'v1': 'btree-v1', 'v2.6:16': 'resolver v2'}
    rows = []
    for src, l in lines(root, f'{SHORT_REL}/runs/bench-hp-2026-10-29-five/runs.txt'):
        d = kv(l)
        if d.get('arm') in arm:
            rows.append([d['round'], arm[d['arm']], d['workload'], d['index'], d['seed'], d['batches'], d['warmup'], d['transactions'],
                         d['committed'], d['tooOld'], d['seconds'], d['txPerSec'], d['checkReadS'], d['mergeS'], d['load_before'],
                         f"{SHORT_REL}/runs/bench-hp-2026-10-29-five/r{d['round']}-{d['workload']}-{d['arm'].replace(':', '-g')}/stdout.txt", src])
    wcsv('conflictset-runs.csv', ['round', 'candidate', 'workload', 'index', 'seed', 'batches', 'warmup', 'transactions', 'committed',
                                  'too_old', 'seconds', 'tx_per_s', 'check_read_s', 'merge_write_s', 'load_before_1min', 'run_stdout', 'source'], rows)

    rows = []
    for src, l in lines(root, f'{SHORT_REL}/runs/macbench-hp-2026-10-29-five/runs.txt'):
        d = kv(l)
        if d.get('arm') in arm:
            rows.append([d['round'], arm[d['arm']], d['workload'], d['index'], d['batches'], d['reads'], d['writes'], d['readS'], d['writeS'],
                         d['readsPerS'], d['writesPerS'], d['conflicts'], d['decisions'], d['load_before'],
                         f"{SHORT_REL}/runs/macbench-hp-2026-10-29-five/r{d['round']}-{d['workload']}-{d['arm'].replace(':', '-g')}/stdout.txt", src])
    wcsv('fdb-integrated-bench-runs.csv', ['round', 'candidate', 'workload', 'index', 'batches', 'reads', 'writes', 'read_s', 'write_s',
                                           'reads_per_s', 'writes_per_s', 'conflicts', 'decisions_hash', 'load_before_1min', 'run_stdout', 'source'], rows)

    ver = {'v1-5a07ab1936': SA_V1, 'v2.6-g16': SA_V2}
    rows = []
    for src, l in lines(root, f'{SA_REL}/out/runs.txt'):
        d = kv(l)
        if d.get('version') in ver:
            rows.append([d['kind'], d['round'], ver[d['version']], d['workload'], d['block'], d['batches'], d['reads'], d['writes'], d['readS'],
                         d['writeS'], d['totalS'], d['readsPerS'], d['writesPerS'], d['conflicts'], d['decisions'], d['reclaimCV'], d['exit'],
                         d['load_before'], src])
    wcsv('standalone-runs.csv', ['kind', 'round', 'candidate', 'workload', 'block_bytes', 'batches', 'reads', 'writes', 'read_s', 'write_s',
                                 'total_s', 'reads_per_s', 'writes_per_s', 'conflicts', 'decisions_hash', 'reclaim_cv', 'exit',
                                 'load_before_1_5_15min', 'source'], rows)

    ver = {'v1': HIST, 'v2.6': 'resolver v2 (dc2802fc2f), 2026-10-29'}
    rows = []
    for src, l in lines(root, f'{SHORT_REL}/mac-standalone/out/runs.txt'):
        d = kv(l)
        if d.get('version') in ver:
            rows.append([d['round'], ver[d['version']], d['workload'], d['block'], d['batches'], d['reads'], d['writes'], d['readS'], d['writeS'],
                         d['totalS'], d['readsPerS'], d['writesPerS'], d['conflicts'], d['decisions'], src])
    wcsv('standalone-historic-073fcbb9af-runs.csv', ['round', 'candidate', 'workload', 'block_bytes', 'batches', 'reads', 'writes', 'read_s',
                                                     'write_s', 'total_s', 'reads_per_s', 'writes_per_s', 'conflicts', 'decisions_hash', 'source'], rows)

    cand = {'fdb': 'FDB', 'v1': 'btree-v1', 'v26': 'resolver v2'}
    rows = []
    for conf in CONFS:
        for r in (1, 2, 3):
            for c, name in cand.items():
                rel = f'{SHORT_REL}/runs/skiplisttest/{conf}-r{r}-{c}'
                d = root / rel
                o, run = (d / 'out.txt').read_text(), (d / 'run.txt').read_text()
                det = float(re.search(r'Detect only:\s+([\d.]+) sec', o)[1])
                ph = kv(re.search(r'^phases:.*$', o, re.M)[0])
                rv = kv(run.split('\n')[0])
                rows.append([conf, r, name, rv['bytes'], rv['layout'], rv['PER_BATCH'], rv['P'], det, ph['checkReadS'], ph['mergeS'],
                             re.search(r'decisionsHash=(\w+)', o)[1], rv['sha256'], kv(run.split('\n')[1])['exit'], f'{rel}/out.txt', f'{rel}/run.txt'])
    wcsv('skiplisttest-runs.csv', ['conf', 'round', 'candidate', 'key_bytes', 'layout', 'per_batch', 'pool_pages', 'detect_s', 'check_read_s',
                                   'merge_write_s', 'decisions_hash', 'fdbserver_sha256', 'exit', 'source_out', 'source_run'], rows)

    rd = lambda p: list(csv.DictReader(open(p)))
    if not (LONG / 'runs/acct-v26/batches.csv').exists():
        print(f'{LONG / "runs/acct-v26/batches.csv"} not found: long-trace CSV files left as they are in {DATA}')
        return
    v2, v1, fd = rd(LONG / 'runs/acct-v26/batches.csv'), rd(LONG_V1 / 'batches.csv'), rd(LONG_FDB / 'batches.csv')
    v2a, v1a, fda = (rd(p / 'batches.csv.acct.csv') for p in (LONG / 'runs/acct-v26', LONG_V1, LONG_FDB))
    assert [x['cv'] for x in v2] == [x['cv'] for x in v1] == [x['cv'] for x in fd]
    rows = [[i, int(v2[i]['cv']) / 1e6, int(v1[i]['retained_us']) / 1e6, int(v2[i]['retained_us']) / 1e6,
             fda[i]['history_bytes'], v1a[i]['history_bytes'], v2a[i]['history_bytes'],
             int(fd[i]['rejected_window']) + int(fd[i]['rejected_memory']), int(v1[i]['rejected_window']) + int(v1[i]['rejected_memory']),
             int(v2[i]['rejected_window']) + int(v2[i]['rejected_memory'])] for i in range(len(v2))]
    wcsv('longrun-per-batch.csv', ['batch', 'trace_time_s', 'retention_btree_v1_s', 'retention_resolver_v2_s', 'history_bytes_fdb265',
                                   'history_bytes_btree_v1', 'history_bytes_resolver_v2', 'rejections_fdb265', 'rejections_btree_v1',
                                   'rejections_resolver_v2'], rows)
    rows = []
    for r in (1, 2, 3):
        for i, x in enumerate(rd(LONG / f'runs/round{r}-v26/batches.csv')):
            rows.append([r, i, x['cv'], x['read_s'], x['write_s'], x['add_s'], x['sort_s'], x['intrabatch_s'], x['combine_s'], x['cleanup_s'], x['total_s']])
    wcsv('longrun-resolver-v2-time-per-batch.csv', ['round', 'batch', 'cv_us', 'read_s', 'write_s', 'add_s', 'sort_s', 'intrabatch_s',
                                                    'combine_s', 'cleanup_s', 'total_s'], rows)


def by(rows, *keys):
    out = {}
    for x in rows:
        out.setdefault(tuple(x[k] for k in keys), []).append(x)
    return out


def ratios(rows, val, num, den, wl_key='workload', wl=None, extra=()):
    g = {(x['round'], x['candidate']) + tuple(x[k] for k in extra): float(x[val]) for x in rows if wl is None or x[wl_key] == wl}
    rounds = sorted({x['round'] for x in rows})
    return [g[(r, num) + tuple()] / g[(r, den)] for r in rounds]


def fig_conflictset():
    rows = rcsv('conflictset-runs.csv')
    for wl in ('points', 'ranges'):
        assert len({x['committed'] for x in rows if x['workload'] == wl}) == 1
    fig = plt.figure(figsize=(13, 4.8))
    ax = fig.add_axes([0.06, 0.14, 0.44, 0.74])
    tx = fig.add_axes([0.54, 0.14, 0.45, 0.74])
    tx.axis('off')
    pos = {'points': 0, 'ranges': 1}
    off = {'FDB': -0.28, 'btree-v1': 0.0, 'resolver v2': 0.28}
    table = []
    for wl in ('points', 'ranges'):
        sub = [x for x in rows if x['workload'] == wl]
        fdb = {x['round']: float(x['tx_per_s']) for x in sub if x['candidate'] == 'FDB'}
        for c in NAMES:
            per = sorted((x['round'], float(x['tx_per_s'])) for x in sub if x['candidate'] == c)
            rs = [v / fdb[r] for r, v in per]
            m = strip(ax, pos[wl] + off[c], rs, COLOR[c], width=0.2)
            ax.annotate(f'{m:.2f}', (pos[wl] + off[c], max(rs)), xytext=(0, 7), textcoords='offset points', ha='center', fontsize=11, color=INK)
            table.append([wl, c] + [f'{v:,.0f}' for _, v in per] + [f'{med([v for _, v in per]):,.0f}', f'{m:.3f}'])
    ax.axhline(1, color=INK2, lw=1, ls='--', zorder=1)
    ax.set_xticks([0, 1], ['points', 'ranges'])
    ax.set_xlim(-0.55, 1.55)
    ax.set_ylim(0.5, 1.25)
    ax.set_ylabel('throughput / FDB, same round')
    ax.set_title('conflictset: throughput relative to FDB   (higher is better ↑)', loc='left')
    h = [Line2D([], [], marker='o', ls='', color=COLOR[c], markersize=8, label=c) for c in NAMES]
    ax.legend(handles=h, loc='lower left', frameon=False, ncol=3)
    t = tx.table(cellText=[r[:2] + r[2:] for r in table], colLabels=['workload', 'candidate', 'r1', 'r2', 'r3', 'median', '÷FDB'],
                 loc='center', cellLoc='right', colLoc='right', colWidths=[0.15, 0.21, 0.13, 0.13, 0.13, 0.13, 0.10])
    t.auto_set_font_size(False)
    t.set_fontsize(10)
    t.scale(1, 1.55)
    for (i, j), cell in t.get_celld().items():
        cell.set_edgecolor(GRID)
        if i == 0:
            cell.set_text_props(color=INK2, weight='bold')
    tx.set_title('transactions per second (tx/s)', loc='left', fontsize=12)
    save(fig, 'fig-v2-eval-hp-conflictset')

    fig, axs = plt.subplots(1, 2, figsize=(11.5, 4.4), sharey=False)
    for ax, val, title in ((axs[0], 'check_read_s', 'read time (checkReadS)'), (axs[1], 'merge_write_s', 'write time (mergeS)')):
        for wl in ('points', 'ranges'):
            sub = [x for x in rows if x['workload'] == wl]
            fdb = {x['round']: float(x[val]) for x in sub if x['candidate'] == 'FDB'}
            for c in ('btree-v1', 'resolver v2'):
                per = sorted((x['round'], float(x[val])) for x in sub if x['candidate'] == c)
                rs = [v / fdb[r] for r, v in per]
                x0 = pos[wl] + (-0.17 if c == 'btree-v1' else 0.17)
                m = strip(ax, x0, rs, COLOR[c], width=0.2)
                ax.annotate(f'{m:.2f}', (x0, max(rs)), xytext=(0, 7), textcoords='offset points', ha='center', fontsize=11)
        ax.axhline(1, color=GRAY, lw=1.2, ls='--', zorder=1)
        ax.text(1.5, 1, ' FDB = 1', va='bottom', ha='right', color=INK2, fontsize=10)
        ax.set_xticks([0, 1], ['points', 'ranges'])
        ax.set_xlim(-0.5, 1.5)
        ax.set_ylim(0, max(ax.get_ylim()[1], 2.3))
        ax.set_title(f'{title} / FDB   (lower is better ↓)', loc='left')
    axs[0].set_ylabel('time / FDB, same round')
    h = [Line2D([], [], marker='o', ls='', color=COLOR[c], markersize=8, label=c) for c in ('btree-v1', 'resolver v2')]
    axs[0].legend(handles=h, loc='lower left', frameon=False)
    fig.tight_layout()
    save(fig, 'fig-v2-eval-hp-conflictset-phases')


def fig_integrated():
    rows = rcsv('fdb-integrated-bench-runs.csv')
    for wl in ('points', 'ranges'):
        assert len({(x['decisions_hash'], x['conflicts']) for x in rows if x['workload'] == wl}) == 1
    fig, axs = plt.subplots(1, 2, figsize=(11.5, 4.6), sharey=True)
    pos = {'points': 0, 'ranges': 1}
    for ax, val, title in ((axs[0], 'reads_per_s', 'reads'), (axs[1], 'writes_per_s', 'writes')):
        for wl in ('points', 'ranges'):
            sub = [x for x in rows if x['workload'] == wl]
            fdb = {x['round']: float(x[val]) for x in sub if x['candidate'] == 'FDB'}
            for c in ('btree-v1', 'resolver v2'):
                per = sorted((x['round'], float(x[val])) for x in sub if x['candidate'] == c)
                rs = [v / fdb[r] for r, v in per]
                x0 = pos[wl] + (-0.17 if c == 'btree-v1' else 0.17)
                m = strip(ax, x0, rs, COLOR[c], width=0.2)
                ax.annotate(f'{m:.2f}', (x0, max(rs)), xytext=(0, 7), textcoords='offset points', ha='center', fontsize=11)
            fm = med(list(fdb.values()))
            ax.annotate(f'FDB {fm / 1e6:.2f} M/s', (pos[wl], 0.04), ha='center', fontsize=10, color=INK2)
        ax.axhline(1, color=GRAY, lw=1.2, ls='--', zorder=1)
        ax.text(1.5, 1, ' FDB = 1', va='bottom', ha='right', color=INK2, fontsize=10)
        ax.set_xticks([0, 1], ['points', 'ranges'])
        ax.set_xlim(-0.5, 1.5)
        ax.set_ylim(0, 1.4)
        ax.set_title(f'{title}: throughput / FDB   (higher is better ↑)', loc='left')
    axs[0].set_ylabel('operations per second / FDB, same round')
    h = [Line2D([], [], marker='o', ls='', color=COLOR[c], markersize=8, label=c) for c in ('btree-v1', 'resolver v2')]
    axs[0].legend(handles=h, loc='upper left', frameon=False)
    fig.tight_layout()
    save(fig, 'fig-v2-eval-hp-fdb-integrated-bench')


def fig_standalone():
    rows = [x for x in rcsv('standalone-runs.csv') if x['kind'] == 'timed']
    assert all(x['exit'] == '0' and x['reclaim_cv'] == '0' for x in rows)
    for wl in ('points', 'ranges'):
        assert len({(x['decisions_hash'], x['conflicts']) for x in rows if x['workload'] == wl}) == 1
    for val, title, name in (('reads_per_s', 'reads', 'standalone-reads'), ('writes_per_s', 'writes', 'standalone-writes')):
        fig, ax = plt.subplots(figsize=(7.5, 4.6))
        for k, wl in enumerate(('points', 'ranges')):
            sub = [x for x in rows if x['workload'] == wl]
            per = {c: sorted((x['round'], float(x[val]) / 1e6) for x in sub if x['candidate'] == c) for c in (SA_V1, SA_V2)}
            for c, dx in ((SA_V1, -0.17), (SA_V2, 0.17)):
                m = strip(ax, k + dx, [v for _, v in per[c]], BLUE if c == SA_V1 else ORANGE, width=0.2)
                ax.annotate(f'{m:.2f}', (k + dx, max(v for _, v in per[c])), xytext=(0, 7), textcoords='offset points', ha='center', fontsize=11)
            rr = med([b / a for (_, a), (_, b) in zip(per[SA_V1], per[SA_V2])])
            ax.annotate(f'v2 / v1 = {rr:.2f}\n(same-round median)', (k, 0.12), ha='center', fontsize=11, color=INK2)
        ax.set_xticks([0, 1], ['points', 'ranges'])
        ax.set_xlim(-0.5, 1.5)
        ax.set_ylim(0, max(ax.get_ylim()[1], 1) * 1.08)
        ax.set_ylabel(f'{title} per second (millions)')
        ax.set_title(f'standalone benchmark, {title}   (higher is better ↑)', loc='left')
        h = [Line2D([], [], marker='o', ls='', color=BLUE, markersize=8, label=SA_V1),
             Line2D([], [], marker='o', ls='', color=ORANGE, markersize=8, label=SA_V2 + ', G=16')]
        ax.legend(handles=h, loc='upper left', frameon=False)
        fig.tight_layout()
        save(fig, f'fig-v2-eval-hp-{name}')


def fig_skiplisttest():
    rows = rcsv('skiplisttest-runs.csv')
    for conf in CONFS:
        assert len({x['decisions_hash'] for x in rows if x['conf'] == conf}) == 1
        assert all(x['exit'] == '0' for x in rows)
    fig, axs = plt.subplots(2, 3, figsize=(14, 7.6), sharex=True)
    for col, (val, title) in enumerate((('detect_s', 'total (Detect only)'), ('check_read_s', 'read (checkReadS)'), ('merge_write_s', 'write (mergeS)'))):
        for row, den in enumerate(('FDB', 'btree-v1')):
            ax = axs[row, col]
            meds = []
            for k, conf in enumerate(CONFS):
                g = {(x['round'], x['candidate']): float(x[val]) for x in rows if x['conf'] == conf}
                rs = [g[(r, 'resolver v2')] / g[(r, den)] for r in ('1', '2', '3')]
                meds.append(strip(ax, k, rs, ORANGE, width=0.45))
            for k, m in enumerate(meds):
                ax.annotate(f'{m:.2f}', (k, m), xytext=(0, 9), textcoords='offset points', ha='center', fontsize=10)
            ax.axhline(1, color=GRAY if den == 'FDB' else BLUE, lw=1.2, ls='--', zorder=1)
            lo, hi = min(meds), max(meds)
            ax.set_ylim(min(0.4, lo - 0.1), max(1.6, hi + 0.2))
            ax.set_title(f'{title}', loc='left')
            if col == 0:
                ax.set_ylabel(f'resolver v2 / {den}\n(time; lower is better ↓)')
    for ax in axs[1]:
        ax.set_xticks(range(len(CONFS)), CONFS, rotation=35, ha='right', rotation_mode='anchor')
    fig.suptitle('skiplisttest: time ratios of resolver v2, same-round pairs (median line, round dots)', x=0.01, ha='left', fontsize=13)
    fig.tight_layout()
    save(fig, 'fig-v2-eval-hp-skiplisttest')


def lsq(xs, ys):
    mx, my = statistics.mean(xs), statistics.mean(ys)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)


def fig_longrun():
    rows = rcsv('longrun-per-batch.csv')
    t = [float(x['trace_time_s']) for x in rows]
    r1 = [float(x['retention_btree_v1_s']) for x in rows]
    r2 = [float(x['retention_resolver_v2_s']) for x in rows]
    idx = range(31, len(rows), 32)
    fig, ax = plt.subplots(figsize=(11, 5))
    ax.plot([t[i] for i in idx], [r1[i] for i in idx], color=BLUE, lw=4.5, alpha=0.45, label='btree-v1 (2026-10-23, archived)')
    ax.plot([t[i] for i in idx], [r2[i] for i in idx], color=ORANGE, lw=1.6, label='resolver v2 (2026-10-30)')
    ax.axhline(265, color=GRAY, lw=1.6, ls='--', label='FDB: configured window, 265 s')
    ax.set_xlim(0, 5400)
    ax.set_ylim(0, 750)
    ax.set_xlabel('trace time since segment start (s)')
    ax.set_ylabel('retained history (s)')
    ax.set_title('Long trace, 6 GiB history budget: retained history   (higher is better ↑)', loc='left')
    ax.legend(loc='lower right', frameon=False)
    save(fig, 'fig-v2-eval-hp-longrun-retention')

    lo = next(i for i, x in enumerate(t) if x > 2400)
    fit = [i for i, x in enumerate(t) if x > 3900]
    s1, s2 = lsq([t[i] for i in fit], [r1[i] for i in fit]) * 600, lsq([t[i] for i in fit], [r2[i] for i in fit]) * 600
    fig, ax = plt.subplots(figsize=(11, 4.8))
    ax.plot(t[lo:], r1[lo:], color=BLUE, lw=4.5, alpha=0.45, label='btree-v1 (2026-10-23, archived)')
    ax.plot(t[lo:], r2[lo:], color=ORANGE, lw=1.2, label='resolver v2 (2026-10-30)')
    mx, my = statistics.mean([t[i] for i in fit]), statistics.mean([r2[i] for i in fit])
    xs = [3900, 5400]
    ax.plot(xs, [my + s2 / 600 * (x - mx) for x in xs], color=INK, lw=1, ls=':', label='least-squares fit, last 1,500 s')
    ax.annotate(f'resolver v2 slope over the last 1,500 s: {s2:+.2f} s per 600 s\n(btree-v1: {s1:+.2f} s per 600 s)',
                (0.98, 0.92), xycoords='axes fraction', ha='right', va='top', fontsize=11, color=INK)
    ax.set_xlim(2400, 5400)
    ax.set_xlabel('trace time since segment start (s)')
    ax.set_ylabel('retained history (s)')
    ax.set_title('Long trace, final 3,000 s: retained history still decreasing', loc='left')
    ax.legend(loc='lower left', frameon=False)
    save(fig, 'fig-v2-eval-hp-longrun-retention-tail')

    fig, axs = plt.subplots(1, 2, figsize=(13, 4.8), gridspec_kw={'width_ratios': [1.6, 1]})
    G = 2 ** 30
    for ax, ylim in ((axs[0], (0, 6.6)), (axs[1], (5.80, 6.02))):
        for col, c, lab, lw, al in (('history_bytes_fdb265', GRAY, 'FDB, window 265 s (2026-10-10/11, archived)', 1.6, 1),
                                    ('history_bytes_btree_v1', BLUE, 'btree-v1 (2026-10-23, archived)', 4.5, 0.45),
                                    ('history_bytes_resolver_v2', ORANGE, 'resolver v2 (2026-10-30)', 1.6, 1)):
            ax.plot([t[i] for i in idx], [max(int(rows[j][col]) for j in range(i - 31, i + 1)) / G for i in idx], color=c, lw=lw, alpha=al, label=lab)
        ax.axhline(6442450944 / G, color=INK, lw=1, ls='--', label='budget 6,442,450,944 B')
        ax.set_xlim(0, 5400)
        ax.set_ylim(*ylim)
        ax.set_xlabel('trace time since segment start (s)')
    axs[0].set_ylabel('counted history (GiB, max per second)')
    axs[0].set_title('Long trace: counted history memory (history_bytes)', loc='left')
    axs[1].set_title('detail near the budget', loc='left')
    pk = {k: max(int(x[k]) for x in rows) for k in ('history_bytes_fdb265', 'history_bytes_btree_v1', 'history_bytes_resolver_v2')}
    axs[1].text(0.97, 0.06, f"peak, bytes:\nFDB {pk['history_bytes_fdb265']:,}\nbtree-v1 {pk['history_bytes_btree_v1']:,}\nresolver v2 {pk['history_bytes_resolver_v2']:,}",
                transform=axs[1].transAxes, ha='right', va='bottom', fontsize=10, color=INK)
    axs[0].legend(loc='lower right', frameon=False)
    fig.tight_layout()
    save(fig, 'fig-v2-eval-hp-longrun-memory')

    tr = rcsv('longrun-resolver-v2-time-per-batch.csv')
    other = ['add_s', 'sort_s', 'intrabatch_s', 'combine_s', 'cleanup_s']
    per_round = {}
    for x in tr:
        per_round.setdefault(int(x['round']), []).append(x)

    def split(seg):
        tot = sum(float(x['total_s']) for x in seg)
        rd_ = sum(float(x['read_s']) for x in seg)
        wr = sum(float(x['write_s']) for x in seg)
        oth = sum(float(x[c]) for x in seg for c in other)
        return rd_, wr, oth, tot - rd_ - wr - oth, tot

    win = {w: [split(rs[w * 19200:(w + 1) * 19200]) for r, rs in sorted(per_round.items())] for w in range(9)}
    whole = [(r,) + split(rs) for r, rs in sorted(per_round.items())]
    wcsv('longrun-resolver-v2-time-summary.csv', ['round', 'read_s', 'write_s', 'other_phases_s', 'unattributed_s', 'rest_s', 'total_s'],
         [[r, f'{a:.3f}', f'{b:.3f}', f'{c:.3f}', f'{d:.3f}', f'{c + d:.3f}', f'{e:.3f}'] for r, a, b, c, d, e in whole])
    wcsv('longrun-resolver-v2-time-600s.csv', ['window_s', 'read_s_median', 'write_s_median', 'other_phases_s_median', 'unattributed_s_median',
                                               'total_s_median', 'total_s_min', 'total_s_max'],
         [[f'{w * 600}-{(w + 1) * 600}'] + [f'{med([v[k] for v in win[w]]):.3f}' for k in range(5)] +
          [f'{min(v[4] for v in win[w]):.3f}', f'{max(v[4] for v in win[w]):.3f}'] for w in range(9)])

    fig, axs = plt.subplots(1, 2, figsize=(13, 5.2), gridspec_kw={'width_ratios': [1, 2.6]})
    sh = {'write': ORANGE, 'read': '#f3a27f', 'other': '#c4c3bd', 'unattributed': '#52514e'}
    order = (('write', 1), ('read', 0), ('other', 2), ('unattributed', 3))
    ax = axs[0]
    for k, v in enumerate(whole):
        b = 0
        for key, i in order:
            ax.bar(k, v[1 + i], bottom=b, color=sh[key], width=0.62, edgecolor='white', linewidth=1.5)
            b += v[1 + i]
        ax.annotate(f'{v[5]:,.0f} s', (k, v[5]), xytext=(0, 4), textcoords='offset points', ha='center', fontsize=10)
    ax.set_xticks(range(3), [f'round {v[0]}' for v in whole])
    ax.set_ylabel('seconds (whole 5,400 s segment)')
    ax.set_title('resolver v2, per round', loc='left')
    ax.set_ylim(0, 2600)
    w0 = whole[0]
    ax.text(0.0, -0.24, f'round 1: write {w0[2] / w0[5]:.1%}, read {w0[1] / w0[5]:.1%}, other phases {w0[3] / w0[5]:.1%},\n'
            f'unattributed {w0[4]:.2f} s ({w0[4] / w0[5]:.2%})', transform=ax.transAxes, fontsize=10, color=INK2, va='top')
    ax = axs[1]
    for w in range(9):
        m = [med([v[k] for v in win[w]]) for k in range(4)]
        b = 0
        for key, i in order:
            ax.bar(w, m[i], bottom=b, color=sh[key], width=0.7, edgecolor='white', linewidth=1.5)
            b += m[i]
    ax.set_xticks(range(9), [f'{w * 600}–\n{(w + 1) * 600}' for w in range(9)], fontsize=10)
    ax.set_xlabel('600 s window of trace time (s)')
    ax.set_ylabel('seconds (median of three rounds)')
    ax.set_title('resolver v2, per 600 s window', loc='left')
    h = [plt.Rectangle((0, 0), 1, 1, color=sh[k]) for k in ('unattributed', 'other', 'read', 'write')]
    ax.legend(h, ['unattributed (total − all timed phases)', 'other timed phases (add, sort, intra-batch, combine, cleanup)', 'read', 'write'],
              loc='upper left', frameon=False, fontsize=10)
    ax.set_ylim(0, 360)
    fig.suptitle('Long trace: where resolver v2 spends its time (native runs, one session, 2026-10-30; lower is better ↓)', x=0.01, ha='left', fontsize=13)
    fig.tight_layout()
    save(fig, 'fig-v2-eval-hp-longrun-time-breakdown')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Draw the fig-v2-eval-hp figures.')
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--campaign-root', type=Path, help='directory holding the original campaigns; the CSV files in data/ are extracted from it first')
    g.add_argument('--from-csv', action='store_true', help='draw only from the CSV files distributed in data/')
    a = ap.parse_args()
    DATA.mkdir(exist_ok=True)
    FIG.mkdir(exist_ok=True)
    if a.campaign_root:
        extract(a.campaign_root.resolve())
    fig_conflictset()
    fig_integrated()
    fig_standalone()
    fig_skiplisttest()
    fig_longrun()
