#!/bin/bash
E=$ROOT/scratch/eval-v2-amd; D=$E/diag-writes; H=$BUILDS; R=$E/scripts/runone.sh
export CPU=5 SIB=13
one() { local name=$1 pat=$2; shift 2
  ( "$R" $D/runs $name "$@" > /dev/null 2>&1 & )
  for i in $(seq 1 400); do p=$(pgrep -n -x "$pat"); [ -n "$p" ] && break; sleep 0.02; done
  sudo -n strace -f -e trace=openat,write,writev,pwrite64,rename,unlink,fsync,fdatasync -o $D/$name.strace -p $p 2>/dev/null
  for i in $(seq 1 600); do [ -f $D/runs/$name/run.txt ] && break; sleep 0.5; done
  echo "$name pid=$p opened_for_write: $(grep -E 'openat.*O_(WRONLY|RDWR|CREAT)' $D/$name.strace | grep -o '"[^"]*"' | sort | uniq -c | tr '\n' ' ') | writes by fd: $(grep -oE '(write|writev|pwrite64)\([0-9]+' $D/$name.strace | sort | uniq -c | tr '\n' ' ') | fsync: $(grep -c 'fsync\|fdatasync' $D/$name.strace)"
}
for n in fdb v1 v2; do one cs-$n fdbserver $H/$n/bin/fdbserver -r unittests -f noSim/fdbserver/conflictset/benchmark --test_workload=points --test_seed=1 --test_batches=200 --test_warmupBatches=100 --knob_paged_resolver_pool_pages=16384 -L /out; done
for n in fdb v1 v2; do one mb-$n fdbserver $H/$n/bin/fdbserver -r unittests -f noSim/fdbserver/conflictset/macBenchmark --test_workload=points --test_batches=300 --knob_paged_resolver_pool_pages=16384 -L /out; done
export DOCKER_ENV="-e SKIPLISTTEST_CPU=5 -e SKIPLISTTEST_KEY_BYTES=16 -e SKIPLISTTEST_KEY_LAYOUT=head -e SKIPLISTTEST_PER_BATCH=5000 -e SKIPLISTTEST_RANGE_SPAN=10"
for n in fdb v1 v2; do one sl-$n fdbserver $H/$n/bin/fdbserver -r skiplisttest -C $E/scripts/fdb.cluster -s 1 --knob_paged_resolver_pool_pages=16384; done
unset DOCKER_ENV
for n in v1 v2; do one sa-$n blocksizebench $E/standalone/$n/blocksizebench points 300 1024; done
