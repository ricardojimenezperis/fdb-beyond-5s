#!/bin/bash
W=$1; d=$2; kind=$3
C=/data/fdb/scratch/btprof/campaigns/2026-10-30-equal-memory-v26-hp
B=${TREE_BUILD:?}; H=${TREE_CMDS:?}; F=$H/compile-fdbserver.txt
[ -e $d ] && { echo "exists: $d"; exit 1; }
mkdir -p $d && cd $d || exit 1
for f in $W/fdbserver/resolver/*.h $W/fdbserver/resolver/*.inc $W/fdbserver/resolver/PagedBTreeConflictSet.cpp $W/fdbserver/resolver/PagedTemporalSkipList.cpp $W/fdbserver/resolver/ConflictSet.cpp; do cp $f $d/ || exit 1; done
cp $W/fdbserver/fdbserver.cpp $d/fdbserver.cpp
case $kind in acct) python3 $C/scripts/by5-acct-v107.py $d && python3 $C/scripts/by5-tempbtree-v26.py $d || exit 1;; nativemem) python3 $C/scripts/by5-membuf.py $d && python3 $C/scripts/by5-tempbtree-v26.py $d || exit 1;; perfctl) python3 $C/scripts/by5-membuf.py $d && python3 $C/scripts/by5-perfctl.py $d || exit 1;; statsdiag) python3 $C/scripts/by5-membuf.py $d && python3 $C/scripts/by5-perfctl.py $d stats || exit 1;; sweepdiag) python3 $C/scripts/by5-membuf.py $d && python3 $C/scripts/by5-perfctl.py $d stats && python3 $C/scripts/by5-sweeps.py $d || exit 1;; native|asan) ;; *) echo "kind?"; exit 1;; esac
def="${DEF--DPAGED_BTREE_INLINE_KEY_BYTES=64 -DPAGED_BTREE_NODE_CAPACITY=23 -DPAGED_RESOLVER_SEARCH_STATS=1}"
[ $kind = asan ] && def="$def -fsanitize=address,undefined -fno-sanitize=alignment -fno-omit-frame-pointer"
ldef=""; [ $kind = asan ] && ldef="-fsanitize=address,undefined"
cmds=""
for o in ConflictSet PagedBTreeConflictSet PagedTemporalSkipList; do
  cmds="$cmds $(sed -e "s# -o [^ ]*# -o $d/$o.cpp.o $def -I$d#" -e "s# -c [^ ]*# -c $d/$o.cpp#" $H/compile-$o.txt);"
done
cmds="$cmds $(sed -e "s# -o [^ ]*# -o $d/fdbserver.cpp.o#" -e "s# -c [^ ]*# -c $d/fdbserver.cpp#" $F);"
link=$(sed -e "s#lib/libfdbserver_resolver.a#$d/libfdbserver_resolver.a#g" -e "s#fdbserver/CMakeFiles/fdbserver.dir/fdbserver.cpp.o#$d/fdbserver.cpp.o#" -e "s# -o bin/fdbserver # -o $d/fdbserver $ldef #" $H/link.txt)
docker run --rm --cpuset-cpus=16-23 -v /data/fdb:/data/fdb -w $B foundationdb/build:rockylinux9-latest bash -c "set -e; $cmds cp $B/lib/libfdbserver_resolver.a $d/libfdbserver_resolver.a; cd $d && llvm-ar r libfdbserver_resolver.a ConflictSet.cpp.o PagedBTreeConflictSet.cpp.o PagedTemporalSkipList.cpp.o && cd $B; $link" > $d/build.txt 2>&1 || { echo "BUILD FAILED $d"; grep -m8 'error' $d/build.txt; exit 1; }
{ echo "hp-v7-$kind tree=$W ($(git -C $W rev-parse HEAD), status $(git -C $W status --porcelain | wc -l) modified) flags=[$def] link=[$ldef] builder=HP tree $B"; (cd $d && sha256sum *.h *.cpp *.inc fdbserver); } > $d/MANIFEST
echo "built $d/fdbserver"
