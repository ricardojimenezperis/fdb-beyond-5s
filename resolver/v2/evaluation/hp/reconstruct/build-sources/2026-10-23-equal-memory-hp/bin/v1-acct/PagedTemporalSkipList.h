#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Diagnostic counters of the index searches. Built in only when this is 1, which the resolver's
// CMake option of the same name sets; otherwise the counters stay at zero and cost nothing
#ifndef PAGED_RESOLVER_SEARCH_STATS
#define PAGED_RESOLVER_SEARCH_STATS 0
#endif

#include "flow/Error.h"         // UNSTOPPABLE_ASSERT, used by Optional.h
#include "flow/Optional.h"      // Optional
#include "fdbclient/FDBTypes.h" // Version

using PageID = uint64_t;
using PageIndex = uint64_t;
using Level = uint16_t;

constexpr uint32_t HISTORY_PAGE_BYTES = 64 * 1024;

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
    uint64_t heapPages = 0;   // Acquired key pages (excluding owned free pages).
    uint64_t indexPages = 0;  // Acquired index pages.
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

enum class PoolBacking : uint8_t { Normal, Index, Both };
const char* poolBackingName(PoolBacking backing);
bool parsePoolBacking(const char* name, PoolBacking& backing);

// Object-local age bound and stable handle into the compact external age vectors.
struct ReclaimLink {
    uint32_t ageIndex = UINT32_MAX;
    // Fills the padding before maxCV, so no object grows. The pool never reads or writes it. The B+
    // keeps here its node's incarnation (stableHints): it changes on allocation, so a write
    // hint from an earlier life of the address is told apart. Heap pages leave it 0
    uint32_t incarnation = 0;
    Version maxCV = std::numeric_limits<Version>::lowest();
    PageUse ageUse = PageUse::Heap;
    bool ageLinked = false;
};
// The incarnation must stay inside the padding before maxCV, so that adding it grows neither the B+'s
// nodes nor heap pages in any configuration
static_assert(offsetof(ReclaimLink, incarnation) + sizeof(uint32_t) <= offsetof(ReclaimLink, maxCV),
              "ReclaimLink::incarnation must fill existing padding");
static_assert(sizeof(ReclaimLink) == 24, "ReclaimLink keeps its 24 bytes: ageIndex and incarnation, maxCV, ageUse and ageLinked");

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
    // Returns nullptr when none are available for this use.
    PoolPage* tryAcquire(PageUse use);

    // Tries available pages, runs synchronous reclamation on exhaustion,
    // then retries once. Reclamation may instead make index cells available.
    // Returns nullptr if no page is available after reclamation.
    PoolPage* acquire(PageUse use);

    // Non-owning callback, invoked only on exhaustion. The context must outlive
    // acquisitions. The callback must not acquire storage from this pool.
    using Reclaimer = void (*)(void* context, PageUse use);
    void setReclaimer(void* context, Reclaimer reclaimer) {
        reclaimContext_ = context;
        reclaimer_ = reclaimer;
    }

    // Returns a page for reuse.
    // The page must belong to this pool, be currently acquired,
    // and be released with the same PageUse used to acquire it.
    void release(PoolPage* page, PageUse use);

    void configureIndexPool(PoolBacking backing = PoolBacking::Normal);
    bool hasAvailable(PageUse use) const;
    bool indexPool() const { return indexPool_; }
    void raiseAge(ReclaimLink* link, Version cv, PageUse use);
    void inheritAge(ReclaimLink* link, const ReclaimLink* source, PageUse use);
    void lowerNodeAgeAfterSplit(ReclaimLink* link, Version cv);
    void unlinkAge(ReclaimLink* link);
    void configureNodeAges(size_t offset, size_t stride, size_t count);
    void registerNodePage(PoolPage* page);
    uint32_t nodeAgeIndex(const void* node) const;
    ReclaimLink* nodeForAgeIndex(uint32_t id) const;
    bool hasNodeAgeIndex(uint32_t id) const { return id < nodeAges_.size(); }
    ReclaimLink* oldestAge() const;
    ReclaimLink* nextAge(const ReclaimLink* link) const;
    ReclaimLink* previousAge(const ReclaimLink* link) const;
    Version oldestAgeCV() const;
    Version ageCV(const ReclaimLink* link) const { return ageEntry(link->ageIndex).maxCV; }
    // Reuse the existing age link only after detaching the object from the age list.
    ReclaimLink* detachedNext(const ReclaimLink* link) const {
        ASSERT(!link->ageLinked);
        return ageObject(ageEntry(link->ageIndex).next);
    }
    void setDetachedNext(ReclaimLink* link, const ReclaimLink* next) {
        ASSERT(!link->ageLinked && (!next || !next->ageLinked));
        ageEntry(link->ageIndex).next = next ? next->ageIndex : UINT32_MAX;
    }
    struct ExceptionAgeStats {
        uint64_t insertions = 0, removals = 0, updates = 0, swaps = 0, peak = 0;
    };
    const ExceptionAgeStats& exceptionAgeStats() const { return exceptionStats_; }
    size_t exceptionAgeCount() const { return exceptionAges_.size(); }
    template<class Visitor> void forEachAge(Visitor&& visit) const {
        for (uint32_t id = oldestAge_; id != NoAge; id = ageEntry(id).next) visit(ageObject(id));
        for (uint32_t id : exceptionAges_) visit(ageObject(id));
    }
    size_t exceptionAgeMetadataBytes() const {
        return exceptionAges_.capacity() * sizeof(uint32_t) + sizeof(exceptionAges_) + sizeof(exceptionStats_);
    }
    size_t ageMetadataBytes() const;
    size_t indexPoolMetadataBytes() const { return indexPages_.capacity() * sizeof(IndexPage); }
    struct AgeEntry {
        uint32_t previous = UINT32_MAX, next = UINT32_MAX;
        Version maxCV = std::numeric_limits<Version>::lowest();
    };
    static_assert(sizeof(AgeEntry) == 16);

    const PageBudget& budget() const {
        return budget_;
    }

    struct BackingRange {
        uintptr_t begin = 0;
        size_t usefulBytes = 0, mappedBytes = 0;
        unsigned pageKiB = 0;
    };
    struct BackingReport {
        PoolBacking backing = PoolBacking::Normal;
        bool mapped = false;
        BackingRange index;
        uint64_t setupNanoseconds = 0;
    };
    const BackingReport& backing() const { return backing_; }

private:
    void gc(PageUse use);
    static constexpr uint32_t NoPage = UINT32_MAX;
    struct IndexPage { uint32_t next = NoPage; bool acquired = false; };
    bool indexPool_ = false;
    std::vector<IndexPage> indexPages_;
    uint32_t indexFree_ = NoPage;
    PoolPage* acquireIndexPage();

    void* reclaimContext_ = nullptr;
    Reclaimer reclaimer_ = nullptr;

    PageBudget budget_;
    static constexpr uint32_t NoAge = UINT32_MAX;
    uint32_t oldestAge_ = NoAge, newestAge_ = NoAge;
    std::vector<uint32_t> exceptionAges_;
    ExceptionAgeStats exceptionStats_;
    bool ageException(uint32_t id) const;
    bool exceptionLess(uint32_t a, uint32_t b) const;
    void exceptionSwap(size_t a, size_t b);
    void exceptionRepair(size_t pos);
    void reserveExceptionSlot();
    void insertException(uint32_t id);
    void eraseException(uint32_t id);
    void appendMainAge(uint32_t id);
    ReclaimLink* adjacentAge(const ReclaimLink* link, bool forward) const;
    // Node IDs address packed blocks, allocated only as index pages are first acquired.
    std::vector<AgeEntry> nodeAges_;
    std::vector<uint32_t> pageAgeBlocks_, nodeBlockPages_, freeAgeBlocks_;
    size_t ageNodeOffset_ = 0, ageNodeStride_ = 0, ageNodesPerPage_ = 0;
    AgeEntry& ageEntry(uint32_t id);
    const AgeEntry& ageEntry(uint32_t id) const;
    ReclaimLink* ageObject(uint32_t id) const;
    void ensureAgeIndex(ReclaimLink* link, PageUse use);

    // Contiguous region aligned to HISTORY_PAGE_BYTES.
    std::byte* base_ = nullptr;
    void* mapping_ = nullptr;
    size_t mappingBytes_ = 0;
    BackingReport backing_;
    void backRegion(PoolBacking backing);

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

struct HistoryPage : ReclaimLink {
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
    // Capacity must cover the maximum number of live heap pages. It stays the limit on live pages;
    // heapPageMask only rounds the number of slots up to a power of two (see the constructor).
    explicit HeapPageTable(size_t capacity);

    // Registers the next consecutive page ID.
    void insert(HistoryPage* page);

    // Returns nullptr if the ID is not currently registered.
    HistoryPage* find(PageID id) const;

    // Removes the oldest registered page.
    void erase(PageID id);
    size_t by5SlotBytes() const { return slots_.capacity() * sizeof(HistoryPage*); }

private:
    // The limit on live pages, as given; and slots_.size() - 1, slots_ holding bit_ceil(capacity_) entries
    size_t capacity_;
    size_t mask_;
    std::vector<HistoryPage*> slots_;

    // Registered IDs occupy [firstID_, nextID_).
    PageID firstID_ = 1;
    PageID nextID_ = 1;
};

// heapLookupInline: defined here, unchanged, so that TemporalHeap::getKeyInline below can use it in line
inline HistoryPage* HeapPageTable::find(PageID id) const {
    if (id < firstID_ || id >= nextID_) {
        return nullptr;
    }
    return slots_[id & mask_];
}

class TemporalHeap {
public:
    // The pool must outlive the heap.
    // Independent heaps retain their own allocation order.
    explicit TemporalHeap(PagePool& pool, bool blockStorage = false);
    struct BlockStorage {
        void* context = nullptr;
        size_t bytes = 0;
        HistoryPage* (*acquire)(void*) = nullptr;
        void (*release)(void*, HistoryPage*) = nullptr;
        void (*raise)(void*, HistoryPage*, Version) = nullptr;
        HistoryPage* (*find)(void*, PageID) = nullptr;
    };
    void configureBlocks(BlockStorage storage);
    void clear();
    size_t maximumRecordBytes() const;
    size_t contiguousRecordBytes() const;
    static bool fragmented(const HeapRef& ref) { return (ref.offset & FragmentOffset) != 0; }
    Optional<StringRef> getKey(const HeapRef& ref, std::vector<uint8_t>& scratch) const;
    Optional<PointConflictView> getPoint(const HeapRef& ref, std::vector<uint8_t>& scratch) const;
    Optional<RangeConflictView> getRange(const HeapRef& ref, std::vector<uint8_t>& scratch) const;
    struct FragmentCursor {
        PageID head = 0, next = 0;
        const uint8_t* data = nullptr;
        size_t start = 0;
    };
    StringRef fragment(const HeapRef& ref, size_t offset, FragmentCursor& cursor) const;
    int compareKey(const HeapRef& ref, StringRef rhs, size_t skip = 0) const;
    size_t storageBytes() const { return blocks_.context ? blocks_.bytes : HISTORY_PAGE_BYTES; }

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
    // Store key once, followed by one reserved zero byte. The returned reference
    // still describes the ordinary point record (getPoint remains compatible).
    // Key views may also include that byte to represent [key, key + zero).
    Optional<HeapRef> appendPointWithSuccessor(Version cv, StringRef key);
    Optional<HeapRef> appendRange(Version cv, StringRef begin, StringRef end);

    // Returns absent if the referenced page has been reclaimed.
    // The reference must identify a record of the corresponding kind.
    // Views remain valid until their page is reclaimed or the heap is destroyed.
    Optional<PointConflictView> getPoint(const HeapRef& ref) const;
    Optional<RangeConflictView> getRange(const HeapRef& ref) const;
    // Returns the referenced key bytes, or absent if its page was reclaimed.
    // The reference identifies key bytes, not a complete record.
    Optional<StringRef> getKey(const HeapRef& key) const;
    // getKey's body, forced in line where a caller asks for it (the B+'s key resolution); getKey itself
    // stays out of line, so the skip list's call sites are unchanged
    Optional<StringRef> getKeyInline(const HeapRef& key) const;

    // Where a record's bytes start, or null if its page was reclaimed; for prefetching only,
    // since nothing about the bytes is checked
    const void* recordAddress(const HeapRef& ref) const;

    bool obsolete(const HeapRef& ref) const {
        return blocks_.context ? findPage(ref.pageID) == nullptr : ref.pageID <= lastReclaimedPageID_;
    }

    // Version bound to install before reclaimOldest(); absent if the heap is empty.
    Optional<Version> oldestPageNewestCV() const;

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
    static constexpr uint32_t FragmentOffset = uint32_t{1} << 31;
    Optional<HeapRef> appendFragmented(const uint8_t* data, size_t bytes, Version cv);
    void copyFragmented(const HeapRef& ref, uint8_t* data) const;
    void linkPage(HistoryPage* page, Version cv);
    Optional<HeapRef> appendPointRecord(Version cv, StringRef key, bool withSuccessor);

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
    BlockStorage blocks_{};

    HistoryPage* oldest_ = nullptr;
    HistoryPage* newest_ = nullptr;

    PageID nextPageID_ = 1;
    PageID lastReclaimedPageID_ = 0;

    Optional<Version> reclaimedThroughCV_;

    // Logical page ID -> physical page.
    HeapPageTable pages_;
public:
    size_t by5PageTableBytes() const { return pages_.by5SlotBytes(); }
private:
};

// heapLookupInline: defined here so that the B+'s key resolution (PagedBTreeConflictSet::key) compiles the
// lookup in line, without the calls into PagedTemporalSkipList.cpp; the bodies, with their range, presence and
// bounds checks, are getKey's, unchanged. Forcing key() itself into its callers was measured and rejected.
inline HistoryPage* TemporalHeap::findPage(PageID id) const {
    return blocks_.context ? blocks_.find(blocks_.context, id) : pages_.find(id);
}

__attribute__((always_inline)) inline Optional<StringRef> TemporalHeap::getKeyInline(const HeapRef& key) const {
    const HistoryPage* page = findPage(key.pageID);
    if (page == nullptr) {
        return {};
    }

    ASSERT(!fragmented(key));
    // References come from allocateRecord. These checks keep the read inside the
    // page; they do not prove the bytes were written.
    ASSERT(key.offset <= storageBytes());
    ASSERT(key.length <= storageBytes() - key.offset);

    const auto* bytes =
        reinterpret_cast<const uint8_t*>(page) + key.offset;

    return StringRef(bytes, static_cast<int>(key.length));
}

// ---------- Summaries ----------

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

// ---------- Individual level entries ----------

struct SkipEntry {
    // Detached, free cell. A zero page ID is never a live heap reference.
    SkipEntry() = default;
    SkipEntry(ConflictKind kind, Level level, const HeapRef& record, Version recordCV);

    // Links refer directly to this cell. Its address remains stable until it is
    // detached from the index and cleanup list and all incoming links are repaired.
    SkipEntry(const SkipEntry&) = delete;
    SkipEntry& operator=(const SkipEntry&) = delete;
    SkipEntry(SkipEntry&&) = delete;
    SkipEntry& operator=(SkipEntry&&) = delete;

    ConflictKind kind = ConflictKind::Point;
    bool linkedInLevel = false;   // true if the cell is linked into a SkipLevel
    Level level = 0;
    HeapRef record{};
    Version recordCV = 0;

    // Neighbours in key order at this level. Equal keys retain insertion order.
    SkipEntry* previous = nullptr;
    SkipEntry* next = nullptr;

    // Copies of this record at adjacent levels.
    SkipEntry* down = nullptr;
    SkipEntry* up = nullptr;

    // Next cell in the global cleanup queue, in append order.
    // CVs need not be monotonic; cells are removed only from the head.
    // After removal this link may be stale; append overwrites it before reuse.
    SkipEntry* newerForCleanup = nullptr;

    // Covers this entry's key-order span, inclusive, to the next entry, exclusive.
    // Before removal, transfer this summary to the predecessor or level prefix.
    // An expired cell can still summarize live records at lower levels.
    SpanSummary summary;

    // Returns the point key or interval begin from record. The caller must check
    // expiry and ensure the record's heap page is still live before calling.
    StringRef beginWriteKey(const TemporalHeap& heap) const;
    // Returns an interval's exclusive end from a range record, under the same
    // conditions as beginWriteKey
    StringRef endKeyOf(const TemporalHeap& heap) const;
};

class SkipLevel {
public:
    SkipEntry* first_ = nullptr;
    SkipEntry* last_ = nullptr;

    bool empty() const { return first_ == nullptr; }

    SpanSummary& prefixSummary() { return prefixSummary_; }
    const SpanSummary& prefixSummary() const { return prefixSummary_; }

    // Links an entry not currently linked into any level or free list.
    // Its previous/next may be stale; both are overwritten without being read.
    // A null predecessor inserts at the beginning.
    // A non-null predecessor must belong to this level and differ from entry.
    // Updates horizontal links only; the caller maintains summaries.
    void insertAfter(SkipEntry* predecessor, SkipEntry* entry);

    // Unlinks an entry belonging to this level without releasing its storage.
    // Its forward summary must already have been transferred.
    // Leaves the entry's previous/next unchanged; they are no longer valid links.
    // Reinsertion or recycling must overwrite them before use.
    // Vertical and cleanup links remain unchanged.
    void unlink(SkipEntry* entry);

private:
    // Span before the first entry of this level.
    SpanSummary prefixSummary_;
};

// ---------- Global temporal cleanup queue ----------

// Shared by both indexes and all levels. Entries leave only through the head.
// Approximate CV order may delay cleanup of expired entries behind a live head.
class CleanupList {
public:
    bool empty() const { return oldest_ == nullptr; }

    // Returns the head's CV, not necessarily the minimum CV in the queue.
    // Requires a nonempty queue.
    Version oldestCV() const {
        ASSERT(oldest_ != nullptr);
        return oldest_->recordCV;
    }

    // Appends a cell not currently linked into any cleanup queue.
    // CVs need not be monotonic. Overwrites newerForCleanup without reading it.
    void append(SkipEntry* entry);

    // Removes and returns the head. Requires a nonempty queue.
    // Does not check expiry or release storage. Leaves newerForCleanup unchanged;
    // it is no longer a valid queue link and must be overwritten before reuse.
    // Horizontal and vertical links remain unchanged.
    SkipEntry* popOldest();

private:
    SkipEntry* oldest_ = nullptr;
    SkipEntry* newest_ = nullptr;
};


// ---------- Index storage ----------

// Packs individual, immovable cells into shared pool pages. All levels and both
// indexes use the same cell layout and free list, without per-level arenas.
class IndexPageAllocator {
public:
    // The pool must outlive this allocator.
    explicit IndexPageAllocator(PagePool& pool);

    // Destroys all cells and returns owned pages. Invalidates every cell pointer.
    ~IndexPageAllocator();

    IndexPageAllocator(const IndexPageAllocator&) = delete;
    IndexPageAllocator& operator=(const IndexPageAllocator&) = delete;
    IndexPageAllocator(IndexPageAllocator&&) = delete;
    IndexPageAllocator& operator=(IndexPageAllocator&&) = delete;

    // Returns an initialized, detached cell. The record must have a nonzero ID.
    // Requests storage through PagePool::acquire, including its reclamation path.
    // Throws std::bad_alloc if neither a cell nor a page can be obtained.
    // Future reclamation must preserve the record being inserted; reserve cells
    // before retaining search predecessors that reclamation could invalidate.
    SkipEntry* acquire(ConflictKind kind,
                        Level level,
                        const HeapRef& record,
                        Version recordCV);

    // Returns a cell acquired from this allocator to its free list in O(1).
    // It must be fully detached and its summary already transferred.
    // Links may be stale. During synchronous destruction of a complete tower,
    // upper cells may still carry down links to recycled cells; never follow them.
    // No other live structure may retain a reference to the cell.
    // Keeps the backing page for reuse, even if this was its last live cell.
    void release(SkipEntry* entry);

    // Scans owned pages and returns those with no live cells to the pool.
    // Removes their free cells before returning storage; returns the page count.
    // Separate from release(): costs one page scan plus work per returned cell.
    uint64_t releaseEmptyPages();

private:
    struct IndexPage;

    PagePool& pool_;
    IndexPage* firstPage_ = nullptr;

    // Free cells use previous/next; record.pageID == 0 marks their free state.
    // Vertical and temporal links may be stale; they are not followed while free.
    // Reuse resets all data and summaries.
    SkipEntry* firstFree_ = nullptr;

    // Gets cells from a new page or from reclamation triggered by the pool.
    // Returns false if neither source provides storage.
    bool acquirePage();

    // Removes an empty page's free cells and returns the page to the pool.
    void releasePage(IndexPage* page);

    static IndexPage* pageOf(const SkipEntry* entry);

    // Prepends a free cell, overwriting both horizontal links.
    // The cell must no longer belong to any level or free list.
    void linkFree(SkipEntry* entry);

    // Removes a free cell without clearing its previous/next fields.
    // The caller must overwrite those fields before reusing them.
    void unlinkFree(SkipEntry* entry);
};

// ---------- Search path ----------

struct SearchStep {
    Level level;

    // Insertion predecessor at this level; nullptr means insert at its head.
    SkipEntry* predecessor = nullptr;
};

// Saves the position found at each level so insertion need not search again.
// Steps run from the highest level to level zero.
// Structural changes or reclamation may invalidate the saved predecessors.
struct SearchPath {
    std::vector<SearchStep> steps;
};

// ---------- Temporal skip list ----------

class TemporalSkipList {
public:
    // Same height limit as FoundationDB's resolver skip list.
    static constexpr int MaxLevels = 26;

    // The heap must outlive this skip list.
    explicit TemporalSkipList(ConflictKind kind, const TemporalHeap& heap)
        : kind_(kind), heap_(heap) {}

    // Work done by the range search, for measurement only; totals since construction.
    // A span is what one iteration of the search decides: a level's prefix, or the span
    // a cell owns at its level. Each examined span has exactly one outcome, so for each
    // kind the six outcome counts add up to the spans examined at every level
    struct RangeSearchStats {
        static constexpr bool enabled = PAGED_RESOLVER_SEARCH_STATS;
        static constexpr int Prefix = 0, Cell = 1;
        uint64_t queries = 0;
        uint64_t spans[MaxLevels][2] = {};
        uint64_t prunedByMaxCV[2] = {};
        uint64_t emptyEnd[2] = {};
        uint64_t validPrunes[2] = {};
        uint64_t validExplores[2] = {};
        uint64_t needsRepair[2] = {};
        uint64_t becameNeedsRepair[2] = {};
        uint64_t descents[2] = {};
        // Explored cells, by whether their own record was read or had expired
        uint64_t recordsLevelZero = 0, recordsAbove = 0;
        uint64_t expiredCellsLevelZero = 0, expiredCellsAbove = 0;
    };
    const RangeSearchStats& rangeSearchStats() const {
        return rangeStats_;
    }
    // Summaries transferred by removing cells from their levels, for measurement and tests
    uint64_t summaryTransfers() const {
        return summaryTransfers_;
    }

    // One point read of a batch and, once answered, whether a write conflicts with it
    struct PointQuery {
        Version readVersion = 0;
        StringRef begin, end;
        bool conflict = false;
    };

    // Answers each query as hasPointConflict does, sixteen searches at a time. A search
    // advances one step at a time and, before yielding, prefetches what its next step reads:
    // a cell, then the heap record that holds the cell's key.
    //
    // Only for the synchronous query phase of a batch. Between two steps of a search, the
    // others may unlink expired towers, and nothing else may change: no insertion, no cell
    // recycled, no heap page reclaimed, no change of validationMinRV. See PointSearch
    void hasPointConflicts(PointQuery* queries, size_t count, Version validationMinRV);

    // A write whose record is in the heap and whose tower's cells are acquired but not linked.
    // findInsertionPaths fills its predecessor at every level; link then publishes it
    struct PreparedWrite {
        HeapRef record{};
        Version cv = 0;
        Level levels = 0;
        SkipEntry* cells[MaxLevels] = {};
        SkipEntry* predecessors[MaxLevels] = {};
    };

    // Draws the tower's height and acquires its cells, bottom first. Acquiring a cell may run
    // reclamation, so no path may be held across this call
    void prepare(PreparedWrite& write, const HeapRef& record, Version cv, IndexPageAllocator& allocator);

    // Finds each prepared write's predecessor at every level, as findInsertionPath does, sixteen
    // searches interleaved and prefetching. Only once every write of the group is prepared, and
    // under the same rules as hasPointConflicts: nothing allocated, recycled or reclaimed, and
    // validationMinRV fixed, until the group is linked. The writes' records must still be live
    void findInsertionPaths(PreparedWrite* writes, size_t count, Version validationMinRV);

    // Links a prepared write after its predecessors and queues its cells, splitting the spans it
    // lands in. A group's writes are linked in decreasing key order: those linked before a write all
    // have greater keys, so none comes between it and its predecessors. Returns the cells linked
    uint64_t linkTower(PreparedWrite& write, CleanupList& cleanup);

    // Grows, with a linked write, the summary of the span that contains it at each level above its
    // tower. owners[level] names the cell owning that span, or null for the level's prefix; only
    // levels from write.levels up are read. Once a whole group is linked, that owner is the
    // nearest smaller write of the group with a cell at the level, if it has the same predecessor
    // there, and otherwise the predecessor itself
    void growAbove(const PreparedWrite& write, SkipEntry* const* owners, Version validationMinRV);

    // linkTower, then growAbove into the predecessors: a single write, where they are the owners
    uint64_t link(PreparedWrite& write, Version validationMinRV, CleanupList& cleanup);

    // Links a group of prepared writes, in increasing key order in the array, whose paths are
    // found: towers in decreasing order, then the growth above each tower in increasing order into
    // the span that owns it with the whole group in place. Returns the cells linked
    uint64_t linkGroup(PreparedWrite* writes, size_t count, Version validationMinRV, CleanupList& cleanup);

    // Finds upper-bound insertion predecessors, from highest level to zero.
    // Removes expired towers before reading keys, transferring their summaries.
    // Removed cells remain in the cleanup queue until recycled.
    SearchPath findInsertionPath(StringRef key, Version validationMinRV);

    // Inserts a tower of random height for a record already in the heap, after
    // any entries with an equal key, and appends its cells to the cleanup queue.
    // Updates the summaries of the spans it splits and of those above it.
    // Bind validationMinRV to the owner's current threshold: allocating cells
    // may advance it. Search reads it only after all cells have been acquired.
    // Returns the number of cells inserted.
    uint64_t insert(const HeapRef& record,
                Version recordCV,
                const Version& validationMinRV,
                IndexPageAllocator& allocator,
                CleanupList& cleanup);

	// One query, through the same search as hasPointConflicts, run to its end
	bool hasPointConflict(Version readVersion, StringRef begin, StringRef end, 
			Version validationMinRV);
	bool hasRangeConflict(Version readVersion, StringRef begin, StringRef end, 
			Version validationMinRV);
	bool empty() const {
        return levels_[0].empty();
    }

    // Transfers and unlinks a cell still linked in its level; otherwise a no-op.
    // Does not release the cell or follow vertical links.
    void detach(SkipEntry* entry, Version validationMinRV);

private:
    ConflictKind kind_;
    const TemporalHeap& heap_;
    std::array<SkipLevel, MaxLevels> levels_;
    RangeSearchStats rangeStats_;
    uint64_t summaryTransfers_ = 0;

    // A point search between two of its steps. What it holds stays valid across the steps of
    // other searches because, during the query phase:
    // - previous is live, and stays live while validationMinRV is fixed, so no search unlinks it
    // - current may be unlinked by another search; unlinking keeps its next, previous, up and down,
    //   and no cell is recycled, so following next from it still leads, in key order, through
    //   cells that are unlinked or live to a live one or the end. Its expiry is checked before its
    //   key is read, and unlinking it again transfers nothing, since unlinkExpiredTower skips
    //   cells no longer linked
    // - no page is reclaimed, so a record prefetched at one step is still readable at the next
    struct PointSearch {
        PointQuery* query = nullptr;
        SkipEntry* previous = nullptr;
        SkipEntry* current = nullptr;
        int level = MaxLevels - 1;
        // Descending ends at level zero's first live key at or past the read's begin; the scan
        // then walks from there toward its end
        bool scanning = false;
        // current's cell has been read and its record prefetched; the key comes next
        bool recordPending = false;
    };
    // A search for a prepared write's predecessors: the last live cell at each level whose key
    // is at or below the write's, so that equal keys precede it. Its fields stay valid across other
    // searches' steps for the reasons given for PointSearch
    struct PathSearch {
        PreparedWrite* write = nullptr;
        StringRef key;
        SkipEntry* previous = nullptr;
        SkipEntry* current = nullptr;
        int level = MaxLevels - 1;
        bool recordPending = false;
    };
    void startPathSearch(PathSearch& search, PreparedWrite& write);
    // Takes one step; returns true when every level has its predecessor
    bool stepPathSearch(PathSearch& search, Version validationMinRV);

    void startPointSearch(PointSearch& search, PointQuery& query);
    // Takes one step; returns true when the query has its answer
    bool stepPointSearch(PointSearch& search, Version validationMinRV);
    void prefetchCell(const SkipEntry* entry) const;

    // Returns the highest level for a new entry, from 0 to MaxLevels - 1.
    // Each promotion has probability 1/2.
    int randomLevel() const;
    // Removes an expired tower from all its levels without releasing storage.
    // Saved search predecessors remain valid because they belong to live towers.
    void unlinkExpiredTower(SkipEntry* entry, Version validationMinRV);
    // Merges the cell's forward summary into its physical predecessor or prefix.
    void transferSummary(SkipEntry* entry, Version validationMinRV);
    // Splits the preceding span when inserting entry.
    // Preserves conservative bounds and incorporates the new record.
    // Called before linking entry into its level.
    void splitSummary(SkipEntry* predecessor, SkipEntry* entry);
    // Searches level's spans from current, or from its prefix when current is null,
    // up to stop, descending wherever a summary cannot rule a span out
    bool hasRangeConflictRec(
        Version readVersion, StringRef readBegin, StringRef readEnd,
        Version validationMinRV, int level,
        SkipEntry* current, SkipEntry* stop);
    // Rebuilds maxEnd for the complete span owned by entry,
    // or the level prefix when entry is nullptr.
    // May combine usable lower-level summaries; the result can remain
    // conservative if those summaries are themselves conservative.
    // Publishes the result only after covering the complete span.
    void repairMaxEnd(Level level, SkipEntry* entry, Version validationMinRV);
};

// ---------- Resolver state ----------

class TemporalConflictSet {
public:
    // Captures and validates the cleanup knobs; they stay fixed for this object's life.
    TemporalConflictSet(uint64_t limitPages, Version initialValidationMinRV);

    TemporalConflictSet(const TemporalConflictSet&) = delete;
    TemporalConflictSet& operator=(const TemporalConflictSet&) = delete;
    TemporalConflictSet(TemporalConflictSet&&) = delete;
    TemporalConflictSet& operator=(TemporalConflictSet&&) = delete;

    Version validationMinRV() const {
        return validationMinRV_;
    }

    const PageBudget& budget() const {
        return pool_.budget();
    }

    // History pages reclaimed under pool pressure so far, for the Resolver's metrics
    PageID reclaimedHistoryPages() const {
        return heap_.lastReclaimedPageID();
    }

    // The range index's search counters, for measurement
    const TemporalSkipList::RangeSearchStats& rangeSearchStats() const {
        return ranges_.rangeSearchStats();
    }

    // Records a point write in the heap and point index.
    void addPoint(Version cv, StringRef key);

    // Records a write to the half-open interval [begin, end).
    // Requires begin < end.
    void addRange(Version cv, StringRef begin, StringRef end);

    // Checks both indexes for writes overlapping [begin, end)
    // whose CV exceeds readVersion.
    // Throws transaction_too_old if readVersion < validationMinRV_.
    // May repair summaries, so this operation is not const.
    bool hasConflict(Version readVersion, StringRef begin, StringRef end);

    // One write of a batch: a point, [begin, keyAfter(begin)), or a range [begin, end)
    struct Write {
        bool point = false;
        StringRef begin, end;
    };

    // Records a batch's writes at version cv. Within each index their begins must be strictly
    // increasing, as the Resolver's combined write ranges are. Works in groups of sixteen: it
    // prepares a group's records and cells in increasing key order, which is where reclamation
    // may run; then, with nothing allocated until the group is linked, finds their paths
    // interleaved and links them in decreasing key order.
    //
    // If preparing a group moves validationMinRV to cv or past it, every read admitted from then
    // on has readVersion >= validationMinRV >= cv, so no write at cv can conflict with it. The
    // group's cells are released unlinked and uncounted, and the batch's remaining writes are
    // dropped. This is decided before any prepared record is read. Throws std::bad_alloc when an
    // undersized pool cannot make room, as addPoint does
    void addWrites(Version cv, const Write* writes, size_t count);

    // The two halves of hasConflict, for reads already admitted, with readVersion at or above
    // validationMinRV(): the range index for one read, and the point index for a batch of
    // reads, searched together with prefetching. Neither throws
    bool hasRangeConflict(Version readVersion, StringRef begin, StringRef end);
    void hasPointConflicts(TemporalSkipList::PointQuery* queries, size_t count);

    // Computes an adaptive cell budget using the cleanup knobs captured at construction,
    // cells inserted since the previous pass, free pages and cleanup lag.
    // Resets the insertion counter for the next pass.
    // Removes expired towers from the temporal queue's head.
    // Stops when the queue is empty, its head's CV exceeds validationMinRV_,
    // or the budget is reached. Always finishes a started tower.
    // Transfers summaries before unlinking cells still in an index,
    // and recycles every cell, including those already unlinked.
    // Returns the number of cells recycled.
    uint64_t cleanup();

private:
    // Shares tower removal between batch cleanup and allocation pressure.
    // Finishes every started tower; does not reset the insertion counter.
    uint64_t cleanupExpired(uint64_t maxCells);

    // Runs only after pool exhaustion, without the per-batch work limit.
    // Frees a page or (for Index) cells; leaves allocation failure to the caller
    // if an undersized pool cannot make progress. Never allocates from the pool.
    void reclaimForAllocation(PageUse use);

    Version validationMinRV_; // Initialised at recruitment.

    // Cells inserted across both indexes since the previous cleanup pass.
    // Counts every tower level, including cells allocated from recycled space.
    uint64_t insertedNodes_ = 0;

    // PAGED_RESOLVER_CLEANUP_* knob values, captured once at construction.
    const double nodesPerInsertedNode_;
    const int minNodes_;
    const double lowFreePageRatio_;
    const int64_t lagVersions_;
    const double boostFactor_;
    const int maxNodes_;

    // Declaration order keeps the pool alive until all consumers are destroyed.
    PagePool pool_;
    TemporalHeap heap_;
    IndexPageAllocator indexAllocator_;

    TemporalSkipList points_;
    TemporalSkipList ranges_;

    CleanupList cleanup_;
};
