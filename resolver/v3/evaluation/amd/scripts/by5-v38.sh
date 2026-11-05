#!/bin/bash
set -u
P=$EVAL; M=$P/measurement; R=$M/runone.sh; OUT=$P/runs/${RUNDIR:-by5-v38}; W=$P/wt-v38; B=$P/builds/v38-a64
TRACE=$TRACES/pilot-2000M-l0-7200.trace
HP=/sys/devices/system/node/node0/hugepages/hugepages-2048kB
mkdir -p $OUT $P/by5bin
log() { echo "$* $(date -Is)" >> $OUT/session.log; }
restore() { echo 0 | sudo -n tee $HP/nr_hugepages > /dev/null; { date -Iseconds; grep -E 'HugePages_(Total|Free)' /proc/meminfo; } > $OUT/restored-settings.txt; }
trap restore EXIT
trap 'exit 143' TERM INT HUP
for k in nativemem acct; do [ -x $P/by5bin/$k/fdbserver ] || docker run --rm -v $HOME:$HOME -w $B foundationdb/build:rockylinux9-latest bash -c "$W/fdbserver/resolver/by5eval/build.sh $W $B $k $P/by5bin/$k > $P/by5bin/build-$k.log 2>&1"; log "build $k rc=$? sha=$(sha256sum $P/by5bin/$k/fdbserver | cut -d' ' -f1)"; done
[ -x $P/by5bin/nativemem/fdbserver ] && [ -x $P/by5bin/acct/fdbserver ] || { log "BUILD FAILED, stopping"; exit 1; }
echo "trace $(stat -c %s $TRACE) $(cat $SCRATCH/eval-v2-amd/longtrace/TRACE-SHA256.txt | cut -d' ' -f1)" >> $OUT/MANIFEST
(cd $M && sha256sum by5-v38.sh runone.sh rusage.py prepare_memory.py) >> $OUT/MANIFEST
echo 0 | sudo -n tee $HP/nr_hugepages > /dev/null; sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches > /dev/null
sudo -n python3 $M/prepare_memory.py --node 0 --pool-bytes $((3300 * 2097152)) --reserve-hugetlb --output $OUT/reserve.json > $OUT/reserve.log 2>&1; rc=$?
log "reserve rc=$rc nr_hugepages=$(cat $HP/nr_hugepages) free=$(cat $HP/free_hugepages)"; [ $rc = 0 ] || { log "RESERVATION FAILED, stopping"; exit 3; }
export DOCKER_OPTS="--privileged -v $TRACES:$TRACES:ro"
ENV="-e RESOLVER1_PAGE_MODE=hugetlb -e BY5_TRACE=$TRACE -e BY5_CPU=7 -e BY5_BATCH_LOG=/out/batches.csv -e BY5_SEGMENT_START=57600 -e BY5_SEGMENT_BATCHES=${SEGB:-172800} -e BY5_PREHISTORY_BATCHES=57600 -e BY5_PRIME=0 -e BY5_FICT_MODEL=0 -e BY5_VALIDATE=0 -e BY5_PROCESS_SEED=1 -e BY5_FICT_SEED=1 -e BY5_WINDOW_US=0 -e BY5_FICT_LEAF=0"
export DOCKER_ENV="$ENV"
printf 'x:y@127.0.0.1:1\n' > $M/dummy.cluster
for name in ${RUNS:-acct-v3 round1-v3 round2-v3 round3-v3}; do
	k=$([ ${name%%-*} = acct ] && echo acct || echo nativemem)
	sudo -n python3 $M/prepare_memory.py --node 0 --pool-bytes 6442188800 --output $OUT/prep-$name.json > /dev/null 2>&1
	[ $(cat $HP/free_hugepages) -ge 3072 ] || { log "HUGETLB SHORT before $name, stopping"; exit 3; }
	$R $OUT $name $P/by5bin/$k/fdbserver -r by5resolvertest -C $M/dummy.cluster -s 1 --knob_paged_resolver_pool_pages=98300 >> $OUT/progress.log 2>&1
	log "$name $(tail -1 $OUT/progress.log) $(grep -oE 'offeredHash=[0-9a-f]+|decisionsHash=[0-9a-f]+' $OUT/$name/stdout.txt | tr '\n' ' ')"
done
log "session done"
