#pragma once

// The blocked layout: TemporalSkipBlock keeps its cells' key order in a KeyPermutation of
// nibbles packed in one 64-bit word. As of commit ebdc3ecce1, the last before individual cells
// replaced the blocks. Kept for reference: no target builds it, since it defines the same
// classes as PagedTemporalSkipList.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "flow/Error.h"         // UNSTOPPABLE_ASSERT, used by Optional.h
#include "flow/Optional.h"      // Optional
#include "fdbclient/FDBTypes.h" // Version

#include <new>

using PageID = uint64_t;
using PageIndex = uint64_t;
using LogicalPosition = uint64_t;
using Level = uint16_t;

constexpr uint32_t HISTORY_PAGE_BYTES = 64 * 1024;
constexpr uint8_t SKIP_BLOCK_CAPACITY = 15;

constexpr PageIndex NULL_PAGE_INDEX =
	std::numeric_limits<PageIndex>::max();

struct PoolPage {
	alignas(HISTORY_PAGE_BYTES)
	std::byte bytes[HISTORY_PAGE_BYTES];
};

static_assert(sizeof(PoolPage) == HISTORY_PAGE_BYTES);
static_assert(alignof(PoolPage) == HISTORY_PAGE_BYTES);

struct PageBudget {
	uint64_t limitPages = 0;  // Global capacity in pages.
	uint64_t heapPages = 0;   // Pages currently assigned to the heap.
	uint64_t indexPages = 0;  // Pages currently assigned to indexes.
	uint64_t freePages = 0;   // Unused pages plus released pages.

	uint64_t occupiedPages() const {
		return heapPages + indexPages;
	}

	uint64_t reservedPages() const {
		// Virtual capacity, not physical memory residency.
		return limitPages;
	}
};

enum class PageUse : uint8_t {
	Heap,
	Index
};

class PagePool {
public:
	// Allocates one aligned region without initializing its contents.
	// All pages are initially free.
	explicit PagePool(uint64_t limitPages);

	// Releases the entire region, including acquired pages.
	// Invalidates all pointers into the pool.
	~PagePool();

	PagePool(const PagePool&) = delete;
	PagePool& operator=(const PagePool&) = delete;
	PagePool(PagePool&&) = delete;
	PagePool& operator=(PagePool&&) = delete;

	// Takes a released page first, then a previously unused page.
	// Returns nullptr when no free pages remain.
	PoolPage* tryAcquire(PageUse use);

	// Returns a page for reuse, overwriting only the free-list link.
	// The page must belong to this pool, be currently acquired,
	// and be released with the same PageUse used to acquire it.
	void release(PoolPage* page, PageUse use);

	const PageBudget& budget() const {
		return budget_;
	}

private:
	PageBudget budget_;

	// Contiguous region aligned to HISTORY_PAGE_BYTES.
	std::byte* base_ = nullptr;

	// Pages in [nextUnusedPage_, budget_.limitPages) have never been acquired.
	PageIndex nextUnusedPage_ = 0;

	// Head of the free list; each released page stores the next index.
	PageIndex firstFreePage_ = NULL_PAGE_INDEX;

	PoolPage* pageAt(PageIndex index) const;
	PageIndex indexOf(const PoolPage* page) const;

	PageIndex getNextFree(PageIndex index) const;
	void setNextFree(PageIndex index, PageIndex next);
};

// ---------- TemporalHeap  ----------

struct HeapRef {
	PageID pageID;
	uint32_t offset;
	uint32_t length;
};

enum class ConflictKind : uint8_t {
	Point,
	Range
};

// Followed by keyLength bytes containing the key.
struct PointConflictHeader {
	Version cv;
	uint32_t keyLength;
};

// Followed by beginLength bytes, then endLength bytes.
// Represents the half-open interval [begin, end).
struct RangeConflictHeader {
	Version cv;
	uint32_t beginLength;
	uint32_t endLength;
};

struct HistoryPage {
	PageID id;
	Optional<Version> newestCV; // absent while empty
	uint32_t usedBytes = 0;

	HistoryPage* older = nullptr;
	HistoryPage* newer = nullptr;

	// Header, padding and payload together occupy HISTORY_PAGE_BYTES.
	std::byte* payload = nullptr;
};

struct PointConflictView {
	Version cv;
	StringRef key;
};

struct RangeConflictView {
	Version cv;
	StringRef begin;
	StringRef end;
};

class HeapPageTable {
public:
	// Capacity must cover the maximum number of live heap pages.
	explicit HeapPageTable(size_t capacity);

	// Registers the next consecutive page ID.
	void insert(HistoryPage* page);

	// Returns nullptr if the ID is not currently registered.
	HistoryPage* find(PageID id) const;

	// Removes the oldest registered page.
	void erase(PageID id);

private:
	std::vector<HistoryPage*> slots_;

	// Registered IDs occupy [firstID_, nextID_).
	PageID firstID_ = 1;
	PageID nextID_ = 1;
};

class TemporalHeap {
public:
	// The pool must outlive the heap.
	explicit TemporalHeap(PagePool& pool);

	// Returns all owned pages to the pool.
	// Invalidates every reference and view into this heap.
	~TemporalHeap();

	TemporalHeap(const TemporalHeap&) = delete;
	TemporalHeap& operator=(const TemporalHeap&) = delete;
	TemporalHeap(TemporalHeap&&) = delete;
	TemporalHeap& operator=(TemporalHeap&&) = delete;

	// Appends a complete record. Returns absent if the pool is exhausted.
	// Records must fit in one page; commit versions must not decrease.
	Optional<HeapRef> appendPoint(Version cv, StringRef key);
	Optional<HeapRef> appendRange(Version cv, StringRef begin, StringRef end);

	// Returns absent if the referenced page has been reclaimed.
	// The reference must identify a record of the corresponding kind.
	// Views remain valid until their page is reclaimed or the heap is destroyed.
	Optional<PointConflictView> getPoint(const HeapRef& ref) const;
	Optional<RangeConflictView> getRange(const HeapRef& ref) const;

	bool obsolete(const HeapRef& ref) const {
		return ref.pageID <= lastReclaimedPageID_;
	}

	// Reclaims the oldest page and returns its newestCV, which a linked page always
	// carries. Returns absent only when there was no page to reclaim.
	Optional<Version> reclaimOldest();

	PageID lastReclaimedPageID() const {
		return lastReclaimedPageID_;
	}

	Optional<Version> reclaimedThroughCV() const {
		return reclaimedThroughCV_;
	}

private:
	// Acquires, initializes and links a page. Returns nullptr if exhausted.
	HistoryPage* acquirePage();

	// Resolves a logical page ID to its current physical page.
	HistoryPage* findPage(PageID id) const;

	// Reserves space for one record of `bytes`, on the newest page or on a fresh
	// one, and updates the page's accounting and its version bound. The reserved
	// span is padded to the common record alignment; the returned length is the
	// caller's own. Returns absent if a required page cannot be acquired.
	Optional<HeapRef> allocateRecord(uint32_t bytes, Version cv);

	// Unlinks the page, removes its lookup entry and returns it to the pool.
	void releasePage(HistoryPage* page);

	PagePool& pool_;

	HistoryPage* oldest_ = nullptr;
	HistoryPage* newest_ = nullptr;

	PageID nextPageID_ = 1;
	PageID lastReclaimedPageID_ = 0;

	Optional<Version> reclaimedThroughCV_;

	// Logical page ID -> physical page.
	HeapPageTable pages_;
};

// ---------- Referencias al índice ----------

class TemporalSkipBlock;

struct EntryRef {
	TemporalSkipBlock* block = nullptr;
	LogicalPosition position = 0;

	bool present() const {
		return block != nullptr;
	}
};

// ---------- Resúmenes ----------

struct EndpointRef {
	HeapRef key;       // bytes of an interval's exclusive end
	Version sourceCV;  // CV of the record containing that end
};

enum class MaxEndState : uint8_t {
	Empty,       // the span contains no live intervals
	Valid,       // value is a conservative upper bound
	NeedsRepair  // cannot be used to discard the span
};

struct MaxEndSummary {
	MaxEndState state = MaxEndState::Empty;
	Optional<EndpointRef> value;
};

struct SpanSummary {
	Optional<Version> maxCV; // conservative maximum over the span
	MaxEndSummary maxEnd;   // used only by the range index
};

// ---------- Celdas y bloques ----------

struct SkipEntry {
	HeapRef record;
	HeapRef key;       // point key or interval begin
	Version recordCV;

	// Copies of this entry at adjacent levels.
	EntryRef down;
	EntryRef up;

	// Covers this entry's key-order span:
	// from this entry, inclusive, to the next entry, exclusive.
	SpanSummary forward;
};

// size in the first nibbles, slots in the remaining nibbles, 
// all in increasing key order
class KeyPermutation {
public:
	uint8_t size() const;
	bool empty() const;
	bool full() const;

	// Returns the physical slot at the given key-order rank.
	uint8_t slotAt(uint8_t rank) const;

	// Returns the key-order rank of an occupied physical slot.
	// Requires the slot to be present: an absent slot, or an empty permutation,
	// leaves nothing to return and trips an assertion.
	uint8_t rankOfSlot(uint8_t slot) const;

	// Inserts a distinct slot at rank; shifts subsequent ranks to the right.
	// Requires rank <= size(), slot < SKIP_BLOCK_CAPACITY and spare capacity.
	void insert(uint8_t rank, uint8_t slot);

	// Removes a rank and shifts subsequent ranks to the left.
	void erase(uint8_t rank);
	void clear();

private:
	// Low four bits: count. Remaining nibbles: slots in increasing key order.
	uint64_t bits_ = 0;
};

// One nibble holds the count and one holds each slot, so the count also has to
// fit in four bits. Both limits are reached at fifteen.
static_assert(SKIP_BLOCK_CAPACITY <= 15);

class TemporalSkipBlock {
public:
	TemporalSkipBlock(ConflictKind kind, Level level);

	// Entries and neighbouring blocks may hold references to this block.
	TemporalSkipBlock(const TemporalSkipBlock&) = delete;
	TemporalSkipBlock& operator=(const TemporalSkipBlock&) = delete;
	TemporalSkipBlock(TemporalSkipBlock&&) = delete;
	TemporalSkipBlock& operator=(TemporalSkipBlock&&) = delete;

	uint8_t size() const;
	bool empty() const;
	bool full() const;
	LogicalPosition beginPosition() const;
	LogicalPosition endPosition() const;

	// The block must still be alive when checking a logical position.
	bool contains(LogicalPosition position) const;
	SkipEntry& entryAt(LogicalPosition position);
	const SkipEntry& entryAt(LogicalPosition position) const;

	// Maps a key-order rank to its live logical position.
	LogicalPosition positionAtRank(uint8_t rank) const;
	SkipEntry& entryAtRank(uint8_t rank);
	const SkipEntry& entryAtRank(uint8_t rank) const;

	// Appends in temporal order and inserts its slot in key order, which it finds
	// by searching the permutation. Requires spare capacity and non-decreasing
	// recordCV within the block. The caller maintains links and span summaries.
	EntryRef append(const SkipEntry& entry, const TemporalHeap& heap);

	// Splits the existing entries and the new entry by key, preserving their relative
	// temporal order within each resulting block. This block must be full; other must
	// be a distinct, empty block of the same kind and level. The new entry must satisfy
	// append's temporal-order precondition.
	// Returns the reference to the newly inserted entry.
	EntryRef split(const SkipEntry& entry, TemporalSkipBlock& other, const TemporalHeap& heap);

	// Removes the oldest cell and its permutation entry, then advances begin.
	// The caller must first repair incoming links and transfer its span summary.
	// Logical positions are not reset when the block becomes empty.
	void popFront();

	ConflictKind kind;
	Level level;

	// Managed by the index: neighbours in key order at this level.
	TemporalSkipBlock* previous = nullptr;
	TemporalSkipBlock* next = nullptr;

	// Conservative bound over resident records and the spans summarised here.
	// Managed by the index; not an individual cell's expiry version.
	Optional<Version> reclaimMaxCV;

	// Managed by the global cleanup list.
	TemporalSkipBlock* newerForCleanup = nullptr;
	TemporalSkipBlock* olderForCleanup = nullptr;

private:
	// Occupied logical positions: [begin_, end_).
	// Physical slot = logical position % SKIP_BLOCK_CAPACITY.
	LogicalPosition begin_ = 0;
	LogicalPosition end_ = 0;
	std::array<SkipEntry, SKIP_BLOCK_CAPACITY> slots_;
	KeyPermutation keyOrder_;

	// The key an entry orders by: a point's key, or an interval's begin.
	StringRef keyOf(const SkipEntry& entry, const TemporalHeap& heap) const;

	// Finds the permutation rank of an occupied physical slot.
	uint8_t rankOfSlot(uint8_t slot) const;
};

struct SkipLevel {
	TemporalSkipBlock* first = nullptr;
	TemporalSkipBlock* last = nullptr;

	// Span before the first entry of this level.
	SpanSummary prefix;
};

struct TemporalSkipList {
	ConflictKind kind;
	std::vector<SkipLevel> levels;
};

// ---------- Limpieza global ----------

struct CleanupList {
	// Shared by all levels of both indexes.
	TemporalSkipBlock* newest = nullptr;
	TemporalSkipBlock* oldest = nullptr;
};

// ---------- Camino de búsqueda ----------

struct SearchStep {
	Level level;

	// Absent means that this level's prefix precedes the insertion.
	EntryRef predecessor;
};

struct SearchPath {
	std::vector<SearchStep> steps;
};

// ---------- Presupuesto global ----------


// Packs multiple SkipBlocks into each index page.
// Reusing a block does not release its backing page unless that
// page becomes entirely reusable.
struct IndexPageAllocator {
	// Allocation metadata and free-block management.
};

// ---------- Estado del Resolver ----------

struct TemporalConflictSet {
	Version validationMinRV; // initialised at recruitment

	PageBudget budget;
	TemporalHeap heap;
	IndexPageAllocator indexAllocator;

	TemporalSkipList points{ ConflictKind::Point, {} };
	TemporalSkipList ranges{ ConflictKind::Range, {} };

	CleanupList cleanup;
};