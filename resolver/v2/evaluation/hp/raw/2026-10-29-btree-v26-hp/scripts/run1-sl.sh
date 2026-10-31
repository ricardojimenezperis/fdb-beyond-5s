#!/bin/bash
# usage: run1.sh <cand: fdb|v107|v1> <conf: head16..tail128> <outdir> [segments]
B=/data/fdb/scratch/btprof/campaigns/2026-10-29-btree-v26-hp; V1=/data/fdb/scratch/btprof/bench-btree-v1; cand=$1; conf=$2; O=$3; seg=${4:-}
P=${P:?P}; PB=${PB:?PB}
layout=${conf%%[0-9]*}; bytes=${conf#$layout}
case $cand in
 fdb) BIN=$V1/build-v107/bin/fdbserver; knobs="";;
 v107) BIN=$V1/build-v107/bin/fdbserver; knobs="--knob_resolver_use_paged_conflict_set=1 --knob_paged_resolver_use_btree=1 --knob_paged_resolver_pool_pages=$P --knob_paged_resolver_pool_backing=normal";;
 v1) BIN=$V1/build-v1/bin/fdbserver; knobs="--knob_paged_resolver_pool_pages=$P";;
 v25) BIN=/data/fdb/scratch/btprof/bench-conflictset-v2/build-v25/bin/fdbserver; knobs="--knob_paged_resolver_pool_pages=$P";;
 v26) BIN=/data/fdb/scratch/btprof/bench-v26-macbench/build-v26/bin/fdbserver; knobs="--knob_paged_resolver_pool_pages=$P";;
 *) echo cand?; exit 1;;
esac
[ -e $O ] && { echo "exists $O"; exit 1; }; mkdir -p $O; S=/dev/shm/sl-$(basename $O); rm -rf $S; mkdir -p $S; cd $S; echo x:y@127.0.0.1:1 > dummy.cluster
echo "cand=$cand conf=$conf layout=$layout bytes=$bytes P=$P PER_BATCH=$PB segments=${seg:-none} bin=$BIN sha256=$(sha256sum $BIN | cut -d' ' -f1) cpu=6 kernel=$(uname -r) load=$(cut -d' ' -f1-3 /proc/loadavg) start=$(date -Is) knobs=[$knobs]" > $O/run.txt
env BTREE_V2_TASKS=16 SKIPLISTTEST_CPU=6 SKIPLISTTEST_KEY_BYTES=$bytes SKIPLISTTEST_KEY_LAYOUT=$layout SKIPLISTTEST_PER_BATCH=$PB ${seg:+SKIPLISTTEST_SEGMENTS=$seg} taskset -c 6 /usr/bin/time -v -o $S/time.txt $BIN -r skiplisttest -C dummy.cluster -s 1 $knobs -L $S > out.txt 2>&1 < /dev/null
rc=$?; cd /; cp -a $S/. $O/; rm -rf $S; echo "end=$(date -Is) exit=$rc" >> $O/run.txt; sync
