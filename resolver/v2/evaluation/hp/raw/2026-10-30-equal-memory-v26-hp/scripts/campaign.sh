#!/bin/bash
C=/data/fdb/scratch/btprof/campaigns/2026-10-30-equal-memory-v26-hp; R=$C/scripts/run-eq.sh
drain() { sync; for i in $(seq 1 60); do d=$(awk '/^(Dirty|Writeback):/{s+=$2} END{print s}' /proc/meminfo); [ "$d" -lt 1024 ] && break; sleep 1; done; echo "drain $(date -Is) dirty+writeback_kib=$d" >> $C/progress.log; }
{ echo "campaign start $(date -Is) kernel=$(uname -r) load=$(cut -d' ' -f1-3 /proc/loadavg) nr_hugepages=$(cat /proc/sys/vm/nr_hugepages)"; for d in $C/bin/*; do echo "bin $(basename $d) $(sha256sum $d/fdbserver | cut -d' ' -f1)"; done; echo "source 22a8ae9fba943b73cf149d21b05dfe6b07c6e013 (btree-v2.6) full build $(sha256sum /data/fdb/scratch/btprof/bench-v26-macbench/build-v26/bin/fdbserver | cut -d' ' -f1)"; echo "trace $(stat -c %s /data/fdb/scratch/btprof/traces7200-copy/pilot-2000M-l0-7200.trace)"; } >> $C/env.txt
V=$C/runs/validation; mkdir -p $V
SEGB=6400 VALIDATE=1 $R $C/bin/v26-nativemem/fdbserver $V/v26 native v2 8 & SEGB=6400 VALIDATE=1 POOLV=300 $R $C/bin/v26-nativemem/fdbserver $V/v26-p300 native v2 9 & SEGB=6400 VALIDATE=1 $R $C/bin/v26-acct/fdbserver $V/v26-acct acct v2 10 & wait
echo "validation done $(date -Is)" >> $C/progress.log
drain; echo "acct start $(date -Is)" >> $C/progress.log; $R $C/bin/v26-acct/fdbserver $C/runs/acct-v26 acct v2 6; echo "acct end $(date -Is) $(tail -1 $C/runs/acct-v26/run.txt)" >> $C/progress.log
for r in 1 2 3; do drain; echo "round$r start $(date -Is)" >> $C/progress.log; $R $C/bin/v26-nativemem/fdbserver $C/runs/round$r-v26 native v2 6; echo "round$r end $(date -Is) $(tail -1 $C/runs/round$r-v26/run.txt)" >> $C/progress.log; done
echo "campaign end $(date -Is) kernel=$(uname -r)" >> $C/env.txt
