/*
 * ConflictSet.cpp
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

#include <stdint.h>
#include <memory.h>
#include <stdio.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <vector>
#ifdef __linux__
#include <sched.h>
#endif

#include "flow/Platform.h"
#include "fdbrpc/fdbrpc.h"
#include "fdbrpc/PerfMetric.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/SystemData.h"
#include "fdbserver/core/Knobs.h"
#include "ConflictSet.h"
#include "BlockReaderLcp.h"
#include "PagedTemporalSkipList.h"
#include "PagedBTreeConflictSet.h"

#include "flow/UnitTest.h"

static std::vector<PerfDoubleCounter*> skc;

static thread_local uint32_t g_seed = 0;

static inline int skfastrand() {
	g_seed = g_seed * 1664525L + 1013904223L;
	return g_seed;
}

PerfDoubleCounter g_buildTest("Build", skc), g_add("Add", skc), g_detectConflicts("Detect", skc), g_sort("D.Sort", skc),
    g_combine("D.Combine", skc), g_checkRead("D.CheckRead", skc), g_checkBatch("D.CheckIntraBatch", skc),
    g_merge("D.MergeWrite", skc), g_removeBefore("D.RemoveBefore", skc);

static force_inline int compare(const StringRef& a, const StringRef& b) {
	const size_t aSize = a.size();
	const size_t bSize = b.size();
	const size_t minSize = std::min(aSize, bSize);
	int c = memcmp(a.begin(), b.begin(), minSize);
	if (c)
		return (c > 0) - (c < 0); // normalize to +1/-1
	return (aSize > bSize) - (aSize < bSize);
}

// ReadConflictRange, KeyInfo and TransactionInfo, shared with btree-v2
#include "ResolverBatchTypes.h"

// btree-v2, compiled in BTreeV2.cpp, resolves every batch of this binary. Its declarations come
// from resolver1's headers in the same namespace; the standard headers they include go first.
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#define RESOLVER1_IN_FDB 1
namespace btree_v2 {
#include "resolver1/tempconflictbtree.h"
} // namespace btree_v2
#undef RESOLVER1_IN_FDB

force_inline int extra_ordering(const KeyInfo& ki) {
	return ki.begin * 2 + (ki.write ^ ki.begin);
}

// returns true if done with string
force_inline bool getCharacter(const KeyInfo& ki, int character, int& outputCharacter) {
	// normal case
	if (character < ki.key.size()) {
		outputCharacter = 5 + ki.key.begin()[character];
		return false;
	}

	// termination
	if (character == ki.key.size()) {
		outputCharacter = 0;
		return false;
	}

	if (character == ki.key.size() + 1) {
		// end/begin+read/write relative sorting
		outputCharacter = extra_ordering(ki);
		return false;
	}

	outputCharacter = 0;
	return true;
}

bool operator<(const KeyInfo& lhs, const KeyInfo& rhs) {
	int i = std::min(lhs.key.size(), rhs.key.size());
	int c = memcmp(lhs.key.begin(), rhs.key.begin(), i);
	if (c != 0)
		return c < 0;

	// Always sort shorter keys before longer keys.
	if (lhs.key.size() < rhs.key.size()) {
		return true;
	}
	if (lhs.key.size() > rhs.key.size()) {
		return false;
	}

	// When the keys are the same length, use the extra ordering constraint.
	return extra_ordering(lhs) < extra_ordering(rhs);
}

bool operator==(const KeyInfo& lhs, const KeyInfo& rhs) {
	return !(lhs < rhs || rhs < lhs);
}

void swapSort(std::vector<KeyInfo>& points, int a, int b) {
	if (points[b] < points[a]) {
		KeyInfo temp;
		temp = points[a];
		points[a] = points[b];
		points[b] = temp;
	}
}

void smallSort(std::vector<KeyInfo>& points, int start, int N) {
	for (int i = 1; i < N; i++)
		for (int j = i; j > 0; j -= 2)
			swapSort(points, start + j - 1, start + j);
	for (int i = N - 2; i > 0; i--)
		for (int j = i; j > 0; j -= 2)
			swapSort(points, start + j - 1, start + j);
}

struct SortTask {
	int begin;
	int size;
	int character;
	SortTask(int begin, int size, int character) : begin(begin), size(size), character(character) {}
};

// Small radix buckets are insertion-sorted while preserving exact adjacent LCPs.
// Equality does not inspect transaction or query IDs.
void sortSmallPointsWithLcp(std::vector<KeyInfo>& points, int begin, int size,
                            std::vector<uint32_t>& lcps) {
    for (int i = 1; i < size; ++i) {
        const KeyInfo value = points[begin + i];
        int j = i;
        uint32_t predecessor = 0, successor = 0;
        while (j > 0) {
            const auto& previous = points[begin + j - 1];
            const size_t common = std::min(value.key.size(), previous.key.size());
            const auto comparison = block_reader_lcp::compare(value.key.begin(), previous.key.begin(), common);
            int order = comparison.order;
            if (!order) order = (value.key.size() > previous.key.size()) - (value.key.size() < previous.key.size());
            if (!order) order = extra_ordering(value) - extra_ordering(previous);
            if (order >= 0) { predecessor = comparison.lcp; break; }
            points[begin + j] = previous;
            lcps[begin + j] = lcps[begin + j - 1];
            successor = comparison.lcp;
            --j;
        }
        points[begin + j] = value;
        if (j) lcps[begin + j] = predecessor; // Preserve the parent bucket's first boundary.
        if (j < i) lcps[begin + j + 1] = successor;
    }
}

void sortPoints(std::vector<KeyInfo>& points, std::vector<uint32_t>* lcps = nullptr) {
    if (lcps) lcps->assign(points.size(), 0);
	std::vector<SortTask> tasks;
	std::vector<KeyInfo> newPoints;
	std::vector<int> counts;

	tasks.reserve(points.size());
	newPoints.reserve(points.size());
	counts.resize(261); // 256+5 = character+sentinal

	tasks.emplace_back(0, points.size(), 0);

	while (!tasks.empty()) {
		SortTask st = tasks.back();
		tasks.pop_back();

		if (st.size < 10) {
			// smallSort(points, st.begin, st.size);
            if (lcps) sortSmallPointsWithLcp(points, st.begin, st.size, *lcps);
            else std::sort(points.begin() + st.begin, points.begin() + st.begin + st.size);
			continue;
		}

		newPoints.resize(st.size);
		std::fill(counts.begin(), counts.end(), 0);

		// get counts
		int c;
		bool allDone = true;
		for (int i = st.begin; i < st.begin + st.size; i++) {
			allDone &= getCharacter(points[i], st.character, c);
			counts[c]++;
		}
        if (allDone) {
            if (lcps && st.size > 1)
                std::fill(lcps->begin() + st.begin + 1, lcps->begin() + st.begin + st.size,
                          static_cast<uint32_t>(points[st.begin].key.size()));
            continue;
        }

		// calculate offsets from counts and build next level of tasks
		int total = 0;
		for (int i = 0; i < counts.size(); i++) {
            int temp = counts[i];
            if (lcps && temp && total)
                (*lcps)[st.begin + total] = static_cast<uint32_t>(
                    std::min(st.character, points[st.begin].key.size()));
			if (temp > 1)
				tasks.emplace_back(st.begin + total, temp, st.character + 1);
			counts[i] = total;
			total += temp;
		}

		// put in their places
		for (int i = st.begin; i < st.begin + st.size; i++) {
			getCharacter(points[i], st.character, c);
			newPoints[counts[c]++] = points[i];
		}

		// copy back into original points array
		for (int i = 0; i < st.size; i++)
			points[st.begin + i] = newPoints[i];
	}
}

class SkipList : NonCopyable {
private:
	static constexpr int MaxLevels = 26;

	int randomLevel() const {
		uint32_t i = uint32_t(skfastrand()) >> (32 - (MaxLevels - 1));
		int level = 0;
		while (i & 1) {
			i >>= 1;
			level++;
		}
		ASSERT(level < MaxLevels);
		return level;
	}

	// Represent a node in the SkipList. The node has multiple (i.e., level) pointers to
	// other nodes, and keeps a record of the max versions for each level.
	struct Node {
		int level() const { return nPointers - 1; }
		uint8_t* value() { return end() + nPointers * (sizeof(Node*) + sizeof(Version)); }
		int length() const { return valueLength; }

		// Returns the next node pointer at the given level.
		Node* getNext(int level) { return *((Node**)end() + level); }
		// Sets the next node pointer at the given level.
		void setNext(int level, Node* n) { *((Node**)end() + level) = n; }

		// Returns the max version at the given level.
		Version getMaxVersion(int i) const { return ((Version*)(end() + nPointers * sizeof(Node*)))[i]; }
		// Sets the max version at the given level.
		void setMaxVersion(int i, Version v) { ((Version*)(end() + nPointers * sizeof(Node*)))[i] = v; }

		// Return a node with initialized value but uninitialized pointers
		// Memory layout: *this, (level+1) Node*, (level+1) Version, value
		static Node* create(const StringRef& value, int level) {
			int nodeSize = sizeof(Node) + value.size() + (level + 1) * (sizeof(Node*) + sizeof(Version));

			Node* n;
			if (nodeSize <= 64) {
				n = (Node*)FastAllocator<64>::allocate();
				INSTRUMENT_ALLOCATE("SkipListNode64");
			} else if (nodeSize <= 128) {
				n = (Node*)FastAllocator<128>::allocate();
				INSTRUMENT_ALLOCATE("SkipListNode128");
			} else {
				n = (Node*)new char[nodeSize];
				INSTRUMENT_ALLOCATE("SkipListNodeLarge");
			}

			n->nPointers = level + 1;

			n->valueLength = value.size();
			if (!value.empty()) {
				memcpy(n->value(), value.begin(), value.size());
			}
			return n;
		}

		// pre: level>0, all lower level nodes between this and getNext(level) have correct maxversions
		void calcVersionForLevel(int level) {
			Node* end = getNext(level);
			Version v = getMaxVersion(level - 1);
			for (Node* x = getNext(level - 1); x != end; x = x->getNext(level - 1))
				v = std::max(v, x->getMaxVersion(level - 1));
			setMaxVersion(level, v);
		}

		void destroy() {
			int nodeSize = getNodeSize();
			if (nodeSize <= 64) {
				FastAllocator<64>::release(this);
				INSTRUMENT_RELEASE("SkipListNode64");
			} else if (nodeSize <= 128) {
				FastAllocator<128>::release(this);
				INSTRUMENT_RELEASE("SkipListNode128");
			} else {
				delete[] (char*)this;
				INSTRUMENT_RELEASE("SkipListNodeLarge");
			}
		}

	private:
		int getNodeSize() const { return sizeof(Node) + valueLength + nPointers * (sizeof(Node*) + sizeof(Version)); }
		// Returns the first Node* pointer
		uint8_t* end() { return (uint8_t*)(this + 1); }
		uint8_t const* end() const { return (uint8_t const*)(this + 1); }
		int nPointers, valueLength;
	};

	static force_inline bool less(const uint8_t* a, int aLen, const uint8_t* b, int bLen) {
		int c = memcmp(a, b, std::min(aLen, bLen));
		if (c < 0)
			return true;
		if (c > 0)
			return false;
		return aLen < bLen;
	}

	Node* header;

	void destroy() {
		Node *next, *x;
		for (x = header; x; x = next) {
			next = x->getNext(0);
			x->destroy();
		}
	}

public:
	// Points the location (i.e., Node*) that value would appear in the SkipList.
	// If the "value" is in the list, then finger[0] points to that exact node;
	// otherwise, the finger points to Nodes that the value should be inserted before.
	// Note the SkipList organizes all nodes at level 0, higher levels contain jump pointers.
	struct Finger {
		Node* finger[MaxLevels]; // valid for levels >= level
		int level = MaxLevels;
		Node* x = nullptr;
		Node* alreadyChecked = nullptr;
		StringRef value;

		Finger() = default;
		Finger(Node* header, const StringRef& ptr) : x(header), value(ptr) {}

		void init(const StringRef& value, Node* header) {
			this->value = value;
			x = header;
			alreadyChecked = nullptr;
			level = MaxLevels;
		}

		// pre: !finished()
		force_inline void prefetch() {
			Node* next = x->getNext(level - 1);
			if (next) {
				_mm_prefetch((const char*)next, _MM_HINT_T0);
				_mm_prefetch((const char*)next + 64, _MM_HINT_T0);
			}
		}

		// pre: !finished()
		// Advances the pointer at the current level to a Node that's >= finger's value
		// if possible; or move to the next level (i.e., level--).
		// Returns true if we have advanced to the next level
		force_inline bool advance() {
			Node* next = x->getNext(level - 1);

			if (next == alreadyChecked || !less(next->value(), next->length(), value.begin(), value.size())) {
				alreadyChecked = next;
				level--;
				finger[level] = x;
				return true;
			} else {
				x = next;
				return false;
			}
		}

		// pre: !finished()
		force_inline void nextLevel() {
			while (!advance())
				;
		}

		force_inline bool finished() const { return level == 0; }

		// Returns if the finger value is found in the SkipList.
		force_inline Node* found() const {
			// valid after finished returns true
			Node* n = finger[0]->getNext(0); // or alreadyChecked, but that is more easily invalidated
			if (n && n->length() == value.size() && !memcmp(n->value(), value.begin(), value.size()))
				return n;
			else
				return nullptr;
		}

		StringRef getValue() const {
			Node* n = finger[0]->getNext(0);
			return n ? StringRef(n->value(), n->length()) : StringRef();
		}
	};

	// Returns the total number of nodes in the list.
	int count() const {
		int count = 0;
		Node* x = header->getNext(0);
		while (x) {
			x = x->getNext(0);
			count++;
		}
		return count;
	}

	explicit SkipList(Version version = 0) {
		header = Node::create(StringRef(), MaxLevels - 1);
		for (int l = 0; l < MaxLevels; l++) {
			header->setNext(l, nullptr);
			header->setMaxVersion(l, version);
		}
	}
	~SkipList() { destroy(); }
	SkipList(SkipList&& other) noexcept : header(other.header) { other.header = nullptr; }
	void operator=(SkipList&& other) noexcept {
		destroy();
		header = other.header;
		other.header = nullptr;
	}
	void swap(SkipList& other) { std::swap(header, other.header); }

	void addConflictRanges(const Finger* fingers, int rangeCount, Version version) {
		for (int r = rangeCount - 1; r >= 0; r--) {
			const size_t fingerIndex = static_cast<size_t>(r) * 2;
			const Finger& startF = fingers[fingerIndex];
			const Finger& endF = fingers[fingerIndex + 1];

			if (endF.found() == nullptr)
				insert(endF, endF.finger[0]->getMaxVersion(0));

			remove(startF, endF);
			insert(startF, version);
		}
	}

	void detectConflicts(ReadConflictRange* ranges, int count, bool* transactionConflictStatus) {
		const int M = 16;
		int nextJob[M];
		CheckMax inProgress[M];
		if (!count)
			return;

		int started = std::min(M, count);
		for (int i = 0; i < started; i++) {
			inProgress[i].init(ranges[i],
			                   header,
			                   transactionConflictStatus,
			                   ranges[i].indexInTx,
			                   ranges[i].conflictingKeyRange,
			                   ranges[i].cKRArena);
			nextJob[i] = i + 1;
		}
		nextJob[started - 1] = 0;

		int prevJob = started - 1;
		int job = 0;
		// vtune: 340 parts
		while (true) {
			if (inProgress[job].advance()) {
				if (started == count) {
					if (prevJob == job)
						break;
					nextJob[prevJob] = nextJob[job];
					job = prevJob;
				} else {
					int temp = started++;
					inProgress[job].init(ranges[temp],
					                     header,
					                     transactionConflictStatus,
					                     ranges[temp].indexInTx,
					                     ranges[temp].conflictingKeyRange,
					                     ranges[temp].cKRArena);
				}
			}
			prevJob = job;
			job = nextJob[job];
		}
	}

	void find(const StringRef* values, Finger* results, int* temp, int count) {
		// Relying on the ordering of values, descend until the values aren't all in the
		// same part of the tree

		// vtune: 11 parts
		results[0].init(values[0], header);
		const StringRef& endValue = values[count - 1];
		while (results[0].level > 1) {
			results[0].nextLevel();
			Node* ac = results[0].alreadyChecked;
			if (ac && less(ac->value(), ac->length(), endValue.begin(), endValue.size()))
				break;
		}

		// Init all the other fingers to start descending where we stopped
		//   the first one

		// SOMEDAY: this loop showed up on vtune, could be faster?
		// vtune: 8 parts
		int startLevel = results[0].level + 1;
		Node* x = startLevel < MaxLevels ? results[0].finger[startLevel] : header;
		for (int i = 1; i < count; i++) {
			results[i].level = startLevel;
			results[i].x = x;
			results[i].alreadyChecked = nullptr;
			results[i].value = values[i];
			for (int j = startLevel; j < MaxLevels; j++)
				results[i].finger[j] = results[0].finger[j];
		}

		int* nextJob = temp;
		for (int i = 0; i < count - 1; i++)
			nextJob[i] = i + 1;
		nextJob[count - 1] = 0;

		int prevJob = count - 1;
		int job = 0;

		// vtune: 225 parts
		while (true) {
			Finger* f = &results[job];
			f->advance();
			if (f->finished()) {
				if (prevJob == job)
					break;
				nextJob[prevJob] = nextJob[job];
			} else {
				f->prefetch();
				prevJob = job;
			}
			job = nextJob[job];
		}
	}

	int removeBefore(Version v, Finger& f, int nodeCount) {
		// f.x, f.alreadyChecked?

		int removedCount = 0;
		bool wasAbove = true;
		while (nodeCount--) {
			Node* x = f.finger[0]->getNext(0);
			if (!x)
				break;

			// double prefetch gives +25% speed (single threaded)
			Node* next = x->getNext(0);
			_mm_prefetch((const char*)next, _MM_HINT_T0);
			next = x->getNext(1);
			_mm_prefetch((const char*)next, _MM_HINT_T0);

			bool isAbove = x->getMaxVersion(0) >= v;
			if (isAbove || wasAbove) { // f.nextItem
				for (int l = 0; l <= x->level(); l++)
					f.finger[l] = x;
			} else { // f.eraseItem
				removedCount++;
				for (int l = 0; l <= x->level(); l++)
					f.finger[l]->setNext(l, x->getNext(l));
				for (int i = 1; i <= x->level(); i++)
					f.finger[i]->setMaxVersion(i, std::max(f.finger[i]->getMaxVersion(i), x->getMaxVersion(i)));
				x->destroy();
			}
			wasAbove = isAbove;
		}

		return removedCount;
	}

private:
	void remove(const Finger& start, const Finger& end) {
		if (start.finger[0] == end.finger[0])
			return;

		Node* x = start.finger[0]->getNext(0);

		// vtune says: this loop is the expensive parts (6 parts)
		for (int i = 0; i < MaxLevels; i++)
			if (start.finger[i] != end.finger[i])
				start.finger[i]->setNext(i, end.finger[i]->getNext(i));

		while (true) {
			Node* next = x->getNext(0);
			x->destroy();
			if (x == end.finger[0])
				break;
			x = next;
		}
	}

	void insert(const Finger& f, Version version) {
		int level = randomLevel();
		// std::cout << std::string((const char*)value,length) << " level: " << level << std::endl;
		Node* x = Node::create(f.value, level);
		x->setMaxVersion(0, version);
		for (int i = 0; i <= level; i++) {
			x->setNext(i, f.finger[i]->getNext(i));
			f.finger[i]->setNext(i, x);
		}
		// vtune says: this loop is the costly part of this function
		for (int i = 1; i <= level; i++) {
			f.finger[i]->calcVersionForLevel(i);
			x->calcVersionForLevel(i);
		}
		for (int i = level + 1; i < MaxLevels; i++) {
			Version v = f.finger[i]->getMaxVersion(i);
			if (v >= version)
				break;
			f.finger[i]->setMaxVersion(i, version);
		}
	}

	void insert(const StringRef& value, Version version) {
		Finger f(header, value);
		while (!f.finished())
			f.nextLevel();
		// SOMEDAY: equality?
		insert(f, version);
	}

	struct CheckMax {
		Finger start, end;
		Version version;
		bool* result;
		int state;
		int indexInTx;
		VectorRef<int>* conflictingKeyRange; // nullptr if report_conflicting_keys is not enabled.
		Arena* cKRArena; // nullptr if report_conflicting_keys is not enabled.

		void init(const ReadConflictRange& r,
		          Node* header,
		          bool* tCS,
		          int indexInTx,
		          VectorRef<int>* cKR,
		          Arena* cKRArena) {
			this->start.init(r.begin, header);
			this->end.init(r.end, header);
			this->version = r.version;
			this->indexInTx = indexInTx;
			this->cKRArena = cKRArena;
			result = &tCS[r.transaction];
			conflictingKeyRange = cKR;
			this->state = 0;
		}

		bool noConflict() const { return true; }
		bool conflict() {
			*result = true;
			if (conflictingKeyRange != nullptr)
				conflictingKeyRange->push_back(*cKRArena, indexInTx);
			return true;
		}

		// Return true if finished
		force_inline bool advance() {
			switch (state) {
			case 0:
				// find where start and end fingers diverge
				while (true) {
					if (!start.advance()) {
						start.prefetch();
						return false;
					}
					end.x = start.x;
					while (!end.advance())
						;

					int l = start.level;
					if (start.finger[l] != end.finger[l])
						break;
					// accept if the range spans the check range, but does not have a greater version
					if (start.finger[l]->getMaxVersion(l) <= version)
						return noConflict();
					if (l == 0)
						return conflict();
				}
				state = 1;
			case 1: {
				// check the end side of the pyramid
				Node* e = end.finger[end.level];
				while (e->getMaxVersion(end.level) > version) {
					if (end.finished())
						return conflict();
					end.nextLevel();
					Node* f = end.finger[end.level];
					while (e != f) {
						if (e->getMaxVersion(end.level) > version)
							return conflict();
						e = e->getNext(end.level);
					}
				}

				// check the start side of the pyramid
				Node* s = end.finger[start.level];
				while (true) {
					Node* nextS = start.finger[start.level]->getNext(start.level);
					Node* p = nextS;
					while (p != s) {
						if (p->getMaxVersion(start.level) > version)
							return conflict();
						p = p->getNext(start.level);
					}
					if (start.finger[start.level]->getMaxVersion(start.level) <= version)
						return noConflict();
					s = nextS;
					if (start.finished()) {
						if (nextS->length() == start.value.size() &&
						    !memcmp(nextS->value(), start.value.begin(), start.value.size()))
							return noConflict();
						else
							return conflict();
					}
					start.nextLevel();
				}
			}
			default:
				__assume(false);
			}
		}
	};

	// Splits the SkipLists so that those after finger is moved to "right".
	void split(const Finger& f, SkipList& right) {
		ASSERT(!right.header->getNext(0)); // right must be empty
		right.header->setMaxVersion(0, f.finger[0]->getMaxVersion(0));
		for (int l = 0; l < MaxLevels; l++) {
			right.header->setNext(l, f.finger[l]->getNext(l));
			f.finger[l]->setNext(l, nullptr);
		}
	}

	// Sets end's finger to the last nodes at all levels.
	void getEnd(Finger& end) {
		Node* node = header;
		for (int l = MaxLevels - 1; l >= 0; l--) {
			Node* next;
			while ((next = node->getNext(l)) != nullptr)
				node = next;
			end.finger[l] = node;
		}
		end.level = 0;
	}
};

enum class ConflictHistory { SkipList, PagedSkipList, PagedBTree };

const char* conflictHistoryName(ConflictHistory history) {
	switch (history) {
	case ConflictHistory::SkipList:
		return "skiplist";
	case ConflictHistory::PagedSkipList:
		return "paged-skiplist";
	case ConflictHistory::PagedBTree:
		return "paged-btree";
	}
	UNREACHABLE();
}

struct ConflictSet {
	// Exactly one history exists, chosen here and kept for the set's lifetime
	ConflictSet(ConflictHistory history, uint64_t pagedPoolPages)
	  : history(history), removalKey(makeString(0)), oldestVersion(0),
	    btreeV2(static_cast<std::size_t>(pagedPoolPages) * 65536) {
		// Versions start above zero, so an initial boundary of zero rejects no read
		switch (history) {
		case ConflictHistory::SkipList:
			versionHistory.emplace();
			break;
		case ConflictHistory::PagedSkipList:
			pagedHistory = std::make_unique<TemporalConflictSet>(pagedPoolPages, 0);
			break;
		case ConflictHistory::PagedBTree:
			// Captured once, as the skip list captures its cleanup knobs: a later change does not reach it
			ASSERT(SERVER_KNOBS->PAGED_BTREE_CLEANUP_LEAF_BUDGET >= 0);
			ASSERT(SERVER_KNOBS->PAGED_BTREE_CLEANUP_MERGE_CANDIDATES >= 0);
			ASSERT(SERVER_KNOBS->PAGED_BTREE_MERGE_PRESSURE_FREE_PAGE_RATIO >= 0 &&
			       SERVER_KNOBS->PAGED_BTREE_MERGE_PRESSURE_FREE_PAGE_RATIO <= 1);
			btreeLeafBudget = SERVER_KNOBS->PAGED_BTREE_CLEANUP_LEAF_BUDGET;
			btreeMergeCandidates = SERVER_KNOBS->PAGED_BTREE_CLEANUP_MERGE_CANDIDATES;
			btreePressureRatio = SERVER_KNOBS->PAGED_BTREE_MERGE_PRESSURE_FREE_PAGE_RATIO;
			{
				paged_btree::PagedBTreeConflictSet::MemoryConfig memory;
				if (const char* unified = std::getenv("BY5_UNIFIED_KEY_BLOCKS"); unified && *unified) {
					TraceEvent(SevError, "PagedPoolSplitRemoved")
					    .detail("Variable", "BY5_UNIFIED_KEY_BLOCKS")
					    .detail("Value", unified);
					throw invalid_option_value();
				}
				ASSERT(SERVER_KNOBS->PAGED_BTREE_READER_GROUP_SIZE == 2 || SERVER_KNOBS->PAGED_BTREE_READER_GROUP_SIZE == 3);
				const std::string& backingName = SERVER_KNOBS->PAGED_RESOLVER_POOL_BACKING;
				if (!parsePoolBacking(backingName.c_str(), memory.backing)) {
					TraceEvent(SevError, "PagedPoolBackingInvalid")
					    .detail("Backing", backingName)
					    .detail("Valid", "normal, index, both");
					throw invalid_option_value();
				}
				try {
					btreeHistory = std::make_unique<paged_btree::PagedBTreeConflictSet>(pagedPoolPages, 0, memory);
				} catch (const std::invalid_argument& e) {
					TraceEvent(SevError, "PagedPoolBackingInvalid")
					    .detail("Backing", backingName)
					    .detail("Pages", pagedPoolPages)
					    .detail("Reason", e.what());
					throw invalid_option_value();
				} catch (const std::exception& e) {
					TraceEvent(SevError, "PagedPoolBackingFailed")
					    .detail("Backing", backingName)
					    .detail("Pages", pagedPoolPages)
					    .detail("Reason", e.what());
					throw platform_error();
				}
				btreeHistory->configureReader(static_cast<unsigned>(SERVER_KNOBS->PAGED_BTREE_READER_GROUP_SIZE));
				const auto& backing = btreeHistory->poolBacking();
				TraceEvent("PagedPoolBacking")
				    .detail("Backing", poolBackingName(backing.backing))
				    .detail("IndexUsefulBytes", backing.index.usefulBytes)
				    .detail("IndexMappedBytes", backing.index.mappedBytes)
				    .detail("IndexPageKiB", backing.index.pageKiB)
				    .detail("SetupNanoseconds", backing.setupNanoseconds)
				    .detail("ReaderGroupSize", SERVER_KNOBS->PAGED_BTREE_READER_GROUP_SIZE);
			}
			break;
		}
	}
	~ConflictSet() = default;

	const ConflictHistory history;
	std::optional<SkipList> versionHistory;
	Key removalKey;
	Version oldestVersion;

	std::unique_ptr<TemporalConflictSet> pagedHistory;
	int64_t pagedCleanedCells = 0;
	int64_t pagedTooOldWithinWindow = 0;
	int64_t pagedBoundaryPastWindow = 0;

	std::unique_ptr<paged_btree::PagedBTreeConflictSet> btreeHistory;
	unsigned btreeLeafBudget = 0;
	unsigned btreeMergeCandidates = 0;
	double btreePressureRatio = 0;
	int64_t btreeCleanedIntervals = 0;

	// btree-v2: the history this binary resolves with, whatever the knobs select above
	btree_v2::TempConflictBTree btreeV2;
};

ConflictHistory selectedConflictHistory() {
	if (!SERVER_KNOBS->RESOLVER_USE_PAGED_CONFLICT_SET) {
		return ConflictHistory::SkipList;
	}
	return SERVER_KNOBS->PAGED_RESOLVER_USE_BTREE ? ConflictHistory::PagedBTree : ConflictHistory::PagedSkipList;
}

ConflictSet* newConflictSet() {
	const ConflictHistory history = selectedConflictHistory();
	TraceEvent("ResolverConflictHistory")
	    .detail("Index", "btree-v2")
	    .detail("PoolPages", history == ConflictHistory::SkipList ? 0 : SERVER_KNOBS->PAGED_RESOLVER_POOL_PAGES);
	return new ConflictSet(history, SERVER_KNOBS->PAGED_RESOLVER_POOL_PAGES);
}
const char* conflictSetIndexName(const ConflictSet*) {
	return "btree-v2";
}

namespace {
void printBTreeStats(const ConflictSetStats& before,
                     const ConflictSetStats& after,
                     const paged_btree::PagedBTreeConflictSet& history);
} // namespace
void clearConflictSet(ConflictSet* cs, Version v) {
	// Only the skip list can start over as if every key had been written at v; nothing calls this
	ASSERT(cs->versionHistory.has_value());
	SkipList(v).swap(*cs->versionHistory);
}

ConflictSetStats getConflictSetStats(const ConflictSet* cs) {
	ConflictSetStats stats;
	stats.checkReadSeconds = g_checkRead.getValue();
	stats.checkIntraBatchSeconds = g_checkBatch.getValue();
	stats.combineSeconds = g_combine.getValue();
	stats.mergeSeconds = g_merge.getValue();
	stats.cleanupSeconds = g_removeBefore.getValue();
	if (cs->pagedHistory) {
		const TemporalConflictSet& history = *cs->pagedHistory;
		const PageBudget& budget = history.budget();
		stats.paged = true;
		stats.validationMinRV = history.validationMinRV();
		stats.poolLimitPages = budget.limitPages;
		stats.poolFreePages = budget.freePages;
		stats.poolHeapPages = budget.heapPages;
		stats.poolIndexPages = budget.indexPages;
		stats.reclaimedHistoryPages = history.reclaimedHistoryPages();
		stats.cleanedCells = cs->pagedCleanedCells;
		stats.tooOldWithinWindow = cs->pagedTooOldWithinWindow;
		stats.boundaryPastWindow = cs->pagedBoundaryPastWindow;
	} else if (cs->btreeHistory) {
		const paged_btree::PagedBTreeConflictSet& history = *cs->btreeHistory;
		const PageBudget& budget = history.budget();
		const paged_btree::PagedBTreeConflictSet::Stats& work = history.stats();
		stats.paged = true;
		stats.btree = true;
		stats.validationMinRV = history.validationMinRV();
		stats.poolLimitPages = budget.limitPages;
		stats.poolFreePages = budget.freePages;
		stats.poolHeapPages = budget.heapPages;
		stats.poolIndexPages = budget.indexPages;
		stats.reclaimedHistoryPages = history.reclaimedHistoryPages();
		stats.cleanedIntervals = cs->btreeCleanedIntervals;
		stats.tooOldWithinWindow = cs->pagedTooOldWithinWindow;
		stats.boundaryPastWindow = cs->pagedBoundaryPastWindow;
		stats.btreeHeight = history.height();
		stats.btreeNodes = history.nodeCount();
		stats.btreeLeaves = history.leafCount();
		stats.btreeIntervalInsertions = work.intervalInsertions;
		stats.btreeOverwrittenIntervals = work.overwrittenIntervals;
		stats.btreeExpiredIntervals = work.expiredIntervals;
		stats.btreeDiscardedWrites = work.discardedWrites;
		stats.btreeSplits = work.splits;
		stats.btreeInternalSplits = work.internalSplits;
		stats.btreeMerges = work.merges;
		stats.btreeInternalMerges = work.internalMerges;
		stats.btreeCleanupLeavesVisited = work.cleanupLeavesVisited;
		stats.btreeMergeCandidatesExamined = work.mergeCandidatesExamined;
		stats.btreeIndexPagesReleased = work.indexPagesReleased;
		stats.btreeReclaimCalls = work.reclaimCalls;
		stats.btreeReclaimLeavesVisited = work.reclaimLeavesVisited;
	}
	// btree-v2 resolves every batch of this binary: its boundary and counters
	stats.paged = true;
	stats.btree = false;
	stats.validationMinRV = cs->btreeV2.reclaimCV();
	stats.tooOldWithinWindow = cs->pagedTooOldWithinWindow;
	stats.boundaryPastWindow = cs->pagedBoundaryPastWindow;
	return stats;
}
void destroyConflictSet(ConflictSet* cs) {
	delete cs;
}

ConflictBatch::ConflictBatch(ConflictSet* cs,
                             std::map<int, VectorRef<int>>* conflictingKeyRangeMap,
                             Arena* resolveBatchReplyArena)
  : cs(cs), transactionCount(0), conflictingKeyRangeMap(conflictingKeyRangeMap),
    resolveBatchReplyArena(resolveBatchReplyArena) {}

ConflictBatch::~ConflictBatch() = default;

bool ConflictBatch::ignoreTooOld() const {
	return bugs && deterministicRandom()->random01() < bugs->ignoreTooOldProbability;
}

bool ConflictBatch::ignoreReadSet() const {
	return bugs && deterministicRandom()->random01() < bugs->ignoreReadSetProbability;
}

bool ConflictBatch::ignoreWriteSet() const {
	return bugs && deterministicRandom()->random01() < bugs->ignoreWriteSetProbability;
}

void ConflictBatch::addTransaction(const CommitTransactionRef& tr, Version newOldestVersion) {
	const int t = transactionCount++;

	Arena& arena = transactionInfo.arena();
	auto* info = new (arena) TransactionInfo;
	info->reportConflictingKeys = tr.report_conflicting_keys;
	bool tooOld = tr.read_snapshot < newOldestVersion && !tr.read_conflict_ranges.empty();
	if (tooOld && ignoreTooOld()) {
		bugs->hit();
		tooOld = false;
	}

	if (tooOld) {
		info->tooOld = true;
	} else {
		info->tooOld = false;
		if (!ignoreReadSet()) {
			info->readRanges.resize(arena, tr.read_conflict_ranges.size());
		} else {
			bugs->hit();
		}
		if (!ignoreWriteSet()) {
			info->writeRanges.resize(arena, tr.write_conflict_ranges.size());
		} else {
			bugs->hit();
		}

		for (int r = 0; r < info->readRanges.size(); r++) {
			const KeyRangeRef& range = tr.read_conflict_ranges[r];
			points.emplace_back(range.begin, true, false, t, &info->readRanges[r].first);
			points.emplace_back(range.end, false, false, t, &info->readRanges[r].second);
			combinedReadConflictRanges.emplace_back(range.begin,
			                                        range.end,
			                                        tr.read_snapshot,
			                                        t,
			                                        r,
			                                        tr.report_conflicting_keys ? &(*conflictingKeyRangeMap)[t]
			                                                                   : nullptr,
			                                        tr.report_conflicting_keys ? resolveBatchReplyArena : nullptr);
		}
		for (int r = 0; r < info->writeRanges.size(); r++) {
			const KeyRangeRef& range = tr.write_conflict_ranges[r];
			points.emplace_back(range.begin, true, true, t, &info->writeRanges[r].first);
			points.emplace_back(range.end, false, true, t, &info->writeRanges[r].second);
		}
	}

	transactionInfo.push_back(arena, info);
}

class MiniConflictSet : NonCopyable {
	std::vector<uint64_t> words;

public:
	explicit MiniConflictSet(size_t size) {
		ASSERT(size <= std::numeric_limits<size_t>::max() - 63); // Prevent overflow in (size + 63)
		words.resize((size + 63) / 64, 0);
	}

	void set(int begin, int end) {
		if (end <= begin)
			return;

		const size_t wordBegin = static_cast<size_t>(begin) / 64;
		const size_t wordEnd = static_cast<size_t>(end - 1) / 64; // Last word containing bits (inclusive)

		if (wordBegin == wordEnd) {
			// Single word case
			const size_t bitStart = static_cast<size_t>(begin) % 64;
			const size_t numBits = static_cast<size_t>(end - begin);

			ASSERT(numBits <= 64); // Single word should never span > 64 bits
			uint64_t mask;
			if (numBits == 64) {
				mask = ~0ULL;
			} else {
				mask = ((1ULL << numBits) - 1) << bitStart;
			}
			words[wordBegin] |= mask;
		} else {
			// Multi-word case: wordBegin (partial) + middle words (full) + wordEnd (partial)
			words[wordBegin] |= ~0ULL << (static_cast<size_t>(begin) % 64);
			for (size_t w = wordBegin + 1; w < wordEnd; w++) { // Fill middle words completely
				words[w] = ~0ULL;
			}
			words[wordEnd] |= ~0ULL >> (63 - (static_cast<size_t>(end - 1) % 64));
		}
	}

	bool any(int begin, int end) const {
		if (end <= begin)
			return false;

		const size_t wordBegin = static_cast<size_t>(begin) / 64;
		const size_t wordEnd = static_cast<size_t>(end - 1) / 64; // Last word containing bits (inclusive)

		for (size_t w = wordBegin; w <= wordEnd; w++) { // Check all words including wordEnd
			uint64_t mask = ~0ULL;
			if (w == wordBegin) {
				mask &= (~0ULL << (static_cast<size_t>(begin) % 64));
			}
			if (w == wordEnd) {
				mask &= (~0ULL >> (63 - (static_cast<size_t>(end - 1) % 64)));
			}

			if (words[w] & mask)
				return true;
		}
		return false;
	}

	void clear() { std::fill(words.begin(), words.end(), 0); }
};

void ConflictBatch::checkIntraBatchConflicts() {
	int index = 0;
	for (int p = 0; p < points.size(); p++)
		*points[p].pIndex = index++;

	MiniConflictSet mcs(index);
	for (int t = 0; t < transactionInfo.size(); t++) {
		const TransactionInfo& tr = *transactionInfo[t];
		if (transactionConflictStatus[t])
			continue;
		bool conflict = tr.tooOld;
		for (int i = 0; i < tr.readRanges.size(); i++) {
			if (mcs.any(tr.readRanges[i].first, tr.readRanges[i].second)) {
				if (tr.reportConflictingKeys) {
					(*conflictingKeyRangeMap)[t].push_back(*resolveBatchReplyArena, i);
				}
				conflict = true;
				break;
			}
		}
		transactionConflictStatus[t] = conflict;
		if (!conflict)
			for (int i = 0; i < tr.writeRanges.size(); i++)
				mcs.set(tr.writeRanges[i].first, tr.writeRanges[i].second);
	}
}

void ConflictBatch::GetTooOldTransactions(std::vector<int>& tooOldTransactions) {
	for (int i = 0; i < transactionInfo.size(); i++) {
		if (transactionInfo[i]->tooOld) {
			tooOldTransactions.push_back(i);
		}
	}
}

void ConflictBatch::detectConflicts(Version now,
                                    Version newOldestVersion,
                                    std::vector<int>& nonConflicting,
                                    std::vector<int>* tooOldTransactions) {
	double t = timer();
	sortPoints(points, cs->btreeHistory ? &pointLcps : nullptr);
	g_sort += timer() - t;

	transactionConflictStatus = new bool[transactionCount];
	memset(transactionConflictStatus, 0, transactionCount * sizeof(bool));

	t = timer();
	checkReadConflictRanges();
	g_checkRead += timer() - t;

	t = timer();
	checkIntraBatchConflicts();
	g_checkBatch += timer() - t;

	t = timer();
	combineWriteConflictRanges();
	g_combine += timer() - t;

	t = timer();
	mergeWriteConflictRanges(now);
	g_merge += timer() - t;

	for (int i = 0; i < transactionCount; i++) {
		if (tooOldTransactions && transactionInfo[i]->tooOld) {
			tooOldTransactions->push_back(i);
		} else if (!transactionConflictStatus[i]) {
			nonConflicting.push_back(i);
		}
	}

	delete[] transactionConflictStatus;

	t = timer();
	// btree-v2 keeps history by capacity: it reclaims, oldest first, only when a write needs room,
	// so there is nothing to clean at the end of the batch
	cs->oldestVersion = std::max(cs->oldestVersion, newOldestVersion);
	cs->pagedBoundaryPastWindow = std::max<int64_t>(
	    cs->pagedBoundaryPastWindow, cs->btreeV2.reclaimCV() - std::max<Version>(newOldestVersion, 0));
	g_removeBefore += timer() - t;
}

namespace {

// Tasks that checkReads interleaves, from BTREE_V2_TASKS: 8, 16, 24 or 32 (the default)
int btreeV2Tasks() {
	static const int tasks = [] {
		const char* value = getenv("BTREE_V2_TASKS");
		const int n = value != nullptr ? atoi(value) : 32;
		ASSERT(n == 8 || n == 16 || n == 24 || n == 32);
		return n;
	}();
	return tasks;
}

} // namespace

void ConflictBatch::checkReadConflictRanges() {
	if (combinedReadConflictRanges.empty())
		return;

	// The first read range of each transaction: its reads are consecutive, in their order
	std::vector<int> offsets(transactionCount, 0);
	for (int i = combinedReadConflictRanges.size(); i-- > 0;) {
		offsets[combinedReadConflictRanges[i].transaction] = i;
	}
	std::vector<uint8_t> conflicts;
	int64_t tooOld = 0;
	btree_v2::TempConflictBTree& history = cs->btreeV2;
	TransactionInfo* const* info = transactionInfo.begin();
	switch (btreeV2Tasks()) {
	case 8:
		history.checkReads<8>(points, info, combinedReadConflictRanges, offsets, transactionConflictStatus, conflicts, tooOld);
		break;
	case 16:
		history.checkReads<16>(points, info, combinedReadConflictRanges, offsets, transactionConflictStatus, conflicts, tooOld);
		break;
	case 24:
		history.checkReads<24>(points, info, combinedReadConflictRanges, offsets, transactionConflictStatus, conflicts, tooOld);
		break;
	default:
		history.checkReads<32>(points, info, combinedReadConflictRanges, offsets, transactionConflictStatus, conflicts, tooOld);
		break;
	}
	cs->pagedTooOldWithinWindow += tooOld;
	for (int i = 0; i < conflicts.size(); ++i) {
		const ReadConflictRange& r = combinedReadConflictRanges[i];
		if (conflicts[i] && r.conflictingKeyRange != nullptr) {
			r.conflictingKeyRange->push_back(*r.cKRArena, r.indexInTx);
		}
	}
}

void ConflictBatch::addConflictRanges(Version now,
                                      std::vector<std::pair<StringRef, StringRef>>::iterator begin,
                                      std::vector<std::pair<StringRef, StringRef>>::iterator end,
                                      SkipList* part) {
	const int count = end - begin;
	static_assert(sizeof(*begin) == sizeof(StringRef) * 2,
	              "Write Conflict Range type not convertible to two StringPtrs");
	const StringRef* strings = reinterpret_cast<const StringRef*>(&*begin);
	const int stringCount = count * 2;

	const int stripeSize = 16;
	SkipList::Finger fingers[stripeSize];
	int temp[stripeSize];
	int stripes = (stringCount + stripeSize - 1) / stripeSize;

	int ss = stringCount - (stripes - 1) * stripeSize;
	for (int s = stripes - 1; s >= 0; s--) {
		part->find(&strings[static_cast<size_t>(s) * stripeSize], fingers, temp, ss);
		part->addConflictRanges(fingers, ss / 2, now);
		ss = stripeSize;
	}
}

void ConflictBatch::checkPagedReadConflictRanges() {
	TemporalConflictSet& history = *cs->pagedHistory;
	// The boundary does not move while reads are checked: nothing is inserted or reclaimed
	const Version boundary = history.validationMinRV();
	std::vector<TemporalSkipList::PointQuery> pointQueries;
	std::vector<const ReadConflictRange*> pointReads;
	pointQueries.reserve(combinedReadConflictRanges.size());
	pointReads.reserve(combinedReadConflictRanges.size());

	for (const ReadConflictRange& r : combinedReadConflictRanges) {
		TransactionInfo& info = *transactionInfo[r.transaction];
		if (info.tooOld) {
			continue;
		}
		if (r.version < boundary) {
			// The window admitted this read, but the history it needs has been reclaimed. All of a
			// transaction's ranges share its read version, so this is its first range and nothing
			// was reported
			info.tooOld = true;
			transactionConflictStatus[r.transaction] = true;
			++cs->pagedTooOldWithinWindow;
			continue;
		}
		// A transaction that already conflicts needs its other ranges only to report them
		if (transactionConflictStatus[r.transaction] && r.conflictingKeyRange == nullptr) {
			continue;
		}
		if (history.hasRangeConflict(r.version, r.begin, r.end)) {
			transactionConflictStatus[r.transaction] = true;
			if (r.conflictingKeyRange != nullptr) {
				r.conflictingKeyRange->push_back(*r.cKRArena, r.indexInTx);
			}
			continue;
		}
		pointQueries.push_back({ r.version, r.begin, r.end });
		pointReads.push_back(&r);
	}

	// The point index answers the reads the range index did not, all in one interleaved batch
	history.hasPointConflicts(pointQueries.data(), pointQueries.size());
	for (size_t k = 0; k < pointQueries.size(); ++k) {
		if (!pointQueries[k].conflict) {
			continue;
		}
		const ReadConflictRange& r = *pointReads[k];
		transactionConflictStatus[r.transaction] = true;
		if (r.conflictingKeyRange != nullptr) {
			r.conflictingKeyRange->push_back(*r.cKRArena, r.indexInTx);
		}
	}
}

void ConflictBatch::checkBTreeReadConflictRanges() {
	paged_btree::PagedBTreeConflictSet& history = *cs->btreeHistory;
	// The boundary does not move while reads are checked: nothing is inserted or reclaimed
	const Version boundary = history.validationMinRV();
	std::vector<paged_btree::PagedBTreeConflictSet::Query> queries;
	std::vector<const ReadConflictRange*> reads;
	queries.reserve(combinedReadConflictRanges.size());
    reads.reserve(combinedReadConflictRanges.size());
    std::vector<uint32_t> queryForRange(combinedReadConflictRanges.size(), UINT32_MAX);

	// Admission is decided per transaction before the batch is formed, so the batch holds only
	// admitted reads and one too-old transaction cannot stop the others from being checked
	for (const ReadConflictRange& r : combinedReadConflictRanges) {
        TransactionInfo& info = *transactionInfo[r.transaction];
        const size_t readIndex = size_t(&r - combinedReadConflictRanges.data());
        ASSERT(readIndex <= size_t(std::numeric_limits<int>::max()));
        // pIndex slots temporarily identify read ranges. The subsequent intra-batch
        // check replaces them with sorted point positions, as in the original code.
        info.readRanges[r.indexInTx].first = info.readRanges[r.indexInTx].second = static_cast<int>(readIndex);
		if (info.tooOld) {
			continue;
		}
		if (r.version < boundary) {
			// All of a transaction's ranges share its read version, so this is its first range
			// and none of its ranges is in the batch yet
			info.tooOld = true;
			transactionConflictStatus[r.transaction] = true;
			++cs->pagedTooOldWithinWindow;
			continue;
		}
        ASSERT(queries.size() < UINT32_MAX);
        queryForRange[size_t(&r - combinedReadConflictRanges.data())] = static_cast<uint32_t>(queries.size());
        queries.push_back({ r.version, r.begin, r.end });
		reads.push_back(&r);
	}

	// One index answers points and ranges alike: every admitted read is in one interleaved
	// batch, so every conflicting range of a transaction that asked for them is reported
    ASSERT(pointLcps.size() == points.size());
    std::vector<paged_btree::PagedBTreeConflictSet::OrderedEndpoint> ordered;
    ordered.reserve(2 * queries.size());
    uint32_t gapLcp = UINT32_MAX;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& point = points[i];
        gapLcp = std::min(gapLcp, pointLcps[i]);
        if (point.write) continue;
        const int readIndex = *point.pIndex;
        ASSERT(readIndex >= 0 && size_t(readIndex) < queryForRange.size());
        const uint32_t query = queryForRange[readIndex];
        if (query == UINT32_MAX || !(queries[query].begin < queries[query].end)) continue;
        ordered.push_back({query, ordered.empty() ? 0u : gapLcp, !point.begin});
        gapLcp = UINT32_MAX;
    }
    history.hasConflictsOrdered(queries.data(), queries.size(), ordered.data(), ordered.size());
	for (size_t k = 0; k < queries.size(); ++k) {
		if (!queries[k].conflict) {
			continue;
		}
		const ReadConflictRange& r = *reads[k];
		transactionConflictStatus[r.transaction] = true;
		if (r.conflictingKeyRange != nullptr) {
			r.conflictingKeyRange->push_back(*r.cKRArena, r.indexInTx);
		}
	}
}

void ConflictBatch::mergeWriteConflictRanges(Version now) {
	if (combinedWriteConflictRanges.empty())
		return;

	// Combined ranges are sorted and disjoint, all at now; an empty one records nothing
	std::vector<std::pair<StringRef, StringRef>> writes;
	writes.reserve(combinedWriteConflictRanges.size());
	for (const auto& [begin, end] : combinedWriteConflictRanges) {
		if (begin < end) {
			writes.emplace_back(begin, end);
		}
	}
	cs->btreeV2.addWrites(std::span<const std::pair<StringRef, StringRef>>(writes.data(), writes.size()), now);
}

void ConflictBatch::combineWriteConflictRanges() {
	int activeWriteCount = 0;
	for (const KeyInfo& point : points) {
		if (point.write && !transactionConflictStatus[point.transaction]) {
			if (point.begin) {
				activeWriteCount++;
				if (activeWriteCount == 1)
					combinedWriteConflictRanges.emplace_back(point.key, KeyRef());
			} else /*if (point.end)*/ {
				activeWriteCount--;
				if (activeWriteCount == 0)
					combinedWriteConflictRanges.back().second = point.key;
			}
		}
	}
}

namespace {
// SKIPLISTTEST_KEY_BYTES sets the key length, 16 by default and never less; SKIPLISTTEST_KEY_LAYOUT
// places the number's four bytes: "tail" (the default) after a common prefix, "head" at bytes 12
// to 15 before a common suffix. Both keep the numbers' order, so the same ranges resolve the same.
// "mixed" ignores the length: it puts the number at bytes 8 to 11 and gives each number one of four
// lengths, 12, 16, 17 or 32 bytes, so a range's ends may fit inline or not; the order is kept too
int testKeyBytes() {
	static const int bytes = [] {
		const char* v = getenv("SKIPLISTTEST_KEY_BYTES");
		return v ? std::max(16, atoi(v)) : 16;
	}();
	return bytes;
}

bool testKeyAtHead() {
	static const bool head = [] {
		const char* v = getenv("SKIPLISTTEST_KEY_LAYOUT");
		return v && std::string(v) == "head";
	}();
	return head;
}

// "mid" places the number at bytes 16 to 19 (from zero), after a 16-byte common prefix: keys of 20 bytes or
// more differ only there, past 16 inline bytes but within a 12-byte node reference plus 16
bool testKeyMid() {
	static const bool mid = [] {
		const char* v = getenv("SKIPLISTTEST_KEY_LAYOUT");
		return v && std::string(v) == "mid";
	}();
	return mid;
}

bool testKeyMixed() {
	static const bool mixed = [] {
		const char* v = getenv("SKIPLISTTEST_KEY_LAYOUT");
		return v && std::string(v) == "mixed";
	}();
	return mixed;
}

StringRef setK(Arena& arena, int i) {
	char t[sizeof(i)];
	*(int*)t = i;

	static const int mixedBytes[] = { 12, 16, 17, 32 };
	const int keySize = testKeyMixed() ? mixedBytes[unsigned(i) % 4] : testKeyBytes();

	char* ss = new (arena) char[keySize];
	memset(ss, '.', keySize);
	const int at = testKeyMixed()   ? 8
	               : testKeyAtHead() ? 16 - int(sizeof(i))
	               : testKeyMid() && keySize >= 16 + int(sizeof(i)) ? 16
	                                                          : keySize - int(sizeof(i));
	for (int c = 0; c < sizeof(i); c++)
		ss[at + c] = t[sizeof(i) - 1 - c];

	return StringRef((const uint8_t*)ss, keySize);
}

void miniConflictSetTest() {
	for (int i = 0; i < 2000000; i++) {
		int size = 64 * 5; // Also run 64*64*5 to test multiple words of andValues and orValues
		MiniConflictSet mini(size);
		for (int j = 0; j < 2; j++) {
			int a = deterministicRandom()->randomInt(0, size);
			int b = deterministicRandom()->randomInt(a, size);
			mini.set(a, b);
		}
		for (int j = 0; j < 4; j++) {
			int a = deterministicRandom()->randomInt(0, size);
			int b = deterministicRandom()->randomInt(a, size);
			mini.any(a, b); // Tests correctness internally
		}
	}
	printf("miniConflictSetTest complete\n");
}

void operatorLessThanTest() {
	{ // Longer strings before shorter strings.
		KeyInfo a("hello"_sr, /*begin=*/false, /*write=*/true, 0, nullptr);
		KeyInfo b("hello\0"_sr, /*begin=*/false, /*write=*/false, 0, nullptr);
		ASSERT(a < b);
		ASSERT(!(b < a));
		ASSERT(!(a == b));
	}

	{ // Reads before writes.
		KeyInfo a("hello"_sr, /*begin=*/false, /*write=*/false, 0, nullptr);
		KeyInfo b("hello"_sr, /*begin=*/false, /*write=*/true, 0, nullptr);
		ASSERT(a < b);
		ASSERT(!(b < a));
		ASSERT(!(a == b));
	}

	{ // Begin reads after writes.
		KeyInfo a("hello"_sr, /*begin=*/false, /*write=*/true, 0, nullptr);
		KeyInfo b("hello"_sr, /*begin=*/true, /*write=*/false, 0, nullptr);
		ASSERT(a < b);
		ASSERT(!(b < a));
		ASSERT(!(a == b));
	}

	{ // Begin writes after writes.
		KeyInfo a("hello"_sr, /*begin=*/false, /*write=*/true, 0, nullptr);
		KeyInfo b("hello"_sr, /*begin=*/true, /*write=*/true, 0, nullptr);
		ASSERT(a < b);
		ASSERT(!(b < a));
		ASSERT(!(a == b));
	}
}
} // namespace

void skipListTest() {
	printf("Skip list test\n");

	miniConflictSetTest();

	operatorLessThanTest();

	// SKIPLISTTEST_CPU chooses the CPU the benchmark pins itself to, 0 by default as upstream;
	// setAffinity ignores failure, so the CPU it actually runs on is printed
	const char* cpuVariable = getenv("SKIPLISTTEST_CPU");
	const int cpu = cpuVariable ? atoi(cpuVariable) : 0;
	setAffinity(cpu);
#ifdef __linux__
	printf("skiplisttest cpu requested=%d running=%d\n", cpu, sched_getcpu());
#endif
	printf("skiplisttest keyBytes=%d layout=%s\n", testKeyBytes(), testKeyMixed() ? "mixed" : testKeyAtHead() ? "head" : testKeyMid() ? "mid" : "tail");

	double start;

	ConflictSet* cs = newConflictSet();

	Arena testDataArena;
	VectorRef<VectorRef<KeyRangeRef>> testData;
	const int batches = 500; // deterministicRandom()->randomInt(500, 5000);
	// SKIPLISTTEST_PER_BATCH sets the ranges per batch (half reads, half writes; 5,000 by default) and
	// SKIPLISTTEST_RANGE_SPAN the most keys a range adds past its begin (10 by default): wider spans make
	// ranges overlap more. The defaults draw the same sequence as before
	const char* perBatchEnv = getenv("SKIPLISTTEST_PER_BATCH");
	const char* spanEnv = getenv("SKIPLISTTEST_RANGE_SPAN");
	const int data_per_batch = perBatchEnv ? std::max(2, atoi(perBatchEnv)) : 5000;
	const int rangeSpan = spanEnv ? std::max(0, atoi(spanEnv)) : 10;
	testData.resize(testDataArena, batches);
	std::vector<std::vector<uint8_t>> success(batches);
	std::vector<std::vector<uint8_t>> success2(batches);
	for (int i = 0; i < batches; i++) {
		testData[i].resize(testDataArena, data_per_batch);
		success[i].assign(data_per_batch, false);
		success2[i].assign(data_per_batch, false);
		for (int j = 0; j < data_per_batch; j++) {
			int key = deterministicRandom()->randomInt(0, 20000000);
			int key2 = key + 1 + deterministicRandom()->randomInt(0, rangeSpan);
			testData[i][j] = KeyRangeRef(setK(testDataArena, key), setK(testDataArena, key2));
		}
	}
	printf("Test data generated: %d batches, %d/batch, span %d\n", batches, data_per_batch, rangeSpan);

	printf("Running\n");

	int readCount = 1, writeCount = 1;
	int cranges = 0, tcount = 0;

	start = timer();
	std::vector<std::vector<int>> nonConflict(batches);
	// SKIPLISTTEST_TOOOLD=1 also collects, per batch, the transactions rejected as too old, and writes
	// them to the decisions file after the committed ones; off, the batch call is unchanged
	static const bool collectTooOld = getenv("SKIPLISTTEST_TOOOLD") != nullptr;
	std::vector<std::vector<int>> tooOld(collectTooOld ? nonConflict.size() : 0);
	// SKIPLISTTEST_MODEL=1, with SKIPLISTTEST_TOOOLD, replays each batch on FDB's skip list without
	// the transactions the history rejected, which therefore wrote nothing, and counts the admitted
	// transactions whose decision differs. It doubles the work and uses the same phase timers, so a
	// run with it checks decisions and is never timed
	static const bool checkModel = collectTooOld && getenv("SKIPLISTTEST_MODEL") != nullptr;
	std::unique_ptr<ConflictSet> model(checkModel ? new ConflictSet(ConflictHistory::SkipList, 0) : nullptr);
	uint64_t modelChecked = 0, modelMismatches = 0, rejectedTransactions = 0, batchesWithRejections = 0;
	Version version = 0;
	// SKIPLISTTEST_SEGMENTS=n prints one line every n batches: phase times, per-batch latency, and
	// for a paged history its pool, reclamation, boundary against the window and structural work.
	// Read between batches, outside the timed calls
	const char* segmentEnv = getenv("SKIPLISTTEST_SEGMENTS");
	const int segment = segmentEnv ? atoi(segmentEnv) : 0;
	ConflictSetStats segmentStart = getConflictSetStats(cs);
	std::vector<double> segmentLatencies;
	for (const auto& data : testData) {
		Arena buf;
		std::vector<CommitTransactionRef> trs;
		double t = timer();
		for (int j = 0; j + readCount + writeCount <= data.size(); j += readCount + writeCount) {
			CommitTransactionRef tr;
			for (int k = 0; k < readCount; k++) {
				KeyRangeRef r(buf, data[j + k]);
				tr.read_conflict_ranges.push_back(buf, r);
			}
			for (int k = 0; k < writeCount; k++) {
				KeyRangeRef r(buf, data[j + readCount + k]);
				tr.write_conflict_ranges.push_back(buf, r);
			}
			cranges += tr.read_conflict_ranges.size() + tr.write_conflict_ranges.size();
			tr.read_snapshot = version;
			trs.push_back(tr);
		}
		tcount += trs.size();
		g_buildTest += timer() - t;

		t = timer();
		ConflictBatch batch(cs);
		for (const auto& tr : trs) {
			batch.addTransaction(tr, version);
		}
		g_add += timer() - t;

		t = timer();
		batch.detectConflicts(version + 50, version, nonConflict[version], collectTooOld ? &tooOld[version] : nullptr);
		const double batchSeconds = timer() - t;
		g_detectConflicts += batchSeconds;

		if (checkModel) {
			std::vector<uint8_t> rejected(trs.size(), 0), committed(trs.size(), 0);
			for (int t : tooOld[version]) rejected[t] = 1;
			for (int t : nonConflict[version]) committed[t] = 1;
			std::vector<int> admitted;
			ConflictBatch replay(model.get());
			for (int t = 0; t < int(trs.size()); ++t) {
				if (rejected[t]) continue;
				admitted.push_back(t);
				replay.addTransaction(trs[t], version);
			}
			std::vector<int> modelCommitted;
			replay.detectConflicts(version + 50, version, modelCommitted);
			std::vector<uint8_t> expected(trs.size(), 0);
			for (int m : modelCommitted) expected[admitted[m]] = 1;
			for (int t : admitted) modelMismatches += expected[t] != committed[t];
			modelChecked += admitted.size();
			rejectedTransactions += tooOld[version].size();
			batchesWithRejections += !tooOld[version].empty();
		}

		if (segment > 0) {
			segmentLatencies.push_back(batchSeconds);
			if ((version + 1) % segment == 0) {
				const ConflictSetStats now = getConflictSetStats(cs);
				std::sort(segmentLatencies.begin(), segmentLatencies.end());
				auto at = [&](double p) {
					return segmentLatencies[std::min(segmentLatencies.size() - 1, size_t(p * segmentLatencies.size()))];
				};
				// A read at version v is admitted by the window (oldest v) and by the boundary when
				// v >= validationMinRV; margin is how far the boundary still is below the window
				printf("Segment batches=%lld-%lld readS=%.4f writeS=%.4f cleanupS=%.4f batchP50ms=%.3f "
				       "batchP99ms=%.3f batchMaxMs=%.3f reclaimedPages=%lld validationMinRV=%lld windowMargin=%lld "
				       "tooOld=%lld heapPages=%lld indexPages=%lld freePages=%lld expiredIntervals=%lld "
				       "cleanedIntervals=%lld indexPagesReleased=%lld splits=%lld merges=%lld leaves=%lld nodes=%lld "
				       "discardedWrites=%lld reclaimCalls=%lld reclaimLeavesVisited=%lld\n",
				       (long long)(version + 1 - segmentLatencies.size()),
				       (long long)version,
				       now.checkReadSeconds - segmentStart.checkReadSeconds,
				       now.mergeSeconds - segmentStart.mergeSeconds,
				       now.cleanupSeconds - segmentStart.cleanupSeconds,
				       at(0.5) * 1e3,
				       at(0.99) * 1e3,
				       segmentLatencies.back() * 1e3,
				       (long long)(now.reclaimedHistoryPages - segmentStart.reclaimedHistoryPages),
				       (long long)now.validationMinRV,
				       (long long)(version - now.validationMinRV),
				       (long long)(now.tooOldWithinWindow - segmentStart.tooOldWithinWindow),
				       (long long)now.poolHeapPages,
				       (long long)now.poolIndexPages,
				       (long long)now.poolFreePages,
				       (long long)(now.btreeExpiredIntervals - segmentStart.btreeExpiredIntervals),
				       (long long)(now.cleanedIntervals - segmentStart.cleanedIntervals),
				       (long long)(now.btreeIndexPagesReleased - segmentStart.btreeIndexPagesReleased),
				       (long long)(now.btreeSplits - segmentStart.btreeSplits),
				       (long long)(now.btreeMerges - segmentStart.btreeMerges),
				       (long long)now.btreeLeaves,
				       (long long)now.btreeNodes,
				       (long long)(now.btreeDiscardedWrites - segmentStart.btreeDiscardedWrites),
				       (long long)(now.btreeReclaimCalls - segmentStart.btreeReclaimCalls),
				       (long long)(now.btreeReclaimLeavesVisited - segmentStart.btreeReclaimLeavesVisited));
				segmentStart = now;
				segmentLatencies.clear();
			}
		}

		version++;
	}
	double elapsed = timer() - start;
	printf("New conflict set: %0.3f sec\n", elapsed);
	printf("                  %0.3f Mtransactions/sec\n", tcount / elapsed / 1e6);
	printf("                  %0.3f Mkeys/sec\n", cranges * 2 / elapsed / 1e6);

	elapsed = g_detectConflicts.getValue();
	printf("Detect only:      %0.3f sec\n", elapsed);
	printf("                  %0.3f Mtransactions/sec\n", tcount / elapsed / 1e6);
	printf("                  %0.3f Mkeys/sec\n", cranges * 2 / elapsed / 1e6);

	elapsed = g_checkRead.getValue() + g_merge.getValue();
	printf("Skiplist only:    %0.3f sec\n", elapsed);
	printf("                  %0.3f Mtransactions/sec\n", tcount / elapsed / 1e6);
	printf("                  %0.3f Mkeys/sec\n", cranges * 2 / elapsed / 1e6);

	printf("Performance counters:\n");
	for (const auto& counter : skc) {
		printf("%20s: %s\n", counter->getMetric().name().c_str(), counter->getMetric().formatted().c_str());
	}

	if (cs->versionHistory) {
		printf("%d entries in version history\n", cs->versionHistory->count());
	}

	// Added to compare the two histories on this workload: the decisions both must agree on, and,
	// for the paged set, how much of its pool the history took and whether reclamation cut into it
	size_t committed = 0;
	for (const auto& batchCommitted : nonConflict) {
		committed += batchCommitted.size();
	}
	printf("%zu transactions without conflict\n", committed);
	// Per-transaction decisions: an FNV-1a hash over every batch's committed indices, in order,
	// so two runs agree on it only if they commit the same transactions of the same batches.
	// SKIPLISTTEST_DECISIONS names a file to receive them in full, one batch per line
	uint64_t decisionsHash = 1469598103934665603ULL;
	auto mix = [&](uint64_t value) {
		for (int b = 0; b < 8; ++b) {
			decisionsHash = (decisionsHash ^ ((value >> (8 * b)) & 0xFF)) * 1099511628211ULL;
		}
	};
	for (size_t b = 0; b < nonConflict.size(); ++b) {
		mix(b);
		mix(nonConflict[b].size());
		for (int t : nonConflict[b]) {
			mix(t);
		}
	}
	printf("index=%s decisionsHash=%016llx\n", conflictSetIndexName(cs), (unsigned long long)decisionsHash);
	if (checkModel) {
		printf("model admitted=%llu mismatches=%llu rejected=%llu batchesWithRejections=%llu\n",
		       (unsigned long long)modelChecked, (unsigned long long)modelMismatches,
		       (unsigned long long)rejectedTransactions, (unsigned long long)batchesWithRejections);
	}
	if (const char* path = getenv("SKIPLISTTEST_DECISIONS")) {
		if (FILE* out = fopen(path, "w")) {
			for (size_t b = 0; b < nonConflict.size(); ++b) {
				fprintf(out, "%zu:", b);
				for (int t : nonConflict[b]) {
					fprintf(out, " %d", t);
				}
				if (collectTooOld) {
					fprintf(out, " |");
					for (int t : tooOld[b]) {
						fprintf(out, " %d", t);
					}
				}
				fprintf(out, "\n");
			}
			fclose(out);
		}
	}
	const ConflictSetStats stats = getConflictSetStats(cs);
	printf("phases: checkReadS=%.4f intraBatchS=%.4f combineS=%.4f mergeS=%.4f cleanupS=%.4f\n",
	       stats.checkReadSeconds,
	       stats.checkIntraBatchSeconds,
	       stats.combineSeconds,
	       stats.mergeSeconds,
	       stats.cleanupSeconds);
	printf("residentMiB=%.1f validationMinRV=%lld boundaryPastWindow=%lld\n",
	       getResidentMemoryUsage() / 1048576.0,
	       (long long)stats.validationMinRV,
	       (long long)stats.boundaryPastWindow);
	if (stats.btree) {
		printBTreeStats(ConflictSetStats(), stats, *cs->btreeHistory);
		const auto& backing = cs->btreeHistory->poolBacking();
		printf("PoolBacking: mode=%s indexBegin=%#lx indexBytes=%zu indexMapped=%zu indexPageKiB=%u setupNs=%llu "
		       "readerGroupSize=%u\n",
		       poolBackingName(backing.backing),
		       (unsigned long)backing.index.begin,
		       backing.index.usefulBytes,
		       backing.index.mappedBytes,
		       backing.index.pageKiB,
		       (unsigned long long)backing.setupNanoseconds,
		       cs->btreeHistory->readerConfig().sweepGroupSize);
	}
	if (stats.paged) {
		printf("paged set: %lld of %lld pool pages used (%lld heap, %lld index), %lld history pages reclaimed, "
		       "%lld reads too old within the window\n",
		       (long long)(stats.poolLimitPages - stats.poolFreePages),
		       (long long)stats.poolLimitPages,
		       (long long)stats.poolHeapPages,
		       (long long)stats.poolIndexPages,
		       (long long)stats.reclaimedHistoryPages,
		       (long long)stats.tooOldWithinWindow);
	}
}

TEST_CASE("/fdbserver/skiplist/miniConflictSetCompatibility") {
	// Unit test written by Claude AI assistant
	// Reference implementation using std::vector<bool> for comparison
	class ReferenceMiniConflictSet {
		std::vector<bool> bits;

	public:
		explicit ReferenceMiniConflictSet(size_t size) : bits(size, false) {}

		void set(int begin, int end) {
			if (end <= begin)
				return;
			for (int i = begin; i < end; i++) {
				bits[i] = true;
			}
		}

		bool any(int begin, int end) const {
			if (end <= begin)
				return false;
			for (int i = begin; i < end; i++) {
				if (bits[i])
					return true;
			}
			return false;
		}

		void clear() { std::fill(bits.begin(), bits.end(), false); }
	};

	auto testCase = [](size_t size,
	                   const std::vector<std::pair<int, int>>& setOperations,
	                   const std::vector<std::pair<int, int>>& anyQueries) {
		MiniConflictSet mcs(size);
		ReferenceMiniConflictSet ref(size);

		// Apply set operations to both implementations
		for (const auto& op : setOperations) {
			mcs.set(op.first, op.second);
			ref.set(op.first, op.second);
		}

		// Compare any() results
		for (const auto& query : anyQueries) {
			bool mcsResult = mcs.any(query.first, query.second);
			bool refResult = ref.any(query.first, query.second);
			ASSERT(mcsResult == refResult);
		}
	};

	// Test 1: Edge cases - empty ranges and boundary conditions
	// Rationale: Empty ranges (begin == end) should be no-ops but could cause issues
	// if the implementation doesn't handle them properly. This catches off-by-one errors.
	{
		std::vector<std::pair<int, int>> setOps = { { 0, 0 }, { 5, 5 }, { 10, 10 } }; // Empty ranges
		std::vector<std::pair<int, int>> queries = { { 0, 1 }, { 5, 6 }, { 9, 11 }, { 0, 64 } };
		testCase(64, setOps, queries);
	}

	// Test 2: Single bit operations at critical positions
	// Rationale: Single bits test the minimal unit of operation and catch issues with
	// bit indexing, especially at word boundaries (0, 63) and middle positions.
	{
		std::vector<std::pair<int, int>> setOps = { { 0, 1 }, { 63, 64 }, { 32, 33 } };
		std::vector<std::pair<int, int>> queries = { { 0, 1 }, { 63, 64 }, { 32, 33 }, { 0, 64 }, { 31, 34 } };
		testCase(64, setOps, queries);
	}

	// Test 3: Full word operations (64 bits)
	// Rationale: Setting/querying entire 64-bit words tests the numBits == 64 special case
	// in set() method and ensures correct mask generation for full words.
	{
		std::vector<std::pair<int, int>> setOps = { { 0, 64 } };
		std::vector<std::pair<int, int>> queries = { { 0, 64 }, { 0, 32 }, { 32, 64 }, { 10, 50 } };
		testCase(64, setOps, queries);
	}

	// Test 4: Multi-word operations
	// Rationale: Operations spanning multiple 64-bit words exercise the multi-word logic
	// in both set() and any(). This catches issues with word iteration and mask application.
	{
		std::vector<std::pair<int, int>> setOps = { { 0, 128 }, { 64, 192 }, { 100, 300 } };
		std::vector<std::pair<int, int>> queries = { { 0, 64 }, { 64, 128 }, { 128, 192 }, { 0, 320 }, { 50, 150 } };
		testCase(320, setOps, queries);
	}

	// Test 5: Overlapping ranges
	// Rationale: Overlapping set operations test that bits remain set correctly when
	// multiple ranges affect the same positions. This ensures proper OR semantics.
	{
		std::vector<std::pair<int, int>> setOps = { { 10, 50 }, { 30, 70 }, { 60, 100 } };
		std::vector<std::pair<int, int>> queries = { { 0, 10 },  { 10, 30 },  { 30, 50 }, { 50, 60 },
			                                         { 60, 70 }, { 70, 100 }, { 0, 128 } };
		testCase(128, setOps, queries);
	}

	// Test 6: Word boundary edge cases
	// Rationale: Operations that cross 64-bit word boundaries are prone to off-by-one
	// errors and incorrect mask calculations. This specifically targets boundary crossings.
	{
		std::vector<std::pair<int, int>> setOps = { { 63, 65 }, { 127, 129 }, { 191, 193 } };
		std::vector<std::pair<int, int>> queries = { { 62, 64 },   { 63, 64 },   { 64, 65 },
			                                         { 126, 128 }, { 127, 128 }, { 128, 129 } };
		testCase(256, setOps, queries);
	}

	// Test 7: Random stress test with small size
	// Rationale: Random testing catches edge cases that structured tests might miss.
	// Small size ensures good coverage of single and double-word scenarios.
	{
		std::vector<std::pair<int, int>> setOps, queries;
		for (int i = 0; i < 20; i++) {
			int a = deterministicRandom()->randomInt(0, 128);
			int b = deterministicRandom()->randomInt(a, 128);
			setOps.push_back({ a, b });
		}
		for (int i = 0; i < 50; i++) {
			int a = deterministicRandom()->randomInt(0, 128);
			int b = deterministicRandom()->randomInt(a, 128);
			queries.push_back({ a, b });
		}
		testCase(128, setOps, queries);
	}

	// Test 8: Random stress test with large size
	// Rationale: Larger random testing stresses multi-word logic and can expose
	// performance issues or correctness bugs that only appear with many words.
	{
		std::vector<std::pair<int, int>> setOps, queries;
		for (int i = 0; i < 50; i++) {
			int a = deterministicRandom()->randomInt(0, 1000);
			int b = deterministicRandom()->randomInt(a, 1000);
			setOps.push_back({ a, b });
		}
		for (int i = 0; i < 100; i++) {
			int a = deterministicRandom()->randomInt(0, 1000);
			int b = deterministicRandom()->randomInt(a, 1000);
			queries.push_back({ a, b });
		}
		testCase(1000, setOps, queries);
	}

	// Test 9: Large ranges spanning multiple words
	// Rationale: Very large ranges test the efficiency and correctness of the multi-word
	// implementation, ensuring it handles bulk operations without errors.
	{
		std::vector<std::pair<int, int>> setOps = { { 50, 500 }, { 200, 800 }, { 700, 900 } };
		std::vector<std::pair<int, int>> queries = { { 0, 50 },    { 50, 200 },  { 200, 500 }, { 500, 700 },
			                                         { 700, 800 }, { 800, 900 }, { 900, 1000 } };
		testCase(1000, setOps, queries);
	}

	// Test 10: Clear operation test
	// Rationale: The clear() operation must reset all bits to zero. This verifies
	// that clear() works correctly and doesn't leave stray bits set.
	{
		MiniConflictSet mcs(64);
		ReferenceMiniConflictSet ref(64);

		// Set some bits
		mcs.set(10, 50);
		ref.set(10, 50);

		// Verify they're set
		ASSERT(mcs.any(20, 30) == ref.any(20, 30));
		ASSERT(mcs.any(20, 30) == true);

		// Clear and verify
		mcs.clear();
		ref.clear();
		ASSERT(mcs.any(20, 30) == ref.any(20, 30));
		ASSERT(mcs.any(20, 30) == false);
		ASSERT(mcs.any(0, 64) == ref.any(0, 64));
		ASSERT(mcs.any(0, 64) == false);
	}

	// Test 11: Patterns that might expose bit manipulation bugs
	// Rationale: Alternating bit patterns create specific mask patterns that could
	// expose bugs in bit manipulation logic, especially with carry/overflow issues.
	{
		// Alternating bits pattern
		std::vector<std::pair<int, int>> setOps;
		for (int i = 0; i < 128; i += 2) {
			setOps.push_back({ i, i + 1 });
		}
		std::vector<std::pair<int, int>> queries = { { 0, 128 }, { 0, 64 }, { 64, 128 }, { 1, 127 } };
		testCase(128, setOps, queries);
	}

	// Test 12: Verify specific bit positions in multi-word scenarios
	// Rationale: First and last bits of each word are most prone to indexing errors.
	// This test specifically targets these critical positions across multiple words.
	{
		std::vector<std::pair<int, int>> setOps = { { 0, 1 }, { 63, 64 }, { 64, 65 }, { 127, 128 } };
		std::vector<std::pair<int, int>> queries = { { 0, 1 }, { 63, 64 }, { 64, 65 }, { 127, 128 }, { 0, 128 } };
		testCase(128, setOps, queries);
	}

	// Test 13: Comprehensive word boundary testing
	// Rationale: Word boundaries (multiples of 64) are critical because bit manipulation
	// logic often has special cases when ranges cross 64-bit word boundaries.
	// This test ensures set() and any() work correctly when ranges start/end at or near
	// word boundaries, catching off-by-one errors and incorrect mask calculations.
	{
		const int testSize = 320; // 5 words
		std::vector<std::pair<int, int>> setOps, queries;

		// Test every word boundary systematically
		for (int wordBoundary : { 64, 128, 192, 256 }) {
			for (int offset : { -1, 0, 1 }) {
				int pos = wordBoundary + offset;
				if (pos >= 0 && pos < testSize) {
					// Single bit at boundary
					setOps.push_back({ pos, pos + 1 });
					queries.push_back({ pos, pos + 1 });

					// Range crossing boundary
					if (pos > 5 && pos < testSize - 5) {
						setOps.push_back({ pos - 2, pos + 3 });
						queries.push_back({ pos - 2, pos + 3 });
						queries.push_back({ pos - 1, pos + 2 });
					}
				}
			}
		}

		// Test ranges that span exactly 1, 2, 3+ words
		setOps.push_back({ 0, 64 }); // Exactly 1 word
		setOps.push_back({ 64, 192 }); // Exactly 2 words
		setOps.push_back({ 128, 320 }); // Exactly 3 words

		queries.push_back({ 0, 64 });
		queries.push_back({ 64, 192 });
		queries.push_back({ 128, 320 });
		queries.push_back({ 0, 320 });

		testCase(testSize, setOps, queries);
	}

	// Test 14: Mask generation edge cases
	// Rationale: The set() method has special logic for numBits == 64 (lines 905-909)
	// and different code paths for single vs multi-word operations. This test specifically
	// targets those branches to ensure correct mask generation in all scenarios.
	{
		std::vector<std::pair<int, int>> setOps, queries;

		// Test numBits == 64 case (single word, full mask)
		setOps.push_back({ 0, 64 });
		setOps.push_back({ 64, 128 });
		queries.push_back({ 0, 64 });
		queries.push_back({ 64, 128 });

		// Test begin % 64 == 0 and end % 64 == 0 cases
		setOps.push_back({ 128, 192 });
		setOps.push_back({ 192, 256 });
		queries.push_back({ 128, 192 });
		queries.push_back({ 192, 256 });

		// Test single-word ranges with different start positions
		for (int start = 0; start < 64; start += 7) {
			setOps.push_back({ start, start + 1 });
			setOps.push_back({ start + 64, start + 65 });
			queries.push_back({ start, start + 1 });
			queries.push_back({ start + 64, start + 65 });
		}

		testCase(256, setOps, queries);
	}

	// Test 15: Extreme size testing
	// Rationale: Large sizes (10,000+ bits) stress-test the multi-word logic and can expose
	// issues that only appear when dealing with many 64-bit words. This catches performance
	// issues and ensures correctness scales beyond typical use cases.
	{
		const int largeSize = 10000;
		std::vector<std::pair<int, int>> setOps, queries;

		// Large ranges across many words
		setOps.push_back({ 100, 5000 });
		setOps.push_back({ 3000, 8000 });
		setOps.push_back({ 7000, 9500 });

		// Query various sections
		queries.push_back({ 0, 100 });
		queries.push_back({ 100, 3000 });
		queries.push_back({ 3000, 5000 });
		queries.push_back({ 5000, 7000 });
		queries.push_back({ 7000, 8000 });
		queries.push_back({ 8000, 9500 });
		queries.push_back({ 9500, largeSize });
		queries.push_back({ 0, largeSize });

		testCase(largeSize, setOps, queries);
	}

	return Void();
}

namespace {

// ---------- Integration of the three histories ----------
//
// Each case resolves batches through the same ConflictBatch path the Resolver uses, once
// with FDB's skip list, once with the paged skip list and once with the paged B+, and
// expects the same outcomes

struct ResolvedBatch {
	std::vector<int> committed, tooOld;
	std::map<int, VectorRef<int>> conflictingKeys;
	Arena arena;

	bool isCommitted(int t) const { return std::find(committed.begin(), committed.end(), t) != committed.end(); }
	bool isTooOld(int t) const { return std::find(tooOld.begin(), tooOld.end(), t) != tooOld.end(); }
	bool isConflict(int t) const { return !isCommitted(t) && !isTooOld(t); }
	std::vector<int> reported(int t) const {
		auto it = conflictingKeys.find(t);
		std::vector<int> ranges;
		if (it != conflictingKeys.end()) {
			ranges.assign(it->second.begin(), it->second.end());
		}
		std::sort(ranges.begin(), ranges.end());
		return ranges;
	}
};

struct TransactionSpec {
	Version snapshot;
	std::vector<std::pair<std::string, std::string>> reads, writes;
	bool report = false;
};

std::string keyAfterOf(const std::string& key) {
	return key + std::string(1, '\0');
}

// A window of 1000 versions stands in for MAX_WRITE_TRANSACTION_LIFE_VERSIONS
constexpr Version testWindow = 1000;

void resolve(ConflictSet* cs, Version now, const std::vector<TransactionSpec>& specs, ResolvedBatch& out) {
	Arena arena;
	std::vector<CommitTransactionRef> transactions;
	for (const TransactionSpec& spec : specs) {
		CommitTransactionRef tr;
		tr.read_snapshot = spec.snapshot;
		tr.report_conflicting_keys = spec.report;
		for (const auto& [from, to] : spec.reads) {
			tr.read_conflict_ranges.push_back(arena, KeyRangeRef(arena, KeyRangeRef(StringRef(from), StringRef(to))));
		}
		for (const auto& [from, to] : spec.writes) {
			tr.write_conflict_ranges.push_back(arena, KeyRangeRef(arena, KeyRangeRef(StringRef(from), StringRef(to))));
		}
		transactions.push_back(tr);
	}
	const Version newOldestVersion = now - testWindow;
	ConflictBatch batch(cs, &out.conflictingKeys, &out.arena);
	for (const CommitTransactionRef& tr : transactions) {
		batch.addTransaction(tr, newOldestVersion);
	}
	batch.detectConflicts(now, newOldestVersion, out.committed, &out.tooOld);
}

// Runs a case against each history in turn, naming the one that failed
template <class Case>
void forEachHistory(const char* name, Case body) {
	for (ConflictHistory history :
	     { ConflictHistory::SkipList, ConflictHistory::PagedSkipList, ConflictHistory::PagedBTree }) {
		ConflictSet* cs = new ConflictSet(history, 64);
		try {
			body(cs);
		} catch (...) {
			TraceEvent(SevError, "ConflictSetIntegrationFailed")
			    .detail("Case", name)
			    .detail("Index", conflictHistoryName(history));
			destroyConflictSet(cs);
			throw;
		}
		destroyConflictSet(cs);
	}
}

} // namespace

TEST_CASE("/fdbserver/conflictset/historyFromAnEarlierBatch") {
	forEachHistory("historyFromAnEarlierBatch", [](ConflictSet* cs) {
		ResolvedBatch first;
		resolve(cs,
		        2000,
		        { { 1900, {}, { { "b", "d" } } }, { 1900, {}, { { "k", keyAfterOf("k") } } } },
		        first);
		ASSERT(first.isCommitted(0) && first.isCommitted(1));

		ResolvedBatch second;
		resolve(cs,
		        2100,
		        {
		            { 1999, { { "c", keyAfterOf("c") } }, {} }, // inside the range written at 2000
		            { 2000, { { "c", keyAfterOf("c") } }, {} }, // at the write's own version
		            { 1999, { { "d", "e" } }, {} }, // begins where the range ends
		            { 1999, { { "a", "b" } }, {} }, // ends where it begins
		            { 1999, { { "k", keyAfterOf("k") } }, {} }, // the point itself
		            { 1999, { { keyAfterOf("k"), "l" } }, {} }, // just after the point
		            { 1999, { { "j", "k" } }, {} }, // ends at the point
		        },
		        second);
		ASSERT(second.isConflict(0));
		ASSERT(second.isCommitted(1));
		ASSERT(second.isCommitted(2) && second.isCommitted(3));
		ASSERT(second.isConflict(4));
		ASSERT(second.isCommitted(5) && second.isCommitted(6));
	});
	return Void();
}

TEST_CASE("/fdbserver/conflictset/conflictWithinTheBatch") {
	forEachHistory("conflictWithinTheBatch", [](ConflictSet* cs) {
		ResolvedBatch batch;
		resolve(cs,
		        2000,
		        {
		            { 1900, {}, { { "m", "n" } } },
		            { 1900, { { "m", keyAfterOf("m") } }, {} }, // reads what the first writes
		            { 1900, { { "n", "o" } }, {} }, // begins where it ends
		            { 1900, { { "a", "b" } }, { { "x", "y" } } }, // commits and writes x
		            { 1900, { { "x", "y" } }, {} }, // reads x after it
		        },
		        batch);
		ASSERT(batch.isCommitted(0));
		ASSERT(batch.isConflict(1));
		ASSERT(batch.isCommitted(2) && batch.isCommitted(3));
		ASSERT(batch.isConflict(4));
	});
	return Void();
}

TEST_CASE("/fdbserver/conflictset/aTransactionDoesNotConflictWithItself") {
	forEachHistory("aTransactionDoesNotConflictWithItself", [](ConflictSet* cs) {
		ResolvedBatch batch;
		resolve(cs, 2000, { { 1900, { { "a", "c" } }, { { "a", "c" }, { "b", keyAfterOf("b") } } } }, batch);
		ASSERT(batch.isCommitted(0));
	});
	return Void();
}

TEST_CASE("/fdbserver/conflictset/rejectedWritesAreNotRecorded") {
	forEachHistory("rejectedWritesAreNotRecorded", [](ConflictSet* cs) {
		ResolvedBatch first;
		resolve(cs, 2000, { { 1900, {}, { { "x", keyAfterOf("x") } } } }, first);

		ResolvedBatch second;
		resolve(cs,
		        2100,
		        {
		            { 1950, { { "x", keyAfterOf("x") } }, { { "y", "z" } } }, // conflicts with x, writes y
		            { 2050, { { "y", keyAfterOf("y") } }, {} }, // so its write does not reach this read
		            { 500, { { "q", "r" } }, { { "p", keyAfterOf("p") } } }, // too old, writes p
		        },
		        second);
		ASSERT(second.isConflict(0));
		ASSERT(second.isCommitted(1));
		ASSERT(second.isTooOld(2));

		ResolvedBatch third;
		resolve(cs,
		        2200,
		        { { 2000, { { "y", "z" } }, {} }, { 2000, { { "p", keyAfterOf("p") } }, {} } },
		        third);
		ASSERT(third.isCommitted(0) && third.isCommitted(1));
	});
	return Void();
}

TEST_CASE("/fdbserver/conflictset/tooOldAndTransactionsWithoutReads") {
	forEachHistory("tooOldAndTransactionsWithoutReads", [](ConflictSet* cs) {
		ResolvedBatch first;
		resolve(cs,
		        5000,
		        {
		            { 3999, { { "a", "b" } }, {} }, // one below the window
		            { 4000, { { "a", "b" } }, {} }, // at the window
		            { 10, {}, { { "w", keyAfterOf("w") } } }, // far below it, but reads nothing
		        },
		        first);
		ASSERT(first.isTooOld(0));
		ASSERT(first.isCommitted(1));
		ASSERT(first.isCommitted(2));

		// The write of the transaction without reads was recorded
		ResolvedBatch second;
		resolve(cs, 5100, { { 4999, { { "w", keyAfterOf("w") } }, {} } }, second);
		ASSERT(second.isConflict(0));
	});
	return Void();
}

TEST_CASE("/fdbserver/conflictset/reportedConflictingRanges") {
	forEachHistory("reportedConflictingRanges", [](ConflictSet* cs) {
		ResolvedBatch first;
		resolve(cs, 2000, { { 1900, {}, { { "b", keyAfterOf("b") }, { "d", "f" } } } }, first);

		ResolvedBatch second;
		resolve(cs,
		        2100,
		        {
		            // Ranges 1 and 3 conflict with history; every one of them is reported
		            { 1950,
		              { { "a", "b" }, { "b", "c" }, { "g", "h" }, { "e", keyAfterOf("e") } },
		              {},
		              true },
		            // No history conflict: the batch reports its first conflicting range only
		            { 2000, {}, { { "m", "n" } } },
		            { 2000, { { "k", "l" }, { "m", keyAfterOf("m") }, { "m", "o" } }, {}, true },
		            // A conflict without reporting leaves no entry
		            { 1950, { { "b", "c" } }, {} },
		        },
		        second);
		ASSERT(second.isConflict(0) && (second.reported(0) == std::vector<int>{ 1, 3 }));
		ASSERT(second.isCommitted(1));
		ASSERT(second.isConflict(2) && (second.reported(2) == std::vector<int>{ 1 }));
		ASSERT(second.isConflict(3) && second.conflictingKeys.count(3) == 0);
	});
	return Void();
}

TEST_CASE("/fdbserver/conflictset/pagedBoundaryRejectsAsTooOld") {
	// Only the paged sets have a boundary of their own: a pool of three pages forces history
	// out while the window still admits the read that needs it
	for (ConflictHistory history : { ConflictHistory::PagedSkipList, ConflictHistory::PagedBTree }) {
		ConflictSet* cs = new ConflictSet(history, 3);
		const std::string pad(1000, '.');
		Version now = 1000;
		for (int i = 0; i < 200; ++i) {
			ResolvedBatch batch;
			resolve(cs, now, { { now - 1, {}, { { format("k%04d", i) + pad, format("k%04d", i) + pad + "~" } } } }, batch);
			ASSERT(batch.isCommitted(0));
			now += 10;
		}
		const ConflictSetStats before = getConflictSetStats(cs);
		ASSERT(before.paged && before.reclaimedHistoryPages > 0 && before.validationMinRV > 1000);

		ResolvedBatch batch;
		resolve(cs,
		        now,
		        {
		            { before.validationMinRV - 1, { { "a", "b" } }, {} }, // inside the window, below the boundary
		            { before.validationMinRV, { { "a", "b" } }, {} }, // at the boundary
		            { 10, {}, { { "z", keyAfterOf("z") } } }, // no reads, so no boundary applies
		        },
		        batch);
		ASSERT(before.validationMinRV - 1 >= now - testWindow);
		ASSERT(batch.isTooOld(0));
		ASSERT(batch.isCommitted(1) && batch.isCommitted(2));
		const ConflictSetStats after = getConflictSetStats(cs);
		ASSERT(after.tooOldWithinWindow == before.tooOldWithinWindow + 1);
		destroyConflictSet(cs);
	}
	return Void();
}

namespace {

// Big-endian, so keys order as the integers they encode
StringRef benchmarkKey(Arena& arena, int64_t n) {
	uint8_t* bytes = new (arena) uint8_t[10];
	bytes[0] = 'c';
	bytes[1] = '/';
	for (int i = 0; i < 8; ++i) {
		bytes[2 + i] = uint8_t(uint64_t(n) >> (56 - 8 * i));
	}
	return StringRef(bytes, 10);
}

// The batch as the Resolver resolves it
force_inline void resolveBenchmarkBatch(ConflictSet* cs,
                                        const std::vector<CommitTransactionRef>& trs,
                                        Version now,
                                        Version newOldestVersion,
                                        std::vector<int>& nonConflicting,
                                        std::vector<int>& tooOldList,
                                        std::map<int, VectorRef<int>>& conflictingKeys,
                                        Arena& replyArena) {
	ConflictBatch batch(cs, &conflictingKeys, &replyArena);
	for (const CommitTransactionRef& tr : trs) {
		batch.addTransaction(tr, newOldestVersion);
	}
	batch.detectConflicts(now, newOldestVersion, nonConflicting, &tooOldList);
}

// Separate entry points, so a profiler can keep the measured batches alone by function name:
// perf report --parent measuredConflictBatch --exclude-other, or callgrind --toggle-collect
__attribute__((noinline)) void warmupConflictBatch(ConflictSet* cs,
                                                   const std::vector<CommitTransactionRef>& trs,
                                                   Version now,
                                                   Version newOldestVersion,
                                                   std::vector<int>& nonConflicting,
                                                   std::vector<int>& tooOldList,
                                                   std::map<int, VectorRef<int>>& conflictingKeys,
                                                   Arena& replyArena) {
	resolveBenchmarkBatch(cs, trs, now, newOldestVersion, nonConflicting, tooOldList, conflictingKeys, replyArena);
}

__attribute__((noinline)) void measuredConflictBatch(ConflictSet* cs,
                                                     const std::vector<CommitTransactionRef>& trs,
                                                     Version now,
                                                     Version newOldestVersion,
                                                     std::vector<int>& nonConflicting,
                                                     std::vector<int>& tooOldList,
                                                     std::map<int, VectorRef<int>>& conflictingKeys,
                                                     Arena& replyArena) {
	resolveBenchmarkBatch(cs, trs, now, newOldestVersion, nonConflicting, tooOldList, conflictingKeys, replyArena);
}

using RangeSearchStats = TemporalSkipList::RangeSearchStats;

// Adds to `into` the work done between two readings of the range-search counters
void addRangeSearchDelta(RangeSearchStats& into, const RangeSearchStats& after, const RangeSearchStats& before) {
	into.queries += after.queries - before.queries;
	for (int k = 0; k < 2; ++k) {
		for (int l = 0; l < TemporalSkipList::MaxLevels; ++l) {
			into.spans[l][k] += after.spans[l][k] - before.spans[l][k];
		}
		into.prunedByMaxCV[k] += after.prunedByMaxCV[k] - before.prunedByMaxCV[k];
		into.emptyEnd[k] += after.emptyEnd[k] - before.emptyEnd[k];
		into.validPrunes[k] += after.validPrunes[k] - before.validPrunes[k];
		into.validExplores[k] += after.validExplores[k] - before.validExplores[k];
		into.needsRepair[k] += after.needsRepair[k] - before.needsRepair[k];
		into.becameNeedsRepair[k] += after.becameNeedsRepair[k] - before.becameNeedsRepair[k];
		into.descents[k] += after.descents[k] - before.descents[k];
	}
	into.recordsLevelZero += after.recordsLevelZero - before.recordsLevelZero;
	into.recordsAbove += after.recordsAbove - before.recordsAbove;
	into.expiredCellsLevelZero += after.expiredCellsLevelZero - before.expiredCellsLevelZero;
	into.expiredCellsAbove += after.expiredCellsAbove - before.expiredCellsAbove;
}

// Prints one group of batches, per query of the range index
void printRangeSearchStats(const char* group, int batches, const RangeSearchStats& s) {
	if (batches == 0) {
		return;
	}
	const double q = s.queries ? double(s.queries) : 1.0;
	uint64_t spans[2] = { 0, 0 };
	for (int k = 0; k < 2; ++k) {
		for (int l = 0; l < TemporalSkipList::MaxLevels; ++l) {
			spans[k] += s.spans[l][k];
		}
	}
	printf("RangeSearch group=%s batches=%d queries=%llu recordsLevelZero=%.2f recordsAbove=%.2f "
	       "expiredCellsLevelZero=%.2f expiredCellsAbove=%.2f\n",
	       group,
	       batches,
	       (unsigned long long)s.queries,
	       s.recordsLevelZero / q,
	       s.recordsAbove / q,
	       s.expiredCellsLevelZero / q,
	       s.expiredCellsAbove / q);
	for (int k = 0; k < 2; ++k) {
		const uint64_t outcomes = s.prunedByMaxCV[k] + s.emptyEnd[k] + s.validPrunes[k] + s.validExplores[k] +
		                          s.needsRepair[k] + s.becameNeedsRepair[k];
		printf("RangeSearch group=%s span=%s spans=%.2f prunedByMaxCV=%.2f emptyEnd=%.2f validPrunes=%.2f "
		       "validExplores=%.2f needsRepair=%.2f becameNeedsRepair=%.2f descents=%.2f outcomesMatchSpans=%s\n",
		       group,
		       k == RangeSearchStats::Prefix ? "prefix" : "cell",
		       spans[k] / q,
		       s.prunedByMaxCV[k] / q,
		       s.emptyEnd[k] / q,
		       s.validPrunes[k] / q,
		       s.validExplores[k] / q,
		       s.needsRepair[k] / q,
		       s.becameNeedsRepair[k] / q,
		       s.descents[k] / q,
		       outcomes == spans[k] ? "yes" : "no");
	}
	printf("RangeSearch group=%s spansPerLevel=", group);
	for (int l = TemporalSkipList::MaxLevels - 1; l >= 0; --l) {
		if (s.spans[l][RangeSearchStats::Cell] + s.spans[l][RangeSearchStats::Prefix]) {
			printf("L%d:%.2f/%.2f ", l, s.spans[l][RangeSearchStats::Cell] / q, s.spans[l][RangeSearchStats::Prefix] / q);
		}
	}
	printf("(cell/prefix per query)\n");
}

// The B+'s structural work between two readings, and its shape at the end: per depth, nodes and
// mean entries per node; per node kind, how many nodes hold each number of entries
void printBTreeStats(const ConflictSetStats& before,
                     const ConflictSetStats& after,
                     const paged_btree::PagedBTreeConflictSet& history) {
	printf("BTree height=%lld nodes=%lld leaves=%lld intervalInsertions=%lld overwrittenIntervals=%lld "
	       "expiredIntervals=%lld discardedWrites=%lld splits=%lld internalSplits=%lld merges=%lld "
	       "internalMerges=%lld cleanupLeavesVisited=%lld mergeCandidatesExamined=%lld cleanedIntervals=%lld\n",
	       (long long)after.btreeHeight,
	       (long long)after.btreeNodes,
	       (long long)after.btreeLeaves,
	       (long long)(after.btreeIntervalInsertions - before.btreeIntervalInsertions),
	       (long long)(after.btreeOverwrittenIntervals - before.btreeOverwrittenIntervals),
	       (long long)(after.btreeExpiredIntervals - before.btreeExpiredIntervals),
	       (long long)(after.btreeDiscardedWrites - before.btreeDiscardedWrites),
	       (long long)(after.btreeSplits - before.btreeSplits),
	       (long long)(after.btreeInternalSplits - before.btreeInternalSplits),
	       (long long)(after.btreeMerges - before.btreeMerges),
	       (long long)(after.btreeInternalMerges - before.btreeInternalMerges),
	       (long long)(after.btreeCleanupLeavesVisited - before.btreeCleanupLeavesVisited),
	       (long long)(after.btreeMergeCandidatesExamined - before.btreeMergeCandidatesExamined),
	       (long long)(after.cleanedIntervals - before.cleanedIntervals));
	if (paged_btree::PagedBTreeConflictSet::Stats::searchEnabled) {
		// Work counts over the whole run, per write (addRange call) and per query
		const auto& w = history.stats();
		const double writes = w.writes ? double(w.writes) : 1.0, queries = w.queries ? double(w.queries) : 1.0;
		printf("BTreeWork writes=%llu descents=%.3f descentNodes=%.3f childRankProbes=%.3f liveRankEntries=%.3f "
		       "refreshes=%.3f refreshEntries=%.3f minimumCalls=%.3f minimumEntries=%.3f overwriteLeaves=%.3f "
		       "overwriteEntries=%.3f leafSearchProbes=%.3f slotInserts=%.3f slotErases=%.3f nodesReserved=%.3f "
		       "nodesTaken=%.3f splits=%.4f (per write)\n",
		       (unsigned long long)w.writes,
		       w.descents / writes,
		       w.descentNodes / writes,
		       w.childRankProbes / writes,
		       w.liveRankEntries / writes,
		       w.refreshes / writes,
		       w.refreshEntries / writes,
		       w.minimumCalls / writes,
		       w.minimumEntries / writes,
		       w.overwriteLeaves / writes,
		       w.overwriteEntries / writes,
		       w.leafSearchProbes / writes,
		       w.slotInserts / writes,
		       w.slotErases / writes,
		       w.nodesReserved / writes,
		       w.nodesTaken / writes,
		       w.splits / writes);
		const double passes = w.writeSearchPasses ? double(w.writeSearchPasses) : 1.0;
		printf("BTreeWriteSearch passes=%llu maxRetiredInBatch=%llu nodesPerPass=%.1f leavesPerPass=%.1f "
		       "maxActive=%llu hintHits=%.4f hintRightMoves=%.4f hintRestarts=%.4f rootSearches=%.4f "
		       "intervalInsertions=%.4f leftFragmentsTrimmed=%.4f rightFragmentsTrimmed=%.4f "
		       "splitFragmentsInserted=%.4f (per write)\n",
		       (unsigned long long)w.writeSearchPasses,
		       (unsigned long long)w.writeBatchMaxRetired,
		       w.writeSearchNodes / passes,
		       w.writeSearchLeaves / passes,
		       (unsigned long long)w.writeSearchMaxActive,
		       w.writeHintHits / writes,
		       w.writeHintRightMoves / writes,
		       w.writeHintRestarts / writes,
		       w.writeRootSearches / writes,
		       w.intervalInsertions / writes,
		       w.leftFragmentsTrimmed / writes,
		       w.rightFragmentsTrimmed / writes,
		       w.splitFragmentsInserted / writes);
		printf("BTreeQueryWork queries=%llu visitedNodes=%.3f leafEntries=%.3f "
		       "separatorRefreshes=%.4f (per query; separator refreshes counted anywhere)\n",
		       (unsigned long long)w.queries,
		       w.visitedNodes / queries,
		       w.queryLeafEntries / queries,
		       w.separatorRefreshes / queries);
		printf("BTreeKeyWork inlineDecisions=%llu fallbacks=%llu heapKeyLoads=%llu "
		       "(whole run, reads and writes)\n",
		       (unsigned long long)w.inlineKeyDecisions,
		       (unsigned long long)w.keyComparisonFallbacks,
		       (unsigned long long)w.heapKeyLoads);
	}
	// Queue blocks live outside the page pool, including free blocks kept for reuse
	printf("BTree cleanupMetadataBytes=%llu\n", (unsigned long long)history.cleanupMetadataBytes());
	using BTree = paged_btree::PagedBTreeConflictSet;
	const BTree::Layout layout = BTree::layout();
	printf("BTree layout capacity=%u inlineKeyBytes=%zu permutationBytes=%zu entryBytes=%zu nodeBytes=%zu nodeAlignment=%zu "
	       "nodesPerPage=%zu slotsPerPage=%zu\n",
	       BTree::Capacity,
	       BTree::InlineKeyBytes,
	       layout.permutationBytes,
	       layout.entryBytes,
	       layout.nodeBytes,
	       layout.nodeAlignment,
	       layout.nodesPerPage,
	       layout.nodesPerPage * BTree::Capacity);
	const auto levels = history.levels();
	printf("BTree levels=");
	for (size_t d = 0; d < levels.size(); ++d) {
		printf("D%zu:%llu/%.2f/%.2f ",
		       d,
		       (unsigned long long)levels[d].nodes,
		       double(levels[d].entries) / double(levels[d].nodes),
		       double(levels[d].liveEntries) / double(levels[d].nodes));
	}
	printf("(nodes/entries per node/live entries per node, root first)\n");
	for (bool leaves : { true, false }) {
		printf("BTree occupancy %s=", leaves ? "leaves" : "internal");
		for (unsigned count = 0; count <= paged_btree::PagedBTreeConflictSet::Capacity; ++count) {
			printf("%llu%s",
			       (unsigned long long)history.occupancy(leaves, count),
			       count < paged_btree::PagedBTreeConflictSet::Capacity ? "," : "");
		}
		printf(" (nodes holding 0..%u entries)\n", paged_btree::PagedBTreeConflictSet::Capacity);
	}
}

double percentile(std::vector<double> samples, double p) {
	if (samples.empty()) {
		return 0;
	}
	std::sort(samples.begin(), samples.end());
	return samples[std::min(samples.size() - 1, size_t(p * double(samples.size())))];
}

} // namespace

// Resolves synthetic batches through the Resolver's batch path and reports throughput,
// per-batch latency, the time of each phase and memory. The history is the one the knobs
// select, so two runs of one binary differ only in the knobs of the index under test.
// Batch generation is outside every timed interval; timers are per batch and per phase
TEST_CASE("noSim/fdbserver/conflictset/benchmark") {
	const std::string workload = params.get("workload").orDefault("points");
	const int warmupBatches = params.getInt("warmupBatches").orDefault(1000);
	const int batches = params.getInt("batches").orDefault(2000);
	const int transactionsPerBatch = params.getInt("transactionsPerBatch").orDefault(500);
	const int readsPerTransaction = params.getInt("readsPerTransaction").orDefault(4);
	const int writesPerTransaction = params.getInt("writesPerTransaction").orDefault(2);
	const int64_t keySpace = params.getInt("keySpace").orDefault(10000000);
	const int64_t versionsPerBatch = params.getInt("versionsPerBatch").orDefault(10000);
	const int64_t maxReadLag = params.getInt("maxReadLagVersions").orDefault(2000000);
	const int64_t maxReadKeys = params.getInt("maxReadKeys").orDefault(1000);
	const int64_t maxWriteKeys = params.getInt("maxWriteKeys").orDefault(10);
	const int64_t seed = params.getInt("seed").orDefault(1);
	const bool ranges = workload == "ranges";
	ASSERT(ranges || workload == "points");

	const Version window = SERVER_KNOBS->MAX_WRITE_TRANSACTION_LIFE_VERSIONS;
	const uint64_t residentBefore = getResidentMemoryUsage();
	ConflictSet* cs = newConflictSet();
	std::mt19937_64 random(seed);
	auto uniform = [&](int64_t lo, int64_t hi) { return lo + int64_t(random() % uint64_t(hi - lo + 1)); };

	// Starts past the window, so no read is too old for want of history
	Version now = window + versionsPerBatch;
	std::vector<double> latencies;
	int64_t transactions = 0, committed = 0, tooOld = 0;
	ConflictSetStats atStart;
	uint64_t residentAfterWarmup = 0;
	double measuredSeconds = 0;
	// Range-search work in four groups: warm-up or measured, before or after history was first reclaimed
	RangeSearchStats rangeGroups[2][2];
	int rangeGroupBatches[2][2] = {};

	for (int b = 0; b < warmupBatches + batches; ++b) {
		if (b == warmupBatches) {
			atStart = getConflictSetStats(cs);
			residentAfterWarmup = getResidentMemoryUsage();
		}
		Arena arena;
		std::vector<CommitTransactionRef> trs(transactionsPerBatch);
		for (CommitTransactionRef& tr : trs) {
			tr.read_snapshot = now - uniform(1, maxReadLag);
			for (int r = 0; r < readsPerTransaction; ++r) {
				const int64_t k = uniform(0, keySpace - 1);
				const StringRef from = benchmarkKey(arena, k);
				const StringRef to = ranges ? benchmarkKey(arena, k + uniform(1, maxReadKeys)) : keyAfter(from, arena);
				tr.read_conflict_ranges.push_back(arena, KeyRangeRef(from, to));
			}
			for (int w = 0; w < writesPerTransaction; ++w) {
				const int64_t k = uniform(0, keySpace - 1);
				const StringRef from = benchmarkKey(arena, k);
				const StringRef to = ranges ? benchmarkKey(arena, k + uniform(1, maxWriteKeys)) : keyAfter(from, arena);
				tr.write_conflict_ranges.push_back(arena, KeyRangeRef(from, to));
			}
		}

		std::vector<int> nonConflicting, tooOldList;
		std::map<int, VectorRef<int>> conflictingKeys;
		Arena replyArena;
		const bool measured = b >= warmupBatches;
		const bool reclaiming = cs->pagedHistory && cs->pagedHistory->reclaimedHistoryPages() > 0;
		const RangeSearchStats rangeBefore =
		    cs->pagedHistory ? cs->pagedHistory->rangeSearchStats() : RangeSearchStats();
		const double start = timer_monotonic();
		if (measured) {
			measuredConflictBatch(cs, trs, now, now - window, nonConflicting, tooOldList, conflictingKeys, replyArena);
		} else {
			warmupConflictBatch(cs, trs, now, now - window, nonConflicting, tooOldList, conflictingKeys, replyArena);
		}
		const double elapsed = timer_monotonic() - start;
		if (cs->pagedHistory) {
			addRangeSearchDelta(rangeGroups[measured][reclaiming], cs->pagedHistory->rangeSearchStats(), rangeBefore);
			++rangeGroupBatches[measured][reclaiming];
		}
		if (b >= warmupBatches) {
			latencies.push_back(elapsed);
			measuredSeconds += elapsed;
			transactions += trs.size();
			committed += nonConflicting.size();
			tooOld += tooOldList.size();
		}
		now += versionsPerBatch;
	}

	const ConflictSetStats atEnd = getConflictSetStats(cs);
	const uint64_t residentAfter = getResidentMemoryUsage();
	const int64_t historyEntries = cs->versionHistory ? cs->versionHistory->count() : 0;
	printf("ConflictSetBenchmark index=%s workload=%s seed=%lld batches=%d warmup=%d transactions=%lld "
	       "committed=%lld tooOld=%lld seconds=%.4f txPerSec=%.0f batchP50us=%.1f batchP90us=%.1f batchP99us=%.1f "
	       "batchMaxUs=%.1f checkReadS=%.4f intraBatchS=%.4f combineS=%.4f mergeS=%.4f cleanupS=%.4f "
	       "rssBeforeMiB=%.1f rssAfterWarmupMiB=%.1f rssAfterMiB=%.1f poolUsedMiB=%.1f poolHeapPages=%lld "
	       "poolIndexPages=%lld reclaimedPages=%lld cleanedCells=%lld tooOldWithinWindow=%lld "
	       "boundaryPastWindow=%lld skipListEntries=%lld\n",
	       conflictSetIndexName(cs),
	       workload.c_str(),
	       (long long)seed,
	       batches,
	       warmupBatches,
	       (long long)transactions,
	       (long long)committed,
	       (long long)tooOld,
	       measuredSeconds,
	       double(transactions) / measuredSeconds,
	       percentile(latencies, 0.5) * 1e6,
	       percentile(latencies, 0.9) * 1e6,
	       percentile(latencies, 0.99) * 1e6,
	       percentile(latencies, 1.0) * 1e6,
	       atEnd.checkReadSeconds - atStart.checkReadSeconds,
	       atEnd.checkIntraBatchSeconds - atStart.checkIntraBatchSeconds,
	       atEnd.combineSeconds - atStart.combineSeconds,
	       atEnd.mergeSeconds - atStart.mergeSeconds,
	       atEnd.cleanupSeconds - atStart.cleanupSeconds,
	       residentBefore / 1048576.0,
	       residentAfterWarmup / 1048576.0,
	       residentAfter / 1048576.0,
	       (atEnd.poolLimitPages - atEnd.poolFreePages) * double(HISTORY_PAGE_BYTES) / 1048576.0,
	       (long long)atEnd.poolHeapPages,
	       (long long)atEnd.poolIndexPages,
	       (long long)atEnd.reclaimedHistoryPages,
	       (long long)atEnd.cleanedCells,
	       (long long)atEnd.tooOldWithinWindow,
	       (long long)atEnd.boundaryPastWindow,
	       (long long)historyEntries);
	if (atEnd.btree) {
		printBTreeStats(atStart, atEnd, *cs->btreeHistory);
	}
	if (RangeSearchStats::enabled && cs->pagedHistory) {
		printRangeSearchStats("warmup-before-reclamation", rangeGroupBatches[0][0], rangeGroups[0][0]);
		printRangeSearchStats("warmup-after-reclamation", rangeGroupBatches[0][1], rangeGroups[0][1]);
		printRangeSearchStats("measured-before-reclamation", rangeGroupBatches[1][0], rangeGroups[1][0]);
		printRangeSearchStats("measured-after-reclamation", rangeGroupBatches[1][1], rangeGroups[1][1]);
	}
	destroyConflictSet(cs);
	return Void();
}

// Long reads over a longer history, on one resolver with more memory or on several resolvers that
// split the key space. skipListTest's workload (2,500 transactions per batch, one read range and
// one write range of 1 to 11 keys each, uniform over 2*10^7 keys, one version per batch), except
// that every read is at a version drawn uniformly from the whole admitted window, as long
// transactions would read, or, with oldReads=0, at the latest version. Each resolver keeps its own
// history; a transaction's ranges are clipped to each resolver's key range as a commit proxy does,
// and it commits only if every resolver accepts it. Batch generation is outside every timed
// interval. Per batch, the time is summed over resolvers (work) and maximized over them (the
// latency of resolvers running in parallel)
TEST_CASE("noSim/fdbserver/conflictset/longReads") {
	const std::string index = params.get("index").orDefault("btree");
	const int resolvers = params.getInt("resolvers").orDefault(1);
	const int64_t poolPages = params.getInt("poolPages").orDefault(512);
	const int64_t window = params.getInt("windowVersions").orDefault(50);
	const int warmupBatches = params.getInt("warmupBatches").orDefault(500);
	const int batches = params.getInt("batches").orDefault(1000);
	const int transactionsPerBatch = params.getInt("transactionsPerBatch").orDefault(2500);
	const bool oldReads = params.getInt("oldReads").orDefault(1) != 0;
	const int64_t seed = params.getInt("seed").orDefault(1);
	ASSERT(index == "skiplist" || index == "paged" || index == "btree");
	ASSERT(resolvers >= 1 && window >= 1);
	const ConflictHistory history = index == "skiplist" ? ConflictHistory::SkipList
	                                : index == "paged"  ? ConflictHistory::PagedSkipList
	                                                    : ConflictHistory::PagedBTree;
	constexpr int keySpace = 20000000;

	std::vector<ConflictSet*> sets;
	std::vector<int> bounds;
	for (int r = 0; r < resolvers; ++r) {
		sets.push_back(new ConflictSet(history, poolPages));
		bounds.push_back(int(int64_t(keySpace) * r / resolvers));
	}
	bounds.push_back(keySpace);

	std::mt19937_64 random(seed);
	auto uniform = [&](int64_t lo, int64_t hi) { return lo + int64_t(random() % uint64_t(hi - lo + 1)); };
	std::vector<double> latencies;
	double workSeconds = 0, wallSeconds = 0;
	int64_t transactions = 0, committed = 0, tooOld = 0, conflicts = 0;
	uint64_t decisionsHash = 1469598103934665603ULL;
	auto mix = [&](uint64_t value) {
		for (int b = 0; b < 8; ++b) {
			decisionsHash = (decisionsHash ^ ((value >> (8 * b)) & 0xFF)) * 1099511628211ULL;
		}
	};
	ConflictSetStats atStart;
	double sortAtStart = 0;
	std::vector<ConflictSetStats> perStart(resolvers);
	std::vector<paged_btree::PagedBTreeConflictSet::Stats> btreeStart(resolvers);

	for (int b = 0; b < warmupBatches + batches; ++b) {
		const bool measured = b >= warmupBatches;
		if (b == warmupBatches) {
			atStart = getConflictSetStats(sets[0]);
			sortAtStart = g_sort.getValue();
			for (int r = 0; r < resolvers; ++r) {
				perStart[r] = getConflictSetStats(sets[r]);
				if (sets[r]->btreeHistory) btreeStart[r] = sets[r]->btreeHistory->stats();
			}
		}
		const Version now = window + 1 + b;
		const Version oldest = now - window;
		Arena arena;
		// One transaction's ranges as key integers, clipped per resolver below
		struct Spec {
			int readBegin, readEnd, writeBegin, writeEnd;
			Version readVersion;
		};
		std::vector<Spec> specs(transactionsPerBatch);
		for (Spec& spec : specs) {
			spec.readBegin = int(uniform(0, keySpace - 1));
			spec.readEnd = spec.readBegin + 1 + int(uniform(0, 10));
			spec.writeBegin = int(uniform(0, keySpace - 1));
			spec.writeEnd = spec.writeBegin + 1 + int(uniform(0, 10));
			spec.readVersion = oldReads ? now - uniform(1, window) : now - 1;
		}
		std::vector<std::vector<CommitTransactionRef>> perResolver(resolvers,
		                                                           std::vector<CommitTransactionRef>(transactionsPerBatch));
		for (int r = 0; r < resolvers; ++r) {
			for (int t = 0; t < transactionsPerBatch; ++t) {
				const Spec& spec = specs[t];
				CommitTransactionRef& tr = perResolver[r][t];
				tr.read_snapshot = spec.readVersion;
				const int rb = std::max(spec.readBegin, bounds[r]), re = std::min(spec.readEnd, bounds[r + 1]);
				if (rb < re) tr.read_conflict_ranges.push_back(arena, KeyRangeRef(setK(arena, rb), setK(arena, re)));
				const int wb = std::max(spec.writeBegin, bounds[r]), we = std::min(spec.writeEnd, bounds[r + 1]);
				if (wb < we) tr.write_conflict_ranges.push_back(arena, KeyRangeRef(setK(arena, wb), setK(arena, we)));
			}
		}

		// 0 committed, 1 conflict, 2 too old, combined over resolvers as the proxy combines them
		std::vector<uint8_t> outcome(transactionsPerBatch, 0);
		double batchWork = 0, batchWall = 0;
		for (int r = 0; r < resolvers; ++r) {
			std::vector<int> nonConflicting, tooOldList;
			const double start = timer_monotonic();
			ConflictBatch batch(sets[r]);
			for (const CommitTransactionRef& tr : perResolver[r]) {
				batch.addTransaction(tr, oldest);
			}
			batch.detectConflicts(now, oldest, nonConflicting, &tooOldList);
			const double elapsed = timer_monotonic() - start;
			batchWork += elapsed;
			batchWall = std::max(batchWall, elapsed);
			std::vector<uint8_t> local(transactionsPerBatch, 1);
			for (int t : nonConflicting) local[t] = 0;
			for (int t : tooOldList) local[t] = 2;
			for (int t = 0; t < transactionsPerBatch; ++t) outcome[t] = std::max(outcome[t], local[t]);
		}
		mix(b);
		for (int t = 0; t < transactionsPerBatch; ++t) mix(outcome[t]);
		if (measured) {
			workSeconds += batchWork;
			wallSeconds += batchWall;
			latencies.push_back(batchWall);
			transactions += transactionsPerBatch;
			for (uint8_t o : outcome) {
				committed += o == 0;
				conflicts += o == 1;
				tooOld += o == 2;
			}
		}
	}

	const ConflictSetStats atEnd = getConflictSetStats(sets[0]);
	printf("LongReads index=%s resolvers=%d poolPagesEach=%lld windowVersions=%lld oldReads=%d seed=%lld batches=%d "
	       "warmup=%d transactions=%lld committed=%lld conflicts=%lld tooOld=%lld decisionsHash=%016llx "
	       "workS=%.4f wallS=%.4f batchWallP50ms=%.3f batchWallP95ms=%.3f batchWallP99ms=%.3f batchWallMaxMs=%.3f "
	       "checkReadS=%.4f "
	       "mergeS=%.4f cleanupS=%.4f sortS=%.4f residentMiB=%.1f\n",
	       conflictHistoryName(history),
	       resolvers,
	       (long long)poolPages,
	       (long long)window,
	       oldReads ? 1 : 0,
	       (long long)seed,
	       batches,
	       warmupBatches,
	       (long long)transactions,
	       (long long)committed,
	       (long long)conflicts,
	       (long long)tooOld,
	       (unsigned long long)decisionsHash,
	       workSeconds,
	       wallSeconds,
	       percentile(latencies, 0.5) * 1e3,
	       percentile(latencies, 0.95) * 1e3,
	       percentile(latencies, 0.99) * 1e3,
	       percentile(latencies, 1.0) * 1e3,
	       atEnd.checkReadSeconds - atStart.checkReadSeconds,
	       atEnd.mergeSeconds - atStart.mergeSeconds,
	       atEnd.cleanupSeconds - atStart.cleanupSeconds,
	       g_sort.getValue() - sortAtStart,
	       getResidentMemoryUsage() / 1048576.0);
	const Version lastNow = window + warmupBatches + batches;
	for (int r = 0; r < resolvers; ++r) {
		const ConflictSetStats s = getConflictSetStats(sets[r]);
		if (!s.paged) {
			printf("LongReadsResolver r=%d skipListEntries=%d\n", r, sets[r]->versionHistory->count());
			continue;
		}
		// The boundary must stay at or below the oldest admitted read version, lastNow - window
		printf("LongReadsResolver r=%d poolUsed=%lld heap=%lld index=%lld reclaimedPages=%lld validationMinRV=%lld "
		       "historyVersions=%lld marginOverWindow=%lld tooOldWithinWindow=%lld leaves=%lld\n",
		       r,
		       (long long)(s.poolLimitPages - s.poolFreePages),
		       (long long)s.poolHeapPages,
		       (long long)s.poolIndexPages,
		       (long long)(s.reclaimedHistoryPages - perStart[r].reclaimedHistoryPages),
		       (long long)s.validationMinRV,
		       (long long)(lastNow - s.validationMinRV),
		       (long long)((lastNow - window) - s.validationMinRV),
		       (long long)(s.tooOldWithinWindow - perStart[r].tooOldWithinWindow),
		       (long long)s.btreeLeaves);
		if (sets[r]->btreeHistory) {
			// Over the measured batches: urgent reclamation and ordinary cleanup, separately
			const auto& a = sets[r]->btreeHistory->stats();
			const auto& b = btreeStart[r];
			printf("LongReadsBTree r=%d reclaimCalls=%llu reclaimLeavesVisited=%llu cleanupLeavesVisited=%llu "
			       "expiredIntervals=%llu merges=%llu splits=%llu heapPagesReclaimed=%llu\n",
			       r,
			       (unsigned long long)(a.reclaimCalls - b.reclaimCalls),
			       (unsigned long long)(a.reclaimLeavesVisited - b.reclaimLeavesVisited),
			       (unsigned long long)(a.cleanupLeavesVisited - b.cleanupLeavesVisited),
			       (unsigned long long)(a.expiredIntervals - b.expiredIntervals),
			       (unsigned long long)(a.merges - b.merges),
			       (unsigned long long)(a.splits - b.splits),
			       (unsigned long long)(a.heapPagesReclaimed - b.heapPagesReclaimed));
		}
	}
	for (ConflictSet* cs : sets) destroyConflictSet(cs);
	return Void();
}

#include "By5ResolverTest.inc"
#include "SmokeReplay.inc"
