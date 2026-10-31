"""Figures of the resolver v2 evaluation on the AMD and Apple M3 machines (fig-v2-eval-amd-*,
fig-v2-eval-m3-*), from runs already kept. No benchmark is run here.

  python3 make_figures.py RAW_DIR    extracts the public candidates from the raw run records in
                                     RAW_DIR into data/*.csv, then draws figures/*.svg and *.png
  python3 make_figures.py --from-csv draws the figures from data/*.csv alone

RAW_DIR holds amd-integrated-runs.txt (one line per run of noSim/fdbserver/conflictset/macBenchmark)
and m3-standalone-runs.txt (one line per run of resolver1's bench/blocksizebench). Lines of other
candidates in those files are not extracted. Needs Python 3 and matplotlib 3.x.
"""
import csv, re, statistics, sys
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

OUT = Path(__file__).resolve().parent
DATA, FIG = OUT / 'data', OUT / 'figures'
DATA.mkdir(exist_ok=True)
FIG.mkdir(exist_ok=True)

GRAY, BLUE, ORANGE = '#7a7974', '#2a78d6', '#eb6834'
INK, INK2, GRID = '#0b0b0b', '#52514e', '#e4e3df'
COLOR = {'FDB': GRAY, 'btree-v1': BLUE, 'resolver v2': ORANGE}
WORKLOADS = ('points', 'ranges')
ROUNDS = ('1', '2', '3')

plt.rcParams.update({
    'svg.hashsalt': 'fig-v2-eval',  # fixed clip-path ids, so that the SVGs are reproducible
    'font.size': 12, 'axes.titlesize': 13, 'axes.labelsize': 12, 'xtick.labelsize': 11, 'ytick.labelsize': 11,
    'legend.fontsize': 11, 'axes.edgecolor': INK2, 'axes.labelcolor': INK, 'xtick.color': INK2, 'ytick.color': INK2,
    'text.color': INK, 'axes.spines.top': False, 'axes.spines.right': False, 'axes.grid': True, 'grid.color': GRID,
    'grid.linewidth': 0.8, 'axes.axisbelow': True, 'figure.facecolor': 'white', 'axes.facecolor': 'white',
    'svg.fonttype': 'none', 'font.family': 'DejaVu Sans'})

# Raw arm names of the public candidates. Every other arm in the raw files is skipped.
AMD_ARMS = {'fdb': 'FDB', 'v1': 'btree-v1', 'v2.6': 'resolver v2'}
M3_ARMS = {'v1': 'btree-v1', 'v2.6': 'resolver v2'}


def kv(line):
	return dict(m.groups() for m in re.finditer(r'(\w+)=(\S+)', line))


def wcsv(name, header, rows):
	with open(DATA / name, 'w', newline='') as f:
		w = csv.writer(f)
		w.writerow(header)
		w.writerows(rows)


def rcsv(name):
	return list(csv.DictReader(open(DATA / name)))


def save(fig, name):
	fig.savefig(FIG / f'{name}.svg', bbox_inches='tight', metadata={'Date': None})
	fig.savefig(FIG / f'{name}.png', dpi=200, bbox_inches='tight')
	plt.close(fig)


def med(xs):
	return statistics.median(xs)


def strip(ax, x, values, color, width=0.2):
	"""One dot per round and a bar at their median."""
	offs = [-0.06, 0.0, 0.06][:len(values)]
	ax.scatter([x + o for o in offs], values, s=46, color=color, edgecolor='white', linewidth=1.0, zorder=3)
	m = med(values)
	ax.plot([x - width / 2, x + width / 2], [m, m], color=color, lw=3, solid_capstyle='round', zorder=4)
	return m


def extract(raw):
	rows = []
	for line in open(raw / 'amd-integrated-runs.txt'):
		d = kv(line)
		if d.get('arm') in AMD_ARMS:
			rows.append([d['round'], AMD_ARMS[d['arm']], d['workload'], d['index'], d['batches'], d['reads'], d['writes'],
			             d['readS'], d['writeS'], d['readsPerS'], d['writesPerS'], d['conflicts'], d['decisions']])
	wcsv('amd-fdb-integrated-bench-runs.csv', ['round', 'candidate', 'workload', 'index', 'batches', 'reads', 'writes', 'read_s',
	                                           'write_s', 'reads_per_s', 'writes_per_s', 'conflicts', 'decisions_hash'], rows)
	rows = []
	for line in open(raw / 'm3-standalone-runs.txt'):
		d = kv(line)
		if d.get('version') in M3_ARMS:
			rows.append([d['round'], M3_ARMS[d['version']], d['workload'], d['block'], d['batches'], d['reads'], d['writes'],
			             d['readS'], d['writeS'], d['totalS'], d['readsPerS'], d['writesPerS'], d['conflicts'], d['reclaimCV'],
			             d['decisions']])
	wcsv('m3-standalone-runs.csv', ['round', 'candidate', 'workload', 'block_bytes', 'batches', 'reads', 'writes', 'read_s', 'write_s',
	                                'total_s', 'reads_per_s', 'writes_per_s', 'conflicts', 'reclaim_cv', 'decisions_hash'], rows)


def check_same_work(rows):
	"""Every run of a workload made the same decisions, so the candidates did the same work."""
	for wl in WORKLOADS:
		sub = [x for x in rows if x['workload'] == wl]
		assert len({(x['decisions_hash'], x['conflicts'], x['reads'], x['writes']) for x in sub}) == 1, wl
		assert sorted({x['round'] for x in sub}) == list(ROUNDS), wl


def per_round(sub, cand, val):
	return {x['round']: float(x[val]) for x in sub if x['candidate'] == cand}


def candidate_legend(fig, cands):
	"""Below the panels, so that it never covers a round."""
	h = [Line2D([], [], marker='o', ls='', color=COLOR[c], markersize=8, label=f'{c} (one dot per round)') for c in cands]
	h += [Line2D([], [], color=INK2, lw=3, label='median of 3 rounds')]
	fig.legend(handles=h, loc='lower center', ncol=len(h), frameon=False, bbox_to_anchor=(0.5, -0.07))


def fig_amd():
	rows = rcsv('amd-fdb-integrated-bench-runs.csv')
	check_same_work(rows)
	cands = ('FDB', 'btree-v1', 'resolver v2')
	dx = {'FDB': -0.26, 'btree-v1': 0.0, 'resolver v2': 0.26}
	# Absolute rates.
	fig, axs = plt.subplots(1, 2, figsize=(12, 4.8))
	for ax, val, title in ((axs[0], 'reads_per_s', 'reads'), (axs[1], 'writes_per_s', 'writes')):
		top = 0
		for k, wl in enumerate(WORKLOADS):
			sub = [x for x in rows if x['workload'] == wl]
			for c in cands:
				vs = [v / 1e6 for v in per_round(sub, c, val).values()]
				m = strip(ax, k + dx[c], vs, COLOR[c], width=0.18)
				ax.annotate(f'{m:.2f}', (k + dx[c], max(vs)), xytext=(0, 6), textcoords='offset points', ha='center', fontsize=10)
				top = max(top, max(vs))
		ax.set_xticks([0, 1], list(WORKLOADS))
		ax.set_xlim(-0.55, 1.55)
		ax.set_ylim(0, top * 1.18)
		ax.set_ylabel(f'{title} per second (millions)')
		ax.set_title(f'{title}   (higher is better ↑)', loc='left')
	candidate_legend(fig, cands)
	fig.suptitle('Integrated benchmark through FDB\'s Resolver batch path, AMD, one session', x=0.01, ha='left', fontsize=13)
	fig.tight_layout()
	save(fig, 'fig-v2-eval-amd-fdb-integrated-bench-rates')
	# Ratios to FDB, within each round.
	fig, axs = plt.subplots(1, 2, figsize=(12, 4.8), sharey=True)
	for ax, val, title in ((axs[0], 'reads_per_s', 'reads'), (axs[1], 'writes_per_s', 'writes')):
		for k, wl in enumerate(WORKLOADS):
			sub = [x for x in rows if x['workload'] == wl]
			fdb = per_round(sub, 'FDB', val)
			for c in ('btree-v1', 'resolver v2'):
				got = per_round(sub, c, val)
				rs = [got[r] / fdb[r] for r in ROUNDS]
				x0 = k + (-0.17 if c == 'btree-v1' else 0.17)
				m = strip(ax, x0, rs, COLOR[c])
				ax.annotate(f'{m:.2f}', (x0, max(rs)), xytext=(0, 6), textcoords='offset points', ha='center', fontsize=11)
		ax.axhline(1, color=GRAY, lw=1.2, ls='--', zorder=1)
		ax.text(1.55, 1, 'FDB = 1 ', va='bottom', ha='right', color=INK2, fontsize=10)
		ax.set_xticks([0, 1], list(WORKLOADS))
		ax.set_xlim(-0.5, 1.55)
		ax.set_ylim(0, 1.45)
		ax.set_title(f'{title}: throughput / FDB   (higher is better ↑)', loc='left')
	axs[0].set_ylabel('candidate / FDB, same round')
	candidate_legend(fig, ('btree-v1', 'resolver v2'))
	fig.suptitle('Integrated benchmark, throughput relative to FDB, AMD, one session', x=0.01, ha='left', fontsize=13)
	fig.tight_layout()
	save(fig, 'fig-v2-eval-amd-fdb-integrated-bench-vs-fdb')


def fig_m3():
	rows = rcsv('m3-standalone-runs.csv')
	check_same_work(rows)
	assert all(x['reclaim_cv'] == '0' for x in rows)
	cands = ('btree-v1', 'resolver v2')
	dx = {'btree-v1': -0.17, 'resolver v2': 0.17}
	for val, title, name in (('reads_per_s', 'reads', 'reads'), ('writes_per_s', 'writes', 'writes')):
		fig, ax = plt.subplots(figsize=(7.5, 4.8))
		top = 0
		for k, wl in enumerate(WORKLOADS):
			sub = [x for x in rows if x['workload'] == wl]
			per = {c: per_round(sub, c, val) for c in cands}
			for c in cands:
				vs = [per[c][r] / 1e6 for r in ROUNDS]
				m = strip(ax, k + dx[c], vs, COLOR[c])
				ax.annotate(f'{m:.2f}', (k + dx[c], max(vs)), xytext=(0, 6), textcoords='offset points', ha='center', fontsize=11)
				top = max(top, max(vs))
			rr = med([per['resolver v2'][r] / per['btree-v1'][r] for r in ROUNDS])
			ax.annotate(f'v2 / v1 = {rr:.2f}', (k, top * 0.04), ha='center', fontsize=11, color=INK2)
		ax.set_xticks([0, 1], list(WORKLOADS))
		ax.set_xlim(-0.5, 1.5)
		ax.set_ylim(0, top * 1.18)
		ax.set_ylabel(f'{title} per second (millions)')
		ax.set_title(f'standalone benchmark, {title}, Apple M3   (higher is better ↑)', loc='left', fontsize=12)
		candidate_legend(fig, cands)
		fig.tight_layout()
		save(fig, f'fig-v2-eval-m3-standalone-{name}')
	# Times of the same runs: read, write and total, each separately.
	fig, axs = plt.subplots(1, 3, figsize=(13.5, 4.6), sharey=False)
	for ax, val, title in zip(axs, ('read_s', 'write_s', 'total_s'), ('read time', 'write time', 'total time')):
		top = 0
		for k, wl in enumerate(WORKLOADS):
			sub = [x for x in rows if x['workload'] == wl]
			for c in cands:
				vs = [per_round(sub, c, val)[r] for r in ROUNDS]
				m = strip(ax, k + dx[c], vs, COLOR[c])
				ax.annotate(f'{m:.2f}', (k + dx[c], max(vs)), xytext=(0, 6), textcoords='offset points', ha='center', fontsize=10)
				top = max(top, max(vs))
		ax.set_xticks([0, 1], list(WORKLOADS))
		ax.set_xlim(-0.5, 1.5)
		ax.set_ylim(0, top * 1.18)
		ax.set_title(f'{title}   (lower is better ↓)', loc='left', fontsize=12)
	axs[0].set_ylabel('seconds for 3,000 batches')
	candidate_legend(fig, cands)
	fig.suptitle('Standalone benchmark, times of the same runs, Apple M3, one session', x=0.01, ha='left', fontsize=13)
	fig.tight_layout()
	save(fig, 'fig-v2-eval-m3-standalone-times')


if __name__ == '__main__':
	if sys.argv[1:] != ['--from-csv']:
		if len(sys.argv) != 2:
			sys.exit(__doc__)
		extract(Path(sys.argv[1]))
	fig_amd()
	fig_m3()
