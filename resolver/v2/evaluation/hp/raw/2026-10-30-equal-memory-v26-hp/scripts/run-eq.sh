#!/bin/bash
# usage: run-eq.sh <bin> <outdir> <acct|native> <fdb265|v107|v1|v2> <cpu>   env: SEGB (default 172800), VALIDATE (0), POOLV (98300)
BIN=$1; OUT=$2; MODE=$3; ARM=$4; CPU=$5; TRACE=/data/fdb/scratch/btprof/traces7200-copy/pilot-2000M-l0-7200.trace
[ -e $OUT ] && { echo "exists: $OUT"; exit 1; }
mkdir -p $OUT
case $ARM in
 fdb265) knobs="--knob_resolver_use_paged_conflict_set=0"; arm="-e BY5_WINDOW_US=265000000 -e BY5_FICT_LEAF=0";;
 v107) knobs="--knob_resolver_use_paged_conflict_set=1 --knob_paged_resolver_use_btree=1 --knob_paged_resolver_pool_pages=97696 --knob_paged_btree_reader_group_size=3 --knob_paged_resolver_pool_backing=both"; arm="-e BY5_FICT_LEAF=1 -e BY5_FICT_ROOT=20 -e BY5_FICT_SPLIT=20 -e BY5_FICT_PRESSURE_LIMIT=23 -e BY5_FICT_RECLAIMABLE_PRIORITY=1 -e BY5_FICT_STARTUP_ONLY=0";;
 v1|v2) knobs="--knob_resolver_use_paged_conflict_set=0 --knob_paged_resolver_pool_pages=${POOLV:-98300}"; arm="-e BY5_FICT_LEAF=0";;
 *) echo arm?; exit 1;;
esac
envp="-e BY5_SEGMENT_START=57600 -e BY5_SEGMENT_BATCHES=${SEGB:-172800} -e BY5_PREHISTORY_BATCHES=57600 -e BY5_PRIME=0 -e BY5_FICT_MODEL=0 -e BY5_VALIDATE=${VALIDATE:-0} -e BY5_PROCESS_SEED=1 -e BY5_FICT_SEED=1"
case $MODE in
 acct) logs=""; [ $ARM = v107 ] && logs="-e BY5_FICT_LOG=$OUT/fictitious.csv -e BY5_FICT_PRESSURE=$OUT/pressure.csv -e BY5_BLOCK_LOG=$OUT/blocks.csv";;
 native) logs="";;
 *) echo mode?; exit 1;;
esac
echo "$BIN arm=$ARM mode=$MODE cpu=$CPU sha=$(sha256sum $BIN | cut -d' ' -f1) kernel=$(uname -r) hugepages=$(cat /proc/sys/vm/nr_hugepages) load=$(cut -d' ' -f1-3 /proc/loadavg) start=$(date -Is) knobs=[$knobs] env=[$envp $arm]" > $OUT/run.txt
docker run --rm --cpuset-cpus=$CPU --cpuset-mems=0 -e BY5_TRACE=$TRACE -e BY5_CPU=$CPU -e BY5_BATCH_LOG=$OUT/batches.csv $envp $arm $logs -v /data/fdb:/data/fdb -w $OUT foundationdb/build:rockylinux9-latest bash -c "echo x:y@127.0.0.1:1 > dummy.cluster; LIBCXX=\$(dirname \$(find / -name libc++.so.1 2>/dev/null | head -1)); export LD_LIBRARY_PATH=\$LIBCXX; $BIN -r by5resolvertest -C dummy.cluster -s 1 $knobs" > $OUT/out.txt 2>&1 < /dev/null
rc=$?; echo "end=$(date -Is) exit=$rc" >> $OUT/run.txt
sync
