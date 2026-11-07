#!/usr/bin/env python3
import csv
import re
import statistics
import sys
from pathlib import Path

C = Path(__file__).resolve().parent.parent
INTEL = C / "intel-reference/2026-10-31-cpu6/out/chase-output.txt"
MEASURED = ("m1-primary", "m2-replicate", "m3-replicate", "m4-replicate", "m5-replicate")
RUNS = {n: C / "runs" / n for n in ("d2-chain",) + MEASURED}
NAMED = [("4096 B", 4096), ("27520 B", 27520), ("32704 B", 32704), ("38912 B", 38912), ("46336 B", 46336),
         ("1 MiB, 1048512 B", 1048512), ("16 MiB, 16777152 B", 16777152), ("512 MiB, 536870848 B", 536870848)]
problems = []


def parse(path):
    head, rows, tail = None, [], None
    for line in path.read_text().splitlines():
        if line.startswith("# cpu="):
            head = dict(re.findall(r"(\w+)=(\S+)", line))
        elif line.startswith("# anon_huge_kb_at_end"):
            tail = int(line.split("=")[1])
        elif line and line[0].isdigit():
            f = line.split(",")
            rows.append((int(f[0]), int(f[1]), [float(x) for x in f[2:7]], float(f[7])))
    return head, rows, tail


def section(path, size_kb=524288):
    block, found = [], False
    for line in path.read_text().splitlines():
        if re.match(r"^[0-9a-f]+-[0-9a-f]+ ", line):
            if found:
                break
            block = [line]
        else:
            block.append(line)
            if re.match(rf"^Size:\s+{size_kb} kB", line):
                found = True
    return block if found else None


def check_mapping(name, block):
    fields = dict(re.match(r"(\w+):\s+(.*)", l).groups() for l in block[1:] if re.match(r"(\w+):\s+", l))
    want = {"KernelPageSize": "4 kB", "MMUPageSize": "4 kB", "AnonHugePages": "0 kB", "Shared_Hugetlb": "0 kB",
            "Private_Hugetlb": "0 kB", "Rss": "524288 kB"}
    for k, v in want.items():
        if fields.get(k) != v:
            problems.append(f"{name}: {k}={fields.get(k)}")
    flags = fields.get("VmFlags", "").split()
    if "ht" in flags or "hg" in flags or "nh" not in flags:
        problems.append(f"{name}: VmFlags {flags}")
    print(f"{name}: " + " ".join(f"{k}={fields.get(k)}" for k in want) + f" VmFlags={' '.join(flags)}")


ih, irows, itail = parse(INTEL)
isizes = [(r[0], r[1]) for r in irows]
print(f"intel: rows={len(irows)} sink={ih['sink']} anon_after_touch={ih['anon_huge_kb_after_touch']} anon_end={itail}")

data = {}
for name, d in RUNS.items():
    h, rows, tail = parse(d / "stdout.txt")
    data[name] = (h, rows, tail)
    run = dict(l.split("=", 1) for l in (d / "run.txt").read_text().splitlines() if "=" in l and not l.startswith("command"))
    end_exit = re.search(r"exit=(\d+)", (d / "run.txt").read_text()).group(1)
    time_txt = (d / "time.txt").read_text()
    print(f"\n{name}: exit={end_exit} time_exit={re.search(r'Exit status: (\d+)', time_txt).group(1)} "
          f"running={h['running']} rows={len(rows)} sink={h['sink']} anon_after_touch={h['anon_huge_kb_after_touch']} anon_end={tail}")
    print(f"  stderr_bytes={(d / 'stderr.txt').stat().st_size} start={run['start']} cpu7_busy={run['cpu7_busy']} "
          f"cpu15_busy={run['cpu15_busy']} docker={run['docker_containers_before']}/{run['docker_containers_after']} "
          f"load_before={run['load_before']} freq_khz={run['freq_khz_before']}->{run['freq_khz_after']}")
    print("  " + " ".join(re.search(rf"{k}.*", time_txt).group(0) for k in ("Elapsed",)) + "; "
          + "; ".join(re.search(rf"{k}.*", time_txt).group(0) for k in ("Voluntary context", "Involuntary context", "Major", "Minor", "File system outputs")))
    if end_exit != "0":
        problems.append(f"{name}: exit {end_exit}")
    if (d / "stderr.txt").stat().st_size:
        problems.append(f"{name}: stderr not empty")
    if h["running"] != "7" or h["cpu"] != "7":
        problems.append(f"{name}: cpu {h['cpu']} running {h['running']}")
    if h["anon_huge_kb_after_touch"] != "0" or tail != 0:
        problems.append(f"{name}: anon huge")
    if [(r[0], r[1]) for r in rows] != isizes:
        problems.append(f"{name}: sizes or accesses differ from Intel")
    for r in rows:
        if abs(statistics.median(r[2]) - r[3]) > 1e-9:
            problems.append(f"{name}: median at {r[0]}")
    delta = int(h["sink"]) - int(ih["sink"])
    print(f"  sink-intel_sink={delta} mod(69*4096)={delta % (69 * 4096)}")
    if delta % (69 * 4096):
        problems.append(f"{name}: sink not congruent with Intel")

m1 = data["m1-primary"]

d = RUNS["d2-chain"]
lines = (d / "chain.csv").read_text().splitlines()
print("\n" + lines[0])
buf = int(re.search(r"buf=(\d+)", lines[0]).group(1))
calls = int(re.search(r"monotonic_calls=(\d+)", lines[0]).group(1))
chain = list(csv.DictReader(lines[1:]))
ok = 0
for row, ir in zip(chain, irows):
    good = (int(row["visited_bytes"]) == ir[0] and int(row["max_off"]) == ir[0] - 64 and row["closed"] == "1"
            and row["distinct"] == "1" and row["closed_last"] == "1" and row["distinct_last"] == "1"
            and row["hash_first"] == row["hash_last"] and row["visited_last"] == row["visited"])
    ok += good
    if not good:
        problems.append(f"chain bad at {ir[0]}: {row}")
print(f"chain walks: sizes_ok={ok} of {len(irows)} rows_reported={len(chain)} monotonic_calls={calls} expected={10 * len(irows)}")
if calls != 10 * len(irows) or len(chain) != len(irows):
    problems.append("chain walk count")
offsets = int(data["d2-chain"][0]["sink"]) - 69 * buf
print(f"sum of final offsets (sink-69*buf)={offsets}")
implied = (int(ih["sink"]) - offsets) // 69
print(f"intel_sink-offsets mod 69*4096={(int(ih['sink']) - offsets) % (69 * 4096)} implied intel buf={implied:#x}")
if (int(ih["sink"]) - offsets) % (69 * 4096):
    problems.append("sink offsets differ from Intel")

print("\nmapping:")
for snap in sorted((C / "runs/d1-smaps").glob("snapshot-t*")):
    check_mapping(f"d1 {snap.name}", section(snap / "smaps.txt"))
check_mapping("d2 512 MiB case of the 69-size sweep", section(d / "smaps-512mib-case.txt"))

i2 = {r[0]: r for r in irows}
by = {n: {r[0]: r for r in data[n][1]} for n in RUNS}
m1d = by["m1-primary"]
print("\nall sizes, ns/access: m1 repetitions and median, then the median of each other sweep and Intel's:")
print("| bytes | m1 rep1 | rep2 | rep3 | rep4 | rep5 | m1 median | m2 | m3 | m4 | m5 | d2 (validator) | Intel |")
print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for r in m1[1]:
    b = r[0]
    others = " | ".join(f"{by[n][b][3]:.3f}" for n in MEASURED[1:])
    print(f"| {b} | " + " | ".join(f"{x:.3f}" for x in r[2]) + f" | {r[3]:.3f} | {others} | {by['d2-chain'][b][3]:.3f} | {i2[b][3]:.3f} |")

print("\nnamed sizes: m1 repetitions, spread within m1, medians of the five sweeps and their spread, d2, Intel:")
for label, b in NAMED:
    r = m1d[b]
    meds = [by[n][b][3] for n in MEASURED]
    print(f"{label}: m1 reps={r[2]} median={r[3]:.3f} within={(max(r[2]) - min(r[2])) / r[3] * 100:.2f}% "
          f"five_medians={meds} median_of_five={statistics.median(meds):.3f} between={(max(meds) - min(meds)) / statistics.median(meds) * 100:.2f}% "
          f"d2={by['d2-chain'][b][3]:.3f} intel={i2[b][3]:.3f}")

between = sorted(((max(by[n][b][3] for n in MEASURED) - min(by[n][b][3] for n in MEASURED)) / statistics.median(by[n][b][3] for n in MEASURED) * 100, b) for b, _ in isizes)
print("\nlargest spreads between the five sweeps, percent of the median:")
for v, b in between[-8:]:
    print(f"  {b}: {v:.2f}%")
print(f"median of the spreads over 69 sizes: {statistics.median(v for v, _ in between):.2f}%")
wd = [(by["d2-chain"][b][3], min(by[n][b][3] for n in MEASURED), max(by[n][b][3] for n in MEASURED), b) for b, _ in isizes]
print("sizes where d2's median falls outside the five sweeps by more than 1%:")
print("  " + ", ".join(f"{b} ({d:.3f} vs {lo:.3f}-{hi:.3f})" for d, lo, hi, b in wd if d < lo * 0.99 or d > hi * 1.01))

print("\nratio 512 MiB / L1-resident, unrounded medians at 27520 B:")
print(f"intel={i2[536870848][3]:.3f}/{i2[27520][3]:.3f}={i2[536870848][3] / i2[27520][3]:.1f}")
print(f"amd m1={m1d[536870848][3]:.3f}/{m1d[27520][3]:.3f}={m1d[536870848][3] / m1d[27520][3]:.1f}")
for n in MEASURED[1:]:
    print(f"amd {n}={by[n][536870848][3] / by[n][27520][3]:.1f}")
print(f"rounded table values: intel 163.2/0.70={163.2 / 0.70:.1f} amd 103.8/0.75={103.8 / 0.75:.1f}")
print(f"\nproblems: {'; '.join(problems) if problems else 'none'}")
sys.exit(1 if problems else 0)
