#!/bin/bash
# noSim/fdbserver/conflictset/benchmark with btree-v1 and the btree-v2 versions, one run at a time
# on CPU 7, interleaved by round. Usage: fdbbench.sh <out name> <rounds> <arms...>, an arm being
# v1 or <version>:<tasks>, e.g. v2.4:16.
# Nothing is written to disk while a run is timed: the working directory of the run, its trace
# (-L) and its stdout and stderr are in /dev/shm, and docker keeps no log. After the process exits
# its output is copied to $OUT and synced, and each run first waits for dirty and writeback pages
# to drain.
set -euo pipefail
NAME=$1; ROUNDS=$2; shift 2
ARMS="$*"
OUT=/data/fdb/scratch/btprof/campaigns/2026-10-29-btree-v26-hp/runs/macbench-$NAME
SHM=/dev/shm/macbench-$NAME
mkdir -p "$OUT" "$SHM"
echo "start $(date -Is) kernel=$(uname -r) load=$(cut -d' ' -f1-3 /proc/loadavg) cpu=6 arms=[$ARMS] rounds=$ROUNDS" >> "$OUT/env.txt"
IMG=foundationdb/build:rockylinux9-latest
drain() {
	local d=0
	for i in $(seq 1 120); do
		d=$(awk '/^(Dirty|Writeback):/ {s += $2} END {print s}' /proc/meminfo)
		[ "$d" -lt 4096 ] && return 0
		sleep 0.5
	done
	echo "dirty pages did not drain: ${d} kB" >&2
}
for round in $(seq 1 "$ROUNDS"); do
	for workload in points ranges; do
		for arm in $ARMS; do
			version=${arm%%:*}; tasks=${arm#*:}; [ "$tasks" = "$arm" ] && tasks=32
			knobs="--knob_paged_resolver_pool_pages=16384"; case $version in fdb) bin=/s/btprof/bench-v26-macbench/build-mb-v107/bin/fdbserver; knobs="";; v107) bin=/s/btprof/bench-v26-macbench/build-mb-v107/bin/fdbserver; knobs="--knob_resolver_use_paged_conflict_set=1 --knob_paged_resolver_use_btree=1 --knob_paged_resolver_pool_pages=16384 --knob_paged_resolver_pool_backing=normal";; v1) bin=/s/btprof/bench-v26-macbench/build-mb-v1/bin/fdbserver;; v2) bin=/s/btprof/bench-conflictset-v2/build-v2/bin/fdbserver;; v2.4) bin=/s/btprof/bench-conflictset-v2/build-v24/bin/fdbserver;; v2.5) bin=/s/btprof/bench-v26-macbench/build-mb-v25/bin/fdbserver;; v2.6) bin=/s/btprof/bench-v26-macbench/build-mb-v26/bin/fdbserver;; *) echo "version?"; exit 1;; esac
			run=r$round-$workload-${arm/:/-g}
			mkdir -p "$SHM/$run"
			sync; drain
			lb=$(cut -d' ' -f1-3 /proc/loadavg)
			docker run --rm --log-driver=none --cpuset-cpus=6 --cpuset-mems=0 -e BTREE_V2_TASKS="$tasks" \
				-v /data/fdb/scratch:/s:ro -v "$SHM/$run":/out -w /out "$IMG" bash -c \
				"export LD_LIBRARY_PATH=\$(dirname \$(/usr/local/bin/clang++ -stdlib=libc++ -print-file-name=libc++.so.1)); \
				exec $bin -r unittests -f noSim/fdbserver/conflictset/macBenchmark --test_workload=$workload \
				$knobs -L /out" > "$SHM/$run/stdout.txt" 2> "$SHM/$run/stderr.txt" || true
			line=$(grep -i -E "macbench|conflicts=" "$SHM/$run/stdout.txt" | tr "\n" " " || true); [ -n "$line" ] || line="MacBenchmark line missing"
			cp -r "$SHM/$run" "$OUT/"
			echo "round=$round arm=$arm load_before=$lb $line" >> "$OUT/runs.txt"
			sync
		done
	done
done
echo "end $(date -Is) kernel=$(uname -r)" >> "$OUT/env.txt"
