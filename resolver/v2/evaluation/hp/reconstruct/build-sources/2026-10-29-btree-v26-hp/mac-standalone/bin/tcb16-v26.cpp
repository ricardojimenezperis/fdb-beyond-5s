#include "tempconflictbtree.cpp"
template void TempConflictBTree::checkReads<16>(const std::vector<KeyInfo>&, TransactionInfo* const*, const std::vector<ReadConflictRange>&, const std::vector<int>&, bool*, std::vector<uint8_t>&, int64_t&);
