#!/bin/bash
N=/data/fdb/scratch/btprof/campaigns/2026-10-29-btree-v26-hp; B=/data/fdb/scratch/btprof/bench-v26-macbench
until [ $(ls $B/exit-build-*.txt 2>/dev/null | wc -l) -ge 5 ]; do sleep 30; done
[ -z "$(grep -L exit=0 $B/exit-build-*.txt)" ] || { echo "build failed" >> $N/progress.log; exit 1; }
drain() { sync; for i in $(seq 1 60); do d=$(awk '/^(Dirty|Writeback):/{s+=$2} END{print s}' /proc/meminfo); [ "$d" -lt 4096 ] && break; sleep 0.5; done; }
sleep 60
{ echo "campaign start $(date -Is) kernel=$(uname -r) load=$(cut -d' ' -f1-3 /proc/loadavg)"; for f in /data/fdb/scratch/btprof/bench-btree-v1/build-v107 /data/fdb/scratch/btprof/bench-btree-v1/build-v1 /data/fdb/scratch/btprof/bench-conflictset-v2/build-v25 $B/build-v26 $B/build-mb-v107 $B/build-mb-v1 $B/build-mb-v25 $B/build-mb-v26; do echo "bin $f $(sha256sum $f/bin/fdbserver | cut -d' ' -f1)"; done; } >> $N/env.txt
st() { n=$1; bin=$2; test=$3; shift 3; S=/dev/shm/stchk-$n; rm -rf $S; mkdir -p $S; cd $S; BTREE_V2_TASKS=16 strace -f -tt -e trace=openat,write,pwrite64,writev -o $N/strace/$n.txt $bin -r unittests -f $test --test_workload=ranges "$@" -L $S > $S/stdout.txt 2>&1; cp $S/stdout.txt $N/strace/$n-stdout.txt; cd /; rm -rf $S; }
st cs-v26 $B/build-v26/bin/fdbserver noSim/fdbserver/conflictset/benchmark --knob_paged_resolver_pool_pages=16384
st mb-fdb $B/build-mb-v107/bin/fdbserver noSim/fdbserver/conflictset/macBenchmark
st mb-v107 $B/build-mb-v107/bin/fdbserver noSim/fdbserver/conflictset/macBenchmark --knob_resolver_use_paged_conflict_set=1 --knob_paged_resolver_use_btree=1 --knob_paged_resolver_pool_pages=16384 --knob_paged_resolver_pool_backing=normal
st mb-v1 $B/build-mb-v1/bin/fdbserver noSim/fdbserver/conflictset/macBenchmark --knob_paged_resolver_pool_pages=16384
st mb-v25 $B/build-mb-v25/bin/fdbserver noSim/fdbserver/conflictset/macBenchmark --knob_paged_resolver_pool_pages=16384
st mb-v26 $B/build-mb-v26/bin/fdbserver noSim/fdbserver/conflictset/macBenchmark --knob_paged_resolver_pool_pages=16384
echo "strace done $(date -Is)" >> $N/progress.log
drain; $N/scripts/fdbbench5-hp.sh hp-2026-10-29-five 3 fdb v107 v1 v2.5:16 v2.6:16; echo "fdb benchmark done $(date -Is)" >> $N/progress.log
drain; $N/scripts/macbench5-hp.sh hp-2026-10-29-five 3 fdb v107 v1 v2.5:16 v2.6:16; echo "mac benchmark in fdb done $(date -Is)" >> $N/progress.log
M=$N/mac-standalone; drain
docker run --rm --log-driver=none --cpuset-cpus=6 --cpuset-mems=0 -v /data/fdb:/data/fdb -w $M foundationdb/build:rockylinux9-latest bash -c "export LD_LIBRARY_PATH=\$(dirname \$(/usr/local/bin/clang++ -stdlib=libc++ -print-file-name=libc++.so.1)); CXX='/usr/local/bin/clang++ -stdlib=libc++' TASKSET='taskset -c 6' ./run-hp.sh 3 $M/out; sha256sum bin/blocksizebench-v1 bin/blocksizebench-v2.5 bin/blocksizebench-v2.6 > $M/out/binaries.sha256; chown -R $(id -u):$(id -g) $M" > $N/mac-standalone.out 2>&1
echo "standalone mac benchmark done $(date -Is)" >> $N/progress.log
export P=262144 PB=40000; R=$N/scripts/run1-sl.sh; mkdir -p $N/runs/skiplisttest
for conf in head16 head32 tail32 head64 tail64 head128 tail128; do
  for c in fdb v107 v1 v25 v26; do drain; $R $c $conf $N/runs/skiplisttest/$conf-warm-$c; done
  r=1; for order in "fdb v107 v1 v25 v26" "v107 v1 v25 v26 fdb" "v1 v25 v26 fdb v107"; do for c in $order; do drain; $R $c $conf $N/runs/skiplisttest/$conf-r$r-$c; done; r=$((r+1)); done
  echo "skiplisttest $conf done $(date -Is)" >> $N/progress.log
done
echo "campaign end $(date -Is) kernel=$(uname -r)" >> $N/env.txt
