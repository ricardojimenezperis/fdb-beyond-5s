#!/bin/bash
P=$EVAL; IMG=foundationdb/build:rockylinux9-latest; A=-falign-functions=64; W=$P/wt-v38
while pgrep -f eval-v3pub-amd/measurement/build.sh > /dev/null; do sleep 10; done
mkdir -p $P/builds/v38-a64
docker run --rm -v $HOME:$HOME -w $P/builds/v38-a64 $IMG bash -c "cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/local/bin/clang -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++ -DCMAKE_C_FLAGS=$A -DCMAKE_CXX_FLAGS=$A -DUSE_LIBCXX=1 -DUSE_LD=LLD $W > $P/builds/v38-a64.cmake.log 2>&1 && ninja -j14 fdbserver > $P/builds/v38-a64.ninja.log 2>&1"
echo "v38 exit=$? $(date -Is) worktree=$W" >> $P/builds/build.exit
S=$P/standalone/v38; mkdir -p $S/resolver1/bench
cp $W/fdbserver/resolver/resolver1/*.cpp $W/fdbserver/resolver/resolver1/*.h $S/resolver1/
cp $W/fdbserver/resolver/ResolverBatchTypes.h $S/
cp $W/fdbserver/resolver/resolver1/bench/blocksizebench.cpp $S/resolver1/bench/
F="-std=c++20 -O3 -DNDEBUG -stdlib=libc++ -DBENCH_API=3 -DBENCH_TASKS=16 $A -Iresolver1"
docker run --rm -v $HOME:$HOME -w $S $IMG bash -c "clang++ $F resolver1/pagepool.cpp resolver1/keystore.cpp resolver1/bnodestore.cpp resolver1/tempconflictbtree.cpp resolver1/bench/blocksizebench.cpp -o blocksizebench > compile.log 2>&1; echo rc=\$? flags=$F >> compile.log"
echo "standalone v38 $(tail -1 $S/compile.log) $(date -Is)" >> $P/builds/build.exit
echo "v38 all done $(date -Is)" >> $P/builds/build.exit
