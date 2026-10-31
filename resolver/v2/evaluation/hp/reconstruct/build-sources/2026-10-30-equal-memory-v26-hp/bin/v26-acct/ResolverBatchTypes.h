/*
 * ResolverBatchTypes.h
 *
 * The records of a conflict batch that a history may read directly: ReadConflictRange, one per
 * read range; KeyInfo, one per endpoint of a read or write range, which sortPoints sorts; and
 * TransactionInfo, one per transaction. ConflictSet.cpp defines its batch with them, and
 * resolver1 takes them as they are.
 *
 * Nothing of flow is included here, so that resolver1 builds without it: StringRef, VectorRef,
 * Arena, Version and a free compare(StringRef, StringRef) must be declared before this header.
 * Inside FDB they come from flow; outside, resolver1's fdbcompat.h provides its own.
 *
 * Inside FDB, resolver1 is compiled inside a namespace of its own. This header must be included
 * before that namespace is opened, so that resolver1 uses these same types; resolver1's
 * fdbcompat.h then checks that it was.
 */

#ifndef FDBSERVER_RESOLVER_BATCH_TYPES_H
#define FDBSERVER_RESOLVER_BATCH_TYPES_H
#pragma once

#include <utility>

struct ReadConflictRange {
	StringRef begin, end;
	Version version;
	int transaction;
	int indexInTx;
	VectorRef<int>* conflictingKeyRange;
	Arena* cKRArena;

	ReadConflictRange(StringRef begin,
	                  StringRef end,
	                  Version version,
	                  int transaction,
	                  int indexInTx,
	                  VectorRef<int>* cKR = nullptr,
	                  Arena* cKRArena = nullptr)
	  : begin(begin), end(end), version(version), transaction(transaction), indexInTx(indexInTx),
	    conflictingKeyRange(cKR), cKRArena(cKRArena) {}
	bool operator<(const ReadConflictRange& rhs) const { return compare(begin, rhs.begin) < 0; }
};

struct KeyInfo {
	StringRef key;
	int* pIndex;
	bool begin;
	bool write;
	int transaction;

	KeyInfo() = default;
	KeyInfo(StringRef key, bool begin, bool write, int transaction, int* pIndex)
	  : key(key), pIndex(pIndex), begin(begin), write(write), transaction(transaction) {}
};

struct TransactionInfo {
	VectorRef<std::pair<int, int>> readRanges;
	VectorRef<std::pair<int, int>> writeRanges;
	bool tooOld;
	bool reportConflictingKeys;
};

#endif
