#!/bin/bash
set -u
OUT=$1; shift; CFGS=${*:-head16 head32 tail32 head64 tail64 head128 tail128}
E=$ROOT/scratch/eval-v2-amd; R=$E/scripts/runone.sh
BATCHES_NOTE=${BATCHES_NOTE:-500}
[ -e "$OUT" ] || mkdir -p "$OUT"
POOL=--knob_paged_resolver_pool_pages=262144
cmd() { echo "$BUILDS/$1/bin/fdbserver -r skiplisttest -C $ROOT/scratch/eval-v2-amd/scripts/fdb.cluster -s 1 $([ $1 = fdb ] || echo $POOL)"; }
[ -f "$OUT/MANIFEST" ] || {
	echo "test=skiplisttest start=$(date -Is) batches=$BATCHES_NOTE perBatch=40000 span=10 seed=1 pool=262144"
	for n in fdb v1 v2; do echo "bin $n $(sha256sum $BUILDS/$n/bin/fdbserver | cut -d' ' -f1) source $(git -C $E/wt-$n rev-parse HEAD) harness_patch $(sha256sum < $E/harness2-$n.patch | cut -d' ' -f1)"; done
	(cd $E/scripts && sha256sum *.sh)
} > "$OUT/MANIFEST"
ORDERS=("fdb v1 v2" "v1 v2 fdb" "v2 fdb v1")
for cfg in $CFGS; do
	case $cfg in head16) KB=16; KL=head;; head32) KB=32; KL=head;; tail32) KB=32; KL=tail;; head64) KB=64; KL=head;; tail64) KB=64; KL=tail;; head128) KB=128; KL=head;; tail128) KB=128; KL=tail;; esac
	export DOCKER_ENV="-e SKIPLISTTEST_CPU=7 -e SKIPLISTTEST_KEY_BYTES=$KB -e SKIPLISTTEST_KEY_LAYOUT=$KL -e SKIPLISTTEST_PER_BATCH=40000 -e SKIPLISTTEST_RANGE_SPAN=10"
	for c in fdb v1 v2; do "$R" "$OUT" $cfg-warmup-$c $(cmd $c) >> "$OUT/progress.log" 2>&1; done
	for r in 0 1 2; do
		for c in ${ORDERS[$r]}; do "$R" "$OUT" $cfg-r$((r+1))-$c $(cmd $c) >> "$OUT/progress.log" 2>&1; done
	done
	echo "$cfg done $(date -Is)" >> "$OUT/progress.log"
done
echo "end $(date -Is) cfgs=[$CFGS]" >> "$OUT/MANIFEST"
