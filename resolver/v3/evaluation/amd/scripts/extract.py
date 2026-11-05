import csv, glob, gzip, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
AMD = os.path.dirname(HERE)
RAW = os.path.join(AMD, "raw")
DATA = os.path.join(AMD, "data")


def kv(s):
    return dict(re.findall(r"(\w+)=([0-9a-f.]+)\b", s))


def runinfo(d):
    rt = open(d + "/run.txt").read()
    ru = kv(open(d + "/rusage.txt").read())
    hp = kv(re.search(r"hugetlb_free_before=.*", rt).group(0))
    return {"exit": re.search(r"\bexit=(\d+)", rt).group(1), "cpu": re.search(r"\bcpu=(\d+)", rt).group(1),
            "start": re.search(r"start=(\S+)", rt).group(1), "end": re.search(r"end=(\S+)", rt).group(1),
            "cpu_busy": re.search(r"cpu_busy=([\d.]+)", rt).group(1), "sibling_busy": re.search(r"sibling_busy=([\d.]+)", rt).group(1),
            "hugetlb_total": hp.get("hugetlb_total", ""), "hugetlb_free_before": hp.get("hugetlb_free_before", ""),
            "hugetlb_min_free": hp.get("hugetlb_min_free", ""), "maxrss_kib": ru.get("maxrss_kib", "")}


def write(name, rows):
    with open(os.path.join(DATA, name), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


def split(name):
    parts = name.split("-")
    return "-".join(parts[:-2]), parts[-2]


rows = []
for d in sorted(glob.glob(RAW + "/v38/conflictset/*")):
    w, rnd = split(os.path.basename(d))
    so = open(d + "/stdout.txt").read()
    x = kv(re.search(r"ConflictSetBenchmark (.*)", so).group(1))
    p = kv(re.search(r"ConflictSetPhases (.*)", so).group(1))
    rows.append({"workload": w, "round": rnd, "seconds": x["seconds"], "addS": p["addS"], "sortS": p["sortS"], "checkReadS": x["checkReadS"],
                 "intraBatchS": x["intraBatchS"], "combineS": x["combineS"], "mergeS": x["mergeS"], "cleanupS": x["cleanupS"], "otherS": p["otherS"],
                 "txPerSec": x["txPerSec"], "batchP50us": x["batchP50us"], "batchP99us": x["batchP99us"], "committed": x["committed"], "tooOld": x["tooOld"],
                 "decisions": x["decisions"], "rssAfterMiB": x["rssAfterMiB"]} | runinfo(d))
write("v38-conflictset-runs.csv", rows)

rows = []
for d in sorted(glob.glob(RAW + "/v38/macbench/*")):
    w, rnd = split(os.path.basename(d))
    so = open(d + "/stdout.txt").read()
    x = kv(re.search(r"MacBenchmark index(.*)", so).group(1))
    p = kv(re.search(r"MacBenchmarkPhases (.*)", so).group(1))
    rows.append({"workload": w, "round": rnd, "readS": x["readS"], "writeS": x["writeS"], "combineS": x["combineS"],
                 "read_write_combineS": "%.3f" % (float(x["readS"]) + float(x["writeS"]) + float(x["combineS"])),
                 "addS": p["addS"], "sortS": p["sortS"], "intraBatchS": p["intraBatchS"], "cleanupS": p["cleanupS"], "phasesTotalS": p["totalS"],
                 "conflicts": x["conflicts"], "decisions": x["decisions"]} | runinfo(d))
write("v38-macbench-runs.csv", rows)

rows = []
for d in sorted(glob.glob(RAW + "/v38/standalone/*")):
    w, rnd = split(os.path.basename(d))
    x = kv(re.search(r"workload=\w+ batches=.*", open(d + "/stdout.txt").read()).group(0))
    rows.append({"workload": w, "round": rnd, "prepS": x["prepS"], "readS": x["readS"], "writeS": x["writeS"], "totalS": x["totalS"],
                 "read_writeS": "%.3f" % (float(x["readS"]) + float(x["writeS"])), "conflicts": x["conflicts"], "reclaimCV": x["reclaimCV"],
                 "decisions": x["decisions"]} | runinfo(d))
write("v38-standalone-runs.csv", rows)

rows = []
for d in sorted(glob.glob(RAW + "/v38/skiplist/*")):
    cfg, rnd = split(os.path.basename(d))
    so = open(d + "/stdout.txt").read()
    t = kv(re.search(r"totals: (.*)", so).group(1))
    p = kv(re.search(r"phases: (.*)", so).group(1))
    rows.append({"config": cfg, "round": rnd, "detectS": t["detectS"], "resolveS": t["resolveS"], "addS": t["addS"], "sortS": t["sortS"],
                 "checkReadS": p["checkReadS"], "intraBatchS": p["intraBatchS"], "combineS": p["combineS"], "mergeS": p["mergeS"], "cleanupS": p["cleanupS"],
                 "committed": re.search(r"(\d+) transactions without conflict", so).group(1),
                 "decisions": re.search(r"decisionsHash=([0-9a-f]+)", so).group(1), "residentMiB": re.search(r"residentMiB=([\d.]+)", so).group(1)} | runinfo(d))
write("v38-skiplisttest-runs.csv", rows)

rows = []
for d in sorted(glob.glob(RAW + "/by5-v38/*-v3")):
    run = os.path.basename(d)
    so = open(d + "/stdout.txt").read()
    r = kv(re.search(r"by5result (.*)", so).group(1))
    a = kv(re.search(r"by5age ageUs=1562500 (.*)", so).group(1))
    row = {"run": run, "batches": "", "total_s": "", "read_s": "", "write_s": "", "cleanup_s": "", "p99_ms": ""}
    f = os.path.join(DATA, "by5", run + "-batch-times.csv.gz")
    if os.path.exists(f):
        b = list(csv.DictReader(gzip.open(f, "rt")))
        tot = sorted(float(x["total_s"]) for x in b)
        row.update(batches=len(b), total_s="%.3f" % sum(tot), read_s="%.3f" % sum(float(x["read_s"]) for x in b),
                   write_s="%.3f" % sum(float(x["write_s"]) for x in b), cleanup_s="%.4f" % sum(float(x["cleanup_s"]) for x in b),
                   p99_ms="%.3f" % (1000 * tot[int(0.99 * (len(tot) - 1))]))
    row.update(offeredHash=r["offeredHash"], decisionsHash=r["decisionsHash"], offered=r["offered"], accepted=r["accepted"],
               rejectedWindow=a["rejectedWindow"], rejectedMemory=a["rejectedMemory"])
    rows.append(row | runinfo(d))
write("v38-by5-runs.csv", rows)

rows = []
for w in ("points", "ranges"):
    for d in sorted(glob.glob(RAW + "/sweep/%s/*" % w)):
        rnd, cfg = os.path.basename(d).split("-", 1)
        x = kv(re.search(r"workload=\w+ batches=.*", open(d + "/stdout.txt").read()).group(0))
        rows.append({"workload": w, "round": rnd, "config": cfg, "prepS": x["prepS"], "readS": x["readS"], "writeS": x["writeS"], "totalS": x["totalS"],
                     "conflicts": x["conflicts"], "decisions": x["decisions"]} | runinfo(d))
write("sweep-stage1-runs.csv", rows)
