#!/bin/bash
set -u
P=$EVAL; M=$P/measurement; R=$M/runone.sh; OUT=$P/runs/${RUNDIR:-v38}
mkdir -p $OUT
BIN=$P/builds/v38-a64/bin/fdbserver; SB=$P/standalone/v38/blocksizebench
export DOCKER_OPTS="--privileged"
HP=/sys/devices/system/node/node0/hugepages/hugepages-2048kB
log() { echo "$* $(date -Is)" >> $OUT/session.log; }
restore() { echo 0 | sudo -n tee $HP/nr_hugepages > /dev/null; { date -Iseconds; grep -E 'HugePages_(Total|Free)' /proc/meminfo; echo "nr_hugepages node0 $(cat $HP/nr_hugepages)"; } > $OUT/restored-settings.txt; }
trap restore EXIT
trap 'exit 143' TERM INT HUP
reserve() { echo 0 | sudo -n tee $HP/nr_hugepages > /dev/null; sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches > /dev/null
	sudo -n python3 $M/prepare_memory.py --node 0 --pool-bytes $1 --reserve-hugetlb --output $OUT/reserve-$2.json > $OUT/reserve-$2.log 2>&1; local rc=$?
	log "reserve $2 bytes=$1 rc=$rc nr_hugepages=$(cat $HP/nr_hugepages) free=$(cat $HP/free_hugepages)"; [ $rc = 0 ]; }
prep() { sudo -n python3 $M/prepare_memory.py --node 0 --pool-bytes $1 --output $OUT/prep/$2.json > /dev/null 2>&1; [ $(cat $HP/free_hugepages) -ge $(( $1 / 2097152 )) ]; }
mkdir -p $OUT/prep
want() { case $1 in conflictset-points) echo 'committed=962526 tooOld=0 decisions=41ccf929e519502f';; conflictset-ranges) echo 'committed=164926 tooOld=0 decisions=224f4d48ebf2cb5d';;
	macbench-points|standalone-points) echo 'conflicts=29209 .*decisions=4787dbf4af9b9ca8';; macbench-ranges|standalone-ranges) echo 'conflicts=3479753 .*decisions=df42a6e5c89796ea';; esac; }
check() { local d=$1 pat=$2; grep -qE "$pat" $d/stdout.txt && grep -q 'exit=0' $d/run.txt; }
one() { local blk=$1 name=$2 bytes=$3; shift 3; prep $bytes $blk-$name || { log "HUGETLB SHORT before $blk $name, stopping"; exit 3; }; $R $OUT/$blk $name "$@" >> $OUT/progress.log 2>&1; }
{ echo "v3.8 2x8 benchmark, HugeTLB, start=$(date -Is)"; echo "bin $(sha256sum $BIN | cut -d' ' -f1) standalone $(sha256sum $SB | cut -d' ' -f1)"; (cd $M && sha256sum *.sh *.py)
  echo "image $(docker image inspect foundationdb/build:rockylinux9-latest --format '{{.Id}}') kernel $(uname -r) governor $(cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_governor) thp $(cat /sys/kernel/mm/transparent_hugepage/enabled)"; } >> $OUT/MANIFEST
G=1073741824
reserve $G small || { log "RESERVATION FAILED small, stopping"; exit 3; }
export DOCKER_ENV="-e RESOLVER1_PAGE_MODE=hugetlb"
for blk in conflictset macbench; do
	case $blk in conflictset) T=noSim/fdbserver/conflictset/benchmark; PARAMS="--test_seed=1 --test_batches=2000 --test_warmupBatches=1000 --test_transactionsPerBatch=500";;
	             macbench) T=noSim/fdbserver/conflictset/macBenchmark; PARAMS="--test_batches=3000";; esac
	for w in points ranges; do
		for r in warmup r1 r2 r3 r4 r5; do one $blk $w-$r-v3 $G $BIN -r unittests -f $T --test_workload=$w $PARAMS --knob_paged_resolver_pool_pages=16384 -L /out
			check $OUT/$blk/$w-$r-v3 "$(want $blk-$w)" || { log "DECISION MISMATCH $blk $w $r, stopping"; exit 2; }; done
		log "$blk $w done"
	done
done
for w in points ranges; do
	for r in warmup r1 r2 r3 r4 r5; do one standalone $w-$r-v3 $G $SB $w 3000 1024
		check $OUT/standalone/$w-$r-v3 "$(want standalone-$w)" || { log "DECISION MISMATCH standalone $w $r, stopping"; exit 2; }; done
	log "standalone $w done"
done
S16=17179869184
reserve $S16 skiplist || { log "RESERVATION FAILED skiplist, stopping"; exit 3; }
for cfg in head16 head32 tail32 head64 tail64 head128 tail128; do
	case $cfg in head16) KB=16; KL=head;; head32) KB=32; KL=head;; tail32) KB=32; KL=tail;; head64) KB=64; KL=head;; tail64) KB=64; KL=tail;; head128) KB=128; KL=head;; tail128) KB=128; KL=tail;; esac
	export DOCKER_ENV="-e RESOLVER1_PAGE_MODE=hugetlb -e SKIPLISTTEST_CPU=7 -e SKIPLISTTEST_KEY_BYTES=$KB -e SKIPLISTTEST_KEY_LAYOUT=$KL -e SKIPLISTTEST_PER_BATCH=40000 -e SKIPLISTTEST_RANGE_SPAN=10"
	for r in warmup r1 r2 r3; do one skiplist $cfg-$r-v3 $S16 $BIN -r skiplisttest -C $M/fdb.cluster -s 1 --knob_paged_resolver_pool_pages=262144
		check $OUT/skiplist/$cfg-$r-v3 'decisionsHash=30afc613d78499ea' && grep -q '^7176921 transactions without conflict' $OUT/skiplist/$cfg-$r-v3/stdout.txt || { log "DECISION MISMATCH skiplist $cfg $r, stopping"; exit 2; }; done
	log "skiplist $cfg done"
done
log "session done"
