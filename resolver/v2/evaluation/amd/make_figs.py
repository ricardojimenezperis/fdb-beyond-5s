import csv, json, os, statistics as st

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
FIGS = os.path.join(HERE, "figures")
DER = os.path.join(HERE, "derived")
os.makedirs(FIGS, exist_ok=True)
os.makedirs(DER, exist_ok=True)
COLOR = {"FDB": "#7f7f7f", "btree-v1": "#1f77b4", "resolver v2": "#ff7f0e"}
MACHINE = "AMD Ryzen 7 7700"
plt.rcParams.update({"font.size": 11, "axes.titlesize": 12, "axes.labelsize": 11, "legend.fontsize": 10,
                     "xtick.labelsize": 10, "ytick.labelsize": 10, "svg.fonttype": "none", "svg.hashsalt": "fig-v2-eval-amd"})


def save(fig, name):
    fig.savefig(os.path.join(FIGS, name + ".svg"), bbox_inches="tight", metadata={"Date": None})
    fig.savefig(os.path.join(FIGS, name + ".png"), bbox_inches="tight", dpi=200, metadata={"Software": None})
    plt.close(fig)


def load(name):
    return list(csv.DictReader(open(os.path.join(DATA, name))))


def measured(rows):
    return [r for r in rows if r["round"]]


def series(rows, cand, key, **match):
    sel = [r for r in rows if r["candidate"] == cand and all(r[k] == v for k, v in match.items())]
    return [float(r[key]) for r in sorted(sel, key=lambda r: int(r["round"]))]


ratio_rows = []


def ratios(rows, block, groupkey, groups, phases, cands):
    pairs = [(a, b) for a, b in (("resolver v2", "FDB"), ("resolver v2", "btree-v1"), ("btree-v1", "FDB")) if a in cands and b in cands]
    for g in groups:
        for key, label in phases:
            for a, b in pairs:
                xa, xb = series(rows, a, key, **{groupkey: g}), series(rows, b, key, **{groupkey: g})
                for i, (p, q) in enumerate(zip(xa, xb), 1):
                    ratio_rows.append({"block": block, "group": g, "phase": label, "numerator": a, "denominator": b,
                                       "round": i, "time_ratio": round(p / q, 4)})
    return pairs


def bars(rows, groupkey, groups, keys, cands, ylabel, scale, title, name, ticks, gtitle):
    fig, axes = plt.subplots(1, len(groups), figsize=(5.5 * len(groups), 4.2), squeeze=False)
    for ax, g in zip(axes[0], groups):
        width = 0.8 / len(cands)
        for i, cand in enumerate(cands):
            for j, key in enumerate(keys):
                vals = [v / scale for v in series(rows, cand, key, **{groupkey: g})]
                x = j + (i - (len(cands) - 1) / 2) * width
                ax.bar(x, st.median(vals), width * 0.92, color=COLOR[cand], label=cand if j == 0 else None)
                ax.scatter([x] * len(vals), vals, s=14, color="black", zorder=3, label="individual rounds" if (i, j) == (0, 0) else None)
        ax.set_xticks(range(len(keys)))
        ax.set_xticklabels(ticks)
        ax.set_title(gtitle(g))
        ax.set_ylabel(ylabel)
        ax.grid(axis="y", alpha=0.3)
    h, l = axes[0][0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=len(cands) + 1, frameon=False, bbox_to_anchor=(0.5, -0.1))
    fig.suptitle(title, y=1.02)
    save(fig, name)


MARK = {("resolver v2", "FDB"): "o", ("resolver v2", "btree-v1"): "s", ("btree-v1", "FDB"): "^"}


def ratio_fig(block, groups, phases, pairs, title, name, gtitle):
    fig, axes = plt.subplots(1, len(groups), figsize=(5.5 * len(groups), 4.4), sharey=True, squeeze=False)
    for ax, g in zip(axes[0], groups):
        for k, pair in enumerate(pairs):
            col = COLOR[pair[0]]
            for j, (_, label) in enumerate(phases):
                vals = [r["time_ratio"] for r in ratio_rows if r["block"] == block and r["group"] == g and r["phase"] == label
                        and (r["numerator"], r["denominator"]) == pair]
                x = j + (k - (len(pairs) - 1) / 2) * 0.25
                ax.scatter([x] * len(vals), vals, s=16, color=col, marker=MARK[pair], alpha=0.55)
                ax.hlines(st.median(vals), x - 0.1, x + 0.1, color=col, linewidth=2.2)
        ax.axhline(1.0, color="black", linewidth=1)
        ax.set_xticks(range(len(phases)))
        ax.set_xticklabels([p for _, p in phases])
        ax.set_title(gtitle(g))
        ax.grid(axis="y", alpha=0.3)
    axes[0][0].set_ylabel("Time ratio, paired by round\nbelow 1: numerator faster")
    fig.legend([Line2D([], [], color=COLOR[p[0]], marker=MARK[p], linewidth=2.2) for p in pairs], ["%s / %s" % p for p in pairs],
               loc="lower center", ncol=len(pairs), frameon=False, bbox_to_anchor=(0.5, -0.1))
    fig.suptitle(title, y=1.02)
    save(fig, name)


ALL = ["FDB", "btree-v1", "resolver v2"]
WL = ["points", "ranges"]
wtitle = lambda g: "%s workload" % g

cs = measured(load("conflictset-runs.csv"))
ph = [("checkReadS", "Read check"), ("mergeS", "Write merge"), ("seconds", "Measured batch\ntime")]
pairs = ratios(cs, "conflictset", "workload", WL, ph, ALL)
bars(cs, "workload", WL, [k for k, _ in ph], ALL, "Time (s), lower is better", 1.0,
     "FDB conflictset benchmark, %s: time, median of 3 rounds" % MACHINE, "fig-v2-eval-amd-conflictset-time", [t for _, t in ph], wtitle)
bars(cs, "workload", WL, ["txPerSec"], ALL, "Thousand transactions per second, higher is better", 1e3,
     "FDB conflictset benchmark, %s: throughput, median of 3 rounds" % MACHINE, "fig-v2-eval-amd-conflictset-throughput", ["Transactions"], wtitle)
ratio_fig("conflictset", WL, ph, pairs, "FDB conflictset benchmark, %s: time ratios per round" % MACHINE, "fig-v2-eval-amd-conflictset-time-ratios", wtitle)

mb = measured(load("macbench-runs.csv"))
for r in mb:
    r["sum_timed_phases_s"] = str(float(r["readS"]) + float(r["writeS"]) + float(r["combineS"]))
ph = [("readS", "Read check"), ("writeS", "Write merge"), ("sum_timed_phases_s", "Sum of timed\nphases")]
pairs = ratios(mb, "macbench", "workload", WL, ph, ALL)
bars(mb, "workload", WL, [k for k, _ in ph], ALL, "Time (s), lower is better", 1.0,
     "Integrated batch-path benchmark (macBenchmark), %s: time, median of 3 rounds" % MACHINE, "fig-v2-eval-amd-macbench-time", [t for _, t in ph], wtitle)
bars(mb, "workload", WL, ["readsPerS", "writesPerS"], ALL, "Millions per second, higher is better", 1e6,
     "Integrated batch-path benchmark (macBenchmark), %s: throughput, median of 3 rounds" % MACHINE, "fig-v2-eval-amd-macbench-throughput",
     ["Reads checked", "Writes merged"], wtitle)
ratio_fig("macbench", WL, ph, pairs, "Integrated batch-path benchmark (macBenchmark), %s: time ratios per round" % MACHINE,
          "fig-v2-eval-amd-macbench-time-ratios", wtitle)

sa = measured(load("standalone-runs.csv"))
ph = [("readS", "Read check"), ("writeS", "Write insertion"), ("totalS", "Sum of timed\nphases")]
pairs = ratios(sa, "standalone", "workload", WL, ph, ["btree-v1", "resolver v2"])
bars(sa, "workload", WL, [k for k, _ in ph], ["btree-v1", "resolver v2"], "Time (s), lower is better", 1.0,
     "Standalone benchmark (blocksizebench, 2 KiB blocks), %s: time, median of 3 rounds" % MACHINE, "fig-v2-eval-amd-standalone-time",
     [t for _, t in ph], wtitle)
bars(sa, "workload", WL, ["readsPerS", "writesPerS"], ["btree-v1", "resolver v2"], "Millions per second, higher is better", 1e6,
     "Standalone benchmark (blocksizebench, 2 KiB blocks), %s: throughput, median of 3 rounds" % MACHINE,
     "fig-v2-eval-amd-standalone-throughput", ["Reads checked", "Writes inserted"], wtitle)
ratio_fig("standalone", WL, ph, pairs, "Standalone benchmark (blocksizebench), %s: time ratios per round" % MACHINE,
          "fig-v2-eval-amd-standalone-time-ratios", wtitle)

sk = measured(load("skiplisttest-runs.csv"))
CFG = ["head16", "head32", "tail32", "head64", "tail64", "head128", "tail128"]
ph = [("detect_only_s", "Detect only"), ("checkReadS", "Read check"), ("mergeS", "Write merge")]
pairs = ratios(sk, "skiplisttest", "config", CFG, ph, ALL)
for key, label, name in (("detect_only_s", "Detect only (s), lower is better", "time"),):
    fig, ax = plt.subplots(figsize=(12, 4.4))
    width = 0.26
    for i, cand in enumerate(ALL):
        for j, cfg in enumerate(CFG):
            vals = series(sk, cand, key, config=cfg)
            x = j + (i - 1) * width
            ax.bar(x, st.median(vals), width * 0.92, color=COLOR[cand], label=cand if j == 0 else None)
            ax.scatter([x] * len(vals), vals, s=12, color="black", zorder=3, label="individual rounds" if (i, j) == (0, 0) else None)
    ax.set_xticks(range(len(CFG)))
    ax.set_xticklabels(CFG)
    ax.set_ylabel(label)
    ax.grid(axis="y", alpha=0.3)
    ax.legend(frameon=False, ncol=4, loc="upper left")
    ax.set_title("skiplisttest, %s: resolution time per key configuration, median of 3 rounds" % MACHINE)
    save(fig, "fig-v2-eval-amd-skiplisttest-time")
fig, axes = plt.subplots(1, 3, figsize=(16, 4.4), sharey=True)
for ax, (key, label) in zip(axes, ph):
    for k, pair in enumerate(pairs):
        for j, cfg in enumerate(CFG):
            vals = [r["time_ratio"] for r in ratio_rows if r["block"] == "skiplisttest" and r["group"] == cfg and r["phase"] == label
                    and (r["numerator"], r["denominator"]) == pair]
            x = j + (k - 1) * 0.25
            ax.scatter([x] * len(vals), vals, s=12, color=COLOR[pair[0]], marker=MARK[pair], alpha=0.55)
            ax.hlines(st.median(vals), x - 0.1, x + 0.1, color=COLOR[pair[0]], linewidth=2)
    ax.axhline(1.0, color="black", linewidth=1)
    ax.set_xticks(range(len(CFG)))
    ax.set_xticklabels(CFG, rotation=45)
    ax.set_title(label)
    ax.grid(axis="y", alpha=0.3)
axes[0].set_ylabel("Time ratio, paired by round\nbelow 1: numerator faster")
fig.legend([Line2D([], [], color=COLOR[p[0]], marker=MARK[p], linewidth=2) for p in pairs], ["%s / %s" % p for p in pairs],
           loc="lower center", ncol=3, frameon=False, bbox_to_anchor=(0.5, -0.12))
fig.suptitle("skiplisttest, %s: time ratios per key configuration and round" % MACHINE, y=1.02)
save(fig, "fig-v2-eval-amd-skiplisttest-time-ratios")

with open(os.path.join(DER, "time-ratios.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(ratio_rows[0]))
    w.writeheader()
    w.writerows(ratio_rows)

lt = {}
for r in csv.DictReader(open(os.path.join(DATA, "longtrace-amd-6gib-per-batch.csv"))):
    s = lt.setdefault(r["candidate"], {"t": [], "ret": [], "mem": []})
    s["t"].append(float(r["segment_s"]))
    s["ret"].append(float(r["retained_s"]))
    s["mem"].append(int(r["history_bytes"]))


def per_second(t, v, fn):
    ot, ov, cur, buf = [], [], None, []
    for a, b in zip(t, v):
        s = int(a)
        if cur is not None and s != cur:
            ot.append(cur + 0.5)
            ov.append(fn(buf))
            buf = []
        cur = s
        buf.append(b)
    if buf:
        ot.append(cur + 0.5)
        ov.append(fn(buf))
    return ot, ov


fig, ax = plt.subplots(figsize=(9, 4.2))
for cand, lab in (("btree-v1", "btree-v1 (archived run, 2026-10-23)"), ("resolver v2", "resolver v2 (2026-10-31)")):
    x, y = per_second(lt[cand]["t"], lt[cand]["ret"], min)
    ax.plot(x, y, color=COLOR[cand], linewidth=1.3, label=lab)
ax.set_xlabel("Time into the measured segment (s)")
ax.set_ylabel("History retained (s), higher keeps more")
ax.set_title("Long trace, pool of 98,300 pages (6,442,188,800 B), %s: retention (minimum per second)" % MACHINE)
ax.grid(alpha=0.3)
ax.legend(frameon=False, loc="lower right")
save(fig, "fig-v2-eval-amd-longtrace-retention")

fig, ax = plt.subplots(figsize=(9, 4.2))
for cand, lab in (("btree-v1", "btree-v1 (archived run)"), ("resolver v2", "resolver v2")):
    x, y = per_second(lt[cand]["t"], lt[cand]["mem"], max)
    ax.plot(x, [v / 2 ** 30 for v in y], color=COLOR[cand], linewidth=2.4 if cand == "btree-v1" else 1.2,
            linestyle="-" if cand == "btree-v1" else "--", label=lab + " (pool, reserved in full)")
ax.axhline(6.0, color="black", linewidth=0.8, linestyle=":", label="6 GiB budget")
ax.set_ylim(0, 6.6)
ax.set_xlabel("Time into the measured segment (s)")
ax.set_ylabel("Accounted history memory (GiB)")
ax.set_title("Long trace, %s: accounted history memory (maximum per second)" % MACHINE)
ax.grid(alpha=0.3)
ax.legend(frameon=False, loc="lower right")
save(fig, "fig-v2-eval-amd-longtrace-memory")

summary = {"ratio_medians": {}}
for r in ratio_rows:
    k = "%s %s %s %s/%s" % (r["block"], r["group"], r["phase"].replace("\n", " "), r["numerator"], r["denominator"])
    vals = [q["time_ratio"] for q in ratio_rows if (q["block"], q["group"], q["phase"], q["numerator"], q["denominator"]) ==
            (r["block"], r["group"], r["phase"], r["numerator"], r["denominator"])]
    summary["ratio_medians"][k] = [st.median(vals), min(vals), max(vals)]
for cand, s in lt.items():
    first = next((i for i, (t, r) in enumerate(zip(s["t"], s["ret"])) if r < t - 1e-6), None)
    summary.setdefault("longtrace", {})[cand] = {
        "batches": len(s["t"]), "first_discard_batch": first, "first_discard_s": s["t"][first] if first is not None else None,
        "max_retained_s": max(s["ret"]), "minimum_after_first_discard_s": min(s["ret"][first:]) if first is not None else None,
        "retained_at_end_s": s["ret"][-1], "peak_history_bytes": max(s["mem"])}
json.dump(summary, open(os.path.join(DER, "summary.json"), "w"), indent=1, sort_keys=True)
print(json.dumps(summary.get("longtrace"), indent=1))
