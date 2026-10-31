#!/bin/bash
set -euo pipefail
N=$(cd "$(dirname "$0")/.." && pwd)
cd "$N"
CXX=${CXX:-clang++}
FLAGS="-std=c++20 -O3 -DNDEBUG"
V1=src/v1-5a07ab1936/fdbserver/resolver/resolver1
V2=src/v2.6-22a8ae9fba/fdbserver/resolver/resolver1
printf '#include "tempconflictbtree.cpp"\ntemplate void TempConflictBTree::checkReads<16>(const std::vector<KeyInfo>&, TransactionInfo* const*, const std::vector<ReadConflictRange>&, const std::vector<int>&, bool*, std::vector<uint8_t>&, int64_t&);\n' > src/tcb16-v26.cpp
$CXX $FLAGS -DRESOLVER1_SEQUENTIAL_READS -I$V1 src/blocksizebench.cpp $V1/pagepool.cpp $V1/keystore.cpp $V1/bnodestore.cpp $V1/tempconflictbtree.cpp -o bin/blocksizebench-v1-5a07ab1936
$CXX $FLAGS -DBENCH_TASKS=16 -I$V2 src/blocksizebench.cpp src/tcb16-v26.cpp $V2/pagepool.cpp $V2/keystore.cpp $V2/bnodestore.cpp -o bin/blocksizebench-v2.6-g16
$CXX --version | head -1 > bin/compiler.txt
sha256sum bin/blocksizebench-v1-5a07ab1936 bin/blocksizebench-v2.6-g16 > bin/binaries.sha256
