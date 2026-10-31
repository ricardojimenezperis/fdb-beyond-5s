"""Draws the figures of the synthetic partition-comparison model from its JSON output.

  python3 make_partition_figures.py   reads partition-comparison-example.json beside this script
                                      and writes ../figs/fig-v2-model-partition-*.svg and .png

The counts are key comparisons in a declared synthetic tree, not measurements. Needs Python 3 and
matplotlib 3.x.
"""
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

HERE = Path(__file__).resolve().parent
FIG = HERE.parent / 'figs'
MODEL = json.load(open(HERE / 'partition-comparison-example.json'))

BLUE, ORANGE = '#2a78d6', '#eb6834'
INK, INK2, GRID = '#0b0b0b', '#52514e', '#e4e3df'
SERIES = (('Independent searches, one per read', BLUE), ('Recursive partitioning', ORANGE))
TAG = 'Synthetic comparison model: counts of key comparisons, not measured time'

plt.rcParams.update({
    'svg.hashsalt': 'fig-v2-model',  # fixed clip-path ids, so that the SVGs are reproducible
    'font.size': 12, 'axes.titlesize': 13, 'axes.labelsize': 12, 'xtick.labelsize': 11, 'ytick.labelsize': 11,
    'legend.fontsize': 11, 'axes.edgecolor': INK2, 'axes.labelcolor': INK, 'xtick.color': INK2, 'ytick.color': INK2,
    'text.color': INK, 'axes.spines.top': False, 'axes.spines.right': False, 'axes.grid': True, 'grid.color': GRID,
    'grid.linewidth': 0.8, 'axes.axisbelow': True, 'figure.facecolor': 'white', 'axes.facecolor': 'white',
    'svg.fonttype': 'none', 'font.family': 'DejaVu Sans'})


def save(fig, name):
	fig.savefig(FIG / f'{name}.svg', bbox_inches='tight', metadata={'Date': None})
	fig.savefig(FIG / f'{name}.png', dpi=200, bbox_inches='tight')
	plt.close(fig)


def legend(fig, below=-0.06):
	fig.legend(handles=[Patch(color=c, label=l) for l, c in SERIES], loc='lower center', ncol=2,
			   frameon=False, bbox_to_anchor=(0.5, below))


def totals():
	u, c = MODEL['uniform'], MODEL['clustered_in_100000_history_entries']
	cases = [
		('Uniform reads,\ninternal nodes only', u['independent_internal_comparisons'], u['grouped_internal_comparisons']),
		('Uniform reads,\nwith initial leaf search', u['independent_total_locating_comparisons'],
		 u['grouped_total_locating_comparisons']),
		('Clustered reads,\nwith initial leaf search', c['independent_total_locating_comparisons'],
		 c['grouped_total_locating_comparisons']),
	]
	fig, ax = plt.subplots(figsize=(9, 4.6))
	h = 0.36
	for i, (label, ind, grp) in enumerate(cases):
		y = len(cases) - 1 - i
		for v, dy, color in ((ind, h / 2, BLUE), (grp, -h / 2, ORANGE)):
			ax.barh(y + dy, v, height=h - 0.04, color=color)
			ax.text(v + 3000, y + dy, f'{v:,}', va='center', fontsize=11, color=INK)
		ax.text(max(ind, grp) + 42000, y, f'{ind / grp:.2f}× fewer', va='center', fontsize=11, color=INK2)
	ax.set_yticks(range(len(cases)), [c[0] for c in reversed(cases)])
	ax.set_xlim(0, 360000)
	ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f'{v / 1000:.0f}k'))
	ax.set_xlabel('Key comparisons to locate 10,000 reads (fewer is better)')
	ax.grid(axis='y', visible=False)
	ax.set_title(TAG, fontsize=11, color=INK2, loc='left')
	legend(fig, -0.13)
	save(fig, 'fig-v2-model-partition-totals')


def by_depth():
	fig, axes = plt.subplots(1, 2, figsize=(11, 4.4), sharey=True)
	levels = ['root', 'level 1', 'level 2', 'level 3', 'level 4', 'leaf']
	for ax, (key, title) in zip(axes, (('uniform', 'Uniform reads'),
									   ('clustered_in_100000_history_entries', 'Reads clustered in 100,000 entries'))):
		m = MODEL[key]
		w = 0.38
		for k, (ind, grp) in enumerate(zip(m['independent_boundary_comparisons_by_depth'],
										   m['grouped_comparisons_by_depth'])):
			ax.bar(k - w / 2, ind, width=w - 0.04, color=BLUE)
			ax.bar(k + w / 2, grp, width=w - 0.04, color=ORANGE)
		ax.set_xticks(range(len(levels)), levels)
		ax.set_title(title)
		ax.grid(axis='x', visible=False)
	axes[0].set_ylabel('Key comparisons (fewer is better)')
	axes[0].yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f'{v / 1000:.0f}k'))
	fig.suptitle(TAG, fontsize=11, color=INK2, x=0.01, ha='left')
	legend(fig)
	save(fig, 'fig-v2-model-partition-by-depth')


if __name__ == '__main__':
	FIG.mkdir(exist_ok=True)
	totals()
	by_depth()
