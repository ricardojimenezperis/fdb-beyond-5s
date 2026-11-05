import csv, json, os, statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
AMD = os.path.dirname(HERE)
DATA = os.path.join(AMD, "data")
ARCH = os.path.join(AMD, "..", "..", "..", "v2", "evaluation", "amd", "data")
REFS = [("FDB", "FDB"), ("btree-v1", "v1"), ("resolver v2", "v2")]
problems = []


def rows(path):
    return list(csv.DictReader(open(path)))


def timed(rs):
    return [r for r in rs if r["round"] not in ("", "warmup")]


def ratio(new, old):
    pairs = [a / b for a in new for b in old]
    return st.median(new) / st.median(old), min(pairs), max(pairs)


def fmt(r):
    return "%.3f [%.3f, %.3f]" % r if r else "–"


LOADS = []


def load(name, new_rows, new_total, arch_rows, arch_total, new_read, new_write, arch_read, arch_write, new_dec, arch_dec):
    out = {"name": name, "new": [new_total(r) for r in new_rows], "refs": {}, "phases": {}}
    decs = {new_dec(r) for r in new_rows}
    for cand, lab in REFS:
        ar = [r for r in arch_rows if r["candidate"] == cand]
        if not ar:
            continue
        out["refs"][lab] = [arch_total(r) for r in ar]
        out["phases"][lab] = (st.median(new_read(r) for r in new_rows) / st.median(arch_read(r) for r in ar),
                              st.median(new_write(r) for r in new_rows) / st.median(arch_write(r) for r in ar))
        if {arch_dec(r) for r in ar} != decs:
            problems.append("%s %s decisions" % (name, lab))
    out["decisions"] = sorted(decs)
    LOADS.append(out)


f = float
cs = rows(os.path.join(DATA, "v38-conflictset-runs.csv"))
acs = timed(rows(os.path.join(ARCH, "conflictset-runs.csv")))
for w in ("points", "ranges"):
    load("conflictset " + w, timed([r for r in cs if r["workload"] == w]), lambda r: f(r["seconds"]), [r for r in acs if r["workload"] == w], lambda r: f(r["seconds"]),
         lambda r: f(r["checkReadS"]), lambda r: f(r["mergeS"]), lambda r: f(r["checkReadS"]), lambda r: f(r["mergeS"]),
         lambda r: (r["committed"], r["tooOld"], r["decisions"]), lambda r: (r["committed"], r["tooOld"], r["decisions"]))

mb = rows(os.path.join(DATA, "v38-macbench-runs.csv"))
amb = timed(rows(os.path.join(ARCH, "macbench-runs.csv")))
rwc = lambda r: f(r["readS"]) + f(r["writeS"]) + f(r["combineS"])
for w in ("points", "ranges"):
    load("macBenchmark " + w, timed([r for r in mb if r["workload"] == w]), rwc, [r for r in amb if r["workload"] == w], rwc,
         lambda r: f(r["readS"]), lambda r: f(r["writeS"]), lambda r: f(r["readS"]), lambda r: f(r["writeS"]),
         lambda r: (r["conflicts"], r["decisions"]), lambda r: (r["conflicts"], r["decisions"]))

sa = rows(os.path.join(DATA, "v38-standalone-runs.csv"))
asa = timed(rows(os.path.join(ARCH, "standalone-runs.csv")))
for w in ("points", "ranges"):
    for label, tot in (("standalone %s, read + write" % w, lambda r: f(r["readS"]) + f(r["writeS"])), ("standalone %s, totalS" % w, lambda r: f(r["totalS"]))):
        load(label, timed([r for r in sa if r["workload"] == w]), tot, [r for r in asa if r["workload"] == w], lambda r: f(r["totalS"]),
             lambda r: f(r["readS"]), lambda r: f(r["writeS"]), lambda r: f(r["readS"]), lambda r: f(r["writeS"]),
             lambda r: (r["conflicts"], r["decisions"]), lambda r: (r["conflicts"], r["decisions"]))

sl = rows(os.path.join(DATA, "v38-skiplisttest-runs.csv"))
asl = timed(rows(os.path.join(ARCH, "skiplisttest-runs.csv")))
for c in ("head16", "head32", "tail32", "head64", "tail64", "head128", "tail128"):
    load("skiplisttest " + c, timed([r for r in sl if r["config"] == c]), lambda r: f(r["detectS"]), [r for r in asl if r["config"] == c], lambda r: f(r["detect_only_s"]),
         lambda r: f(r["checkReadS"]), lambda r: f(r["mergeS"]), lambda r: f(r["checkReadS"]), lambda r: f(r["mergeS"]),
         lambda r: (r["committed"], r["decisions"]), lambda r: (r["committed"], r["decisions"]))

b5 = [r for r in rows(os.path.join(DATA, "v38-by5-runs.csv")) if r["run"].startswith("round")]
ab5 = rows(os.path.join(DATA, "archived-by5-runs.csv"))
by5 = {"name": "by5 6 GiB, whole segment", "new": [f(r["total_s"]) for r in b5], "refs": {}, "phases": {}, "decisions": sorted({(r["offeredHash"], r["decisionsHash"]) for r in b5})}
for cand, lab in (("FDB", "FDB"), ("btree-v1", "v1")):
    ar = [r for r in ab5 if r["arm"] == cand]
    by5["refs"][lab] = [f(r["total_s"]) for r in ar]
    by5["phases"][lab] = (st.median(f(r["read_s"]) for r in b5) / st.median(f(r["read_s"]) for r in ar), st.median(f(r["write_s"]) for r in b5) / st.median(f(r["write_s"]) for r in ar))
LOADS.append(by5)

print("## Whole workload: v3.8 median and ratio to each archived reference\n")
print("Ratio = v3.8 median / reference median; brackets: minimum and maximum over every pair of a v3.8 run with a reference run.\n")
print("| load | v3.8 runs (s) | v3.8 median | FDB median | v1 median | v2 median | v3.8 / FDB | v3.8 / v1 | v3.8 / v2 |")
print("|---|---|---|---|---|---|---|---|---|")
for L in LOADS:
    med = {k: st.median(v) for k, v in L["refs"].items()}
    print("| %s | %s | %.3f | %s | %s | %s | %s | %s | %s |" % (L["name"], " ".join("%.3f" % x for x in L["new"]), st.median(L["new"]),
          *("%.3f" % med[k] if k in med else "–" for k in ("FDB", "v1", "v2")),
          *(fmt(ratio(L["new"], L["refs"][k])) if k in L["refs"] else "–" for k in ("FDB", "v1", "v2"))))
print("\n## Diagnostic only: read and write phase ratios (medians)\n")
print("| load | against | read phase | write phase |")
print("|---|---|---|---|")
for L in LOADS:
    if "totalS" in L["name"]:
        continue
    for k, (rd, wr) in L["phases"].items():
        print("| %s | %s | %.3f | %.3f |" % (L["name"].replace(", read + write", ""), k, rd, wr))

sw = rows(os.path.join(DATA, "sweep-stage1-runs.csv"))
print("\n## Group sweep, stage 1: totalS (prepS + readS + writeS), ratio to 2x8 in the same round\n")
print("| workload | config | totalS r1 / r2 / r3 | median | / 2x8 r1 / r2 / r3 | median | all rounds below 1 |")
print("|---|---|---|---|---|---|---|")
for w in ("points", "ranges"):
    D = {(r["round"], r["config"]): r for r in sw if r["workload"] == w and r["round"] != "warmup"}
    rounds = sorted({k[0] for k in D})
    for c in ("2x8", "2x10", "2x12", "3x12", "3x20"):
        t = [f(D[(r, c)]["totalS"]) for r in rounds]
        q = [f(D[(r, c)]["totalS"]) / f(D[(r, "2x8")]["totalS"]) for r in rounds]
        print("| %s | %s | %s | %.3f | %s | %s | %s |" % (w, c, " / ".join("%.3f" % x for x in t), st.median(t),
              "–" if c == "2x8" else " / ".join("%.4f" % x for x in q), "–" if c == "2x8" else "%.4f" % st.median(q), "–" if c == "2x8" else ("yes" if max(q) < 1 else "no")))
    for r in D.values():
        if r["exit"] != "0" or r["hugetlb_min_free"] != "0":
            problems.append("sweep %s %s %s" % (w, r["round"], r["config"]))

allruns = cs + mb + sa + sl + [r for r in rows(os.path.join(DATA, "v38-by5-runs.csv"))]
for r in allruns:
    if r["exit"] != "0":
        problems.append("exit %s" % r)
print("\nCPU sibling busy, all v3.8 runs: max %.3f%%" % (100 * max(f(r["sibling_busy"]) for r in allruns)))
print("problems:", problems or "none")
