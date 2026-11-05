#!/bin/bash
set -u
OUT=$1; NAME=$2; shift 2
CPU=${CPU:-7}; SIB=${SIB:-15}; IMG=foundationdb/build:rockylinux9-latest
P=$EVAL
[ -e "$OUT/$NAME" ] && { echo "exists: $OUT/$NAME"; exit 1; }
SHM=/dev/shm/eval-v3pub-amd/$NAME; mkdir -p "$SHM" "$OUT"
drain() {
	local d=0
	for i in $(seq 1 120); do
		d=$(awk '/^(Dirty|Writeback):/ {s += $2} END {print s}' /proc/meminfo)
		[ "$d" -lt 4096 ] && break
		sleep 0.5
	done
	echo "$d"
}
cpustat() { awk -v c="cpu$1" '$1==c{print $2+$3+$4+$7+$8+$9, $2+$3+$4+$5+$6+$7+$8+$9}' /proc/stat; }
sync; d0=$(drain)
others=$(docker ps -q | wc -l); load=$(cut -d' ' -f1-3 /proc/loadavg)
topo=$(lscpu -p=CPU,CORE,SOCKET,NODE | awk -F, -v c=$CPU -v s=$SIB '$1==c||$1==s{printf "%s ", $0}')
read b7 t7 < <(cpustat $CPU); read b15 t15 < <(cpustat $SIB)
HPF=/sys/devices/system/node/node0/hugepages/hugepages-2048kB/free_hugepages; hp0=$(cat $HPF); ( m=$hp0; trap 'echo $m > $SHM/hugetlb_min_free; exit 0' TERM; while :; do v=$(cat $HPF); [ $v -lt $m ] && m=$v; sleep 0.2; done ) & SAMP=$!; taskset -cp 3 $SAMP > /dev/null
start=$(date -Is)
docker run --rm --log-driver=none ${DOCKER_OPTS:-} --cpuset-cpus=$CPU --cpuset-mems=0 -v $EVAL:$EVAL:ro -v $HOME/eval-v3-amd:$HOME/eval-v3-amd:ro -v "$SHM":/out -w /out \
	${DOCKER_ENV:-} "$IMG" bash -c "export LD_LIBRARY_PATH=\$(dirname \$(/usr/local/bin/clang++ -stdlib=libc++ -print-file-name=libc++.so.1)); exec python3 $P/measurement/rusage.py /out/rusage.txt $*" \
	> "$SHM/stdout.txt" 2> "$SHM/stderr.txt"
rc=$?
kill -TERM $SAMP; wait $SAMP 2> /dev/null
end=$(date -Is)
read b7e t7e < <(cpustat $CPU); read b15e t15e < <(cpustat $SIB)
busy() { awk -v a=$1 -v b=$2 -v c=$3 -v d=$4 'BEGIN{printf "%.5f", (c-a)/(d-b)}'; }
{
	echo "name=$NAME start=$start end=$end exit=$rc cpu=$CPU sibling=$SIB topology=[$topo] docker_before=$others load_before=[$load] dirty_kib_before=$d0"
	echo "cpu_busy=$(busy $b7 $t7 $b7e $t7e) sibling_busy=$(busy $b15 $t15 $b15e $t15e) hugepages=$(cat /proc/sys/vm/nr_hugepages) thp=$(cat /sys/kernel/mm/transparent_hugepage/enabled) kernel=$(uname -r)"
	echo "docker_env=[${DOCKER_ENV:-}] docker_opts=[${DOCKER_OPTS:-}] hugetlb_free_before=$hp0 hugetlb_min_free=$(cat $SHM/hugetlb_min_free 2>/dev/null) hugetlb_total=$(cat /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages)"
	echo "command=[$*]"
} > "$SHM/run.txt"
cp -r "$SHM" "$OUT/"
sync
echo "$NAME exit=$rc $(date -Is)"
