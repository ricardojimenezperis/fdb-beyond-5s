#!/bin/bash
C=$CAMPAIGNS/2026-11-05-v38sweep-hp; L=$CAMPAIGNS/2026-11-04-pub3-hp/lib
M=$CAMPAIGNS/2026-11-03-v35-hp/input/v3.5-2026-11-03/measurement/capture_backing.py
log() { echo "$(date -Is) $*" >> $C/progress.log; }
drain() { sync; for i in $(seq 1 120); do d=$(awk '/^(Dirty|Writeback):/{s+=$2} END{print s}' /proc/meminfo); [ "$d" -lt 4096 ] && return 0; sleep 0.5; done; }
hp() { echo "nr=$(cat /proc/sys/vm/nr_hugepages) free=$(awk '/HugePages_Free/{print $2}' /proc/meminfo)"; }
echo "stage1 start $(date -Is) kernel=$(uname -r) hugetlb [$(hp)] load=$(cut -d' ' -f1-3 /proc/loadavg)" >> $C/env.txt
ARMS="2x8 2x10 2x12 3x12 3x20"
for a in $ARMS; do for w in points ranges; do
  RESOLVER1_PAGE_MODE=hugetlb LD_LIBRARY_PATH=$L taskset -c 6 $C/bin/bsb-$a $w 3000 1024 > $C/backing/$a-$w.out 2>&1 & pid=$!; sleep 2
  python3 $M --pid $pid --pool-bytes 1073741824 --expect hugetlb --require-full-huge --output $C/backing/$a-$w.json > /dev/null 2>&1; wait $pid
  log "check $a $w backing=$(python3 -c "import json;print(json.load(open('$C/backing/$a-$w.json')).get('coverage_check_passed'))") $(grep -o -E 'conflicts=[0-9]+|reclaimCV=[0-9]+|decisions=[0-9a-f]+' $C/backing/$a-$w.out | tr '\n' ' ')"
done; done
run() { local a=$1 w=$2 tag=$3; local S=/dev/shm/sw-$a-$w-$tag; rm -rf $S; mkdir -p $S; drain; local lb=$(cut -d' ' -f1-3 /proc/loadavg)
  RESOLVER1_PAGE_MODE=hugetlb LD_LIBRARY_PATH=$L taskset -c 6 /usr/bin/time -v -o $S/time.txt $C/bin/bsb-$a $w 3000 1024 > $S/out.txt 2> $S/err.txt
  echo "run=$tag arm=$a workload=$w load_before=$lb $(cat $S/out.txt) invol=$(awk -F': ' '/Involuntary/{print $2}' $S/time.txt) maxrss_kib=$(awk -F': ' '/Maximum resident/{print $2}' $S/time.txt)" >> $C/runs/runs.txt
  mkdir -p $C/runs/$a-$w-$tag; cp -a $S/. $C/runs/$a-$w-$tag/; rm -rf $S; sync; }
for w in points ranges; do for a in $ARMS; do run $a $w warm; done; done
O1="2x8 2x10 2x12 3x12 3x20"; O2="3x12 3x20 2x8 2x10 2x12"; O3="2x10 2x12 3x12 3x20 2x8"
r=0; for order in "$O1" "$O2" "$O3"; do r=$((r+1)); for w in points ranges; do for a in $order; do run $a $w r$r; done; done; log "round $r [$order] done"; done
echo "stage1 end $(date -Is) kernel=$(uname -r) hugetlb [$(hp)] load=$(cut -d' ' -f1-3 /proc/loadavg)" >> $C/env.txt; log "stage1 done"
