#!/bin/bash
C=$CAMPAIGNS/2026-11-04-pub3-hp; TRACE=$BTPROF/traces7200-copy/pilot-2000M-l0-7200.trace
log() { echo "$(date -Is) $*" >> $C/progress.log; }
drain() { sync; for i in $(seq 1 60); do d=$(awk '/^(Dirty|Writeback):/{s+=$2} END{print s}' /proc/meminfo); [ "$d" -lt 1024 ] && break; sleep 1; done; echo "drain $(date -Is) dirty+writeback_kib=$d" >> $C/progress.log; }
run() { local BIN=$1 OUT=$2 MODE=$3 CPU=$4
  [ -e $OUT ] && { echo "exists: $OUT"; return 1; }; mkdir -p $OUT
  local knobs="--knob_paged_resolver_pool_pages=${POOLV:-98300}"
  local envp="-e BY5_SEGMENT_START=57600 -e BY5_SEGMENT_BATCHES=${SEGB:-172800} -e BY5_PREHISTORY_BATCHES=57600 -e BY5_PRIME=0 -e BY5_FICT_MODEL=0 -e BY5_VALIDATE=${VALIDATE:-0} -e BY5_PROCESS_SEED=1 -e BY5_FICT_SEED=1 -e BY5_FICT_LEAF=0"
  echo "$BIN arm=v38 mode=$MODE cpu=$CPU sha=$(sha256sum $BIN | cut -d' ' -f1) kernel=$(uname -r) hugepages=$(cat /proc/sys/vm/nr_hugepages) load=$(cut -d' ' -f1-3 /proc/loadavg) start=$(date -Is) knobs=[$knobs] env=[$envp]" > $OUT/run.txt
  docker run --rm --cpuset-cpus=$CPU --cpuset-mems=0 -e RESOLVER1_PAGE_MODE=hugetlb -v /usr/bin/time:/usr/bin/time:ro -e BY5_TRACE=$TRACE -e BY5_CPU=$CPU -e BY5_BATCH_LOG=$OUT/batches.csv $envp -v $FDB_ROOT:$FDB_ROOT -w $OUT foundationdb/build:rockylinux9-latest bash -c "echo x:y@127.0.0.1:1 > dummy.cluster; export LD_LIBRARY_PATH=\$(dirname \$(/usr/local/bin/clang++ -stdlib=libc++ -print-file-name=libc++.so.1)); /usr/bin/time -v -o $OUT/time.txt $BIN -r by5resolvertest -C dummy.cluster -s 1 $knobs" > $OUT/out.txt 2>&1 < /dev/null
  local rc=$?; echo "end=$(date -Is) exit=$rc" >> $OUT/run.txt; sync; }
NB=$C/bin/v38-nativemem/fdbserver; AB=$C/bin/v38-acct/fdbserver
echo "long start $(date -Is) kernel=$(uname -r) load=$(cut -d' ' -f1-3 /proc/loadavg) nr_hugepages=$(cat /proc/sys/vm/nr_hugepages) native=$(sha256sum $NB | cut -c1-64) acct=$(sha256sum $AB | cut -c1-64) trace=$(stat -c %s $TRACE)" >> $C/env.txt
V=$C/runs/by5/validation; mkdir -p $V
SEGB=6400 VALIDATE=1 run $NB $V/v38 native 8 & SEGB=6400 VALIDATE=1 POOLV=300 run $NB $V/v38-p300 native 9 & SEGB=6400 VALIDATE=1 run $AB $V/v38-acct acct 10 & wait
log "by5 validation done"
drain; log "by5 acct start"; run $AB $C/runs/by5/acct-v38 acct 6; log "by5 acct end $(tail -1 $C/runs/by5/acct-v38/run.txt)"
for r in 1 2 3; do drain; log "by5 round$r start"; run $NB $C/runs/by5/round$r-v38 native 6; log "by5 round$r end $(tail -1 $C/runs/by5/round$r-v38/run.txt)"; done
echo "long end $(date -Is) kernel=$(uname -r) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> $C/env.txt; log "long done"
