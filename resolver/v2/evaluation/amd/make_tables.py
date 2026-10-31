import csv, json, os, statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
DER = os.path.join(HERE, "derived")
os.makedirs(DER, exist_ok=True)
ALL = ["FDB", "btree-v1", "resolver v2"]
out = []


def load(name):
    return [r for r in csv.DictReader(open(os.path.join(DATA, name))) if r["round"]]


def med(rows, cand, key, **m):
    v = [float(r[key]) for r in rows if r["candidate"] == cand and all(r[k] == x for k, x in m.items())]
    return st.median(v), v


def same(rows, key, **m):
    vals = {r[key] for r in rows if all(r[k] == x for k, x in m.items())}
    return next(iter(vals)) if len(vals) == 1 else "DIFFER: " + " ".join(sorted(vals))


def table(title, head, rows):
    out.append("### " + title + "\n")
    out.append("| " + " | ".join(head) + " |")
    out.append("|" + "---|" * len(head))
    for r in rows:
        out.append("| " + " | ".join(r) + " |")
    out.append("")


def fmt(m, v, d=3):
    return ("%." + str(d) + "f (%s)") % (m, ", ".join(("%." + str(d) + "f") % x for x in v))


cs = load("conflictset-runs.csv")
rows = []
for w in ("points", "ranges"):
    for c in ALL:
        r = [w, c]
        for k in ("checkReadS", "mergeS", "seconds"):
            r.append(fmt(*med(cs, c, k, workload=w)))
        r.append(fmt(*med(cs, c, "txPerSec", workload=w), d=0))
        r += [same(cs, k, workload=w, candidate=c) for k in ("committed", "tooOld", "decisions")]
        rows.append(r)
table("conflictset: median (rounds 1, 2, 3)", ["workload", "candidate", "checkReadS", "mergeS", "measured batch time (s)",
                                                "tx/s", "committed", "tooOld", "decisions"], rows)

mb = load("macbench-runs.csv")
for r in mb:
    r["sum"] = str(float(r["readS"]) + float(r["writeS"]) + float(r["combineS"]))
rows = []
for w in ("points", "ranges"):
    for c in ALL:
        r = [w, c] + [fmt(*med(mb, c, k, workload=w)) for k in ("readS", "writeS", "combineS", "sum")]
        r += [same(mb, k, workload=w, candidate=c) for k in ("conflicts", "decisions")]
        rows.append(r)
table("macBenchmark: median (rounds 1, 2, 3); the last time column is the sum of timed phases",
      ["workload", "candidate", "readS", "writeS", "combineS", "sum of timed phases (s)", "conflicts", "decisions"], rows)

sa = load("standalone-runs.csv")
rows = []
for w in ("points", "ranges"):
    for c in ("btree-v1", "resolver v2"):
        r = [w, c] + [fmt(*med(sa, c, k, workload=w)) for k in ("readS", "writeS", "totalS")]
        r += [same(sa, k, workload=w, candidate=c) for k in ("conflicts", "decisions")]
        rows.append(r)
table("standalone blocksizebench, 2 KiB blocks: median (rounds 1, 2, 3); totalS is the sum of timed phases",
      ["workload", "candidate", "readS", "writeS", "totalS", "conflicts", "decisions"], rows)

sk = load("skiplisttest-runs.csv")
rows = []
for cfg in ["head16", "head32", "tail32", "head64", "tail64", "head128", "tail128"]:
    for c in ALL:
        r = [cfg, c] + [fmt(*med(sk, c, k, config=cfg)) for k in ("detect_only_s", "checkReadS", "mergeS")]
        r += [same(sk, k, config=cfg, candidate=c) for k in ("committed", "decisions")]
        rows.append(r)
table("skiplisttest: median (rounds 1, 2, 3)", ["config", "candidate", "Detect only (s)", "checkReadS", "mergeS", "committed",
                                                 "decisions"], rows)

summ = json.load(open(os.path.join(DER, "summary.json")))
rows = []
for k, (m, lo, hi) in sorted(summ["ratio_medians"].items()):
    rows.append([k, "%.4f" % m, "%.4f" % lo, "%.4f" % hi])
table("time ratios paired by round: median, minimum, maximum of the three rounds", ["block group phase ratio", "median", "min", "max"],
      rows)

open(os.path.join(DER, "tables.md"), "w").write("\n".join(out) + "\n")
print("\n".join(out))
