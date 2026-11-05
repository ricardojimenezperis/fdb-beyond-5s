#!/bin/bash
C=$CAMPAIGNS/2026-11-04-pub3-hp
BIN=$C/bin/fdbserver-v38; L=$C/lib
log() { echo "$(date -Is) $*" >> $C/progress.log; }
drain() { sync; for i in $(seq 1 120); do d=$(awk '/^(Dirty|Writeback):/{s+=$2} END{print s}' /proc/meminfo); [ "$d" -lt 4096 ] && return 0; sleep 0.5; done; }
echo "skiplisttest head16 rerun start $(date -Is) kernel=$(uname -r) hugepages=$(cat /proc/sys/vm/nr_hugepages) free=$(awk '/HugePages_Free/{print $2}' /proc/meminfo) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> $C/env.txt
sl() { local conf=$1 tag=$2; local layout=${conf%%[0-9]*}; local bytes=${conf#$layout}; local O=$C/runs/skiplisttest/$conf-$tag; local S=/dev/shm/p3-sl-$conf-$tag
  [ -e $O ] && { echo "exists $O"; return; }; mkdir -p $O; rm -rf $S; mkdir -p $S; drain; cd $S; echo x:y@127.0.0.1:1 > dummy.cluster
  echo "conf=$conf layout=$layout bytes=$bytes P=262144 PER_BATCH=40000 bin=$BIN sha256=$(sha256sum $BIN | cut -c1-64) cpu=6 kernel=$(uname -r) load=$(cut -d' ' -f1-3 /proc/loadavg) start=$(date -Is) knobs=[--knob_paged_resolver_pool_pages=262144]" > $O/run.txt
  env RESOLVER1_PAGE_MODE=hugetlb LD_LIBRARY_PATH=$L SKIPLISTTEST_CPU=6 SKIPLISTTEST_KEY_BYTES=$bytes SKIPLISTTEST_KEY_LAYOUT=$layout SKIPLISTTEST_PER_BATCH=40000 taskset -c 6 /usr/bin/time -v -o $S/time.txt $BIN -r skiplisttest -C dummy.cluster -s 1 --knob_paged_resolver_pool_pages=262144 -L $S > out.txt 2>&1 < /dev/null
  local rc=$?; cd /; cp -a $S/. $O/; rm -rf $S; echo "end=$(date -Is) exit=$rc" >> $O/run.txt; sync; }
sl head16 warm; for r in 1 2 3; do sl head16 r$r; done; log "skiplisttest head16 rerun done"
