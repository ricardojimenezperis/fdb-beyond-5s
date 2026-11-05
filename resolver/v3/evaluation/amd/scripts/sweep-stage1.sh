#!/bin/bash
set -u
E=$EVAL; M=$E/measurement; R=$M/runone.sh; OUT=$E/runs/${RUNDIR:-stage1}; mkdir -p $OUT/prep
HP=/sys/devices/system/node/node0/hugepages/hugepages-2048kB
log() { echo "$* $(date -Is)" >> $OUT/session.log; }
restore() { echo 0 | sudo -n tee $HP/nr_hugepages > /dev/null; { date -Iseconds; grep -E 'HugePages_(Total|Free)' /proc/meminfo; } > $OUT/restored-settings.txt; }
trap restore EXIT
trap 'exit 143' TERM INT HUP
export DOCKER_OPTS="--privileged" DOCKER_ENV="-e RESOLVER1_PAGE_MODE=hugetlb"
G=1073741824
{ echo "v3.8 group/size sweep, stage 1, blocksizebench, HugeTLB, start=$(date -Is)"; sha256sum $E/bin/blocksizebench-gate-*; (cd $M && sha256sum *.sh *.py); cat $E/bin/compile.log
  echo "kernel $(uname -r) governor $(cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_governor) thp $(cat /sys/kernel/mm/transparent_hugepage/enabled)"; } > $OUT/MANIFEST
echo 0 | sudo -n tee $HP/nr_hugepages > /dev/null; sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches > /dev/null
sudo -n python3 $M/prepare_memory.py --node 0 --pool-bytes $G --reserve-hugetlb --output $OUT/reserve.json > $OUT/reserve.log 2>&1; rc=$?
log "reserve rc=$rc nr_hugepages=$(cat $HP/nr_hugepages) free=$(cat $HP/free_hugepages)"; [ $rc = 0 ] || { log "RESERVATION FAILED, stopping"; exit 3; }
want() { case $1 in points) echo 'conflicts=29209 reclaimCV=0 decisions=4787dbf4af9b9ca8';; ranges) echo 'conflicts=3479753 reclaimCV=0 decisions=df42a6e5c89796ea';; esac; }
one() { local w=$1 name=$2 cfg=$3
	sudo -n python3 $M/prepare_memory.py --node 0 --pool-bytes $G --output $OUT/prep/$w-$name.json > /dev/null 2>&1
	[ $(cat $HP/free_hugepages) -ge 512 ] || { log "HUGETLB SHORT before $w $name, stopping"; exit 3; }
	$R $OUT/$w $name $E/bin/blocksizebench-gate-v38-$cfg $w 3000 1024 >> $OUT/progress.log 2>&1
	grep -q "$(want $w)" $OUT/$w/$name/stdout.txt && grep -q 'exit=0' $OUT/$w/$name/run.txt && grep -q 'hugetlb_min_free=0 ' $OUT/$w/$name/run.txt || { log "CHECK FAILED $w $name, stopping"; exit 2; }; }
ROUNDS=("2x8 2x10 2x12 3x12 3x20" "3x20 3x12 2x12 2x10 2x8" "2x8 3x20 2x10 3x12 2x12")
for w in points ranges; do
	for c in 2x8 2x10 2x12 3x12 3x20; do one $w warmup-$c $c; done
	for r in 0 1 2; do for c in ${ROUNDS[$r]}; do one $w r$((r+1))-$c $c; done; log "$w round $((r+1)) done"; done
done
log "session done"
