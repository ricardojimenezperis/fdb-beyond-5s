import csv, gzip, json, os, statistics as st

INTEL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = json.load(open(os.path.join(INTEL, "analysis", "short.json")))
B = list(csv.DictReader(open(os.path.join(INTEL, "data", "by5-runs.csv"))))
problems = []


def ratio(new, old):
    pairs = [a / b for a in new for b in old]
    return "%.3f [%.3f, %.3f]" % (st.median(new) / st.median(old), min(pairs), max(pairs))


for r in range(1, 4):
    b = list(csv.DictReader(gzip.open(os.path.join(INTEL, "runs", "by5", "round%d-v38" % r, "batch-times.csv.gz"), "rt")))
    t = "%.2f" % sum(float(x["total_s"]) for x in b)
    if len(b) != 172800 or t != next(x["total_s"] for x in B if x["arm"] == "v3.8" and x["round"] == str(r)):
        problems.append("by5 v3.8 round %d: %d batches, %s s" % (r, len(b), t))

rows = []
for load, name, unit in (("standalone", "standalone", "totalS"), ("conflictset", "conflictset", "seconds"), ("macbench", "macBenchmark", "totalS"), ("skiplisttest", "skiplisttest", "resolveS")):
    for w, e in sorted(S[load].items()):
        if "fdb" not in e and "v1" not in e:
            continue
        rows.append(("%s %s" % (name, w), unit, e["v38"], {k: e[k] for k in ("fdb", "v1", "v2") if k in e}))
by5 = {a: [float(x["total_s"]) for x in B if x["arm"] == a] for a in ("v3.8", "FDB", "v1", "v2")}
rows.append(("by5 6 GiB, whole segment", "sum of batch totals", by5["v3.8"], {"fdb": by5["FDB"], "v1": by5["v1"], "v2": by5["v2"]}))

print("## Whole workload: v3.8 median and ratio to each archived reference\n")
print("Ratio = v3.8 median / reference median; brackets: minimum and maximum over every pair of a v3.8 run with a reference run.\n")
print("| load | figure | v3.8 runs (s) | v3.8 median | FDB median | v1 median | v2 median | v3.8 / FDB | v3.8 / v1 | v3.8 / v2 |")
print("|---|---|---|---|---|---|---|---|---|---|")
for name, unit, new, refs in rows:
    print("| %s | %s | %s | %.3f | %s | %s | %s | %s | %s | %s |" % (name, unit, " ".join("%.3f" % x for x in new), st.median(new),
          *("%.3f" % st.median(refs[k]) if k in refs else "–" for k in ("fdb", "v1", "v2")),
          *(ratio(new, refs[k]) if k in refs else "–" for k in ("fdb", "v1", "v2"))))
print("\nv3.8 only, no reference run of the same work: " + "; ".join("skiplisttest %s %.3f s" % (w, st.median(e["v38"])) for w, e in sorted(S["skiplisttest"].items()) if "fdb" not in e))
for load in S:
    for w, e in S[load].items():
        if len(e.get("v38_dec", [])) != 1 or ("ref_dec" in e and e["ref_dec"] != e["v38_dec"]):
            problems.append("%s %s decisions" % (load, w))
print("problems:", problems or "none")
