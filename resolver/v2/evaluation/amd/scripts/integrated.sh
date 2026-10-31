#!/bin/bash
set -u
TEST=$1; OUT=$2
E=$ROOT/scratch/eval-v2-amd; R=$E/scripts/runone.sh
[ -e "$OUT" ] && { echo "exists: $OUT"; exit 1; }; mkdir -p "$OUT"
POOL=--knob_paged_resolver_pool_pages=16384
case $TEST in
 conflictset) T=noSim/fdbserver/conflictset/benchmark; PARAMS="--test_seed=1 --test_batches=2000 --test_warmupBatches=1000 --test_transactionsPerBatch=500";;
 macbench) T=noSim/fdbserver/conflictset/macBenchmark; PARAMS="--test_batches=3000";;
 *) echo "test?"; exit 1;;
esac
cmd() {
	case $1 in
	 fdb) echo "$BUILDS/fdb/bin/fdbserver -r unittests -f $T --test_workload=$2 $PARAMS -L /out";;
	 v1) echo "$BUILDS/v1/bin/fdbserver -r unittests -f $T --test_workload=$2 $PARAMS $POOL -L /out";;
	 v2) echo "$BUILDS/v2/bin/fdbserver -r unittests -f $T --test_workload=$2 $PARAMS $POOL -L /out";;
	esac
}
{
	echo "test=$TEST start=$(date -Is)"
	for n in fdb v1 v2; do echo "bin $n $(sha256sum $BUILDS/$n/bin/fdbserver | cut -d' ' -f1) source $(git -C $E/wt-$n rev-parse HEAD) harness_patch $(sha256sum < $E/harness-$n.patch | cut -d' ' -f1)"; done
	(cd $E/scripts && sha256sum *.sh)
} > "$OUT/MANIFEST"
ORDERS=("fdb v1 v2" "v1 v2 fdb" "v2 fdb v1")
for w in points ranges; do
	for c in fdb v1 v2; do "$R" "$OUT" warmup-$w-$c $(cmd $c $w) >> "$OUT/progress.log" 2>&1; done
	for r in 0 1 2; do
		for c in ${ORDERS[$r]}; do "$R" "$OUT" r$((r+1))-$w-$c $(cmd $c $w) >> "$OUT/progress.log" 2>&1; done
	done
done
echo "end $(date -Is)" >> "$OUT/MANIFEST"; echo done >> "$OUT/progress.log"
