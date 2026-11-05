#!/bin/bash
E=$EVAL; S=$EVAL/standalone/v38
cd $S || exit 1
: > $E/bin/compile.log
for cfg in 2x8 2x10 2x12 3x12 3x20; do g=${cfg%x*}; lg=${cfg#*x}
	F="-std=c++20 -stdlib=libc++ -DBENCH_API=3 -DBENCH_TASKS=16 -DRESOLVER1_WRITE_GROUPS=$g -DRESOLVER1_WRITE_LG=$lg -falign-functions=64 -Iresolver1"
	for k in "gate|-O3 -DNDEBUG" "assert|-O1 -g"; do IFS='|' read n o <<< "$k"
		clang++ $F $o resolver1/pagepool.cpp resolver1/keystore.cpp resolver1/bnodestore.cpp resolver1/tempconflictbtree.cpp resolver1/bench/blocksizebench.cpp -o $E/bin/blocksizebench-$n-v38-$cfg
		echo "blocksizebench-$n-v38-$cfg rc=$? flags=$F $o" >> $E/bin/compile.log; done
done
clang++ --version | head -1 >> $E/bin/compile.log
