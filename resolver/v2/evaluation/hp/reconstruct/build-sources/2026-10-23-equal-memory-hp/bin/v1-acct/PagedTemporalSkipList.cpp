#include "PagedTemporalSkipList.h"
#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <bit>
#include <stdexcept>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <cmath>
#include "flow/FaultInjection.h"
#include "flow/Platform.h"
#include "flow/Trace.h"
#include "fdbserver/core/Knobs.h"

// A statement of the diagnostic counters, compiled only when they are built in
#if PAGED_RESOLVER_SEARCH_STATS
#define SEARCH_STAT(statement) statement
#else
#define SEARCH_STAT(statement) \
    do {                        \
    } while (false)
#endif



namespace {

constexpr size_t RECORD_ALIGNMENT =
    std::max(alignof(PointConflictHeader), alignof(RangeConflictHeader));

constexpr size_t alignRecordSize(size_t bytes) {
    return (bytes + RECORD_ALIGNMENT - 1) & ~(RECORD_ALIGNMENT - 1);
}

constexpr size_t FIRST_RECORD_OFFSET = alignRecordSize(sizeof(HistoryPage));

static_assert(FIRST_RECORD_OFFSET < HISTORY_PAGE_BYTES);
static_assert(HISTORY_PAGE_BYTES % RECORD_ALIGNMENT == 0);

// Promotion randomness, the same generator as ConflictSet.cpp's.
thread_local uint32_t g_seed = 0;

inline int skfastrand() {
    g_seed = g_seed * 1664525L + 1013904223L;
    return g_seed;
}

} // namespace

// A released page stores the next free-page index in its first bytes.
static_assert(sizeof(PageIndex) <= HISTORY_PAGE_BYTES);

PagePool::PagePool(uint64_t limitPages) {
    constexpr uint64_t maxPages =
        std::numeric_limits<size_t>::max() / HISTORY_PAGE_BYTES;
    ASSERT(limitPages > 0);
    ASSERT(limitPages <= maxPages);
    const size_t bytes = static_cast<size_t>(limitPages) * HISTORY_PAGE_BYTES;

    INJECT_FAULT(platform_error, "PagePool reservation");
    void* region = aligned_alloc(HISTORY_PAGE_BYTES, bytes);
    if (region == nullptr) {
        TraceEvent(SevError, "PagePoolReservationFailed")
            .detail("Pages", limitPages)
            .detail("Bytes", bytes)
            .GetLastError();
        throw platform_error();
    }
    base_ = static_cast<std::byte*>(region);
    budget_.limitPages = limitPages;
    budget_.freePages = limitPages;
    // Previously unused pages are first touched by their consumers.
}

PagePool::~PagePool() {
    // The constructor either reserves the region or throws, and a constructor that
    // throws leaves no object to destroy, so base_ is always a live region here.
#ifdef __linux__
    if (mapping_) {
        munmap(mapping_, mappingBytes_);
        return;
    }
#endif
    aligned_free(base_);
}

const char* poolBackingName(PoolBacking backing) {
    switch (backing) {
    case PoolBacking::Normal: return "normal";
    case PoolBacking::Index: return "index";
    case PoolBacking::Both: return "both";
    }
    return "invalid";
}

bool parsePoolBacking(const char* name, PoolBacking& backing) {
    for (auto candidate : {PoolBacking::Normal, PoolBacking::Index, PoolBacking::Both}) {
        if (std::strcmp(name, poolBackingName(candidate)) == 0) {
            backing = candidate;
            return true;
        }
    }
    return false;
}

#ifdef __linux__
namespace {
unsigned smapsPageKiB(uintptr_t address, bool& anonHuge) {
    std::ifstream smaps("/proc/self/smaps");
    std::string line;
    bool inside = false;
    unsigned kib = 0;
    anonHuge = false;
    while (std::getline(smaps, line)) {
        unsigned long lo, hi;
        char perms[8];
        if (std::sscanf(line.c_str(), "%lx-%lx %7s", &lo, &hi, perms) == 3) {
            inside = address >= lo && address < hi;
            continue;
        }
        if (!inside) continue;
        if (line.rfind("KernelPageSize:", 0) == 0) kib = static_cast<unsigned>(std::strtoul(line.c_str() + 15, nullptr, 10));
        if (line.rfind("AnonHugePages:", 0) == 0 && std::strtoul(line.c_str() + 14, nullptr, 10) != 0) anonHuge = true;
    }
    return kib;
}
} // namespace
#endif

void PagePool::backRegion(PoolBacking backing) {
    const bool hugeIndex = backing == PoolBacking::Index || backing == PoolBacking::Both;
#ifndef __linux__
    if (hugeIndex) throw std::invalid_argument("HugeTLB pool backing requires Linux");
#else
    const auto start = std::chrono::steady_clock::now();
    constexpr size_t huge = size_t{2} << 20;
    const size_t totalBytes = static_cast<size_t>(budget_.limitPages) * HISTORY_PAGE_BYTES;
    const size_t indexBytes = totalBytes;
    const size_t reserve = totalBytes + 3 * huge;
    void* area = mmap(nullptr, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (area == MAP_FAILED)
        throw std::runtime_error("pool address reservation of " + std::to_string(reserve) +
                                 " bytes failed: " + std::strerror(errno));
    const uintptr_t low = reinterpret_cast<uintptr_t>(area);
    const uintptr_t boundary = (low + huge + huge - 1) & ~(huge - 1);
    auto fail = [](const std::string& message) { throw std::runtime_error(message); };
    const unsigned smallKiB = static_cast<unsigned>(sysconf(_SC_PAGESIZE) / 1024);
    auto map = [&](const char* name, uintptr_t begin, size_t bytes, bool hugetlb) {
        BackingRange range;
        range.begin = begin;
        range.usefulBytes = bytes;
        if (!bytes) return range;
        uintptr_t from = begin;
        size_t mapped = bytes;
        if (hugetlb) {
            from = begin & ~(huge - 1);
            mapped = ((begin + bytes + huge - 1) & ~(huge - 1)) - from;
        }
        void* p = mmap(reinterpret_cast<void*>(from), mapped, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | (hugetlb ? MAP_HUGETLB | MAP_POPULATE : 0), -1, 0);
        if (p == MAP_FAILED) {
            const std::string reason = std::strerror(errno);
            if (hugetlb)
                fail(std::string("HugeTLB pool backing failed for the ") + name + " range: mmap(MAP_HUGETLB) of " +
                     std::to_string(mapped) + " bytes (" + std::to_string(mapped / huge) +
                     " pages of 2 MiB): " + reason +
                     "; reserve 2 MiB pages (vm.nr_hugepages) or select normal pages");
            fail(std::string("pool backing failed for the ") + name + " range: mmap of " + std::to_string(mapped) +
                 " bytes: " + reason);
        }
        if (!hugetlb && madvise(p, mapped, MADV_NOHUGEPAGE) != 0)
            fail(std::string("MADV_NOHUGEPAGE failed for the ") + name + " range: " + std::strerror(errno));
        for (size_t offset = 0; offset < mapped; offset += size_t(smallKiB) * 1024)
            static_cast<volatile std::byte*>(p)[offset] = std::byte{ 0 };
        range.mappedBytes = mapped;
        range.pageKiB = smallKiB;
        if (hugetlb) {
            bool anonHuge = false;
            const unsigned first = smapsPageKiB(begin, anonHuge);
            const unsigned last = smapsPageKiB(begin + bytes - 1, anonHuge);
            if (first != 2048u || last != 2048u)
                fail(std::string("HugeTLB pool backing of the ") + name + " range has " + std::to_string(first) +
                     " and " + std::to_string(last) + " KiB pages at its ends, not 2048 KiB");
            range.pageKiB = first;
        }
        return range;
    };
    BackingReport report;
    report.backing = backing;
    report.mapped = true;
    try {
        report.index = map("index", boundary, indexBytes, hugeIndex);
    } catch (...) {
        munmap(area, reserve);
        throw;
    }
    aligned_free(base_);
    base_ = reinterpret_cast<std::byte*>(boundary);
    mapping_ = area;
    mappingBytes_ = reserve;
    report.setupNanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    backing_ = report;
#endif
}

void PagePool::configureNodeAges(size_t offset, size_t stride, size_t count) {
    ASSERT(!ageNodesPerPage_ && count && budget_.limitPages < NoAge);
    ageNodeOffset_ = offset; ageNodeStride_ = stride; ageNodesPerPage_ = count;
    pageAgeBlocks_.assign(budget_.limitPages, NoAge);
}

void PagePool::registerNodePage(PoolPage* page) {
    ASSERT(ageNodesPerPage_);
    const auto pageID = indexOf(page);
    ASSERT(pageAgeBlocks_[pageID] == NoAge);
    uint32_t block;
    if (freeAgeBlocks_.empty()) {
        ASSERT(nodeAges_.size() + ageNodesPerPage_ < NoAge);
        block = static_cast<uint32_t>(nodeBlockPages_.size());
        nodeAges_.resize(nodeAges_.size() + ageNodesPerPage_);
        nodeBlockPages_.push_back(static_cast<uint32_t>(pageID));
    } else {
        block = freeAgeBlocks_.back(); freeAgeBlocks_.pop_back();
        nodeBlockPages_[block] = static_cast<uint32_t>(pageID);
        std::fill_n(nodeAges_.begin() + block * ageNodesPerPage_, ageNodesPerPage_, AgeEntry{});
    }
    pageAgeBlocks_[pageID] = block;
}

uint32_t PagePool::nodeAgeIndex(const void* node) const {
    const size_t offset = reinterpret_cast<const std::byte*>(node) - base_;
    const size_t page = offset / HISTORY_PAGE_BYTES;
    ASSERT(page < pageAgeBlocks_.size() && pageAgeBlocks_[page] != NoAge);
    const size_t inPage = offset % HISTORY_PAGE_BYTES;
    ASSERT(inPage >= ageNodeOffset_ && (inPage - ageNodeOffset_) % ageNodeStride_ == 0);
    const size_t slot = (inPage - ageNodeOffset_) / ageNodeStride_;
    ASSERT(slot < ageNodesPerPage_);
    return static_cast<uint32_t>(pageAgeBlocks_[page] * ageNodesPerPage_ + slot);
}

PagePool::AgeEntry& PagePool::ageEntry(uint32_t id) {
    ASSERT(id != NoAge);
    return nodeAges_[id];
}
const PagePool::AgeEntry& PagePool::ageEntry(uint32_t id) const {
    return const_cast<PagePool*>(this)->ageEntry(id);
}
ReclaimLink* PagePool::ageObject(uint32_t id) const {
    if (id == NoAge) return nullptr;
    const size_t block = id / ageNodesPerPage_, slot = id % ageNodesPerPage_;
    return reinterpret_cast<ReclaimLink*>(reinterpret_cast<std::byte*>(pageAt(nodeBlockPages_[block]))
                                         + ageNodeOffset_ + slot * ageNodeStride_);
}
void PagePool::ensureAgeIndex(ReclaimLink* link, PageUse use) {
    (void)use;
    ASSERT(link->ageIndex != NoAge);
}
#include "ExceptionAgeOrder.inc"

ReclaimLink* PagePool::nodeForAgeIndex(uint32_t id) const { return ageObject(id); }

void PagePool::configureIndexPool(PoolBacking backing) {
    if (nextUnusedPage_ || indexPool_ || budget_.limitPages >= NoPage)
        throw std::invalid_argument("index pool configuration must precede allocations");
    backRegion(backing);
    indexPages_.resize(budget_.limitPages);
    indexPool_ = true;
    for (uint64_t i = budget_.limitPages; i-- > 0;) {
        indexPages_[i].next = indexFree_;
        indexFree_ = static_cast<uint32_t>(i);
    }
}
bool PagePool::hasAvailable(PageUse use) const {
    if (!indexPool_) return budget_.freePages != 0;
    return use == PageUse::Index && indexFree_ != NoPage;
}
PoolPage* PagePool::acquireIndexPage() {
    const uint32_t id = indexFree_;
    if (id == NoPage) return nullptr;
    auto& page = indexPages_[id];
    ASSERT(!page.acquired);
    indexFree_ = page.next;
    page.acquired = true; page.next = NoPage;
    --budget_.freePages;
    ++budget_.indexPages;
    return pageAt(id);
}

PoolPage* PagePool::tryAcquire(PageUse use) {
    if (indexPool_) return use == PageUse::Index ? acquireIndexPage() : nullptr;
    PageIndex index;
    if (firstFreePage_ != NULL_PAGE_INDEX) {
        index = firstFreePage_;
        firstFreePage_ = getNextFree(index);
    } else if (nextUnusedPage_ < budget_.limitPages) {
        index = nextUnusedPage_++;
    } else {
        ASSERT(budget_.freePages == 0);
        return nullptr;
    }
    ASSERT(budget_.freePages > 0);
    --budget_.freePages;
    if (use == PageUse::Heap) {
        ++budget_.heapPages;
    } else {
        ++budget_.indexPages;
    }
    return pageAt(index);
}

PoolPage* PagePool::acquire(PageUse use) {
    PoolPage* page = tryAcquire(use);
    if (page != nullptr) {
        return page;
    }
    gc(use);
    return tryAcquire(use);
}

void PagePool::gc(PageUse use) {
    if (reclaimer_ != nullptr) {
        reclaimer_(reclaimContext_, use);
    }
}

void PagePool::release(PoolPage* page, PageUse use) {
    const PageIndex index = indexOf(page);
    if (indexPool_) {
        auto& slot = indexPages_[index];
        ASSERT(use == PageUse::Index && slot.acquired);
        slot.acquired = false; slot.next = indexFree_;
        indexFree_ = static_cast<uint32_t>(index);
        ++budget_.freePages;
        --budget_.indexPages;
        return;
    }
    ASSERT(index < nextUnusedPage_);
    if (use == PageUse::Index && !pageAgeBlocks_.empty() && pageAgeBlocks_[index] != NoAge) {
        freeAgeBlocks_.push_back(pageAgeBlocks_[index]);
        pageAgeBlocks_[index] = NoAge;
    }

    // Retain the storage; overwrite only the free-list link.
    setNextFree(index, firstFreePage_);
    firstFreePage_ = index;
    ++budget_.freePages;
    ASSERT(budget_.freePages <= budget_.limitPages);
    if (use == PageUse::Heap) {
        ASSERT(budget_.heapPages > 0);
        --budget_.heapPages;
    } else {
        ASSERT(budget_.indexPages > 0);
        --budget_.indexPages;
    }
}

PoolPage* PagePool::pageAt(PageIndex index) const {
    ASSERT(index < budget_.limitPages);
    return reinterpret_cast<PoolPage*>(
        base_ + static_cast<size_t>(index) * HISTORY_PAGE_BYTES);
}

PageIndex PagePool::indexOf(const PoolPage* page) const {
    // Integer addresses avoid subtracting pointers to unrelated allocations.
    const uintptr_t address = reinterpret_cast<uintptr_t>(page);
    const uintptr_t baseAddress = reinterpret_cast<uintptr_t>(base_);
    ASSERT(address >= baseAddress);
    const uintptr_t offset = address - baseAddress;
    const size_t regionBytes =
        static_cast<size_t>(budget_.limitPages) * HISTORY_PAGE_BYTES;
    ASSERT(offset < regionBytes);
    ASSERT(offset % HISTORY_PAGE_BYTES == 0);
    return static_cast<PageIndex>(offset / HISTORY_PAGE_BYTES);
}

PageIndex PagePool::getNextFree(PageIndex index) const {
    PageIndex next;
    std::memcpy(&next, pageAt(index)->bytes, sizeof(next));
    return next;
}

void PagePool::setNextFree(PageIndex index, PageIndex next) {
    std::memcpy(pageAt(index)->bytes, &next, sizeof(next));
}

// ---------- HeapPageTable ----------

// heapPageMask: the slots are the capacity rounded up to a power of two, so a page's slot is id & mask_
// instead of a division by the capacity. Live IDs always form one interval [firstID_, nextID_): insert
// takes the next consecutive ID and erase only the oldest. The interval holds at most capacity_ IDs,
// which insert enforces against the capacity as given, not against the rounded size. Two live IDs
// a < b therefore differ by less than capacity_ <= slots_.size(), so a and b cannot share a slot under
// any modulus at least capacity_, and in particular not under the power of two. A retired ID is below
// firstID_ forever, since IDs are never reused, so find rejects it by range before reading any slot,
// whichever page holds that slot now. This presupposes that PageID, 64 bits counted from 1 with one
// increment per page, never overflows, as the heap already assumes; nothing here reuses an ID.
// The rounding costs (bit_ceil(capacity) - capacity) pointers: 3,840 bytes for 544 pages, none for a
// power of two.
static size_t heapPageTableSlots(size_t capacity) {
    ASSERT(capacity > 0);
    // bit_ceil is defined only when the next power of two is representable
    ASSERT(capacity <= (std::numeric_limits<size_t>::max() >> 1) + 1);
    return std::bit_ceil(capacity);
}

HeapPageTable::HeapPageTable(size_t capacity)
    : capacity_(capacity), mask_(heapPageTableSlots(capacity) - 1), slots_(mask_ + 1, nullptr) {}

void HeapPageTable::insert(HistoryPage* page) {
    ASSERT(page->id == nextID_);
    ASSERT(nextID_ - firstID_ < capacity_);
    const size_t slot = page->id & mask_;
    slots_[slot] = page;
    ++nextID_;
}

void HeapPageTable::erase(PageID id) {
    ASSERT(id == firstID_);
    const size_t slot = id & mask_;
    ASSERT(slots_[slot] != nullptr);
    slots_[slot] = nullptr;
    ++firstID_;
}

// ---------- TemporalHeap ----------

TemporalHeap::TemporalHeap(PagePool& pool, bool blockStorage)
    : pool_(pool), pages_(blockStorage ? 1 : pool.budget().limitPages) {}

void TemporalHeap::configureBlocks(BlockStorage storage) {
    if (oldest_ || !storage.context || !storage.acquire || !storage.release || !storage.raise || !storage.find ||
        storage.bytes <= FIRST_RECORD_OFFSET || storage.bytes > HISTORY_PAGE_BYTES)
        throw std::invalid_argument("invalid temporal block storage");
    blocks_ = storage;
}

size_t TemporalHeap::maximumRecordBytes() const {
    return (HISTORY_PAGE_BYTES - FIRST_RECORD_OFFSET) & ~(RECORD_ALIGNMENT - 1);
}

size_t TemporalHeap::contiguousRecordBytes() const {
    return (storageBytes() - FIRST_RECORD_OFFSET) & ~(RECORD_ALIGNMENT - 1);
}

TemporalHeap::~TemporalHeap() { clear(); }

void TemporalHeap::clear() {
    while (oldest_ != nullptr) {
        reclaimOldest();
    }
}

Optional<HeapRef> TemporalHeap::appendPoint(Version cv, StringRef key) {
    return appendPointRecord(cv, key, false);
}

Optional<HeapRef> TemporalHeap::appendPointWithSuccessor(Version cv, StringRef key) {
    return appendPointRecord(cv, key, true);
}

Optional<HeapRef> TemporalHeap::appendPointRecord(Version cv, StringRef key, bool withSuccessor) {
    const size_t recordSize = sizeof(PointConflictHeader) + key.size();
    if (recordSize + withSuccessor > maximumRecordBytes()) throw std::invalid_argument("record exceeds temporal storage limit");
    if (blocks_.context && recordSize + withSuccessor > contiguousRecordBytes()) {
        std::vector<uint8_t> data(recordSize + withSuccessor);
        PointConflictHeader header{cv, static_cast<uint32_t>(key.size())};
        std::memcpy(data.data(), &header, sizeof(header));
        if (key.size()) std::memcpy(data.data() + sizeof(header), key.begin(), key.size());
        auto ref = appendFragmented(data.data(), data.size(), cv);
        if (ref.present()) ref.get().length = static_cast<uint32_t>(recordSize);
        return ref;
    }
    const Optional<HeapRef> ref =
        allocateRecord(static_cast<uint32_t>(recordSize + withSuccessor), cv);
    if (!ref.present()) {
        return {};
    }

    // The reservation is always on the newest page.
    auto* record = reinterpret_cast<std::byte*>(newest_) + ref.get().offset;
    new (record) PointConflictHeader{cv, static_cast<uint32_t>(key.size())};
    if (key.size() > 0) {
        std::memcpy(
            record + sizeof(PointConflictHeader), key.begin(), key.size());
    }
    if (withSuccessor) record[recordSize] = std::byte{0};
    HeapRef point = ref.get();
    point.length = static_cast<uint32_t>(recordSize);
    return point;
}

Optional<HeapRef> TemporalHeap::appendRange(
    Version cv, StringRef begin, StringRef end) {
    ASSERT(begin < end);
    const size_t recordSize =
        sizeof(RangeConflictHeader) + begin.size() + end.size();
    if (recordSize > maximumRecordBytes()) throw std::invalid_argument("record exceeds temporal storage limit");
    if (blocks_.context && recordSize > contiguousRecordBytes()) {
        std::vector<uint8_t> data(recordSize);
        RangeConflictHeader header{cv, static_cast<uint32_t>(begin.size()), static_cast<uint32_t>(end.size())};
        std::memcpy(data.data(), &header, sizeof(header));
        if (begin.size()) std::memcpy(data.data() + sizeof(header), begin.begin(), begin.size());
        std::memcpy(data.data() + sizeof(header) + begin.size(), end.begin(), end.size());
        return appendFragmented(data.data(), data.size(), cv);
    }
    const Optional<HeapRef> ref =
        allocateRecord(static_cast<uint32_t>(recordSize), cv);
    if (!ref.present()) {
        return {};
    }

    auto* record = reinterpret_cast<std::byte*>(newest_) + ref.get().offset;
    new (record) RangeConflictHeader{
        cv,
        static_cast<uint32_t>(begin.size()),
        static_cast<uint32_t>(end.size())
    };
    auto* keys = record + sizeof(RangeConflictHeader);
    if (begin.size() > 0) {
        std::memcpy(keys, begin.begin(), begin.size());
    }
    std::memcpy(keys + begin.size(), end.begin(), end.size());
    return ref;
}

Optional<PointConflictView> TemporalHeap::getPoint(const HeapRef& ref) const {
    HistoryPage* page = findPage(ref.pageID);
    if (page == nullptr) {
        return {};
    }

    // References come from allocateRecord. These checks keep the read inside the
    // page; they do not prove the bytes were written.
    ASSERT(!fragmented(ref));
    ASSERT(ref.offset <= storageBytes());
    ASSERT(ref.length <= storageBytes() - ref.offset);
    ASSERT(ref.length >= sizeof(PointConflictHeader));

    const auto* record =
        reinterpret_cast<const std::byte*>(page) + ref.offset;
    const auto* header =
        reinterpret_cast<const PointConflictHeader*>(record);
    ASSERT(header->keyLength == ref.length - sizeof(PointConflictHeader));

    const auto* key =
        reinterpret_cast<const uint8_t*>(record + sizeof(PointConflictHeader));
    return PointConflictView{
        header->cv,
        StringRef(key, static_cast<int>(header->keyLength))
    };
}

Optional<RangeConflictView> TemporalHeap::getRange(const HeapRef& ref) const {
    HistoryPage* page = findPage(ref.pageID);
    if (page == nullptr) {
        return {};
    }

    // References come from allocateRecord. These checks keep the read inside the
    // page; they do not prove the bytes were written.
    ASSERT(!fragmented(ref));
    ASSERT(ref.offset <= storageBytes());
    ASSERT(ref.length <= storageBytes() - ref.offset);
    ASSERT(ref.length >= sizeof(RangeConflictHeader));

    const auto* record =
        reinterpret_cast<const std::byte*>(page) + ref.offset;
    const auto* header =
        reinterpret_cast<const RangeConflictHeader*>(record);
    const size_t keyBytes = ref.length - sizeof(RangeConflictHeader);
    ASSERT(header->beginLength <= keyBytes);
    ASSERT(header->endLength == keyBytes - header->beginLength);

    const auto* keys =
        reinterpret_cast<const uint8_t*>(record + sizeof(RangeConflictHeader));
    return RangeConflictView{
        header->cv,
        StringRef(keys, static_cast<int>(header->beginLength)),
        StringRef(keys + header->beginLength, static_cast<int>(header->endLength))
    };
}

Optional<StringRef> TemporalHeap::getKey(const HeapRef& key) const {
    return getKeyInline(key);
}

void TemporalHeap::linkPage(HistoryPage* page, Version cv) {
    page->older = newest_;
    page->newer = nullptr;
    if (newest_) newest_->newer = page; else oldest_ = page;
    newest_ = page;
    page->newestCV = cv;
    blocks_.raise(blocks_.context, page, cv);
}

Optional<HeapRef> TemporalHeap::appendFragmented(const uint8_t* data, size_t bytes, Version cv) {
    ASSERT(blocks_.context && bytes && bytes <= maximumRecordBytes());
    const size_t capacity = storageBytes() - FIRST_RECORD_OFFSET - sizeof(PageID);
    const size_t count = (bytes + capacity - 1) / capacity;
    std::vector<HistoryPage*> pages;
    pages.reserve(count);
    try {
        for (size_t i = 0; i < count; ++i) {
            auto* page = blocks_.acquire(blocks_.context);
            if (!page) throw std::bad_alloc();
            pages.push_back(page);
        }
    } catch (...) {
        for (auto* page : pages) blocks_.release(blocks_.context, page);
        throw;
    }
    size_t copied = 0;
    for (size_t i = 0; i < count; ++i) {
        auto* page = pages[i];
        page->usedBytes = static_cast<uint32_t>(storageBytes());
        page->payload = reinterpret_cast<std::byte*>(page) + FIRST_RECORD_OFFSET;
        const PageID next = i + 1 < count ? pages[i + 1]->id : 0;
        std::memcpy(page->payload, &next, sizeof(next));
        const size_t n = std::min(capacity, bytes - copied);
        std::memcpy(page->payload + sizeof(next), data + copied, n);
        copied += n;
        linkPage(page, cv);
    }
    return HeapRef{pages.front()->id, FragmentOffset, static_cast<uint32_t>(bytes)};
}

void TemporalHeap::copyFragmented(const HeapRef& ref, uint8_t* data) const {
    ASSERT(blocks_.context && fragmented(ref));
    size_t offset = ref.offset & ~FragmentOffset;
    ASSERT(offset <= maximumRecordBytes() && ref.length <= maximumRecordBytes() - offset);
    const size_t capacity = storageBytes() - FIRST_RECORD_OFFSET - sizeof(PageID);
    auto* page = findPage(ref.pageID);
    size_t remaining = ref.length;
    while (remaining) {
        ASSERT(page);
        PageID next;
        std::memcpy(&next, page->payload, sizeof(next));
        if (offset < capacity) {
            const size_t n = std::min(remaining, capacity - offset);
            std::memcpy(data, page->payload + sizeof(next) + offset, n);
            data += n;
            remaining -= n;
            offset = 0;
        } else offset -= capacity;
        if (remaining) page = findPage(next);
    }
}

Optional<StringRef> TemporalHeap::getKey(const HeapRef& ref, std::vector<uint8_t>& scratch) const {
    if (!fragmented(ref)) return getKey(ref);
    if (!findPage(ref.pageID)) return {};
    scratch.resize(ref.length);
    copyFragmented(ref, scratch.data());
    return StringRef(scratch.data(), static_cast<int>(scratch.size()));
}

StringRef TemporalHeap::fragment(const HeapRef& ref, size_t offset, FragmentCursor& cursor) const {
    ASSERT(blocks_.context && fragmented(ref) && offset < ref.length);
    const size_t target = (ref.offset & ~FragmentOffset) + offset;
    const size_t capacity = storageBytes() - FIRST_RECORD_OFFSET - sizeof(PageID);
    if (cursor.head != ref.pageID || !cursor.data || target < cursor.start) {
        auto* page = findPage(ref.pageID);
        ASSERT(page);
        cursor.head = ref.pageID;
        cursor.start = 0;
        cursor.data = reinterpret_cast<const uint8_t*>(page->payload) + sizeof(PageID);
        std::memcpy(&cursor.next, page->payload, sizeof(PageID));
    }
    while (target - cursor.start >= capacity) {
        auto* page = findPage(cursor.next);
        ASSERT(page);
        cursor.start += capacity;
        cursor.data = reinterpret_cast<const uint8_t*>(page->payload) + sizeof(PageID);
        std::memcpy(&cursor.next, page->payload, sizeof(PageID));
    }
    const size_t local = target - cursor.start;
    return StringRef(cursor.data + local, static_cast<int>(std::min(capacity - local, ref.length - offset)));
}

int TemporalHeap::compareKey(const HeapRef& ref, StringRef rhs, size_t skip) const {
    ASSERT(fragmented(ref) && blocks_.context);
    const size_t common = std::min<size_t>(ref.length, rhs.size());
    ASSERT(skip <= common);
    FragmentCursor cursor;
    for (size_t compared = skip; compared < common;) {
        const auto part = fragment(ref, compared, cursor);
        const size_t n = std::min<size_t>(part.size(), common - compared);
        const int order = std::memcmp(part.begin(), rhs.begin() + compared, n);
        if (order) return order;
        compared += n;
    }
    return (ref.length > size_t(rhs.size())) - (ref.length < size_t(rhs.size()));
}

Optional<PointConflictView> TemporalHeap::getPoint(const HeapRef& ref, std::vector<uint8_t>& scratch) const {
    if (!fragmented(ref)) return getPoint(ref);
    auto view = getKey(ref, scratch);
    if (!view.present()) return {};
    ASSERT(ref.length >= sizeof(PointConflictHeader));
    PointConflictHeader header;
    std::memcpy(&header, view.get().begin(), sizeof(header));
    ASSERT(header.keyLength == ref.length - sizeof(header));
    return PointConflictView{header.cv, StringRef(view.get().begin() + sizeof(header), header.keyLength)};
}

Optional<RangeConflictView> TemporalHeap::getRange(const HeapRef& ref, std::vector<uint8_t>& scratch) const {
    if (!fragmented(ref)) return getRange(ref);
    auto view = getKey(ref, scratch);
    if (!view.present()) return {};
    ASSERT(ref.length >= sizeof(RangeConflictHeader));
    RangeConflictHeader header;
    std::memcpy(&header, view.get().begin(), sizeof(header));
    ASSERT(size_t(header.beginLength) + header.endLength == ref.length - sizeof(header));
    return RangeConflictView{header.cv,
        StringRef(view.get().begin() + sizeof(header), header.beginLength),
        StringRef(view.get().begin() + sizeof(header) + header.beginLength, header.endLength)};
}

Optional<Version> TemporalHeap::oldestPageNewestCV() const {
    return oldest_ != nullptr ? oldest_->newestCV : Optional<Version>();
}

Optional<Version> TemporalHeap::reclaimOldest() {
    if (oldest_ == nullptr) {
        return {};
    }
    HistoryPage* pageToRelease = oldest_;
    const Optional<Version> cv = pageToRelease->newestCV;
    oldest_ = pageToRelease->newer;
    if (oldest_ != nullptr) {
        oldest_->older = nullptr;
    } else {
        newest_ = nullptr;
    }
    if (blocks_.context) ++lastReclaimedPageID_;
    else {
        pages_.erase(pageToRelease->id);
        lastReclaimedPageID_ = pageToRelease->id;
    }
    // A linked page always carries a version: allocateRecord sets newestCV on the page
    // it selects, and nothing between the two can fail. An absent one would break that
    // invariant, so it is reported rather than skipped.
    ASSERT(cv.present());
    if (!reclaimedThroughCV_.present() || cv.get() > reclaimedThroughCV_.get()) {
        reclaimedThroughCV_ = cv;
    }
    if (blocks_.context) blocks_.release(blocks_.context, pageToRelease);
    else {
        pool_.unlinkAge(pageToRelease);
        pool_.release(reinterpret_cast<PoolPage*>(pageToRelease), PageUse::Heap);
    }
    return cv;
}

HistoryPage* TemporalHeap::acquirePage() {
    HistoryPage* newHistoryPage;
    if (blocks_.context) newHistoryPage = blocks_.acquire(blocks_.context);
    else {
        PoolPage* page = pool_.acquire(PageUse::Heap);
        if (!page) return nullptr;
        newHistoryPage = new (page->bytes) HistoryPage{};
        newHistoryPage->id = nextPageID_++;
    }
    if (!newHistoryPage) return nullptr;
    newHistoryPage->usedBytes = static_cast<uint32_t>(FIRST_RECORD_OFFSET);
    newHistoryPage->payload = reinterpret_cast<std::byte*>(newHistoryPage) + FIRST_RECORD_OFFSET;
    newHistoryPage->newer = nullptr;
    newHistoryPage->older = newest_;
    if (newest_ != nullptr) {
        newest_->newer = newHistoryPage;
    } else {
        oldest_ = newHistoryPage;
    }
    newest_ = newHistoryPage;
    if (!blocks_.context) pages_.insert(newHistoryPage);
    return newHistoryPage;
}

const void* TemporalHeap::recordAddress(const HeapRef& ref) const {
    const HistoryPage* page = findPage(ref.pageID);
    return page == nullptr ? nullptr : reinterpret_cast<const std::byte*>(page) +
        (fragmented(ref) ? FIRST_RECORD_OFFSET + sizeof(PageID) : ref.offset);
}

Optional<HeapRef> TemporalHeap::allocateRecord(uint32_t bytes, Version cv) {
    // A record that cannot fit in an empty page violates the caller's contract.
    if (bytes > contiguousRecordBytes()) throw std::invalid_argument("record exceeds temporal storage block");
    const size_t reserved = alignRecordSize(bytes);

    if (newest_ == nullptr || reserved > storageBytes() - newest_->usedBytes) {
        if (acquirePage() == nullptr) {
            return {};
        }
    }

    const uint32_t offset = newest_->usedBytes;

    // Checked before anything is written: the pages are adjacent inside one
    // reservation, so a record crossing this page overwrites the next page, which
    // may hold index cells, and neither the system allocator nor a sanitizer sees it.
    ASSERT(offset <= storageBytes());
    ASSERT(reserved <= storageBytes() - offset);

    newest_->usedBytes += static_cast<uint32_t>(reserved);
    newest_->newestCV = cv;
    if (blocks_.context) blocks_.raise(blocks_.context, newest_, cv);
    return HeapRef{ newest_->id, offset, bytes };
}

// ---------- SkipEntry ----------

SkipEntry::SkipEntry(ConflictKind kind, Level level, const HeapRef& record, Version recordCV)
    : kind(kind), level(level), record(record), recordCV(recordCV) {}

StringRef SkipEntry::beginWriteKey(const TemporalHeap& heap) const {
    if (kind == ConflictKind::Point) {
        const Optional<PointConflictView> view = heap.getPoint(record);
        return view.get().key;
    }

    const Optional<RangeConflictView> view = heap.getRange(record);
    return view.get().begin;
}

StringRef SkipEntry::endKeyOf(const TemporalHeap& heap) const {
    ASSERT(kind == ConflictKind::Range);
    const Optional<RangeConflictView> view = heap.getRange(record);
    return view.get().end;
}

void SkipLevel::insertAfter(SkipEntry* predecessor, SkipEntry* entry) {
    ASSERT(!entry->linkedInLevel);
    ASSERT(predecessor == nullptr || predecessor->linkedInLevel);

    SkipEntry* successor =
        predecessor != nullptr ? predecessor->next : first_;

    entry->previous = predecessor;
    entry->next = successor;

    if (predecessor != nullptr) {
        predecessor->next = entry;
    } else {
        first_ = entry;
    }

    if (successor != nullptr) {
        successor->previous = entry;
    } else {
        last_ = entry;
    }

    entry->linkedInLevel = true;
}

void SkipLevel::unlink(SkipEntry* entry) {
    ASSERT(entry->linkedInLevel);

    SkipEntry* predecessor = entry->previous;
    SkipEntry* successor = entry->next;

    if (predecessor != nullptr) {
        predecessor->next = successor;
    } else {
        first_ = successor;
    }

    if (successor != nullptr) {
        successor->previous = predecessor;
    } else {
        last_ = predecessor;
    }

    entry->linkedInLevel = false;
}

// ---------- IndexPageAllocator ----------

struct IndexPageAllocator::IndexPage {
    IndexPageAllocator* owner;
    IndexPage* previous = nullptr;
    IndexPage* next = nullptr;
    size_t liveCells = 0;

    static constexpr size_t cellsOffset() {
        return (sizeof(IndexPage) + alignof(SkipEntry) - 1) &
            ~(alignof(SkipEntry) - 1);
    }

    static constexpr size_t cellCount() {
        return (HISTORY_PAGE_BYTES - cellsOffset()) / sizeof(SkipEntry);
    }

    void* cellStorage(size_t index) {
        return reinterpret_cast<std::byte*>(this) + cellsOffset() +
            index * sizeof(SkipEntry);
    }

    SkipEntry* cellAt(size_t index) {
        return std::launder(reinterpret_cast<SkipEntry*>(cellStorage(index)));
    }
};

IndexPageAllocator::IndexPageAllocator(PagePool& pool) : pool_(pool) {
    static_assert((HISTORY_PAGE_BYTES & (HISTORY_PAGE_BYTES - 1)) == 0);
    static_assert(HISTORY_PAGE_BYTES % alignof(SkipEntry) == 0);
    static_assert(IndexPage::cellsOffset() + sizeof(SkipEntry) <= HISTORY_PAGE_BYTES);
}

IndexPageAllocator::~IndexPageAllocator() {
    // Destruction invalidates every cell, including any still acquired.
    while (firstPage_ != nullptr) {
        IndexPage* page = firstPage_;
        firstPage_ = page->next;
        for (size_t i = 0; i < IndexPage::cellCount(); ++i) {
            page->cellAt(i)->~SkipEntry();
        }
        page->~IndexPage();
        pool_.release(reinterpret_cast<PoolPage*>(page), PageUse::Index);
    }
}

bool IndexPageAllocator::acquirePage() {
    PoolPage* storage = pool_.acquire(PageUse::Index);

    // GC may have recycled cells without returning an entire page.
    if (firstFree_ != nullptr) {
        if (storage != nullptr) {
            pool_.release(storage, PageUse::Index);
        }
        return true;
    }
    if (storage == nullptr) {
        return false;
    }

    IndexPage* page = new (storage) IndexPage{this};
    page->next = firstPage_;
    if (firstPage_ != nullptr) {
        firstPage_->previous = page;
    }
    firstPage_ = page;

    for (size_t i = 0; i < IndexPage::cellCount(); ++i) {
        SkipEntry* entry = new (page->cellStorage(i)) SkipEntry{};
        linkFree(entry);
    }
    return true;
}

IndexPageAllocator::IndexPage* IndexPageAllocator::pageOf(const SkipEntry* entry) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(entry);
    return reinterpret_cast<IndexPage*>(
        address & ~(static_cast<uintptr_t>(HISTORY_PAGE_BYTES) - 1));
}

void IndexPageAllocator::release(SkipEntry* entry) {
    IndexPage* page = pageOf(entry);
    ASSERT(page->owner == this);
    ASSERT(entry->record.pageID != 0);
    ASSERT(!entry->linkedInLevel);
    ASSERT(page->liveCells > 0);

    // Membership in the cleanup queue must already have ended. Its old link,
    // like the horizontal and vertical links, may remain stale until reuse.
    entry->record.pageID = 0;
    --page->liveCells;
    linkFree(entry);
}

void IndexPageAllocator::releasePage(IndexPage* page) {
    for (size_t i = 0; i < IndexPage::cellCount(); ++i) {
        SkipEntry* entry = page->cellAt(i);
        unlinkFree(entry);
        entry->~SkipEntry();
    }

    if (page->previous != nullptr) {
        page->previous->next = page->next;
    } else {
        firstPage_ = page->next;
    }
    if (page->next != nullptr) {
        page->next->previous = page->previous;
    }

    page->~IndexPage();
    pool_.release(reinterpret_cast<PoolPage*>(page), PageUse::Index);
}

uint64_t IndexPageAllocator::releaseEmptyPages() {
    uint64_t released = 0;
    IndexPage* page = firstPage_;
    while (page != nullptr) {
        IndexPage* next = page->next;
        if (page->liveCells == 0) {
            releasePage(page);
            ++released;
        }
        page = next;
    }
    return released;
}

SkipEntry* IndexPageAllocator::acquire(
    ConflictKind kind,
    Level level,
    const HeapRef& record,
    Version recordCV) {

    ASSERT(record.pageID != 0);
    if (firstFree_ == nullptr && !acquirePage()) {
        throw std::bad_alloc();
    }
    SkipEntry* entry = firstFree_;
    unlinkFree(entry);
    entry->kind = kind;
    entry->level = level;
    entry->record = record;
    entry->recordCV = recordCV;
    entry->previous = nullptr;
    entry->next = nullptr;
    entry->up = nullptr;
    entry->down = nullptr;
    entry->newerForCleanup = nullptr;
    entry->linkedInLevel = false;
    entry->summary = SpanSummary{};
    ++pageOf(entry)->liveCells;
    return entry;
}

void IndexPageAllocator::linkFree(SkipEntry* entry) {
    entry->previous = nullptr;
    entry->next = firstFree_;

    if (firstFree_ != nullptr) {
        firstFree_->previous = entry;
    }
    firstFree_ = entry;
}

void IndexPageAllocator::unlinkFree(SkipEntry* entry) {
    // Should be marked as free if it is in the free list
    ASSERT(entry->record.pageID == 0);

    SkipEntry* predecessor = entry->previous;
    SkipEntry* successor = entry->next;

    if (predecessor != nullptr) {
        predecessor->next = successor;
    } else {
        firstFree_ = successor;
    }

    if (successor != nullptr) {
        successor->previous = predecessor;
    }
}

void CleanupList::append(SkipEntry* entry) {
    if (newest_ != nullptr) {
        newest_->newerForCleanup = entry;
    } else {
        oldest_ = entry;
    }
    newest_ = entry;
    entry->newerForCleanup = nullptr;
}

SkipEntry* CleanupList::popOldest() {
    SkipEntry* entry = oldest_;
    oldest_ = entry->newerForCleanup;
    if (oldest_ == nullptr) {
        newest_ = nullptr;
    }
    return entry;
}

SearchPath TemporalSkipList::findInsertionPath(
    StringRef key,
    Version validationMinRV) {

    SearchPath path;
    path.steps.reserve(MaxLevels);

    SkipEntry* predecessor = nullptr;

    for (int level = MaxLevels - 1; level >= 0; --level) {
        // The predecessor inherited from above is live at this level too.
        SkipEntry* successor = predecessor != nullptr
            ? predecessor->next
            : levels_[level].first_;

        while (successor != nullptr) {
            if (successor->recordCV <= validationMinRV) {
                SkipEntry* expired = successor;
                successor = expired->next;

                // Removing this tower preserves the live predecessor and
                // makes successor its next physical neighbour again.
                unlinkExpiredTower(expired, validationMinRV);
                continue;
            }

            // Equal keys precede the new entry.
            if (successor->beginWriteKey(heap_) > key) {
                break;
            }

            predecessor = successor;
            successor = successor->next;
        }

        path.steps.push_back({
            static_cast<Level>(level),
            predecessor
        });

        if (level > 0 && predecessor != nullptr) {
            // Live towers have a cell at every lower level.
            ASSERT(predecessor->down != nullptr);
            predecessor = predecessor->down;
        }
    }

    return path;
}

void TemporalSkipList::unlinkExpiredTower(
    SkipEntry* entry,
    Version validationMinRV) {

    while (entry->up != nullptr) {
        entry = entry->up;
    }

    while (entry != nullptr) {
        ASSERT(entry->recordCV <= validationMinRV);

        SkipEntry* below = entry->down;

        if (entry->linkedInLevel) {
            // Other levels may have different physical predecessors.
            transferSummary(entry, validationMinRV);
            levels_[entry->level].unlink(entry);
        }

        entry = below;
    }
}

void TemporalSkipList::transferSummary(
    SkipEntry* entry,
    Version validationMinRV) {

    SpanSummary& target = entry->previous != nullptr
        ? entry->previous->summary
        : levels_[entry->level].prefixSummary();
    SEARCH_STAT(++summaryTransfers_);

    const SpanSummary& source = entry->summary;

    // Keeping an expired maximum is conservative; losing a live one is not.
    if (source.maxCV.present() &&
        (!target.maxCV.present() ||
            source.maxCV.get() > target.maxCV.get())) {
        target.maxCV = source.maxCV;
    }

    if (kind_ != ConflictKind::Range) {
        return;
    }

    bool needsRepair = false;

    if (target.maxEnd.state == MaxEndState::Empty) {
        target.maxEnd = source.maxEnd;
    } else if (source.maxEnd.state != MaxEndState::Empty) {
        needsRepair = true;
    }

    if (target.maxEnd.state == MaxEndState::Valid) {
        needsRepair |=
            target.maxEnd.value.get().sourceCV <= validationMinRV;
    }

    if (needsRepair) {
        target.maxEnd.state = MaxEndState::NeedsRepair;
        target.maxEnd.value = Optional<EndpointRef>();
    }
}

int TemporalSkipList::randomLevel() const {
    const uint32_t bits =
        uint32_t(skfastrand()) >> (32 - (MaxLevels - 1));

    return ctz(~bits);
}

void TemporalSkipList::splitSummary(
    SkipEntry* predecessor,
    SkipEntry* entry) {

    SpanSummary& previousSummary = predecessor != nullptr
        ? predecessor->summary
        : levels_[entry->level].prefixSummary();
    entry->summary = entry->level == 0
        ? SpanSummary{}
        : previousSummary;
    if (!entry->summary.maxCV.present() ||
            entry->recordCV > entry->summary.maxCV.get()) {
        entry->summary.maxCV = entry->recordCV;
    }
    if (kind_ != ConflictKind::Range) {
        return;
    }
    MaxEndSummary& maxEnd = entry->summary.maxEnd;
    if (maxEnd.state == MaxEndState::NeedsRepair) {
        return;
    }
    const auto range = heap_.getRange(entry->record);
    const StringRef end = range.get().end;
    const uint32_t endLength = static_cast<uint32_t>(end.size());
    const EndpointRef newEnd{
        HeapRef{
            entry->record.pageID,
            entry->record.offset + (entry->record.length - endLength),
            endLength
        },
        range.get().cv
    };
    if (maxEnd.state == MaxEndState::Valid) {
        const auto previousEnd = heap_.getKey(maxEnd.value.get().key);
        if (!previousEnd.present()) {
            maxEnd.state = MaxEndState::NeedsRepair;
            maxEnd.value = Optional<EndpointRef>();
            return;
        }
        if (previousEnd.get() >= end) {
            return;
        }
    }

    maxEnd.state = MaxEndState::Valid;
    maxEnd.value = newEnd;
}

void TemporalSkipList::prepare(PreparedWrite& write,
                               const HeapRef& record,
                               Version cv,
                               IndexPageAllocator& allocator) {
    write = PreparedWrite{};
    write.record = record;
    write.cv = cv;
    write.levels = static_cast<Level>(randomLevel() + 1);
    for (Level i = 0; i < write.levels; ++i) {
        write.cells[i] = allocator.acquire(kind_, i, record, cv);
    }
}

uint64_t TemporalSkipList::linkTower(PreparedWrite& write, CleanupList& cleanup) {
    SkipEntry** newEntries = write.cells;
    for (Level i = 0; i < write.levels; ++i) {
        SkipEntry* predecessor = write.predecessors[i];
        splitSummary(predecessor, newEntries[i]);
        levels_[i].insertAfter(predecessor, newEntries[i]);
        cleanup.append(newEntries[i]);
        if (i > 0) {
            newEntries[i]->down = newEntries[i - 1];
            newEntries[i - 1]->up = newEntries[i];
        }
    }
    return write.levels;
}

void TemporalSkipList::growAbove(const PreparedWrite& write, SkipEntry* const* owners, Version validationMinRV) {
    const Level levels = write.levels;
    const Version recordCV = write.cv;

    // The level-zero summary contains exactly the new write.
    const SpanSummary& newSummary = write.cells[0]->summary;
    Optional<StringRef> newEnd;
    if (kind_ == ConflictKind::Range && levels < MaxLevels) {
        newEnd = heap_.getKey(newSummary.maxEnd.value.get().key);
    }

    // Above the tower, the existing spans grow without being split.
    for (int i = levels; i < MaxLevels; ++i) {
        SkipEntry* owner = owners[i];
        SpanSummary& summary = owner != nullptr
            ? owner->summary
            : levels_[i].prefixSummary();
        if (!summary.maxCV.present() ||
            recordCV > summary.maxCV.get()) {
            summary.maxCV = recordCV;
        }
        if (kind_ != ConflictKind::Range) {
            continue;
        }

        MaxEndSummary& maxEnd = summary.maxEnd;
        if (maxEnd.state == MaxEndState::NeedsRepair) {
            continue;
        }
        if (maxEnd.state == MaxEndState::Empty) {
            maxEnd = newSummary.maxEnd;
            continue;
        }
        Optional<StringRef> previousEnd;
        if (maxEnd.value.get().sourceCV > validationMinRV) {
            previousEnd = heap_.getKey(maxEnd.value.get().key);
        }
        if (!previousEnd.present()) {
            maxEnd.state = MaxEndState::NeedsRepair;
            maxEnd.value = Optional<EndpointRef>();
        } else if (previousEnd.get() < newEnd.get()) {
            maxEnd = newSummary.maxEnd;
        }
    }
}

uint64_t TemporalSkipList::link(PreparedWrite& write, Version validationMinRV, CleanupList& cleanup) {
    const uint64_t linked = linkTower(write, cleanup);
    growAbove(write, write.predecessors, validationMinRV);
    return linked;
}

uint64_t TemporalSkipList::linkGroup(PreparedWrite* writes,
                                     size_t count,
                                     Version validationMinRV,
                                     CleanupList& cleanup) {
    uint64_t linked = 0;
    for (size_t i = count; i-- > 0;) {
        linked += linkTower(writes[i], cleanup);
    }

    // Growth follows the order per-write insertion had. At each level, the nearest smaller write
    // of the group with a cell there owns this write's span if it has the same predecessor at that
    // level, which is what places it between that predecessor and this write; otherwise no write
    // of the group lies between them and the predecessor owns it
    SkipEntry* nearestCell[MaxLevels] = {};
    SkipEntry* nearestPredecessor[MaxLevels] = {};
    SkipEntry* owners[MaxLevels];
    for (size_t i = 0; i < count; ++i) {
        PreparedWrite& write = writes[i];
        for (int level = write.levels; level < MaxLevels; ++level) {
            const bool shared = nearestCell[level] != nullptr && nearestPredecessor[level] == write.predecessors[level];
            owners[level] = shared ? nearestCell[level] : write.predecessors[level];
        }
        growAbove(write, owners, validationMinRV);
        for (int level = 0; level < write.levels; ++level) {
            nearestCell[level] = write.cells[level];
            nearestPredecessor[level] = write.predecessors[level];
        }
    }
    return linked;
}

uint64_t TemporalSkipList::insert(
    const HeapRef& record,
    Version recordCV,
    const Version& validationMinRV,
    IndexPageAllocator& allocator,
    CleanupList& cleanup) {

    PreparedWrite write;
    prepare(write, record, recordCV, allocator);

    // Allocation may have advanced validationMinRV; the search reads it only now
    const StringRef key = write.cells[0]->beginWriteKey(heap_);
    SearchPath path = findInsertionPath(key, validationMinRV);
    for (int level = 0; level < MaxLevels; ++level) {
        write.predecessors[level] = path.steps[MaxLevels - 1 - level].predecessor;
    }
    return link(write, validationMinRV, cleanup);
}

void TemporalSkipList::repairMaxEnd(
    Level level,
    SkipEntry* entry,
    Version validationMinRV) {

    ASSERT(kind_ == ConflictKind::Range);
    ASSERT(level < MaxLevels);
    ASSERT(entry == nullptr ||
        (entry->linkedInLevel && entry->level == level));

    SpanSummary& summary = entry != nullptr
        ? entry->summary
        : levels_[level].prefixSummary();
    // The next tower marks the exclusive end of this span.
    SkipEntry* stop = entry != nullptr
        ? entry->next
        : levels_[level].first_;
    for (Level i = level; i > 0 && stop != nullptr; --i) {
        ASSERT(stop->down != nullptr);
        stop = stop->down;
    }
    SkipEntry* current = entry;
    if (current != nullptr) {
        for (Level i = level; i > 0; --i) {
            ASSERT(current->down != nullptr);
            current = current->down;
        }
    } else {
        current = levels_[0].first_;
    }

    MaxEndSummary repaired;
    Optional<StringRef> greatestEnd;
    while (current != stop) {
        ASSERT(current != nullptr);
        if (current->recordCV > validationMinRV) {
            const auto range = heap_.getRange(current->record);
            const StringRef end = range.get().end;
            if (!greatestEnd.present() || greatestEnd.get() < end) {
                const uint32_t endLength =
                    static_cast<uint32_t>(end.size());
                greatestEnd = end;
                repaired.state = MaxEndState::Valid;
                repaired.value = EndpointRef{
                    HeapRef{
                        current->record.pageID,
                        current->record.offset +
                            (current->record.length - endLength),
                        endLength
                    },
                    range.get().cv
                };
            }
        }
        current = current->next;
    }

    // Remains Empty if the span contains no live intervals.
    summary.maxEnd = repaired;
}

void TemporalSkipList::detach(SkipEntry* entry, Version validationMinRV) {
    if (!entry->linkedInLevel) {
        return;
    }

    transferSummary(entry, validationMinRV);
    levels_[entry->level].unlink(entry);
}

TemporalConflictSet::TemporalConflictSet(uint64_t limitPages, Version initialValidationMinRV)
    : validationMinRV_(initialValidationMinRV),
      nodesPerInsertedNode_(SERVER_KNOBS->PAGED_RESOLVER_CLEANUP_NODES_PER_INSERTED_NODE),
      minNodes_(SERVER_KNOBS->PAGED_RESOLVER_CLEANUP_MIN_NODES),
      lowFreePageRatio_(SERVER_KNOBS->PAGED_RESOLVER_CLEANUP_LOW_FREE_PAGE_RATIO),
      lagVersions_(SERVER_KNOBS->PAGED_RESOLVER_CLEANUP_LAG_VERSIONS),
      boostFactor_(SERVER_KNOBS->PAGED_RESOLVER_CLEANUP_BOOST_FACTOR),
      maxNodes_(SERVER_KNOBS->PAGED_RESOLVER_CLEANUP_MAX_NODES),
      pool_(limitPages),
      heap_(pool_),
      indexAllocator_(pool_),
      points_(ConflictKind::Point, heap_),
      ranges_(ConflictKind::Range, heap_) {
    ASSERT(std::isfinite(nodesPerInsertedNode_) && nodesPerInsertedNode_ > 0);
    ASSERT(std::isfinite(lowFreePageRatio_) && lowFreePageRatio_ >= 0 && lowFreePageRatio_ <= 1);
    ASSERT(lagVersions_ >= 0);
    ASSERT(std::isfinite(boostFactor_) && boostFactor_ >= 1);
    ASSERT(minNodes_ > 0 && minNodes_ <= maxNodes_);

    pool_.setReclaimer(this, [](void* context, PageUse use) {
        static_cast<TemporalConflictSet*>(context)->reclaimForAllocation(use);
    });
}

void TemporalConflictSet::addPoint(Version cv, StringRef key) {
    const Write write{ true, key, StringRef() };
    addWrites(cv, &write, 1);
}

void TemporalConflictSet::addRange(Version cv, StringRef begin, StringRef end) {
    const Write write{ false, begin, end };
    addWrites(cv, &write, 1);
}

void TemporalConflictSet::addWrites(Version cv, const Write* writes, size_t count) {
    constexpr size_t Group = 16;
    TemporalSkipList::PreparedWrite points[Group];
    TemporalSkipList::PreparedWrite ranges[Group];
    for (size_t done = 0; done < count;) {
        if (validationMinRV_ >= cv) {
            // An earlier group's reclamation reached cv: these writes can conflict with no
            // admitted read, and neither can those already linked, which are now expired
            return;
        }

        // Prepare in increasing key order. Reclamation can only run here, before any path exists
        const size_t n = std::min(Group, count - done);
        size_t pointCount = 0;
        size_t rangeCount = 0;
        for (size_t i = 0; i < n; ++i) {
            const Write& w = writes[done + i];
            const Optional<HeapRef> record = w.point ? heap_.appendPoint(cv, w.begin)
                                                     : heap_.appendRange(cv, w.begin, w.end);
            if (!record.present()) {
                throw std::bad_alloc();
            }
            if (w.point) {
                points_.prepare(points[pointCount++], record.get(), cv, indexAllocator_);
            } else {
                ranges_.prepare(ranges[rangeCount++], record.get(), cv, indexAllocator_);
            }
        }
        done += n;

        // Decided before any prepared record is read: once validationMinRV >= cv, no admitted
        // read can conflict with a write at cv, and some of the group's records may be reclaimed.
        // The cells were never linked or queued, so they go back with no transfer and no count
        if (validationMinRV_ >= cv) {
            for (TemporalSkipList::PreparedWrite* group : { points, ranges }) {
                const size_t groupCount = group == points ? pointCount : rangeCount;
                for (size_t i = 0; i < groupCount; ++i) {
                    for (Level l = 0; l < group[i].levels; ++l) {
                        indexAllocator_.release(group[i].cells[l]);
                    }
                }
            }
            return;
        }

        // From here until the group is linked nothing is allocated, so validationMinRV is fixed,
        // no cell is recycled and no page reclaimed: the paths found stay valid
        points_.findInsertionPaths(points, pointCount, validationMinRV_);
        ranges_.findInsertionPaths(ranges, rangeCount, validationMinRV_);
        insertedNodes_ += points_.linkGroup(points, pointCount, validationMinRV_, cleanup_);
        insertedNodes_ += ranges_.linkGroup(ranges, rangeCount, validationMinRV_, cleanup_);
    }
}

void TemporalConflictSet::reclaimForAllocation(PageUse use) {
    if (use == PageUse::Index) {
        // One complete expired tower provides cells for the pending allocation.
        // acquirePage() already checks for recycled cells after pool_.acquire().
        if (cleanupExpired(1) != 0) {
            return;
        }
    } else {
        // Heap allocation needs a whole page, not merely free index cells.
        cleanupExpired(std::numeric_limits<uint64_t>::max());
        if (indexAllocator_.releaseEmptyPages() != 0) {
            return;
        }
    }

    // During index allocation the newest heap page contains the record being
    // inserted. If it is the only heap page left, an undersized pool must fail
    // rather than recycle that record. This check exists only on exhaustion.
    if (use == PageUse::Index && pool_.budget().heapPages <= 1) {
        return;
    }

    const Optional<Version> newestCV = heap_.oldestPageNewestCV();
    if (!newestCV.present()) {
        return;
    }

    // Publish the boundary before invalidating references or reusing storage.
    validationMinRV_ = std::max(validationMinRV_, newestCV.get());
    heap_.reclaimOldest();
}

uint64_t TemporalConflictSet::cleanup() {
    const PageBudget& pageBudget = pool_.budget();
    const bool lowFreePages =
        pageBudget.limitPages > 0 &&
        static_cast<double>(pageBudget.freePages) /
                static_cast<double>(pageBudget.limitPages) <
            lowFreePageRatio_;
    bool cleanupBehind = false;
    if (!cleanup_.empty()) {
        const Version oldestCV = cleanup_.oldestCV();

        // Check ordering before computing the unsigned version distance.
        cleanupBehind =
            oldestCV <= validationMinRV_ &&
            (static_cast<uint64_t>(validationMinRV_) -
             static_cast<uint64_t>(oldestCV)) >=
                static_cast<uint64_t>(lagVersions_);
    }
    double budget = std::max(
        static_cast<double>(minNodes_),
        std::ceil(static_cast<double>(insertedNodes_) * nodesPerInsertedNode_));
    if (lowFreePages || cleanupBehind) {
        budget *= boostFactor_;
    }

    // Cap before integer conversion, even if multiplication produced infinity.
    const uint64_t nodesToClean = static_cast<uint64_t>(
        std::min(static_cast<double>(maxNodes_), std::ceil(budget)));
    insertedNodes_ = 0;
    return cleanupExpired(nodesToClean);
}

uint64_t TemporalConflictSet::cleanupExpired(uint64_t maxCells) {
    uint64_t cleaned = 0;
    while (cleaned < maxCells &&
           !cleanup_.empty() &&
           cleanup_.oldestCV() <= validationMinRV_) {
        SkipEntry* above;
        do {
            SkipEntry* entry = cleanup_.popOldest();
            above = entry->up;

            TemporalSkipList& index =
                entry->kind == ConflictKind::Point ? points_ : ranges_;

            index.detach(entry, validationMinRV_);
            indexAllocator_.release(entry);
            ++cleaned;
        } while (above != nullptr);
    }

    return cleaned;
}

void TemporalSkipList::prefetchCell(const SkipEntry* entry) const {
    // The search reads recordCV, record, next and down; the cell spans two cache lines
    __builtin_prefetch(entry);
    __builtin_prefetch(reinterpret_cast<const std::byte*>(entry) + 64);
}

void TemporalSkipList::startPointSearch(PointSearch& search, PointQuery& query) {
    search = PointSearch{};
    search.query = &query;
    query.conflict = false;
    search.current = levels_[search.level].first_;
    if (search.current != nullptr) {
        prefetchCell(search.current);
    }
}

bool TemporalSkipList::stepPointSearch(PointSearch& s, Version validationMinRV) {
    PointQuery& query = *s.query;
    while (true) {
        if (s.current == nullptr) {
            // This level, or the scan, has run out
            if (s.scanning || s.level == 0) {
                query.conflict = false;
                return true;
            }
            if (s.previous != nullptr) {
                s.previous = s.previous->down;
            }
            --s.level;
            s.current = s.previous != nullptr ? s.previous->next : levels_[s.level].first_;
            if (s.current != nullptr) {
                prefetchCell(s.current);
                return false;
            }
            continue;
        }

        if (!s.recordPending) {
            // The cell was prefetched by the previous step; its expiry comes before its key
            if (s.current->recordCV <= validationMinRV) {
                SkipEntry* expired = s.current;
                s.current = expired->next;
                unlinkExpiredTower(expired, validationMinRV);
                if (s.current != nullptr) {
                    prefetchCell(s.current);
                    return false;
                }
                continue;
            }
            const void* record = heap_.recordAddress(s.current->record);
            if (record != nullptr) {
                __builtin_prefetch(record);
            }
            s.recordPending = true;
            return false;
        }

        s.recordPending = false;
        const StringRef key = s.current->beginWriteKey(heap_);
        if (!s.scanning) {
            if (key < query.begin) {
                s.previous = s.current;
                s.current = s.current->next;
                if (s.current != nullptr) {
                    prefetchCell(s.current);
                    return false;
                }
                continue;
            }
            if (s.level > 0) {
                // The first key at or past begin ends this level; descend from the predecessor
                if (s.previous != nullptr) {
                    s.previous = s.previous->down;
                }
                --s.level;
                s.current = s.previous != nullptr ? s.previous->next : levels_[s.level].first_;
                if (s.current != nullptr) {
                    prefetchCell(s.current);
                    return false;
                }
                continue;
            }
            // Level zero's first live key at or past begin starts the scan
            s.scanning = true;
        }

        if (key >= query.end) {
            query.conflict = false;
            return true;
        }
        if (s.current->recordCV > query.readVersion) {
            query.conflict = true;
            return true;
        }
        s.current = s.current->next;
        if (s.current != nullptr) {
            prefetchCell(s.current);
            return false;
        }
    }
}

bool TemporalSkipList::hasPointConflict(Version readVersion, StringRef begin,
		StringRef end, Version validationMinRV) {
	PointQuery query{ readVersion, begin, end };
	PointSearch search;
	startPointSearch(search, query);
	while (!stepPointSearch(search, validationMinRV)) {
	}
	return query.conflict;
}

void TemporalSkipList::hasPointConflicts(PointQuery* queries, size_t count, Version validationMinRV) {
    constexpr size_t Width = 16;
    PointSearch searches[Width];
    size_t started = 0;
    size_t active = 0;
    while (active < Width && started < count) {
        startPointSearch(searches[active++], queries[started++]);
    }
    // Round robin over the searches in flight; a finished one is replaced by the next query,
    // or by the last search in flight when none is left
    size_t i = 0;
    while (active > 0) {
        if (stepPointSearch(searches[i], validationMinRV)) {
            if (started < count) {
                startPointSearch(searches[i], queries[started++]);
            } else {
                searches[i] = searches[--active];
                if (i == active) {
                    i = 0;
                }
                continue;
            }
        }
        if (++i == active) {
            i = 0;
        }
    }
}

void TemporalSkipList::startPathSearch(PathSearch& search, PreparedWrite& write) {
    search = PathSearch{};
    search.write = &write;
    search.key = write.cells[0]->beginWriteKey(heap_);
    search.current = levels_[search.level].first_;
    if (search.current != nullptr) {
        prefetchCell(search.current);
    }
}

bool TemporalSkipList::stepPathSearch(PathSearch& s, Version validationMinRV) {
    while (true) {
        if (s.current == nullptr) {
            // This level is done: previous is the last live cell at or below the key, or none
            s.write->predecessors[s.level] = s.previous;
            if (s.level == 0) {
                return true;
            }
            if (s.previous != nullptr) {
                // Live towers have a cell at every lower level
                ASSERT(s.previous->down != nullptr);
                s.previous = s.previous->down;
            }
            --s.level;
            s.current = s.previous != nullptr ? s.previous->next : levels_[s.level].first_;
            if (s.current != nullptr) {
                prefetchCell(s.current);
                return false;
            }
            continue;
        }

        if (!s.recordPending) {
            // The cell was prefetched by the previous step; its expiry comes before its key
            if (s.current->recordCV <= validationMinRV) {
                SkipEntry* expired = s.current;
                s.current = expired->next;
                unlinkExpiredTower(expired, validationMinRV);
                if (s.current != nullptr) {
                    prefetchCell(s.current);
                    return false;
                }
                continue;
            }
            const void* record = heap_.recordAddress(s.current->record);
            if (record != nullptr) {
                __builtin_prefetch(record);
            }
            s.recordPending = true;
            return false;
        }

        s.recordPending = false;
        if (s.current->beginWriteKey(heap_) > s.key) {
            // Equal keys precede the new entry; the first greater key ends the level
            s.current = nullptr;
            continue;
        }
        s.previous = s.current;
        s.current = s.current->next;
        if (s.current != nullptr) {
            prefetchCell(s.current);
            return false;
        }
    }
}

void TemporalSkipList::findInsertionPaths(PreparedWrite* writes, size_t count, Version validationMinRV) {
    constexpr size_t Width = 16;
    PathSearch searches[Width];
    size_t started = 0;
    size_t active = 0;
    while (active < Width && started < count) {
        startPathSearch(searches[active++], writes[started++]);
    }
    size_t i = 0;
    while (active > 0) {
        if (stepPathSearch(searches[i], validationMinRV)) {
            if (started < count) {
                startPathSearch(searches[i], writes[started++]);
            } else {
                searches[i] = searches[--active];
                if (i == active) {
                    i = 0;
                }
                continue;
            }
        }
        if (++i == active) {
            i = 0;
        }
    }
}

bool TemporalSkipList::hasRangeConflictRec(
    Version readVersion, StringRef readBegin,
    StringRef readEnd, Version validationMinRV,
    int level, SkipEntry* current, SkipEntry* stop) {

    // A null starting entry means the prefix before the first entry
    bool prefix = current == nullptr;

    // Keep expired towers linked so recursive boundaries stay stable
    while (prefix || (current != nullptr && current != stop)) {
        SkipEntry* next = prefix ? levels_[level].first_ : current->next;
        SpanSummary& summary = prefix
            ? levels_[level].prefixSummary()
            : current->summary;
        [[maybe_unused]] const int kind = prefix ? RangeSearchStats::Prefix : RangeSearchStats::Cell;
        SEARCH_STAT(++rangeStats_.spans[level][kind]);

        bool explore = summary.maxCV.present() &&
                       summary.maxCV.get() > readVersion;

        if (!explore) {
            SEARCH_STAT(++rangeStats_.prunedByMaxCV[kind]);
        } else {
            MaxEndSummary& maxEnd = summary.maxEnd;
            if (maxEnd.state == MaxEndState::Empty) {
                SEARCH_STAT(++rangeStats_.emptyEnd[kind]);
                explore = false;
            } else if (maxEnd.state == MaxEndState::Valid) {
                const EndpointRef& endpoint = maxEnd.value.get();
                Optional<StringRef> greatestEnd;
                if (endpoint.sourceCV > validationMinRV) {
                    greatestEnd = heap_.getKey(endpoint.key);
                }

                if (greatestEnd.present()) {
                    explore = readBegin < greatestEnd.get();
                    SEARCH_STAT(++(explore ? rangeStats_.validExplores : rangeStats_.validPrunes)[kind]);
                } else {
                    SEARCH_STAT(++rangeStats_.becameNeedsRepair[kind]);
                    maxEnd.state = MaxEndState::NeedsRepair;
                    maxEnd.value = Optional<EndpointRef>();
                }
            } else {
                SEARCH_STAT(++rangeStats_.needsRepair[kind]);
            }
        }

        if (explore) {
            SEARCH_STAT(if (!prefix) {
                const bool live = current->recordCV > validationMinRV;
                ++(level == 0 ? (live ? rangeStats_.recordsLevelZero : rangeStats_.expiredCellsLevelZero)
                              : (live ? rangeStats_.recordsAbove : rangeStats_.expiredCellsAbove));
            });
            // Expired entries delimit spans but their records may be gone
            if (!prefix && current->recordCV > validationMinRV) {
                if (current->beginWriteKey(heap_) >= readEnd) {
                    return false;
                }
                if (current->recordCV > readVersion &&
                    current->endKeyOf(heap_) > readBegin) {
                    return true;
                }
            }

            // An unusable maxEnd cannot prune the span, so explore below
            SEARCH_STAT(rangeStats_.descents[kind] += level > 0);
            if (level > 0 && hasRangeConflictRec(
                    readVersion, readBegin, readEnd, validationMinRV,
                    level - 1,
                    prefix ? nullptr : current->down,
                    next != nullptr ? next->down : nullptr)) {
                return true;
            }
        }

        current = next;
        prefix = false;
    }

    return false;
}

bool TemporalSkipList::hasRangeConflict(
    Version readVersion, StringRef readBegin,
    StringRef readEnd, Version validationMinRV) {

    if (!(readBegin < readEnd)) {
        return false;
    }
    SEARCH_STAT(++rangeStats_.queries);

    // Start with the prefix, including when the top level is empty
    return hasRangeConflictRec(
        readVersion, readBegin, readEnd, validationMinRV,
        MaxLevels - 1, nullptr, nullptr);
}

bool TemporalConflictSet::hasRangeConflict(Version readVersion, StringRef begin, StringRef end) {
    ASSERT(readVersion >= validationMinRV_);
    return ranges_.hasRangeConflict(readVersion, begin, end, validationMinRV_);
}

void TemporalConflictSet::hasPointConflicts(TemporalSkipList::PointQuery* queries, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        ASSERT(queries[i].readVersion >= validationMinRV_);
    }
    points_.hasPointConflicts(queries, count, validationMinRV_);
}

bool TemporalConflictSet::hasConflict(Version readVersion, StringRef begin, StringRef end) {
	// Required history has been discarded, so conflict checking is no longer reliable

	if (readVersion < validationMinRV_)
		throw transaction_too_old();
	return ranges_.hasRangeConflict(readVersion, begin, end, validationMinRV_) 
				|| points_.hasPointConflict(readVersion, begin, end, validationMinRV_);
}