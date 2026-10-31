#!/bin/bash
set -u
OUT=$1
E=$ROOT/scratch/eval-v2-amd; R=$E/scripts/runone.sh; S=$E/standalone
[ -e "$OUT" ] && { echo "exists: $OUT"; exit 1; }; mkdir -p "$OUT"
{
	echo "test=standalone start=$(date -Is) compile=[$(cat $S/COMPILE.txt)]"
	for n in v1 v2; do echo "bin $n $(sha256sum $S/$n/blocksizebench | cut -d' ' -f1) source $(git -C $E/wt-$n rev-parse HEAD) bench $(sha256sum $S/$n/resolver1/bench/blocksizebench.cpp | cut -d' ' -f1)"; done
	(cd $E/scripts && sha256sum *.sh)
} > "$OUT/MANIFEST"
ORDERS=("v1 v2" "v2 v1" "v1 v2")
for w in points ranges; do
	for c in v1 v2; do "$R" "$OUT" warmup-$w-$c $S/$c/blocksizebench $w 3000 1024 >> "$OUT/progress.log" 2>&1; done
	for r in 0 1 2; do
		for c in ${ORDERS[$r]}; do "$R" "$OUT" r$((r+1))-$w-$c $S/$c/blocksizebench $w 3000 1024 >> "$OUT/progress.log" 2>&1; done
	done
done
echo "end $(date -Is)" >> "$OUT/MANIFEST"; echo done >> "$OUT/progress.log"
