#!/bin/bash
C=$CAMPAIGNS/2026-11-04-pub3-hp
BIN=$C/bin/fdbserver-v38; SB=$C/bin/blocksizebench-v38; L=$C/lib
IMG=foundationdb/build:rockylinux9-latest
log() { echo "$(date -Is) $*" >> $C/progress.log; }
drain() { sync; for i in $(seq 1 120); do d=$(awk '/^(Dirty|Writeback):/{s+=$2} END{print s}' /proc/meminfo); [ "$d" -lt 4096 ] && return 0; sleep 0.5; done; echo "drain timeout $d" >> $C/progress.log; }
echo "short start $(date -Is) kernel=$(uname -r) load=$(cut -d' ' -f1-3 /proc/loadavg) fdbserver=$(sha256sum $BIN | cut -c1-64) blocksizebench=$(sha256sum $SB | cut -c1-64)" >> $C/env.txt

mac_sa() { local w=$1 tag=$2; local S=/dev/shm/p3-sa-$tag-$w; rm -rf $S; mkdir -p $S; drain
  local lb=$(cut -d' ' -f1-3 /proc/loadavg)
  RESOLVER1_PAGE_MODE=hugetlb LD_LIBRARY_PATH=$L taskset -c 6 /usr/bin/time -v -o $S/time.txt $SB $w 3000 1024 > $S/out.txt 2> $S/err.txt
  echo "run=$tag workload=$w load_before=$lb $(cat $S/out.txt) maxrss_kib=$(awk -F': ' '/Maximum resident/{print $2}' $S/time.txt)" >> $C/runs/mac-standalone.txt
  mkdir -p $C/runs/mac-standalone; cp -r $S $C/runs/mac-standalone/; rm -rf $S; sync; }
M=$CAMPAIGNS/2026-11-03-v35-hp/input/v3.5-2026-11-03/measurement/capture_backing.py
bk_sa() { local w=$1; RESOLVER1_PAGE_MODE=hugetlb LD_LIBRARY_PATH=$L taskset -c 6 $SB $w 3000 1024 > $C/backing/sa-$w.out 2>&1 & local pid=$!; sleep 2
  python3 $M --pid $pid --pool-bytes 1073741824 --expect hugetlb --require-full-huge --output $C/backing/sa-$w.json > /dev/null 2>&1; wait $pid
  log "backing mac-standalone $w $(python3 -c "import json;print(json.load(open('$C/backing/sa-$w.json')).get('coverage_check_passed'))") $(grep -o 'decisions=[0-9a-f]*' $C/backing/sa-$w.out)"; }
bk_dk() { local test=$1 w=$2 n=$3; local S=/dev/shm/p3-bk-$n-$w; rm -rf $S; mkdir -p $S
  docker run -d --name p3bk --user $(id -u):$(id -g) --log-driver=none --cpuset-cpus=6 --cpuset-mems=0 -e RESOLVER1_PAGE_MODE=hugetlb -v $C/bin:/b:ro -v $S:/out -w /out $IMG bash -c "export LD_LIBRARY_PATH=\$(dirname \$(/usr/local/bin/clang++ -stdlib=libc++ -print-file-name=libc++.so.1)); exec /b/fdbserver-v38 -r unittests -f $test --test_workload=$w --knob_paged_resolver_pool_pages=16384 -L /out > /out/stdout.txt 2>&1" > /dev/null
  sleep 4; local pid=$(pgrep -n -f 'b/fdbserver-v38')
  python3 $M --pid $pid --pool-bytes 1073741824 --expect hugetlb --require-full-huge --output $C/backing/$n-$w.json > /dev/null 2>&1; docker wait p3bk > /dev/null; docker rm p3bk > /dev/null; cp $S/stdout.txt $C/backing/$n-$w.out; rm -rf $S
  log "backing $n $w $(python3 -c "import json;print(json.load(open('$C/backing/$n-$w.json')).get('coverage_check_passed'))") $(grep -o -E 'decisions=[0-9a-f]*|committed=[0-9]*' $C/backing/$n-$w.out | tr '\n' ' ')"; }
mkdir -p $C/backing
for w in points ranges; do bk_sa $w; bk_dk noSim/fdbserver/conflictset/benchmark $w conflictset; bk_dk noSim/fdbserver/conflictset/macBenchmark $w macbench; done
sync
for w in points ranges; do mac_sa $w warm; done
for r in 1 2 3; do for w in points ranges; do mac_sa $w r$r; done; done
log "mac standalone done"

dk() { local test=$1 w=$2 tag=$3 out=$4; local S=/dev/shm/p3-$out-$tag-$w; rm -rf $S; mkdir -p $S; drain
  local lb=$(cut -d' ' -f1-3 /proc/loadavg)
  docker run --rm --log-driver=none --cpuset-cpus=6 --cpuset-mems=0 -e RESOLVER1_PAGE_MODE=hugetlb -v /usr/bin/time:/usr/bin/time:ro -v $C/bin:/b:ro -v $S:/out -w /out $IMG bash -c \
    "export LD_LIBRARY_PATH=\$(dirname \$(/usr/local/bin/clang++ -stdlib=libc++ -print-file-name=libc++.so.1)); exec /usr/bin/time -v -o /out/time.txt /b/fdbserver-v38 -r unittests -f $test --test_workload=$w --knob_paged_resolver_pool_pages=16384 -L /out" > $S/stdout.txt 2> $S/stderr.txt
  local line=$(grep -E '^ConflictSetBenchmark|MacBenchmark index' $S/stdout.txt | tr '\n' ' ')
  echo "run=$tag workload=$w load_before=$lb $line maxrss_kib=$(awk -F': ' '/Maximum resident/{print $2}' $S/time.txt)" >> $C/runs/$out.txt
  mkdir -p $C/runs/$out; cp -r $S $C/runs/$out/; rm -rf $S; sync; }
for w in points ranges; do dk noSim/fdbserver/conflictset/benchmark $w warm conflictset; done
for r in 1 2 3; do for w in points ranges; do dk noSim/fdbserver/conflictset/benchmark $w r$r conflictset; done; done
log "conflictset done"
for w in points ranges; do dk noSim/fdbserver/conflictset/macBenchmark $w warm macbench; done
for r in 1 2 3; do for w in points ranges; do dk noSim/fdbserver/conflictset/macBenchmark $w r$r macbench; done; done
log "macbench done"

echo "short end $(date -Is) kernel=$(uname -r) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> $C/env.txt; log "short done"
