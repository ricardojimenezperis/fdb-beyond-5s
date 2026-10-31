#!/bin/bash
# The Mac benchmark (resolver1's bench/blocksizebench.cpp) for btree-v1 and btree-v2.5, interleaved.
# It drives TempConflictBTree directly: FDB's skip list and v107 cannot run in it.
# Usage: run.sh <rounds> <out dir>. Each run prints one line at its end, kept in memory and
# appended to <out dir>/runs.txt after the run.
set -euo pipefail
cd "$(dirname "$0")"
ROUNDS=$1; OUT=$2
mkdir -p "$OUT" bin
CXX=${CXX:-clang++}
FLAGS="-std=c++20 -O3 -DNDEBUG"
# v1: as it was measured, the benchmark and resolver1 of commit 073fcbb9af (btree-v1's algorithm).
(cd v1-073fcbb9af/fdbserver/resolver/resolver1 && $CXX $FLAGS -I. bench/blocksizebench.cpp \
	pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp -o ../../../../bin/blocksizebench-v1)
# v2.5: commit 7809eea63f, with 16 tasks; tempconflictbtree.cpp instantiates 24 and 32, so 16 is added here.
D=v2.5-7809eea63f/fdbserver/resolver/resolver1
sed 's/constexpr std::uint8_t Tasks = 32;/constexpr std::uint8_t Tasks = 16;/' $D/bench/blocksizebench.cpp > bin/bench16.cpp
printf '#include "tempconflictbtree.cpp"\ntemplate void TempConflictBTree::checkReads<16>(const std::vector<KeyInfo>&, TransactionInfo* const*, const std::vector<ReadConflictRange>&, const std::vector<int>&, bool*, std::vector<uint8_t>&, int64_t&);\n' > bin/tcb16.cpp
$CXX $FLAGS -I$D bin/bench16.cpp bin/tcb16.cpp $D/pagepool.cpp $D/keystore.cpp $D/bnodestore.cpp -o bin/blocksizebench-v2.5
for round in $(seq 1 "$ROUNDS"); do
	for workload in points ranges; do
		for v in v1 v2.5; do
			line=$(${TASKSET:-} bin/blocksizebench-$v $workload 3000 1024)
			echo "round=$round version=$v $line" >> "$OUT/runs.txt"
		done
	done
done
