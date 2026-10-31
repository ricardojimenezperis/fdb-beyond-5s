/*
 * ConflictSet.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2026 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CONFLICTSET_H
#define CONFLICTSET_H
#pragma once

#include <utility>
#include <vector>

#include "fdbclient/CommitTransaction.h"
#include "fdbserver/core/ConflictBatch.h"
#include "fdbserver/resolver/ResolverBug.h"

struct ConflictSet;
ConflictSet* newConflictSet();
void clearConflictSet(ConflictSet*, Version);
void destroyConflictSet(ConflictSet*);

// What a conflict set reports to the Resolver's metrics. Phase times are the process-wide
// totals of the batch phases, in seconds; the paged fields are zero for FDB's skip list
struct ConflictSetStats {
	bool paged = false;
	double checkReadSeconds = 0, checkIntraBatchSeconds = 0, combineSeconds = 0, mergeSeconds = 0,
	       cleanupSeconds = 0;
	Version validationMinRV = 0;
	int64_t poolLimitPages = 0, poolFreePages = 0, poolHeapPages = 0, poolIndexPages = 0;
	int64_t reclaimedHistoryPages = 0;
	int64_t cleanedCells = 0;
	// Transactions the external window admitted and the paged set's own boundary rejected
	int64_t tooOldWithinWindow = 0;
	// Largest distance by which the paged boundary has passed the external window
	int64_t boundaryPastWindow = 0;
	// The paged B+ only: which index, its shape and its structural work. cleanedCells stays zero
	// for it; cleanedIntervals counts expired intervals its cleanup removed, a different unit
	bool btree = false;
	int64_t cleanedIntervals = 0;
	int64_t btreeHeight = 0, btreeNodes = 0, btreeLeaves = 0;
	int64_t btreeIntervalInsertions = 0, btreeOverwrittenIntervals = 0, btreeExpiredIntervals = 0;
	int64_t btreeDiscardedWrites = 0, btreeSplits = 0, btreeInternalSplits = 0, btreeMerges = 0;
	int64_t btreeInternalMerges = 0, btreeCleanupLeavesVisited = 0, btreeMergeCandidatesExamined = 0;
	int64_t btreeIndexPagesReleased = 0, btreeReclaimCalls = 0, btreeReclaimLeavesVisited = 0;
};
// "skiplist", "paged-skiplist" or "paged-btree"
const char* conflictSetIndexName(const ConflictSet*);
ConflictSetStats getConflictSetStats(const ConflictSet*);

struct ConflictBatch : ConflictBatchStatus {
	explicit ConflictBatch(ConflictSet*,
	                       std::map<int, VectorRef<int>>* conflictingKeyRangeMap = nullptr,
	                       Arena* resolveBatchReplyArena = nullptr);
	~ConflictBatch();

	void addTransaction(const CommitTransactionRef& transaction, Version newOldestVersion);
	void detectConflicts(Version now,
	                     Version newOldestVersion,
	                     std::vector<int>& nonConflicting,
	                     std::vector<int>* tooOldTransactions = nullptr);
	void GetTooOldTransactions(std::vector<int>& tooOldTransactions);

private:
	ConflictSet* cs;
	Standalone<VectorRef<struct TransactionInfo*>> transactionInfo;
	std::vector<struct KeyInfo> points;
    std::vector<uint32_t> pointLcps; // Adjacent byte LCPs from the common FDB sort.
	int transactionCount;
	std::vector<std::pair<StringRef, StringRef>> combinedWriteConflictRanges;
	std::vector<struct ReadConflictRange> combinedReadConflictRanges;
	bool* transactionConflictStatus;
	// Stores the map: a transaction -> conflicted transactions' indices
	std::map<int, VectorRef<int>>* conflictingKeyRangeMap;
	Arena* resolveBatchReplyArena;
	std::shared_ptr<ResolverBug> bugs = SimBugInjector().get<ResolverBug>(ResolverBugID());

	// bug injection
	bool ignoreTooOld() const;
	bool ignoreWriteSet() const;
	bool ignoreReadSet() const;

	void checkIntraBatchConflicts();
	void combineWriteConflictRanges();
	void checkReadConflictRanges();
	void checkPagedReadConflictRanges();
	void checkBTreeReadConflictRanges();
	void mergeWriteConflictRanges(Version now);
	void addConflictRanges(Version now,
	                       std::vector<std::pair<StringRef, StringRef>>::iterator begin,
	                       std::vector<std::pair<StringRef, StringRef>>::iterator end,
	                       class SkipList* part);
};

#endif
