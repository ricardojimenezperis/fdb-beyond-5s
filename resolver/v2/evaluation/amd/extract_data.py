import csv, json, os, re, sys

ROOT = os.environ.get("FDB_ROOT")
if not ROOT:
    sys.exit("set FDB_ROOT")
E = os.path.join(ROOT, "scratch/eval-v2-amd")
OUT = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(OUT, "data")
os.makedirs(DATA, exist_ok=True)
LABEL = {"fdb": "FDB", "v1": "btree-v1", "v2": "resolver v2"}


def kv(line):
    return dict(x.split("=", 1) for x in line.split() if "=" in x)


def runmeta(d):
    lines = open(os.path.join(d, "run.txt")).read().splitlines()
    m = dict(re.findall(r"(start|end|exit|cpu|docker_before)=(\S+)", lines[0]))
    m2 = kv(lines[1])
    return {"start": m["start"], "end": m["end"], "exit": m["exit"], "cpu": m["cpu"], "docker_before": m["docker_before"],
            "cpu_busy": m2["cpu_busy"], "sibling_busy": m2["sibling_busy"]}


def name_parts(name, prefixed_cfg=False):
    parts = name.split("-")
    if prefixed_cfg:
        cfg, phase, cand = parts[0], parts[1], parts[2]
        return cfg, phase, cand
    phase, workload, cand = parts[0], parts[1], parts[2]
    return phase, workload, cand


def write(name, rows):
    with open(os.path.join(DATA, name), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)


manifest = {}
for block, key, fields in (("conflictset", "ConflictSetBenchmark",
                            ["index", "workload", "seed", "batches", "warmup", "transactions", "committed", "tooOld", "decisions",
                             "seconds", "txPerSec", "batchP50us", "batchP99us", "checkReadS", "intraBatchS", "combineS", "mergeS",
                             "cleanupS", "rssAfterMiB", "validationMinRV", "tooOldWithinWindow"]),
                           ("macbench", "MacBenchmark",
                            ["index", "workload", "batches", "reads", "writes", "readS", "writeS", "combineS", "readsPerS",
                             "writesPerS", "conflicts", "decisions"]),
                           ("standalone", "block=",
                            ["block", "workload", "batches", "reads", "writes", "readS", "writeS", "totalS", "readsPerS",
                             "writesPerS", "conflicts", "reclaimCV", "decisions"])):
    base = os.path.join(E, "runs", block)
    rows = []
    for d in sorted(os.listdir(base)):
        p = os.path.join(base, d, "stdout.txt")
        if not os.path.isfile(p):
            continue
        line = next(l for l in open(p) if l.startswith(key))
        v = kv(line)
        phase, workload, cand = name_parts(d)
        row = {"session": "amd-eval-v2-2026-10-31", "block": block, "run": d, "phase": phase, "round": phase[1:] if phase.startswith("r") else "",
               "candidate": LABEL[cand]}
        row.update({("block_bytes" if f == "block" else f): v[f] for f in fields})
        row.update(runmeta(os.path.join(base, d)))
        rows.append(row)
    write("%s-runs.csv" % block, rows)
    manifest[block] = open(os.path.join(base, "MANIFEST")).read()

base = os.path.join(E, "runs", "skiplist")
rows = []
for d in sorted(os.listdir(base)):
    p = os.path.join(base, d, "stdout.txt")
    if not os.path.isfile(p):
        continue
    s = open(p).read()
    cfg, phase, cand = name_parts(d, True)
    det = re.search(r"Detect only:\s+([\d.]+) sec\s+([\d.]+) Mtransactions/sec", s)
    ph = kv(re.search(r"^phases: .*$", s, re.M).group(0))
    res = kv(re.search(r"^residentMiB=.*$", s, re.M).group(0))
    row = {"session": "amd-eval-v2-2026-10-31", "block": "skiplisttest", "run": d, "config": cfg, "phase": phase,
           "round": phase[1:] if phase.startswith("r") else "", "candidate": LABEL[cand],
           "detect_only_s": det.group(1), "mtransactions_per_s": det.group(2), "checkReadS": ph["checkReadS"],
           "intraBatchS": ph["intraBatchS"], "combineS": ph["combineS"], "mergeS": ph["mergeS"], "cleanupS": ph["cleanupS"],
           "committed": re.search(r"(\d+) transactions without conflict", s).group(1),
           "decisions": re.search(r"decisionsHash=([0-9a-f]+)", s).group(1), "residentMiB": res["residentMiB"],
           "validationMinRV": res["validationMinRV"], "running_cpu": re.search(r"running=(\d+)", s).group(1)}
    row.update(runmeta(os.path.join(base, d)))
    rows.append(row)
write("skiplisttest-runs.csv", rows)
manifest["skiplisttest"] = open(os.path.join(base, "MANIFEST")).read()

lt = []
for cand, d in (("resolver v2", os.path.join(E, "longtrace", "acct-v2")),
                ("btree-v1", os.path.join(ROOT, "scratch/groupreads/eqmem/run1/acct-v1"))):
    run = open(os.path.join(d, "run.txt")).read()
    sha = re.search(r"sha=([0-9a-f]{64})", run).group(1)
    acct = {}
    with open(os.path.join(d, "batches.csv.acct.csv")) as f:
        for r in csv.DictReader(f):
            acct[r["batch"]] = r["history_bytes"]
    with open(os.path.join(d, "batches.csv")) as f:
        for r in csv.DictReader(f):
            lt.append({"candidate": cand, "source": "new 2026-10-31" if cand == "resolver v2" else "archived 2026-10-23",
                       "binary_sha256": sha, "batch": r["batch"], "cv": r["cv"], "segment_s": int(r["cv"]) / 1e6,
                       "capacity_floor_after": r["capacity_floor_after"], "retained_s": int(r["retained_us"]) / 1e6,
                       "history_bytes": acct[r["batch"]], "accepted": r["accepted"], "conflicts": r["conflicts"],
                       "rejected_window": r["rejected_window"], "rejected_memory": r["rejected_memory"],
                       "offered_hash": r["offered_hash"], "decisions_hash": r["decisions_hash"]})
write("longtrace-amd-6gib-per-batch.csv", lt)
json.dump(manifest, open(os.path.join(DATA, "campaign-manifests.json"), "w"), indent=1)
print({k: len(v) for k, v in (("lt", lt),)})
