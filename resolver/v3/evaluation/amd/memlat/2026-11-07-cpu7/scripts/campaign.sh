#!/bin/bash
set -u
C=${C:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
BIN=$C/chase
EXPECT=a8c5d3ff83f4ff5a1fc54319a8a5d4d1e3900fb4d86ac22b853c4d8357069673
PRELOAD=$C/validation/chainwalk.so
SHM=/dev/shm/memlat-amd
LOG=$C/runs/progress.log
MIN=4096
MAX=536870912
LIMIT=1500
mkdir -p $SHM

cpustat() { awk -v c="cpu$1" '$1==c{print $2+$3+$4+$7+$8+$9, $2+$3+$4+$5+$6+$7+$8+$9}' /proc/stat; }
busy() { awk -v a="$1" -v b="$2" -v c="$3" -v d="$4" 'BEGIN{printf "%.5f", (d>b)?(c-a)/(d-b):0}'; }
prep() { sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches > /dev/null; echo 1 | sudo -n tee /proc/sys/vm/compact_memory > /dev/null; sync; sleep 10; }
freq() { cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq; }

start_run() {
  NAME=$1
  OUT=$SHM/$NAME
  mkdir -p $OUT
  [ "$(sha256sum $BIN | cut -d' ' -f1)" = "$EXPECT" ] || { echo "binary hash mismatch" >> $LOG; exit 3; }
  echo "$NAME prep $(date -Is)" >> $LOG
  prep
  {
    echo "name=$NAME"
    echo "docker_containers_before=$(docker ps -q | wc -l)"
    echo "load_before=$(cut -d' ' -f1-3 /proc/loadavg)"
    echo "dirty_kib_before=$(awk '/^Dirty:/{print $2}' /proc/meminfo)"
    echo "freq_khz_before=$(freq)"
    echo "nr_hugepages_before=$(cat /proc/sys/vm/nr_hugepages)"
    echo "thp_before=$(cat /sys/kernel/mm/transparent_hugepage/enabled)"
    echo "governor_before=$(cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_governor)"
  } > $OUT/run.txt
  ps -eo pcpu,psr,pid,comm --sort=-pcpu | head -8 > $OUT/top-before.txt
  cat /proc/buddyinfo > $OUT/buddyinfo-before.txt
  cat /proc/interrupts > $OUT/interrupts-before.txt
  S7=($(cpustat 7)); S15=($(cpustat 15))
  echo "start=$(date -Is)" >> $OUT/run.txt
  echo "$NAME start $(date -Is)" >> $LOG
}

finish_run() {
  local rc=$1
  E7=($(cpustat 7)); E15=($(cpustat 15))
  {
    echo "end=$(date -Is) exit=$rc"
    echo "cpu7_busy=$(busy ${S7[0]} ${S7[1]} ${E7[0]} ${E7[1]})"
    echo "cpu15_busy=$(busy ${S15[0]} ${S15[1]} ${E15[0]} ${E15[1]})"
    echo "freq_khz_after=$(freq)"
    echo "load_after=$(cut -d' ' -f1-3 /proc/loadavg)"
    echo "docker_containers_after=$(docker ps -q | wc -l)"
    echo "nr_hugepages_after=$(cat /proc/sys/vm/nr_hugepages)"
  } >> $OUT/run.txt
  cat /proc/interrupts > $OUT/interrupts-after.txt
  mkdir -p $C/runs/$NAME
  cp -a $OUT/. $C/runs/$NAME/
  sync
  echo "$NAME end $(date -Is) exit=$rc" >> $LOG
}

measured() {
  start_run $1
  echo "command=timeout $LIMIT /usr/bin/time -v -o OUT/time.txt taskset -c 7 numactl --membind=0 $BIN 7 $MIN $MAX" >> $OUT/run.txt
  timeout $LIMIT /usr/bin/time -v -o $OUT/time.txt taskset -c 7 numactl --membind=0 $BIN 7 $MIN $MAX > $OUT/stdout.txt 2> $OUT/stderr.txt
  finish_run $?
}

diag_smaps() {
  start_run $1
  echo "command=timeout $LIMIT /usr/bin/time -v -o OUT/time.txt taskset -c 7 numactl --membind=0 $BIN 7 $MAX $MAX" >> $OUT/run.txt
  timeout $LIMIT /usr/bin/time -v -o $OUT/time.txt taskset -c 7 numactl --membind=0 $BIN 7 $MAX $MAX > $OUT/stdout.txt 2> $OUT/stderr.txt &
  RUNPID=$!
  PID=""
  for i in $(seq 1 600); do
    PID=$(pgrep -x chase | head -1)
    if [ -n "$PID" ]; then
      R=$(awk '/^VmRSS:/{print $2}' /proc/$PID/status 2>/dev/null)
      [ -n "$R" ] && [ "$R" -ge 524288 ] && break
    fi
    sleep 0.1
  done
  T0=$(date +%s.%N)
  for at in 3 9 14; do
    sleep $(awk -v a="$at" -v t0="$T0" -v n="$(date +%s.%N)" 'BEGIN{d=a-(n-t0); print (d>0)?d:0}')
    [ -d /proc/$PID ] || break
    D=$OUT/snapshot-t$at
    mkdir -p $D
    date -Is > $D/time.txt
    cat /proc/$PID/smaps > $D/smaps.txt
    cat /proc/$PID/numa_maps > $D/numa_maps.txt
    cat /proc/$PID/status > $D/status.txt
    cat /proc/$PID/stat > $D/stat.txt
    taskset -cp $PID > $D/affinity.txt
    tr '\0' ' ' < /proc/$PID/cmdline > $D/cmdline.txt
    grep -E 'HugePages|Hugetlb|AnonHuge' /proc/meminfo > $D/meminfo.txt
  done
  wait $RUNPID
  finish_run $?
}

diag_chain() {
  start_run $1
  ( while :; do echo "$(date +%s) $(freq)"; sleep 1; done ) > $OUT/freq-cpu7.txt &
  SAMP=$!
  echo "command=timeout $LIMIT /usr/bin/time -v -o OUT/time.txt taskset -c 7 numactl --membind=0 env LD_PRELOAD=$PRELOAD CHAINWALK_BYTES=$MAX CHAINWALK_OUT=OUT/chain.csv CHAINWALK_SMAPS=OUT/smaps-512mib-case.txt $BIN 7 $MIN $MAX" >> $OUT/run.txt
  timeout $LIMIT /usr/bin/time -v -o $OUT/time.txt taskset -c 7 numactl --membind=0 env LD_PRELOAD=$PRELOAD CHAINWALK_BYTES=$MAX CHAINWALK_OUT=$OUT/chain.csv CHAINWALK_SMAPS=$OUT/smaps-512mib-case.txt $BIN 7 $MIN $MAX > $OUT/stdout.txt 2> $OUT/stderr.txt
  RC=$?
  kill $SAMP
  finish_run $RC
}

echo "campaign start $(date -Is)" > $LOG
diag_smaps d1-smaps
diag_chain d2-chain
measured m1-primary
measured m2-replicate
echo 3 | sudo -n tee /proc/sys/vm/drop_caches > /dev/null
echo "campaign done $(date -Is)" >> $LOG
