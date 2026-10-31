#!/bin/bash
set -euo pipefail
N=$(cd "$(dirname "$0")/.." && pwd)
cd "$N"
OUT=out/runs.txt
drain() { for i in $(seq 1 120); do d=$(awk '/^(Dirty|Writeback):/ {s += $2} END {print s}' /proc/meminfo); [ "$d" -lt 4096 ] && return 0; sleep 0.5; done; }
one() {
	local kind=$1 round=$2 workload=$3 v=$4
	sync; drain
	local lb; lb=$(cut -d' ' -f1-3 /proc/loadavg)
	local line; line=$(taskset -c 6 bin/blocksizebench-$v $workload 3000 1024); local rc=$?
	echo "kind=$kind round=$round version=$v load_before=${lb// /,} exit=$rc $line" >> $OUT
}
echo "start $(date -Is) kernel=$(uname -r) load=$(cut -d' ' -f1-3 /proc/loadavg) cpu=6" >> out/env.txt
for workload in points ranges; do for v in v1-5a07ab1936 v2.6-g16; do one warm 0 $workload $v; done; done
for round in 1 2 3; do
	if [ $((round % 2)) = 1 ]; then order="v1-5a07ab1936 v2.6-g16"; else order="v2.6-g16 v1-5a07ab1936"; fi
	for workload in points ranges; do for v in $order; do one timed $round $workload $v; done; done
done
echo "end $(date -Is) kernel=$(uname -r)" >> out/env.txt
sync
