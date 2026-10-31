#include "PagedBTreeConflictSet.h"
#include "BlockReaderLcp.h"
#include "flow/IRandom.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <functional>
#include <stdexcept>

#if PAGED_BTREE_PERMUTATION_SIMD && defined(__SSE2__)
#include <emmintrin.h>
#endif

#if PAGED_RESOLVER_SEARCH_STATS
#define BTREE_SEARCH_STAT(statement) statement
#else
#define BTREE_SEARCH_STAT(statement)
#endif

namespace paged_btree {

unsigned KeyPermutation::slotAt(unsigned rank) const {
    ASSERT(rank < size());
    return order_[rank];
}

unsigned KeyPermutation::rankOfSlot(unsigned slot) const {
    ASSERT(slot < Capacity);
#if PAGED_BTREE_PERMUTATION_SIMD && defined(__SSE2__)
    const __m128i needle = _mm_set1_epi8(static_cast<char>(slot));
    const __m128i first = _mm_loadu_si128(reinterpret_cast<const __m128i*>(order_.data()));
    uint32_t matches = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(first, needle)));
    if constexpr (Capacity > 16) {
        const __m128i second = _mm_loadu_si128(reinterpret_cast<const __m128i*>(order_.data() + 16));
        matches |= static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(second, needle))) << 16;
    }
    // The size byte, padding and stale ranks must not match an occupied rank
    matches &= (uint32_t{1} << size()) - 1;
    ASSERT(matches != 0);
    return __builtin_ctz(matches);
#else
    unsigned rank = 0;
    while (rank < size() && order_[rank] != slot) ++rank;
    ASSERT(rank < size());
    return rank;
#endif
}

void KeyPermutation::insert(unsigned rank, unsigned slot) {
    ASSERT(size() < Capacity && rank <= size() && slot < Capacity);
    std::memmove(order_.data() + rank + 1, order_.data() + rank, size() - rank);
    order_[rank] = static_cast<uint8_t>(slot);
    ++order_[Capacity];
}

void KeyPermutation::erase(unsigned rank) {
    ASSERT(rank < size());
    std::memmove(order_.data() + rank, order_.data() + rank + 1, size() - rank - 1);
    --order_[Capacity];
}

PagedBTreeConflictSet::PagedBTreeConflictSet(uint64_t limitPages, Version initialValidationMinRV)
  : PagedBTreeConflictSet(limitPages, initialValidationMinRV, MemoryConfig{}) {}

PagedBTreeConflictSet::PagedBTreeConflictSet(uint64_t limitPages, Version initialValidationMinRV, MemoryConfig memory)
  : validationMinRV_(initialValidationMinRV), pool_(limitPages), heap_(pool_, true) {
    if (memory.heapPages == 0 && !InlineOnlyKeys)
        throw std::invalid_argument("zero key budget requires inline-only keys");
    pool_.configurePartitions(memory.heapPages, memory.indexPages, memory.minimumPercent, memory.backing);
    // Permanent index-page ownership keeps every hint address valid node storage.
    ASSERT(pool_.partitioned());
    pool_.configureNodeAges(nodeOffset(), sizeof(Node), nodesPerPage());
    pool_.setReclaimer(this, reclaimThunk);
}

PagedBTreeConflictSet::~PagedBTreeConflictSet() {
    // Nodes contain no owning allocations, and the heap is destroyed before the pool
    pool_.setReclaimer(nullptr, nullptr);
}

size_t PagedBTreeConflictSet::nodeOffset() {
    return (sizeof(NodePage) + alignof(Node) - 1) & ~(alignof(Node) - 1);
}

size_t PagedBTreeConflictSet::nodesPerPage() {
    constexpr size_t offset = (sizeof(NodePage) + alignof(Node) - 1) & ~(alignof(Node) - 1);
    static_assert(sizeof(Node) <= HISTORY_PAGE_BYTES - offset,
                  "Inline key size leaves no room for a node in an index page");
    return (HISTORY_PAGE_BYTES - nodeOffset()) / sizeof(Node);
}

PagedBTreeConflictSet::Layout PagedBTreeConflictSet::layout() {
    return { sizeof(KeyPermutation), sizeof(Entry), sizeof(Node), alignof(Node), nodesPerPage() };
}

void PagedBTreeConflictSet::addFree(Node* node) {
    pool_.setDetachedNext(node, free_);
    free_ = node;
    ++freeNodes_;
}

void PagedBTreeConflictSet::removeFree(Node* node) {
    ASSERT(node == free_);
    free_ = static_cast<Node*>(pool_.detachedNext(node));
    pool_.setDetachedNext(node, nullptr);
    --freeNodes_;
}

PagedBTreeConflictSet::Node* PagedBTreeConflictSet::allocateNode() {
    if (!free_) {
        PoolPage* raw = pool_.acquire(PageUse::Index);
        if (raw) {
            pool_.registerNodePage(raw);
            auto* page = new (raw) NodePage{nullptr, pages_, 0};
            if (pages_) pages_->previous = page;
            pages_ = page;
            linkEmptyPage(page);
            auto* bytes = reinterpret_cast<std::byte*>(page) + nodeOffset();
            for (size_t i = 0; i < nodesPerPage(); ++i) {
                auto* node = new (bytes + i * sizeof(Node)) Node;
                node->owner = page;
                node->ageIndex = pool_.nodeAgeIndex(node);
                addFree(node);
            }
        }
    }
    // Reclamation can supply free nodes instead of a whole page
    if (!free_) throw std::bad_alloc();
    Node* node = free_;
    removeFree(node);
    if (free_) prefetchCleanupNode(free_, false);
    // A new allocation invalidates all hints from the previous life of this address.
    if (node->incarnation != SaturatedIncarnation) ++node->incarnation;
    node->allocated = true;
    if (!node->owner->live) unlinkEmptyPage(node->owner);
    ++node->owner->live;
    return node;
}

void PagedBTreeConflictSet::releaseNode(Node* node) {
    if (node->fict.fictitious) {
        stats_.fictitiousRetired += node->fict.fictitious;
        setFictitious(node, 0);
    }
    if (node->fict.pair.priority) setPair(node, 0);
    pool_.unlinkAge(node);
    --nodeCount_;
    ASSERT(node->allocated);
    node->allocated = false;
    if (!--node->owner->live) linkEmptyPage(node->owner);
    if (writeBatchActive_ && !reclaiming_) {
        // Permanent index pages keep retired storage alive until the list is spliced.
        pool_.setDetachedNext(node, retired_);
        if (!retired_) retiredTail_ = node;
        retired_ = node;
        ++retiredCount_;
        BTREE_SEARCH_STAT(stats_.writeBatchMaxRetired = std::max(stats_.writeBatchMaxRetired, retiredCount_));
    } else {
        addFree(node);
    }
}

void PagedBTreeConflictSet::recycleNode(Node* node) {
    // Only unpublished reservations or nodes whose hints have been discarded reach here
    pool_.unlinkAge(node);
    node->allocated = false;
    if (!--node->owner->live) linkEmptyPage(node->owner);
    addFree(node);
}

void PagedBTreeConflictSet::finishWriteBatch() {
    flushPairs();
    // Discard batch hints before returning retired nodes to the free queue.
    writeHints_.clear();
    drainRetired();
    writeBatchActive_ = false;
}

void PagedBTreeConflictSet::drainRetired() {
    if (!retired_) return;
    pool_.setDetachedNext(retiredTail_, free_);
    free_ = retired_;
    freeNodes_ += retiredCount_;
    retired_ = retiredTail_ = nullptr;
    retiredCount_ = 0;
}

void PagedBTreeConflictSet::linkEmptyPage(NodePage* page) {
    ASSERT(!page->live);
    page->emptyPrevious = nullptr;
    page->emptyNext = emptyPages_;
    if (emptyPages_) emptyPages_->emptyPrevious = page;
    emptyPages_ = page;
}

void PagedBTreeConflictSet::unlinkEmptyPage(NodePage* page) {
    ASSERT(!page->live);
    if (page->emptyPrevious) page->emptyPrevious->emptyNext = page->emptyNext;
    else { ASSERT(emptyPages_ == page); emptyPages_ = page->emptyNext; }
    if (page->emptyNext) page->emptyNext->emptyPrevious = page->emptyPrevious;
    page->emptyPrevious = page->emptyNext = nullptr;
}

void PagedBTreeConflictSet::releaseEmptyPages() {
    // The constructor requires permanent index-page ownership. Empty pages may
    // contain retired nodes; their storage remains available for node reuse.
    ASSERT(pool_.partitioned());
}

bool PagedBTreeConflictSet::reserve(Reservation& reservation, unsigned required) {
    ASSERT(required <= reservation.nodes.size());
    bool mayHaveReclaimed = false;
    while (reservation.size < required) {
        mayHaveReclaimed |= !freeNodes_;
        Node* node = allocateNode();
        reservation.nodes[reservation.size++] = node;
        BTREE_SEARCH_STAT(++stats_.nodesReserved);
    }
    return mayHaveReclaimed;
}

PagedBTreeConflictSet::Node* PagedBTreeConflictSet::take(Reservation& reservation, bool leaf) {
    ASSERT(reservation.size != 0);
    Node* node = reservation.nodes[--reservation.size];
    BTREE_SEARCH_STAT(++stats_.nodesTaken);
    NodePage* owner = node->owner;
    const uint32_t incarnation = node->incarnation;
    *node = Node{};
    node->incarnation = incarnation;
    node->ageIndex = pool_.nodeAgeIndex(node);
    node->owner = owner;
    node->allocated = true;
    node->leaf = leaf;
    if (!leaf) new (&node->navigation.prefix) Node::ReferencePrefix{};
    ++nodeCount_;
    if (leaf) {
        ++leafCount_;
    }
    return node;
}

void PagedBTreeConflictSet::releaseReservation(Reservation& reservation) {
    // These nodes were never published, so no pending hint can refer to them
    while (reservation.size) recycleNode(reservation.nodes[--reservation.size]);
}

void PagedBTreeConflictSet::reclaimThunk(void* context, PageUse use) {
    static_cast<PagedBTreeConflictSet*>(context)->reclaim(use);
}

void PagedBTreeConflictSet::reclaim(PageUse use) {
    ++stats_.reclaimCalls;
    // In-batch reclamation retains hints while recycling retired nodes.
    // Validate each hint against the node's current incarnation.
    ASSERT(!reclaiming_);
    reclaiming_ = true;
    struct Restore { bool& flag; ~Restore() { flag = false; } } restore{reclaiming_};
    if (writeBatchActive_) drainRetired();
    releaseEmptyPages();
    while (!pool_.hasAvailable(use) && !(use == PageUse::Index && freeNodes_)) {
        ReclaimLink* oldest = pool_.oldestAge();
        if (!oldest) return; // Only unpublished/pinned allocations can remain.
        ++stats_.reclaimAgeCandidates;
        validationMinRV_ = std::max(validationMinRV_, pool_.oldestAgeCV());
        observeFictitiousFloor();
        if (oldest->ageUse == PageUse::Heap) {
            // Heap pages retain their append order within the shared CV list.
            const auto cv = heap_.oldestPageNewestCV();
            ASSERT(cv.present() && cv.get() == oldest->maxCV);
            heap_.reclaimOldest();
            ++stats_.heapPagesReclaimed;
        } else {
            ++stats_.reclaimNodeCandidates;
            reclaimNode(static_cast<Node*>(oldest));
            releaseEmptyPages();
        }
    }
}

void PagedBTreeConflictSet::reclaimNode(Node* node) {
    ASSERT(node->allocated && node->maxCV <= validationMinRV_);
    Node* parent = node->parent;
    // Detach the subtree's contiguous leaf span before returning its nodes.
    Node* first = node;
    Node* last = node;
    while (!first->leaf) first = first->at(0).payload.link.child;
    while (!last->leaf) last = last->at(last->size()-1).payload.link.child;
    Node* before = first->navigation.leaf.previous;
    Node* after = last->navigation.leaf.next;
    if (before) before->navigation.leaf.next = after; else firstLeaf_ = after;
    if (after) after->navigation.leaf.previous = before;
    if (before) markPair(before);
    if (after) markPair(after);
    if (parent) eraseSlot(parent, parent->order.rankOfSlot(node->parentSlot));
    else root_ = nullptr;
    destroySubtree(node);
    if (parent && !parent->size()) removeEmpty(parent);
    else if (parent) refreshUp(parent);
    collapseRoot();
}

void PagedBTreeConflictSet::discardExpiredRoot() {
    if (root_ && (root_->maxCV <= validationMinRV_ || minimum(root_).cv == NoVersion)) {
        destroySubtree(root_);
        root_ = firstLeaf_ = nullptr;
    }
}

void PagedBTreeConflictSet::prefetchCleanupNode(Node* node, bool wholeNode) {
    if constexpr (PAGED_BTREE_CLEANUP_PREFETCH) {
        const size_t bytes = wholeNode ? sizeof(Node) : (reinterpret_cast<const char*>(&node->slots) - reinterpret_cast<const char*>(node));
        for (size_t offset = 0; offset < bytes; offset += 64)
            __builtin_prefetch(reinterpret_cast<const char*>(node) + offset, 0, 1);
    }
}

void PagedBTreeConflictSet::insertSlot(Node* node, unsigned rank, const Entry& entry) {
    const uint32_t freeMask = (~node->occupied) & SlotMask;
    ASSERT(freeMask != 0);
    unsigned slot;
    if constexpr (PAGED_BTREE_BITMAP_CLEANUP >= 2) {
        slot = __builtin_ctz(freeMask);
    } else {
        unsigned candidates = freeMask & (~0U << node->cursor);
        if (!candidates) candidates = freeMask;
        slot = __builtin_ctz(candidates);
        node->cursor = (slot + 1) % Capacity;
    }
    if constexpr (PAGED_BTREE_BITMAP_CLEANUP == 0) {
        if (!node->occupied) node->reclaimHead = slot;
    }
    node->slots[slot] = entry;
    node->occupied |= 1U << slot;
    node->order.insert(rank, slot);
    BTREE_SEARCH_STAT(++stats_.slotInserts);
    if (!node->leaf) {
        ASSERT(entry.payload.link.child);
        entry.payload.link.child->parent = node;
        entry.payload.link.child->parentSlot = slot;
        if (entry.payload.link.child->leaf) markPair(entry.payload.link.child);
    } else markPair(node);
}

void PagedBTreeConflictSet::eraseSlot(Node* node, unsigned rank) {
    const unsigned slot = node->order.slotAt(rank);
    node->occupied &= ~(1U << slot);
    if constexpr (PAGED_BTREE_BITMAP_CLEANUP == 0) {
        if (slot == node->reclaimHead && node->occupied) {
            const uint32_t following = node->occupied & (~0U << (slot + 1));
            node->reclaimHead = __builtin_ctz(following ? following : node->occupied);
        }
    }
    node->order.erase(rank);
    BTREE_SEARCH_STAT(++stats_.slotErases);
    if (node->leaf) markPair(node);
}

StringRef PagedBTreeConflictSet::key(const HeapRef& ref) const {
    ASSERT(!isInlineOnly(ref));
    BTREE_SEARCH_STAT(++stats_.heapKeyLoads);
    return heap_.getKeyInline(ref).get();
}

HeapRef PagedBTreeConflictSet::inlineReference(size_t length) {
    if (inlineIdentity_ == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("inline key identity exhausted");
    // Heap offsets are within a 64 KiB page, so this tag cannot collide with a
    // heap reference. Identity distinguishes unrelated inline begin/end pairs.
    return {++inlineIdentity_, std::numeric_limits<uint32_t>::max(), static_cast<uint32_t>(length)};
}

int PagedBTreeConflictSet::compareImplicitZero(const HeapRef& ref, StringRef rhs) {
    ASSERT(implicitZero(ref) && ref.length == InlineKeyBytes + 1 && size_t(rhs.size()) > InlineKeyBytes);
    const unsigned byte = rhs.begin()[InlineKeyBytes];
    if (byte) return -1;
    return (ref.length > size_t(rhs.size())) - (ref.length < size_t(rhs.size()));
}

void PagedBTreeConflictSet::cacheKey(KeyPrefix& prefix, StringRef value) {
    const size_t bytes = std::min<size_t>(InlineKeyBytes, value.size());
    if (bytes) std::memcpy(prefix.data(), value.begin(), bytes);
}

void PagedBTreeConflictSet::setEnd(Entry& entry, const HeapRef& ref, const KeyPrefix& prefix) {
    // Copy first: the source can alias the destination union or beginInline.
    const KeyPrefix bytes = prefix;
    entry.end = ref;
    if (endSharesStorage(entry)) entry.beginInline = bytes;
    else new (&entry.payload.endInline) KeyPrefix(bytes);
}

void PagedBTreeConflictSet::setBegin(Entry& entry, const HeapRef& ref, const KeyPrefix& prefix) {
    const KeyPrefix endBytes = endPrefix(entry);
    entry.begin = ref;
    entry.beginInline = prefix;
    setEnd(entry, entry.end, endBytes);
}

StringRef PagedBTreeConflictSet::storedKey(const HeapRef& ref, const KeyPrefix& prefix, KeyScratch& scratch) const {
    if (implicitZero(ref)) {
        if constexpr (InlineKeyBytes) std::memcpy(scratch.data(), prefix.data(), InlineKeyBytes);
        scratch[InlineKeyBytes] = 0;
        return StringRef(scratch.data(), ref.length);
    }
    if (isInlineOnly(ref)) return StringRef(prefix.data(), ref.length);
    if constexpr (InlineKeyBytes > 0)
        if (ref.length <= InlineKeyBytes) return StringRef(prefix.data(), ref.length);
    return key(ref);
}

int PagedBTreeConflictSet::compareKey(const HeapRef& ref, const KeyPrefix& prefix,
                                    StringRef rhs, const uint8_t** cachedFull) const {
    const size_t common = std::min<size_t>(ref.length, rhs.size());
    const size_t local = std::min(InlineKeyBytes, common);
    const int prefixOrder = local ? std::memcmp(prefix.data(), rhs.begin(), local) : 0;
    if (prefixOrder || common <= InlineKeyBytes) {
        BTREE_SEARCH_STAT(++stats_.inlineKeyDecisions);
        if (prefixOrder) return prefixOrder;
        return (ref.length > static_cast<size_t>(rhs.size())) -
               (ref.length < static_cast<size_t>(rhs.size()));
    }
    if (implicitZero(ref)) {
        BTREE_SEARCH_STAT(++stats_.inlineKeyDecisions);
        return compareImplicitZero(ref, rhs);
    }
    BTREE_SEARCH_STAT(++stats_.keyComparisonFallbacks);
    const uint8_t* full = cachedFull ? *cachedFull : nullptr;
    if (!full) {
        full = key(ref).begin();
        if (cachedFull) *cachedFull = full;
    }
    const int suffixOrder = std::memcmp(full + local, rhs.begin() + local, common - local);
    if (suffixOrder) return suffixOrder;
    return (ref.length > static_cast<size_t>(rhs.size())) -
           (ref.length < static_cast<size_t>(rhs.size()));
}

PagedBTreeConflictSet::Entry PagedBTreeConflictSet::plainSeparator(const Node* node, const Entry& entry) const {
    if constexpr (!NodePrefixEnabled) return entry;
    Entry result = entry;
    if constexpr (NodePrefixEnabled) {
        const size_t skip = entry.end.length;
        if (skip) {
            const size_t first = std::min(skip, InlineKeyBytes);
            if constexpr (InlineKeyBytes) {
                std::memcpy(result.beginInline.data(), node->navigation.prefix.bytes.data(), first);
                const size_t rest = std::min<size_t>(InlineKeyBytes, entry.begin.length) - first;
                if (rest) std::memcpy(result.beginInline.data() + first, entry.beginInline.data(), rest);
            }
        }
    } else { (void)node; }
    result.end = {};
    return result;
}

void PagedBTreeConflictSet::encodeSeparator(Node* node, Entry& entry) {
    entry.end = {}; // input has a normal full-key prefix
    if constexpr (NodePrefixEnabled) {
        const auto& reference = node->navigation.prefix;
        const size_t skip = reference.length;
        if (!reference.initialized || !skip || entry.payload.link.firstCV <= validationMinRV_ || entry.begin.length < skip) return;
        // A mismatching separator keeps its ordinary inline bytes. Avoid loading
        // its full key just to discover a mismatch already visible in the node.
        if constexpr (InlineKeyBytes >= NodePrefixBytes) {
            if (std::memcmp(entry.beginInline.data(), reference.bytes.data(), skip)) return;
        }
        KeyScratch scratch;
        if (entry.begin.length > InlineKeyBytes && !isInlineOnly(entry.begin)) {
            BTREE_SEARCH_STAT(++stats_.separatorEncodeHeapLoads);
        }
        const StringRef full = storedKey(entry.begin, entry.beginInline, scratch);
        if constexpr (InlineKeyBytes < NodePrefixBytes) {
            if (std::memcmp(full.begin(), reference.bytes.data(), skip)) return;
        }
        const size_t n = std::min<size_t>(InlineKeyBytes, entry.begin.length - skip);
        // full may alias beginInline for a key that originally fitted there.
        KeyPrefix suffix{};
        if constexpr (InlineKeyBytes) if (n) std::memcpy(suffix.data(), full.begin() + skip, n);
        entry.beginInline = suffix;
        entry.end.length = static_cast<uint32_t>(skip);
    } else { (void)node; }
}

void PagedBTreeConflictSet::assignSeparator(Node* node, Entry& entry, const Entry& first, const Node* encodedAt) {
    const bool unchanged = entry.payload.link.firstCV != NoVersion && first.cv != NoVersion &&
        entry.begin.pageID == first.begin.pageID && entry.begin.offset == first.begin.offset &&
        entry.begin.length == first.begin.length;
    entry.payload.link.firstCV = first.cv;
    if constexpr (NodePrefixEnabled) if (unchanged) return;
    (void)unchanged;
    entry.begin = first.begin;
    if constexpr (NodePrefixEnabled) {
        if (encodedAt && !encodedAt->leaf && first.end.length) {
            const auto& reference = node->navigation.prefix;
            const size_t skip = first.end.length;
            if (reference.initialized && reference.length == skip &&
                !std::memcmp(reference.bytes.data(), encodedAt->navigation.prefix.bytes.data(), skip)) {
                entry.beginInline = first.beginInline;
                entry.end = {}; entry.end.length = first.end.length;
                BTREE_SEARCH_STAT(++stats_.separatorEncodedReuses);
                return;
            }
            entry.beginInline = plainSeparator(encodedAt, first).beginInline;
        } else entry.beginInline = first.beginInline;
        encodeSeparator(node, entry);
    } else { entry.beginInline = first.beginInline; (void)encodedAt; }
}

void PagedBTreeConflictSet::initializeReferencePrefix(Node* node) {
    if constexpr (NodePrefixEnabled) {
        auto& reference = node->navigation.prefix;
        if (reference.initialized || node->size() < 2) return;
        const Entry* first = nullptr;
        const Entry* second = nullptr;
        for (unsigned i = 0; i < node->size(); ++i) {
            const auto& entry = node->at(i);
            if (entry.payload.link.firstCV <= validationMinRV_) continue;
            if (!first) first = &entry;
            else { second = &entry; break; }
        }
        if (!second) return;
        KeyScratch aScratch, bScratch;
        const auto prefixBytes = [&](const Entry& entry, KeyScratch& scratch) {
            if constexpr (InlineKeyBytes >= NodePrefixBytes)
                return StringRef(entry.beginInline.data(), std::min<size_t>(NodePrefixBytes, entry.begin.length));
            else return storedKey(entry.begin, entry.beginInline, scratch);
        };
        const auto a = prefixBytes(*first, aScratch);
        const auto b = prefixBytes(*second, bScratch);
        const size_t limit = std::min<size_t>(NodePrefixBytes, std::min(a.size(), b.size()));
        size_t common = 0;
        while (common < limit && a.begin()[common] == b.begin()[common]) ++common;
        if (common) std::memcpy(reference.bytes.data(), a.begin(), common);
        reference.length = static_cast<uint8_t>(common);
        reference.initialized = true; // even a zero-length reference is frozen
        for (unsigned i = 0; i < node->size(); ++i) encodeSeparator(node, node->at(i));
    } else { (void)node; }
}

int PagedBTreeConflictSet::compareSeparator(const Node* node, const Entry& entry, StringRef rhs,
                                           const uint8_t** cachedFull, PrefixMatch* saved) const {
    if constexpr (!NodePrefixEnabled) return compareBegin(entry, rhs, cachedFull);
    int order; size_t matched;
    if (separatorPrefix(node, entry, rhs, order, matched, saved)) {
        BTREE_SEARCH_STAT(++stats_.inlineKeyDecisions);
        return order;
    }
    BTREE_SEARCH_STAT(++stats_.keyComparisonFallbacks);
    const uint8_t* full = cachedFull ? *cachedFull : nullptr;
    if (!full) { full = key(entry.begin).begin(); if (cachedFull) *cachedFull = full; }
    const size_t common = std::min<size_t>(entry.begin.length, rhs.size());
    order = std::memcmp(full + matched, rhs.begin() + matched, common - matched);
    return order ? order : (entry.begin.length > size_t(rhs.size())) - (entry.begin.length < size_t(rhs.size()));
}

PagedBTreeConflictSet::Entry PagedBTreeConflictSet::minimum(Node* node, const Node** encodedAt) {
    if (encodedAt) *encodedAt = nullptr;
    BTREE_SEARCH_STAT(++stats_.minimumCalls);
    for (unsigned rank = 0; rank < node->size(); ++rank) {
        Entry& entry = node->at(rank);
        BTREE_SEARCH_STAT(++stats_.minimumEntries);
        if (entry.cv <= validationMinRV_) continue;
        if (node->leaf) {
            if (encodedAt) *encodedAt = node;
            return entry;
        }
        if (!refreshSeparator(entry)) continue;
        if (encodedAt) *encodedAt = node;
        Entry result = encodedAt ? entry : plainSeparator(node, entry);
        result.cv = entry.payload.link.firstCV;
        return result;
    }
    return Entry{};
}

bool PagedBTreeConflictSet::refreshSeparator(Entry& entry) {
    if (entry.payload.link.firstCV > validationMinRV_) return true;
    ASSERT(entry.cv > validationMinRV_);
    const Node* encodedAt = nullptr;
    const Entry first = minimum(entry.payload.link.child, &encodedAt);
    if (first.cv == NoVersion) return false;
    assignSeparator(entry.payload.link.child->parent, entry, first, encodedAt);
    BTREE_SEARCH_STAT(++stats_.separatorRefreshes);
    return true;
}

void PagedBTreeConflictSet::refresh(Node* node) {
    BTREE_SEARCH_STAT(++stats_.refreshes);
    BTREE_SEARCH_STAT(stats_.refreshEntries += node->size());
    Version maximum = node->maxCV;
    unsigned criticalChildren = 0;
    for (unsigned i = 0; i < node->size(); ++i) {
        Entry& entry = node->at(i);
        if (!node->leaf) {
            entry.cv = entry.payload.link.child->maxCV;
            const Node* encodedAt = nullptr;
            const Entry first = minimum(entry.payload.link.child, &encodedAt);
            assignSeparator(node, entry, first, encodedAt);
            criticalChildren += entry.payload.link.child->critical;
        }
        maximum = std::max(maximum, entry.cv);
    }
    if (!node->leaf) initializeReferencePrefix(node);
    if (maximum > node->maxCV) pool_.raiseAge(node, maximum, PageUse::Index);
    // Deletions increase slack, and merges must explicitly preserve this invariant
    ASSERT(node->leaf || Capacity - node->size() >= criticalChildren);
    ASSERT(!node->leaf ? !node->fict.fictitious : node->size() + node->fict.fictitious <= Capacity);
    node->criticalChildren = criticalChildren;
    node->critical = node->leaf ? node->size() + node->fict.fictitious == Capacity
                               : Capacity - node->size() == criticalChildren;
}

// Refresh the changed node fully. Each ancestor updates only its changed child,
// then recomputes its maximum and adjusts critical-child accounting.
void PagedBTreeConflictSet::refreshUp(Node* node) {
    bool wasCritical = node->critical;
    refresh(node);
    for (Node* child = node, *parent = node->parent; parent; child = parent, parent = parent->parent) {
        const bool parentWasCritical = parent->critical;
        refreshEntry(parent, child, wasCritical);
        wasCritical = parentWasCritical;
    }
}

void PagedBTreeConflictSet::refreshEntry(Node* parent, Node* child, bool childWasCritical) {
    BTREE_SEARCH_STAT(++stats_.refreshes);
    Entry& entry = parent->slots[child->parentSlot];
    ASSERT(entry.payload.link.child == child);
    entry.cv = child->maxCV;
    const Node* encodedAt = nullptr;
    const Entry first = minimum(child, &encodedAt);
    assignSeparator(parent, entry, first, encodedAt);
    parent->criticalChildren = parent->criticalChildren + child->critical - childWasCritical;
    if (child->maxCV > parent->maxCV) pool_.raiseAge(parent, child->maxCV, PageUse::Index);
    BTREE_SEARCH_STAT(stats_.refreshEntries += parent->size());
    ASSERT(Capacity - parent->size() >= parent->criticalChildren);
    parent->critical = Capacity - parent->size() == parent->criticalChildren;
}

unsigned PagedBTreeConflictSet::liveRanks(Node* node, std::array<unsigned, Capacity>& ranks) {
    unsigned count = 0;
    for (unsigned i = 0; i < node->size(); ++i) {
        Entry& entry = node->at(i);
        if (entry.cv > validationMinRV_ && (node->leaf || refreshSeparator(entry))) ranks[count++] = i;
    }
    BTREE_SEARCH_STAT(stats_.liveRankEntries += node->size());
    return count;
}

unsigned PagedBTreeConflictSet::childRank(Node* node, StringRef begin) {
    std::array<unsigned, Capacity> ranks;
    const unsigned count = liveRanks(node, ranks);
    if (!count) return node->size();
    unsigned lo = 0, hi = count;
    PrefixMatch prefix;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        BTREE_SEARCH_STAT(++stats_.childRankProbes);
        Entry& entry = node->at(ranks[mid]);
        refreshSeparator(entry);
        if (compareSeparator(node, entry, begin, nullptr, &prefix) <= 0) lo = mid + 1;
        else hi = mid;
    }
    return ranks[lo == 0 ? 0 : lo - 1];
}

PagedBTreeConflictSet::Node* PagedBTreeConflictSet::findLeaf(StringRef begin) {
    BTREE_SEARCH_STAT(++stats_.writeRootSearches);
    Node* node = root_;
    BTREE_SEARCH_STAT(++stats_.descents);
    while (!node->leaf) {
        BTREE_SEARCH_STAT(++stats_.descentNodes);
        node = node->at(childRank(node, begin)).payload.link.child;
    }
    return node;
}

void PagedBTreeConflictSet::collapseRoot() {
    while (root_ && !root_->leaf && root_->size() == 1) {
        Node* old = root_;
        root_ = old->at(0).payload.link.child;
        root_->parent = nullptr;
        releaseNode(old);
    }
}

void PagedBTreeConflictSet::removeEmpty(Node* node) {
    ASSERT(node->size() == 0);
    Node* parent = node->parent;
    if (node->leaf) {
        if (node->navigation.leaf.previous) node->navigation.leaf.previous->navigation.leaf.next = node->navigation.leaf.next;
        else firstLeaf_ = node->navigation.leaf.next;
        if (node->navigation.leaf.next) node->navigation.leaf.next->navigation.leaf.previous = node->navigation.leaf.previous;
        if (node->navigation.leaf.previous) markPair(node->navigation.leaf.previous);
        if (node->navigation.leaf.next) markPair(node->navigation.leaf.next);
        --leafCount_;
    }
    if (parent) eraseSlot(parent, parent->order.rankOfSlot(node->parentSlot));
    else root_ = nullptr;
    releaseNode(node);
    if (parent && !parent->size()) removeEmpty(parent);
    else if (parent) refreshUp(parent);
    collapseRoot();
}

bool PagedBTreeConflictSet::purgeHead(Node* leaf) {
    bool changed = false;
    if constexpr (PAGED_BTREE_BITMAP_CLEANUP != 0) {
        uint32_t pending = leaf->occupied;
        while (pending) {
            const unsigned slot = __builtin_ctz(pending);
            pending &= pending - 1;
            if (leaf->slots[slot].cv > validationMinRV_) continue;
            eraseSlot(leaf, leaf->order.rankOfSlot(slot));
            ++stats_.expiredIntervals;
            changed = true;
        }
        return changed;
    }
    while (leaf->occupied && leaf->slots[leaf->reclaimHead].cv <= validationMinRV_) {
        eraseSlot(leaf, leaf->order.rankOfSlot(leaf->reclaimHead));
        ++stats_.expiredIntervals; changed = true;
    }
    return changed;
}

bool PagedBTreeConflictSet::purgeInsertionHead(Node* node) {
    // First reclaim locally at the already installed floor. Never move the floor
    // or consult the global age list merely to make room in this node.
    if (node->leaf) {
        const auto before = node->size();
        if (!purgeHead(node)) return false;
        stats_.localHeadEntriesReclaimed += before - node->size();
        refreshUp(node);
        return true;
    }
    bool changed = false;
    if constexpr (PAGED_BTREE_BITMAP_CLEANUP != 0) {
        uint32_t pending = node->occupied;
        while (node->allocated && pending) {
            // Reclamation may detach or collapse this node. Nothing is allocated here, and surviving
            // entries keep their slots; no Entry reference is held across a reclamation.
            pending &= node->occupied;
            if (!pending) break;
            const unsigned slot = __builtin_ctz(pending);
            pending &= pending - 1;
            if (node->slots[slot].cv > validationMinRV_) continue;
            Node* child = node->slots[slot].payload.link.child;
            ++stats_.localHeadChildrenReclaimed;
            reclaimNode(child);
            changed = true;
        }
        return changed;
    }
    while (node->allocated && node->occupied) {
        Entry& head = node->slots[node->reclaimHead];
        if (head.cv > validationMinRV_) break;
        Node* child = head.payload.link.child;
        ++stats_.localHeadChildrenReclaimed;
        // Tree maintenance can also remove/collapse this node. No allocation
        // intervenes; a retired node stays readable until hints are discarded.
        reclaimNode(child);
        changed = true;
    }
    return changed;
}

void PagedBTreeConflictSet::destroySubtree(Node* node) {
    if (!node->leaf) {
        for (unsigned i = 0; i < node->size(); ++i) destroySubtree(node->at(i).payload.link.child);
    } else {
        --leafCount_;
        stats_.expiredIntervals += node->size();
    }
    releaseNode(node);
}

// Maximum of this node's entries, without descending into subtrees. Requires refreshed entries.
Version PagedBTreeConflictSet::entriesMaximum(const Node* node) const {
    Version maximum = std::numeric_limits<Version>::lowest();
    for (unsigned i = 0; i < node->size(); ++i) {
        const Entry& entry = node->at(i);
        maximum = std::max(maximum, node->leaf ? entry.cv : entry.payload.link.child->maxCV);
    }
    return maximum;
}

PagedBTreeConflictSet::Node* PagedBTreeConflictSet::split(Node* node, Reservation& reservation, unsigned incomingRank) {
    ASSERT(node->critical);
    Node* parent = node->parent;
    unsigned cut = node->size() / 2;
    by5fictitious4::Policy<Capacity>::Split plan{};
    const bool fictitiousLeaf = fictitious_.enabled && node->leaf;
    const unsigned splitReserve = fictitiousReplenishmentStopped_ ? 0 : fictitious_.splitReserve;
    if (fictitiousLeaf) {
        uint64_t z = (fictitiousChoice_ += UINT64_C(0x9e3779b97f4a7c15));
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        plan = by5fictitious4::Policy<Capacity>::split(node->size(), node->fict.fictitious, incomingRank, splitReserve, z ^ (z >> 31));
        cut = plan.leftReal;
    }
    Node* right = take(reservation, node->leaf);
    if (!node->leaf) right->navigation.prefix = node->navigation.prefix;
    while (node->size() > cut) {
        Entry entry = node->at(cut);
        eraseSlot(node, cut);
        insertSlot(right, right->size(), entry);
    }
    if (node->leaf) {
        right->navigation.leaf.next = node->navigation.leaf.next;
        right->navigation.leaf.previous = node;
        if (right->navigation.leaf.next) right->navigation.leaf.next->navigation.leaf.previous = right;
        node->navigation.leaf.next = right;
    }
    if (fictitiousLeaf) {
        setFictitious(node, plan.leftFictitious);
        setFictitious(right, plan.rightFictitious);
        stats_.fictitiousCreated += splitReserve;
        ++stats_.fictitiousSplits;
    }
    pool_.inheritAge(right, node, PageUse::Index);
    refresh(node);
    refresh(right);
    const Version leftMaximum = entriesMaximum(node);
    const Version rightMaximum = entriesMaximum(right);
    pool_.lowerNodeAgeAfterSplit(node, leftMaximum);
    pool_.lowerNodeAgeAfterSplit(right, rightMaximum);
    if (!parent) {
        parent = take(reservation, false);
        if (!node->leaf) parent->navigation.prefix = node->navigation.prefix;
        // The left half need not hold the original maximum any more. Give the
        // new root the larger half's bound and temporal position directly.
        pool_.inheritAge(parent, leftMaximum >= rightMaximum ? node : right, PageUse::Index);
        root_ = parent;
        Entry leftEntry;
        leftEntry.payload.link.child = node;
        insertSlot(parent, 0, leftEntry);
    }
    Entry rightEntry;
    rightEntry.payload.link.child = right;
    const unsigned rank = parent->order.rankOfSlot(node->parentSlot);
    insertSlot(parent, rank + 1, rightEntry);
    refreshUp(parent);
    ++stats_.splits;
    if (!node->leaf) ++stats_.internalSplits;
    return right;
}

PagedBTreeConflictSet::InsertCursor PagedBTreeConflictSet::insertAt(
        InsertCursor cursor, const Entry& interval, Reservation& reservation) {
    for (;;) {
        // After reclamation, validate versions before dereferencing keys or cursors.
        // Pending right fragments retain their original CV.
        if (interval.cv <= validationMinRV_) return {};
        if (!cursor.leaf) discardExpiredRoot();
        Node* critical = nullptr;
        unsigned required = 1;
        if (root_) {
            if (!cursor.leaf) {
                KeyScratch scratch;
                const StringRef begin = storedKey(interval.begin, interval.beginInline, scratch);
                cursor.leaf = root_->leaf ? root_ : findLeaf(begin);
                // A raised floor can invalidate old heap references in this leaf.
                // Keep an empty destination attached, just as overwrite does.
                bool purged = false;
                for (unsigned rank = 0; rank < cursor.leaf->size();) {
                    if (cursor.leaf->at(rank).cv <= validationMinRV_) {
                        eraseSlot(cursor.leaf, rank);
                        ++stats_.expiredIntervals;
                        purged = true;
                    } else ++rank;
                }
                if (purged) refreshUp(cursor.leaf);
                unsigned lo = 0, hi = cursor.leaf->size();
                while (lo < hi) {
                    const unsigned mid = lo + (hi - lo) / 2;
                    if (compareBegin(cursor.leaf->at(mid), begin) < 0) lo = mid + 1;
                    else hi = mid;
                }
                cursor.rank = lo;
            }
            bool reclaimedLocally = false;
            for (Node* node = cursor.leaf; node; node = node->parent) {
                if (!node->critical) continue;
                if (purgeInsertionHead(node)) {
                    // Expired children may contain the destination or change its rank; reposition before reserving.
                    cursor = {};
                    reclaimedLocally = true;
                } else critical = node;
                break;
            }
            if (reclaimedLocally) continue;
            if (fictitious_.enabled && critical && critical == cursor.leaf && critical->fict.fictitious) {
                ASSERT(by5fictitious4::Policy<Capacity>::needsSingleRelease(critical->size(), critical->fict.fictitious));
                setFictitious(critical, critical->fict.fictitious - 1u);
                ++stats_.fictitiousReleased;
                refreshUp(critical);
                continue;
            }
            if (fictitious_.enabled && critical && critical == cursor.leaf && redistribute(cursor, interval)) {
                ++stats_.intervalInsertions;
                return cursor;
            }
            required = critical ? (critical->parent ? 1 : 2) : 0;
            if (fictitious_.enabled && reservation.size < required && !freeNodes_ && !retiredCount_ && !pool_.hasAvailable(PageUse::Index)) {
                if (pressureFusion(cursor.leaf)) {
                    cursor = {};
                    continue;
                }
            }
        }
        if (reserve(reservation, required)) {
            cursor = {}; // Allocation could reclaim or recycle this path; locate it again.
            continue;
        }
        while (reservation.size > required) recycleNode(reservation.nodes[--reservation.size]);
        if (!root_) {
            root_ = take(reservation, true);
            if (fictitious_.enabled) {
                setFictitious(root_, fictitiousReplenishmentStopped_ ? 0 : std::min(fictitious_.rootReserve, Capacity));
                stats_.fictitiousCreated += root_->fict.fictitious;
                refresh(root_);
            }
            firstLeaf_ = root_;
            cursor = {root_, 0};
        } else if (critical) {
            Node* right = split(critical, reservation, critical == cursor.leaf ? cursor.rank : 0);
            if (critical == cursor.leaf && cursor.rank >= cursor.leaf->size()) {
                cursor.rank -= cursor.leaf->size();
                cursor.leaf = right;
            }
        }
        insertSlot(cursor.leaf, cursor.rank, interval);
        refreshUp(cursor.leaf);
        ++stats_.intervalInsertions;
        ++cursor.rank;
        return cursor;
    }
}

void PagedBTreeConflictSet::configureFictitiousLeaves(const FictitiousLeafConfig& config) {
    if (writeBatchActive_ || root_ || nodeCount_) throw std::logic_error("configure fictitious leaves on an empty tree");
    if (config.rootReserve >= Capacity || config.splitReserve >= Capacity) throw std::invalid_argument("invalid fictitious leaf reserve");
    if (config.pressureMergedLimit && (config.pressureMergedLimit < 2 || config.pressureMergedLimit > Capacity))
        throw std::invalid_argument("invalid pressure fusion limit");
    if (config.startupOnly && (!config.enabled || config.realHistoryStart <= 0))
        throw std::invalid_argument("startup reserve requires an enabled policy and positive real-history boundary");
    fictitious_ = config;
    fictitiousReplenishmentStopped_ = false;
    fictitiousStopFloor_ = 0;
    observeFictitiousFloor();
    fictitiousChoice_ = config.choiceSeed;
    pairIndex_ = {};
    pairDirty_.clear();
    pairIndexSize_ = 0;
}

void PagedBTreeConflictSet::observeFictitiousFloor() {
    if (fictitious_.enabled && fictitious_.startupOnly && !fictitiousReplenishmentStopped_ &&
        validationMinRV_ >= fictitious_.realHistoryStart) {
        fictitiousReplenishmentStopped_ = true;
        fictitiousStopFloor_ = validationMinRV_;
    }
}

void PagedBTreeConflictSet::setFictitious(Node* node, unsigned count) {
    ASSERT((node->leaf || !count) && count <= Capacity);
    node->fict.fictitious = static_cast<uint8_t>(count);
    markPair(node);
}

unsigned PagedBTreeConflictSet::pairPriority(const Node* left) const {
    const Node* right = left->navigation.leaf.next;
    if (!left->allocated || !left->leaf || !right || !left->parent || right->parent != left->parent) return 0;
    const auto plan = by5v5::planFusion<Capacity>(left->size(), left->fict.fictitious, right->size(), right->fict.fictitious, true,
                                                  pressureMergedLimit(), pressureSourceLimit());
    if (!plan.possible) return 0;
    return 1u + (fictitious_.prioritizeReclaimable ? 2u * Capacity - left->size() - right->size()
                                                : left->fict.fictitious + right->fict.fictitious);
}

void PagedBTreeConflictSet::setPair(Node* left, unsigned priority) {
    const unsigned old = left->fict.pair.priority;
    if (old == priority) return;
    pairIndex_.set(left->ageIndex, priority, [this](uint32_t id) -> by5v5::PairLink& {
        return static_cast<Node*>(pool_.nodeForAgeIndex(id))->fict.pair;
    });
    pairIndexSize_ = pairIndexSize_ + (priority != 0) - (old != 0);
    ++stats_.pairIndexUpdates;
}

void PagedBTreeConflictSet::flushPairs() {
    if (pairDirty_.empty()) return;
    ++stats_.pairIndexFlushes;
    stats_.pairIndexDirtyLeaves += pairDirty_.size();
    stats_.pairIndexDirtyMax = std::max<uint64_t>(stats_.pairIndexDirtyMax, pairDirty_.size());
    for (Node* node : pairDirty_) {
        if (!node->fict.pairDirty) continue;
        node->fict.pairDirty = false;
        if (!node->allocated || !node->leaf) continue;
        setPair(node, pairPriority(node));
        if (Node* previous = node->navigation.leaf.previous) setPair(previous, pairPriority(previous));
    }
    pairDirty_.clear();
}

bool PagedBTreeConflictSet::checkPairIndex() {
    flushPairs();
    uint64_t expected = 0, listed = 0;
    unsigned highest = 0;
    for (const Node* x = firstLeaf_; x; x = x->navigation.leaf.next) {
        const unsigned priority = pairPriority(x);
        if (x->fict.pair.priority != priority || x->fict.pairDirty) return false;
        expected += priority != 0;
        highest = std::max(highest, priority);
    }
    for (unsigned n = 1; n <= 2 * Capacity + 1; ++n) {
        uint32_t previous = by5v5::absent;
        for (uint32_t id = pairIndex_.head(n); id != by5v5::absent;) {
            const Node* x = static_cast<const Node*>(pool_.nodeForAgeIndex(id));
            if (!x || !x->allocated || !x->leaf || x->fict.pair.priority != n || x->fict.pair.previous != previous || ++listed > expected)
                return false;
            previous = id;
            id = x->fict.pair.next;
        }
    }
    const uint32_t candidate = pairIndex_.candidate();
    if (candidate == by5v5::absent) return !highest && !listed && !pairIndexSize_;
    const Node* x = static_cast<const Node*>(pool_.nodeForAgeIndex(candidate));
    return listed == expected && listed == pairIndexSize_ && x->fict.pair.priority == highest;
}

bool PagedBTreeConflictSet::tryPressurePair(Node* left, Node* right, bool global) {
    if (!left || !right || !left->leaf || !right->leaf || left->navigation.leaf.next != right) return false;
    if (left->parent != right->parent || !left->parent) {
        ++stats_.pressureSkippedNotSameParent;
        return false;
    }
    const auto plan = by5v5::planFusion<Capacity>(left->size(), left->fict.fictitious, right->size(), right->fict.fictitious, true,
                                                  pressureMergedLimit(), pressureSourceLimit());
    if (!plan.possible) {
        ++stats_.pressureSkippedLimits;
        return false;
    }
    const uint64_t availableBefore = freeNodes_ + retiredCount_;
    const unsigned removed = plan.left + plan.right;
    if (plan.left) setFictitious(left, left->fict.fictitious - plan.left);
    if (plan.right) setFictitious(right, right->fict.fictitious - plan.right);
    stats_.fictitiousFusionConsumed += removed;
    refreshUp(left);
    refreshUp(right);
    fuseLeafNeighbour(left, false, pressureMergedLimit());
    const bool available = freeNodes_ + retiredCount_ > availableBefore;
    if (available) ++stats_.pressureNodesMadeAvailable;
    else ++stats_.pressureFusionNotApplied;
    if (global) ++stats_.pressureGlobalFusions;
    else ++stats_.pressureLocalFusions;
    if (pressureObserver) pressureObserver(global, 0, plan.left, plan.right, available);
    return true;
}

bool PagedBTreeConflictSet::promotionSafe(const Node* node) const {
    for (const Node* parent = node->parent; parent; parent = parent->parent) {
        const unsigned slack = Capacity - parent->size(), critical = parent->criticalChildren + 1u;
        if (slack != critical) return slack > critical;
    }
    return true;
}

bool PagedBTreeConflictSet::redistribute(InsertCursor& cursor, const Entry& interval) {
    Node* source = cursor.leaf;
    const std::array<Node*, 2> nodes{ source->navigation.leaf.previous, source->navigation.leaf.next };
    std::array<by5v5::Neighbor, 2> sides{};
    for (unsigned side = 0; side < 2; ++side) {
        const Node* x = nodes[side];
        if (!x) continue;
        const unsigned effective = x->size() + x->fict.fictitious;
        sides[side] = { true, x->size(), x->fict.fictitious, effective + 1u != Capacity || promotionSafe(x) };
    }
    const auto plan = by5v5::planInsert<Capacity>(source->size(), source->fict.fictitious, cursor.rank, sides[0], sides[1]);
    const bool left = plan.action == by5v5::InsertAction::MoveLeft;
    const unsigned rejected = left ? 0 : plan.action == by5v5::InsertAction::MoveRight ? 1 : 2;
    for (unsigned side = 0; side < rejected; ++side) {
        if (!sides[side].exists) ++stats_.redistNoNeighbour;
        else if (!sides[side].structuralSafe) ++stats_.redistRejectedSlack;
        else ++stats_.redistRejectedFull;
    }
    if (rejected == 2) return false;
    ASSERT(plan.action == (left ? by5v5::InsertAction::MoveLeft : by5v5::InsertAction::MoveRight));
    Node* target = nodes[!left];
    if (plan.consume) {
        setFictitious(target, target->fict.fictitious - 1u);
        ++stats_.fictitiousReleased;
    }
    if (plan.incomingGoesToNeighbor) {
        insertSlot(target, plan.incomingRank, interval);
        ++stats_.redistIncomingMoved;
    } else {
        const unsigned boundary = left ? 0 : source->size() - 1;
        const Entry moved = source->at(boundary);
        eraseSlot(source, boundary);
        insertSlot(target, left ? target->size() : 0, moved);
        insertSlot(source, plan.incomingRank, interval);
    }
    refreshUp(target);
    if (!plan.incomingGoesToNeighbor) refreshUp(source);
    ++(left ? (plan.consume ? stats_.redistLeftFictitious : stats_.redistLeftFree)
            : (plan.consume ? stats_.redistRightFictitious : stats_.redistRightFree));
    if (target->parent != source->parent) ++stats_.redistCrossParent;
    cursor = { plan.incomingGoesToNeighbor ? target : source, plan.incomingRank + 1u };
    return true;
}

bool PagedBTreeConflictSet::pressureFusion(Node* target) {
    ++stats_.pressureEvents;
    if (target && target->leaf && target->allocated) {
        ++stats_.pressureLocalAttempts;
        if (tryPressurePair(target->navigation.leaf.previous, target, false)) return true;
        if (target->allocated && tryPressurePair(target, target->navigation.leaf.next, false)) return true;
    }
    ++stats_.pressureGlobalAttempts;
    flushPairs();
    uint64_t inspected = 0;
    bool found = false;
    if (pairIndex_.candidate() == by5v5::absent) ++stats_.pairIndexEmpty;
    for (uint32_t id = pairIndex_.candidate(); id != by5v5::absent && !found; id = pairIndex_.candidate()) {
        ++inspected;
        Node* left = static_cast<Node*>(pool_.nodeForAgeIndex(id));
        if (tryPressurePair(left, left->navigation.leaf.next, true)) {
            found = true;
            break;
        }
        ++stats_.pairIndexStale;
        const unsigned priority = left->fict.pair.priority;
        setPair(left, pairPriority(left));
        if (left->fict.pair.priority == priority) {
            ++stats_.pairIndexBlocked;
            break;
        }
    }
    stats_.pressureCandidatesInspected += inspected;
    stats_.pressureCandidatesMax = std::max(stats_.pressureCandidatesMax, inspected);
    if (!found) ++stats_.pressureFallbacks;
    if (pressureObserver) pressureObserver(true, inspected, 0, 0, found);
    return found;
}

void PagedBTreeConflictSet::leafOccupancy(std::array<uint64_t, Capacity + 1>& real, std::array<uint64_t, Capacity + 1>& effective,
                                          uint64_t& fictitiousPresent) const {
    real.fill(0);
    effective.fill(0);
    fictitiousPresent = 0;
    for (const Node* x = firstLeaf_; x; x = x->navigation.leaf.next) {
        ++real[x->size()];
        ++effective[x->size() + x->fict.fictitious];
        fictitiousPresent += x->fict.fictitious;
    }
}

void PagedBTreeConflictSet::configureProbFusion(const ProbFusionConfig& config) {
    if (writeBatchActive_) throw std::logic_error("configure probFusion outside a write batch");
    if (config.maxMergedEntries < 2 || config.maxMergedEntries > Capacity)
        throw std::invalid_argument("invalid probFusion merge limit");
    if (!config.maxSourceEntries || config.maxSourceEntries > Capacity)
        throw std::invalid_argument("invalid probFusion source limit");
    probFusion_ = config;
    for (Node* x = firstLeaf_; x; x = x->navigation.leaf.next) markPair(x);
    flushPairs();
}

void PagedBTreeConflictSet::maybeProbFusion(const FusionCandidate& candidate) {
    Node* node = candidate.node;
    // Index-page ownership keeps storage readable. Never accept a saturated generation:
    // allocation/reclamation during insertAt can recycle this address, even in one batch.
    if (!node || candidate.incarnation == SaturatedIncarnation || !node->allocated ||
        node->incarnation != candidate.incarnation || !node->leaf || !node->parent) return;
    const unsigned n = node->size();
    if (!n || n > probFusion_.maxSourceEntries || n >= candidate.before || n >= probFusion_.maxMergedEntries) return;
    const uint32_t probability = ProbFusionAttemptPer65536[n];
    if (!probability) return;
    // One FDB draw supplies an independent 16-bit acceptance ticket and direction bit.
    const unsigned ticket = deterministicRandom()->randomInt(0, 131072);
    if ((ticket & 65535) >= probability) return;
    ++stats_.mergeCandidatesExamined;
    fuseLeafNeighbour(node, (ticket & 65536) != 0);
}

bool PagedBTreeConflictSet::fuseLeafNeighbour(Node* node, bool previous, unsigned mergedLimit) {
    if (!mergedLimit) mergedLimit = probFusion_.maxMergedEntries;
    ASSERT(mergedLimit >= 2 && mergedLimit <= Capacity);
    ASSERT(node->allocated && node->leaf && node->size());
    Node* neighbour = previous ? node->navigation.leaf.previous : node->navigation.leaf.next;
    // Exactly one neighbour. The first experiment excludes cross-parent fusions.
    if (!neighbour || !node->parent || neighbour->parent != node->parent || !neighbour->size()) return false;
    ASSERT(neighbour->allocated && neighbour->leaf);
    if constexpr (PAGED_BTREE_PURGE_FUSION_NEIGHBOUR) {
        if (purgeHead(neighbour)) {
            if (!neighbour->size()) {
                removeEmpty(neighbour);
                return false;
            }
            refreshUp(neighbour);
        }
    }
    if (node->size() + node->fict.fictitious + neighbour->size() + neighbour->fict.fictitious > mergedLimit) return false;
    Node* left = previous ? neighbour : node;
    Node* right = previous ? node : neighbour;
    Node* parent = left->parent;
    ASSERT(left->navigation.leaf.next == right && right->navigation.leaf.previous == left);
    // Retain the newer age bound and its list position before retiring the other leaf.
    // Moving expired entries is safe: this copies references without reading heap bytes.
    if (right->maxCV > left->maxCV) pool_.inheritAge(left, right, PageUse::Index);
    while (right->size()) {
        const Entry entry = right->at(0);
        eraseSlot(right, 0);
        insertSlot(left, left->size(), entry);
    }
    const unsigned mergedFictitious = left->fict.fictitious + right->fict.fictitious;
    setFictitious(right, 0);
    setFictitious(left, mergedFictitious);
    left->navigation.leaf.next = right->navigation.leaf.next;
    if (right->navigation.leaf.next) right->navigation.leaf.next->navigation.leaf.previous = left;
    --leafCount_;
    eraseSlot(parent, parent->order.rankOfSlot(right->parentSlot));
    releaseNode(right);
    refresh(left);
    refreshUp(parent);
    collapseRoot();
    ++stats_.merges;
    return true;
}

void PagedBTreeConflictSet::overwrite(const Entry& incoming, Reservation& reservation, const LeafHint* hint,
                                      const Write* write) {
    discardExpiredRoot();
    KeyScratch beginScratch, endScratch;
    // Input keys remain valid throughout the batch and match the prepared record.
    // Compare their begin/end directly; obtain a point's implicit end from the record.
    const StringRef begin = write ? write->begin : storedKey(incoming.begin, incoming.beginInline, beginScratch);
    const StringRef end = write && write->kind == ConflictKind::Range
        ? write->end : storedKey(incoming.end, endPrefix(incoming), endScratch);
    if (!root_) {
        insertAt({}, incoming, reservation);
        if (incoming.cv <= validationMinRV_) ++stats_.discardedWrites;
        return;
    }
    Node* anchor = root_->leaf ? root_ : (hint ? resolveWriteHint(begin, *hint) : findLeaf(begin));
    const uint32_t anchorIncarnation = anchor->incarnation;
    FusionCandidate fusionAnchor{anchor, anchor->incarnation, anchor->size()};
    FusionCandidate fusionEnd;
    InsertCursor cursor{anchor, 0};
    Entry splitRight;

    // Keep the insertion leaf attached even if this write temporarily empties it. Its expired entries
    // go before an insertion splits around them: by purgeHead, or with arm 3 by the rank sweep below.
    bool anchorPurged = false;
    if constexpr (PAGED_BTREE_BITMAP_CLEANUP != 3) anchorPurged = purgeHead(anchor);
    // Unlike background head reclamation, the write binary search must not
    // compare an expired key whose heap page may already have been recycled.
    for (unsigned rank = 0; rank < anchor->size();) {
        if (anchor->at(rank).cv <= validationMinRV_) {
            eraseSlot(anchor, rank);
            ++stats_.expiredIntervals;
            anchorPurged = true;
        } else ++rank;
    }

    // Disjoint intervals preceding the last begin <= write begin cannot overlap the write.
    unsigned start = 0;
    {
        unsigned lo = 0, hi = anchor->size();
        while (lo < hi) {
            const unsigned mid = lo + (hi - lo) / 2;
            BTREE_SEARCH_STAT(++stats_.leafSearchProbes);
            if (compareBegin(anchor->at(mid), begin) <= 0) lo = mid + 1;
            else hi = mid;
        }
        start = lo ? lo - 1 : 0;
    }

    bool done = false;
    for (Node* leaf = anchor; leaf && !done;) {
        Node* next = leaf->navigation.leaf.next;
        const unsigned beforeFusion = leaf->size();
        bool metadataChanged = leaf == anchor && anchorPurged;
        BTREE_SEARCH_STAT(++stats_.overwriteLeaves);
        unsigned rank = leaf == anchor ? start : 0;
        while (rank < leaf->size()) {
            Entry& entry = leaf->at(rank);
            const Entry old = entry;
            BTREE_SEARCH_STAT(++stats_.overwriteEntries);
            if (old.cv <= validationMinRV_) {
                eraseSlot(leaf, rank);
                ++stats_.expiredIntervals;
                metadataChanged = true;
                continue;
            }
            if (compareBegin(old, end) >= 0) { done = true; break; }
            if (compareEnd(old, begin) <= 0) { ++rank; continue; }
            ++stats_.overwrittenIntervals;
            if (compareBegin(old, begin) < 0) {
                // The left survivor retains its slot, CV and begin reference
                setEnd(entry, incoming.begin, incoming.beginInline);
                ++stats_.leftFragmentsTrimmed;
                ++rank;
                if (compareEnd(old, end) > 0) {
                    // One old interval surrounds the write, so only its right half needs a slot
                    splitRight = old;
                    setBegin(splitRight, incoming.end, endPrefix(incoming));
                    done = true;
                    break;
                }
            } else if (compareEnd(old, end) > 0) {
                // Increasing begin within this interval preserves its position among neighbours
                setBegin(entry, incoming.end, endPrefix(incoming));
                ++stats_.rightFragmentsTrimmed;
                metadataChanged = true;
                done = true;
                break;
            } else {
                eraseSlot(leaf, rank);
                metadataChanged = true;
            }
        }
        // Capture deletion-driven shrinkage before insertion can split the anchor.
        // Splits alone must not trigger fusion and immediately undo themselves.
        if (leaf == anchor && leaf->size() >= fusionAnchor.before) fusionAnchor.node = nullptr;
        // For a contiguous overwrite, all fully covered non-anchor leaves disappear.
        // At most one non-anchor leaf survives with fewer entries: the end boundary.
        if (leaf != anchor && leaf->size() && leaf->size() < beforeFusion) {
            ASSERT(!fusionEnd.node);
            fusionEnd = {leaf, leaf->incarnation, beforeFusion};
        }
        if (leaf == anchor) cursor.rank = rank;
        if (leaf != anchor && !leaf->size()) removeEmpty(leaf);
        else if (metadataChanged) refreshUp(leaf);
        leaf = next;
    }
    // Allocation may advance the floor. Relocate the destination and revalidate the copied right fragment.
    cursor = insertAt(cursor, incoming, reservation);
    if (incoming.cv <= validationMinRV_) ++stats_.discardedWrites;
    if (splitRight.cv > validationMinRV_) {
        insertAt(cursor, splitRight, reservation);
        ++stats_.splitFragmentsInserted;
    }
    closeEmptiedAnchor(anchor, anchorIncarnation);
    // No insertion cursor is used after this point. Revalidate both identities after
    // reservation/reclamation and again after the first merge may retire the second leaf.
    maybeProbFusion(fusionAnchor);
    maybeProbFusion(fusionEnd);
}

bool PagedBTreeConflictSet::attachedLeaf(const Node* leaf) const {
    if (!leaf->parent) return root_ == leaf;
    const Node* parent = leaf->parent;
    if (!parent->allocated || parent->leaf) return false;
    bool child = false;
    for (unsigned rank = 0; rank < parent->size() && !child; ++rank) child = parent->at(rank).payload.link.child == leaf;
    if (!child) return false;
    const Node* previous = leaf->navigation.leaf.previous;
    const Node* next = leaf->navigation.leaf.next;
    return (previous ? previous->navigation.leaf.next == leaf : firstLeaf_ == leaf) &&
           (!next || next->navigation.leaf.previous == leaf);
}

void PagedBTreeConflictSet::closeEmptiedAnchor(Node* anchor, uint32_t incarnation) {
    if (!anchor->allocated || !anchor->leaf || anchor->size() || anchor->incarnation != incarnation) return;
    if (incarnation == SaturatedIncarnation && !attachedLeaf(anchor)) return;
    ++stats_.emptiedAnchorsRemoved;
    removeEmpty(anchor);
}

PagedBTreeConflictSet::Entry PagedBTreeConflictSet::prepareWrite(
        Version cv, StringRef begin, StringRef end, bool point) {
    ASSERT(point || begin < end);
    // Recognize [key, key + zero) as a point.
    point = point || (end.size() == begin.size() + 1 && end.begin()[begin.size()] == 0 &&
                      (!begin.size() || std::memcmp(begin.begin(), end.begin(), begin.size()) == 0));
    BTREE_SEARCH_STAT(++stats_.writes);
    if constexpr (InlineOnlyKeys) {
        Entry incoming;
        incoming.cv = cv;
        cacheKey(incoming.beginInline, begin);
        if (point) {
            if (size_t(begin.size()) <= InlineKeyBytes) incoming.begin = inlineReference(begin.size());
            else {
                const auto record = heap_.appendPointWithSuccessor(cv, begin);
                if (!record.present()) throw std::bad_alloc();
                incoming.begin = {record.get().pageID, record.get().offset + uint32_t(sizeof(PointConflictHeader)), uint32_t(begin.size())};
            }
            incoming.end = incoming.begin; ++incoming.end.length;
        } else {
            KeyPrefix endBytes; cacheKey(endBytes, end);
            const bool shortBegin = size_t(begin.size()) <= InlineKeyBytes;
            const bool shortEnd = size_t(end.size()) <= InlineKeyBytes;
            HeapRef endRef{};
            if (!shortBegin && !shortEnd) {
                const auto record = heap_.appendRange(cv, begin, end);
                if (!record.present()) throw std::bad_alloc();
                incoming.begin = {record.get().pageID, record.get().offset + uint32_t(sizeof(RangeConflictHeader)), uint32_t(begin.size())};
                endRef = {record.get().pageID, incoming.begin.offset + incoming.begin.length, uint32_t(end.size())};
            } else {
                if (shortBegin) incoming.begin = inlineReference(begin.size());
                if (shortEnd) endRef = inlineReference(end.size());
                if (!shortBegin || !shortEnd) {
                    const StringRef longKey = shortBegin ? end : begin;
                    const auto record = heap_.appendPoint(cv, longKey);
                    if (!record.present()) throw std::bad_alloc();
                    const HeapRef ref{record.get().pageID, record.get().offset + uint32_t(sizeof(PointConflictHeader)), uint32_t(longKey.size())};
                    if (shortBegin) endRef = ref; else incoming.begin = ref;
                }
            }
            setEnd(incoming, endRef, endBytes);
        }
        return incoming;
    }
    const auto record = point ? heap_.appendPointWithSuccessor(cv, begin) : heap_.appendRange(cv, begin, end);
    if (!record.present()) throw std::bad_alloc();
    Entry incoming;
    incoming.cv = cv;
    incoming.begin = {record.get().pageID,
                      record.get().offset + static_cast<uint32_t>(point ? sizeof(PointConflictHeader) : sizeof(RangeConflictHeader)),
                      static_cast<uint32_t>(begin.size())};
    cacheKey(incoming.beginInline, begin);
    if (point) {
        incoming.end = incoming.begin;
        ++incoming.end.length;
        // Zero-initialized inline storage also holds a short point key's successor byte.
    } else {
        KeyPrefix endBytes;
        cacheKey(endBytes, end);
        setEnd(incoming, {record.get().pageID, incoming.begin.offset + incoming.begin.length,
                         static_cast<uint32_t>(end.size())}, endBytes);
    }
    return incoming;
}

void PagedBTreeConflictSet::addRange(Version cv, StringRef begin, StringRef end) {
    const Write write{begin, end, ConflictKind::Range};
    addWrites(cv, &write, 1);
}

void PagedBTreeConflictSet::addPoint(Version cv, StringRef point) {
    const Write write{point, StringRef(), ConflictKind::Point};
    addWrites(cv, &write, 1);
}

void PagedBTreeConflictSet::addWrites(Version cv, const Write* writes, size_t count) {
    // Validate the entire batch before mutation. A zero-heap configuration rejects
    // keys outside the inline domain even when index storage is available.
    if (pool_.budget().heapCapacity == 0 && pool_.budget().unusedPages == 0) {
        for (size_t i = 0; i < count; ++i)
            if (size_t(writes[i].begin.size()) > InlineKeyBytes ||
                (writes[i].kind == ConflictKind::Range && size_t(writes[i].end.size()) > InlineKeyBytes))
                throw std::invalid_argument("key exceeds zero-heap inline-only contract");
    }
    if (!count) return;
    ASSERT(cv >= lastWriteCV_);
    lastWriteCV_ = cv;
    if (cv <= validationMinRV_) { stats_.discardedWrites += count; return; }
    if (count > 1) {
        writeOrder_.reserve(count);
        writeHints_.resize(count);
        writePending_.reserve(count);
    }
    // Reservations are sized at each actual insertion, never topped up speculatively.
    // The scope returns unpublished nodes on every exit and drains retired nodes.
    Reservation reservation;
    WriteBatchScope batch(*this);
    batch.reservation = &reservation;
    bool searched = false;
    for (size_t i = 0; i < count; ++i) {
        if (cv <= validationMinRV_) { stats_.discardedWrites += count - i; break; }
        const Write& write = writes[i];
        const Entry incoming = prepareWrite(cv, write.begin, write.end, write.kind == ConflictKind::Point);
        // No prepared endpoint may be read before checking the floor after heap allocation
        if (cv <= validationMinRV_) {
            stats_.discardedWrites += count - i;
            break;
        }
        // One grouped search per batch: hints survive reclamation and are validated where used
        if (count > 1 && !searched) {
            findWriteLeaves(writes, i, count);
            searched = true;
        }
        overwrite(incoming, reservation, count > 1 ? &writeHints_[i] : nullptr, &write);
    }
}

void PagedBTreeConflictSet::prefetchWriteNode(Node* node) {
    // The permutation can place a separator in any physical slot of the node
    if constexpr (PAGED_BTREE_WRITE_PREFETCH) {
        for (size_t offset = 0; offset < sizeof(Node); offset += 64)
            __builtin_prefetch(reinterpret_cast<const char*>(node) + offset, 0, 1);
    }
}

void PagedBTreeConflictSet::findWriteLeaves(const Write* writes, size_t first, size_t count) {
    BTREE_SEARCH_STAT(++stats_.writeSearchPasses);
    writeOrder_.clear();
    writePending_.clear();
    for (size_t i = first; i < count; ++i) {
        writeOrder_.push_back(i);
        writeHints_[i] = LeafHint{};
    }
    if (!root_ || root_->maxCV <= validationMinRV_) return;
    auto less = [&](size_t a, size_t b) { return writes[a].begin < writes[b].begin; };
    // Sort search indices only; apply writes in input order.
    if (!std::is_sorted(writeOrder_.begin(), writeOrder_.end(), less))
        std::sort(writeOrder_.begin(), writeOrder_.end(), less);
    writePending_.push_back({root_, 0, writeOrder_.size()});
    if constexpr (NodePrefixEnabled) {
        // Reuse storage across batches; tags must be reset after intervening writes.
        writePrefixMatches_.clear();
        writePrefixMatches_.resize(count - first);
    }
    std::array<WriteSearchWork, WriteSearchWidth> active{};
    // Cache heap addresses by active slot and separator rank for one node visit.
    // Initialize each row before use; resolve missing addresses once.
    // No writes, allocation or reclamation may occur while cached addresses are live.
    // Scratch space is WriteSearchWidth * Capacity pointers.
    std::array<std::array<const uint8_t*, Capacity>, WriteSearchWidth> separatorKeys{};
    unsigned activeCount = 0;
    while (activeCount || !writePending_.empty()) {
        for (WriteSearchWork& work : active) {
            auto& keys = separatorKeys[&work - active.data()];
            if (!work.node) {
                if (writePending_.empty()) continue;
                work = writePending_.back();
                writePending_.pop_back();
                prefetchWriteNode(work.node);
                ++activeCount;
                BTREE_SEARCH_STAT(stats_.writeSearchMaxActive =
                    std::max<uint64_t>(stats_.writeSearchMaxActive, activeCount));
                // A newly admitted node is first read on the next sweep
                continue;
            }
            Node* node = work.node;
            if (work.prepare) {
                BTREE_SEARCH_STAT(++stats_.writeSearchNodes);
                if (node->leaf) {
                    BTREE_SEARCH_STAT(++stats_.writeSearchLeaves);
                    const LeafHint hint{node, node->incarnation};
                    for (size_t i = work.first; i < work.end; ++i)
                        writeHints_[writeOrder_[i]] = hint;
                    work.node = nullptr;
                    --activeCount;
                    continue;
                }
                for (unsigned rank = 0; rank < node->size(); ++rank) {
                    keys[rank] = nullptr;
                    Entry& entry = node->at(rank);
                    if (entry.cv <= validationMinRV_) continue;
                    if (!refreshSeparator(entry)) continue;
                    // Kept after the refresh, which may have changed the separator's key reference
                    if constexpr (PAGED_BTREE_WRITE_PREFETCH)
                        if (entry.begin.length > separatorInlineBytes(entry) && !isInlineOnly(entry.begin)) {
                            const void* address = heap_.recordAddress(entry.begin);
                            __builtin_prefetch(address, 0, 1);
                            keys[rank] = static_cast<const uint8_t*>(address);
                        }
                }
                work.prepare = false;
                // Separate node and separator loads by another sweep over independent work
                continue;
            }
            std::array<unsigned, Capacity> ranks;
            const unsigned live = liveRanks(node, ranks);
            std::array<WriteSearchWork, Capacity> children{};
            unsigned childCount = 0;
            size_t begin = work.first;
            for (unsigned i = 0; i < live && begin < work.end; ++i) {
                size_t end = work.end;
                if (i + 1 < live) {
                    const Entry* separator = &node->at(ranks[i + 1]);
                    const uint8_t** cachedKey = &keys[ranks[i + 1]];
                    const auto boundary = std::lower_bound(
                        writeOrder_.begin() + begin, writeOrder_.begin() + work.end, separator,
                        [&](size_t index, const Entry* limit) {
                            PrefixMatch* prefix = nullptr;
                            if constexpr (NodePrefixEnabled) {
                                auto& cached = writePrefixMatches_[index - first];
                                if (cached.node != node) { cached.node = node; cached.match = {}; }
                                prefix = &cached.match;
                            }
                            return compareSeparator(node, *limit, writes[index].begin, cachedKey, prefix) > 0;
                        });
                    end = boundary - writeOrder_.begin();
                }
                if (begin < end) children[childCount++] = {node->at(ranks[i]).payload.link.child, begin, end};
                begin = end;
            }
            // LIFO scheduling visits left descendants first. Disjoint nonempty subsets bound
            // pending plus active descriptors by the write count.
            work.node = nullptr;
            --activeCount;
            while (childCount) writePending_.push_back(children[--childCount]);
        }
    }
}

PagedBTreeConflictSet::Node* PagedBTreeConflictSet::resolveWriteHint(StringRef begin, const LeafHint& hint) {
    if (root_->leaf) return root_;
    Node* leaf = hint.node;
    // Permanent index-page ownership makes incarnation reads safe after recycling.
    // Reject changed or saturated incarnations, then validate leaf membership and key range.
    if (leaf && (hint.incarnation != leaf->incarnation || hint.incarnation == SaturatedIncarnation)) {
        BTREE_SEARCH_STAT(++stats_.writeHintStale);
        leaf = nullptr;
    }
    // A retired node is not allocated and must never be used as a destination
    if (leaf && leaf->allocated && leaf->leaf) {
        // Bound hint correction after splits or overwrites. A conservative maxCV does not
        // prove a leaf nonempty; compute its live minimum once while the tree is unchanged.
        Entry nextMinimum;
        auto empty = [&](Node* node) {
            if (node->maxCV <= validationMinRV_) return true;
            nextMinimum = minimum(node);
            return nextMinimum.cv == NoVersion;
        };
        for (unsigned correction = 0; leaf && correction <= 2; ++correction) {
            if (leaf->maxCV <= validationMinRV_) break;
            const Entry leafMinimum = minimum(leaf);
            if (leafMinimum.cv == NoVersion) break;
            if (compareBegin(leafMinimum, begin) > 0 && leaf != firstLeaf_) break;
            Node* next = leaf->navigation.leaf.next;
            unsigned skipped = 0;
            // Check the second skipped node for emptiness as well.
            bool nextEmpty = next && empty(next);
            while (next && nextEmpty && skipped < 2) {
                next = next->navigation.leaf.next;
                ++skipped;
                nextEmpty = next && empty(next);
            }
            if (next && nextEmpty) break;
            if (!next || compareBegin(nextMinimum, begin) > 0) {
                BTREE_SEARCH_STAT(++stats_.writeHintHits);
                return leaf;
            }
            leaf = next;
            BTREE_SEARCH_STAT(++stats_.writeHintRightMoves);
        }
    }
    BTREE_SEARCH_STAT(++stats_.writeHintRestarts);
    return findLeaf(begin);
}

void PagedBTreeConflictSet::hasConflictsOrdered(Query* queries, size_t count,
                                                const OrderedEndpoint* endpoints, size_t endpointCount) {
    if (count > MaxBlockQueries) {
        hasConflicts(queries, count);
        return;
    }
    readBlocks(queries, count, readerConfig(), endpoints, endpointCount, true);
}

void PagedBTreeConflictSet::hasConflicts(Query* queries, size_t count) {
    hasConflicts(queries, count, readerConfig());
}

PagedBTreeConflictSet::ReadPipelineConfig PagedBTreeConflictSet::readerConfig() const {
    ReadPipelineConfig cfg;
    cfg.sweepGroupSize = readerGroupSize_;
    return cfg;
}

void PagedBTreeConflictSet::configureReader(unsigned sweepGroupSize) {
    if (sweepGroupSize != 2 && sweepGroupSize != 3)
        throw std::invalid_argument("B+ reader group size must be 2 or 3");
    readerGroupSize_ = sweepGroupSize;
}

void PagedBTreeConflictSet::hasConflicts(Query* queries, size_t count,
                                       const ReadPipelineConfig& cfg) {
    const size_t limit = std::numeric_limits<size_t>::max() / 1024;
    if ((cfg.sweepGroupSize != 2 && cfg.sweepGroupSize != 3) || cfg.breadthBoundaryLevels > MaxHeight ||
        !cfg.lineBytes || (cfg.lineBytes & (cfg.lineBytes - 1)) || cfg.lineBytes > 4096 ||
        cfg.l2Bytes > limit || cfg.l2ReserveBytes >= cfg.l2Bytes)
        throw std::invalid_argument("invalid B+ reader configuration");
    if (count <= MaxBlockQueries) {
        readBlocks(queries, count, cfg);
        return;
    }
    for (size_t q = 0; q < count; ++q)
        if (queries[q].readVersion < validationMinRV_) throw transaction_too_old();
    for (size_t first = 0; first < count; first += MaxBlockQueries)
        readBlocks(queries + first, std::min(MaxBlockQueries, count - first), cfg);
}

struct PagedBTreeConflictSet::BlockReader {
    PagedBTreeConflictSet& tree;
    Query* queries;
    const ReadPipelineConfig& cfg;
    const size_t halfBudget, line;
    const bool localMask;
    const OrderedEndpoint* ordered;
    const size_t orderedCount;
    const bool presorted;
    size_t position = 0, depth = 1;
    Version minimumRV = std::numeric_limits<Version>::max();

    BlockReader(PagedBTreeConflictSet& tree, Query* queries, const ReadPipelineConfig& cfg,
                const OrderedEndpoint* ordered = nullptr, size_t orderedCount = 0, bool presorted = false)
      : tree(tree), queries(queries), cfg(cfg),
        halfBudget(std::max<size_t>(1, cfg.l2Bytes > cfg.l2ReserveBytes ?
                                       (cfg.l2Bytes - cfg.l2ReserveBytes) / 2 : 1)),
        line(cfg.lineBytes),
        localMask(cfg.sweepGroupSize == 3), ordered(ordered), orderedCount(orderedCount), presorted(presorted) {}

    // Caches are local to one frame and comparison stream. A proof is reusable
    // only for the same boundary and the same or an adjacent sorted endpoint.
    struct LcpCursor {
        size_t endpoint = std::numeric_limits<size_t>::max();
        unsigned boundary = std::numeric_limits<unsigned>::max();
        size_t matched = 0;
    };

    size_t previousLcp(size_t endpoint) const {
        const auto& e = tree.sortedEndpoints_[endpoint];
        return tree.sortedEndpointLcp_[size_t(e.query) * 2 + unsigned(e.end)];
    }

    size_t reusablePrefix(const LcpCursor& cursor, unsigned boundary, size_t endpoint) const {
        if (cursor.boundary != boundary) return 0;
        if (cursor.endpoint == endpoint) return cursor.matched;
        if (cursor.endpoint + 1 == endpoint) return std::min(cursor.matched, previousLcp(endpoint));
        if (endpoint + 1 == cursor.endpoint) return std::min(cursor.matched, previousLcp(cursor.endpoint));
        return 0; // No range-minimum structure: nonadjacent probes restart safely.
    }

    bool initialize(size_t count) {
        
        for (size_t q = 0; q < count; ++q)
            if (queries[q].readVersion < tree.validationMinRV_) throw transaction_too_old();
        ASSERT(count <= UINT32_MAX / 2);
        BTREE_SEARCH_STAT(tree.stats_.queries += count);
        tree.readPipelineStats_ = {};
        tree.sortedEndpoints_.clear(); tree.openQueries_.clear();
        tree.sortedEndpoints_.reserve(2 * count); tree.openQueries_.reserve(count);
        tree.sortedEndpointLcp_.assign(2 * count, 0);
        tree.breadthCurrent_.clear(); tree.breadthNext_.clear();
        minimumRV = std::numeric_limits<Version>::max();
        size_t expectedEndpoints = 0;
        for (size_t q = 0; q < count; ++q) {
            Query& query = queries[q];
            query.conflict = false; query.endIndex = UINT32_MAX;
            if (!(query.begin < query.end)) continue;
            expectedEndpoints += 2;
            if (!presorted) {
                tree.sortedEndpoints_.push_back({query.begin, static_cast<uint32_t>(q), false});
                tree.sortedEndpoints_.push_back({query.end, static_cast<uint32_t>(q), true});
            }
            minimumRV = std::min(minimumRV, query.readVersion);
        }
        if (presorted) {
            ASSERT(orderedCount == expectedEndpoints && (ordered || !orderedCount));
            for (size_t i = 0; i < orderedCount; ++i) {
                const auto& e = ordered[i];
                ASSERT(e.query < count && queries[e.query].begin < queries[e.query].end);
                const StringRef key = e.end ? queries[e.query].end : queries[e.query].begin;
                ASSERT(e.lcp <= size_t(key.size()));
                ASSERT(i ? e.lcp <= size_t(tree.sortedEndpoints_.back().key.size()) : e.lcp == 0);
                tree.sortedEndpoints_.push_back({key, e.query, e.end});
                tree.sortedEndpointLcp_[size_t(e.query) * 2 + unsigned(e.end)] = e.lcp;
            }
            BTREE_SEARCH_STAT(tree.readPipelineStats_.presortedEndpointCount = orderedCount);
        } else {
        std::sort(tree.sortedEndpoints_.begin(), tree.sortedEndpoints_.end(), [&](const SortedEndpoint& a,
                                                                      const SortedEndpoint& b) {
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.endpointSortComparisons);
            const size_t common = std::min(a.key.size(), b.key.size());
            const auto comparison = block_reader_lcp::compare(a.key.begin(), b.key.begin(), common);
            int order = comparison.order;
            if (!order && a.key.size() != b.key.size()) order = a.key.size() < b.key.size() ? -1 : 1;
            if (!order && a.end != b.end) order = a.end ? -1 : 1;
            if (!order && a.query != b.query) order = a.query < b.query ? -1 : 1;
            if (order) {
                const auto& greater = order < 0 ? b : a;
                auto& saved = tree.sortedEndpointLcp_[size_t(greater.query) * 2 + unsigned(greater.end)];
                // The immediate predecessor maximizes LCP among all smaller keys.
                // A comparison sort compares every adjacent pair in its total order.
                if (comparison.lcp > saved) {
                    saved = comparison.lcp;
                    BTREE_SEARCH_STAT(++tree.readPipelineStats_.endpointLcpUpdates);
                }
            }
            return order < 0;
        });
        }

        for (size_t i = 0; i < tree.sortedEndpoints_.size(); ++i)
            if (tree.sortedEndpoints_[i].end) queries[tree.sortedEndpoints_[i].query].endIndex = static_cast<uint32_t>(i);
        BTREE_SEARCH_STAT(tree.readPipelineStats_.sortedEndpointCount = tree.sortedEndpoints_.size());
        BTREE_SEARCH_STAT(tree.readPipelineStats_.rootQueries = tree.sortedEndpoints_.size() / 2);
        BTREE_SEARCH_STAT(tree.readPipelineStats_.scratchBytes =
            tree.sortedEndpoints_.capacity() * sizeof(SortedEndpoint) +
            tree.sortedEndpointLcp_.capacity() * sizeof(uint32_t) + tree.openQueries_.capacity() * sizeof(uint32_t) +
            (tree.breadthCurrent_.capacity() + tree.breadthNext_.capacity()) * sizeof(BreadthFrame));
        return tree.root_ && !tree.sortedEndpoints_.empty();
    }

    void consume(size_t end, Version recordCV) {
        for (; position < end; ++position) {
            const auto& endpoint = tree.sortedEndpoints_[position];
            if (endpoint.end) continue;
            Query& query = queries[endpoint.query];
            if (query.conflict) continue;
            if (recordCV > query.readVersion) query.conflict = true;
            else if (query.endIndex >= end) tree.openQueries_.push_back(endpoint.query);
        }
        size_t kept = 0;
        for (uint32_t q : tree.openQueries_) {
            if (!queries[q].conflict && queries[q].endIndex >= end) tree.openQueries_[kept++] = q;
        }
        tree.openQueries_.resize(kept);
        BTREE_SEARCH_STAT(tree.readPipelineStats_.maxOpenQueries =
            std::max<uint64_t>(tree.readPipelineStats_.maxOpenQueries, kept));
    }

    void requestLines(const uint8_t* bytes, size_t length, unsigned kind) {
        if (!length) return;
        const uintptr_t first = reinterpret_cast<uintptr_t>(bytes) & ~(uintptr_t(line) - 1);
        const uintptr_t last = (reinterpret_cast<uintptr_t>(bytes) + length - 1) & ~(uintptr_t(line) - 1);
        for (uintptr_t address = first;; address += line) {
            
            __builtin_prefetch(reinterpret_cast<const void*>(address), 0, 2);
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.l2Requests);
            if (kind == 0) { BTREE_SEARCH_STAT(++tree.readPipelineStats_.breadthNodeLines); }
            else {
                BTREE_SEARCH_STAT(++tree.readPipelineStats_.breadthSuffixLines);
                if (kind == 1) { BTREE_SEARCH_STAT(++tree.readPipelineStats_.breadthHeapLines); }
                else { BTREE_SEARCH_STAT(++tree.readPipelineStats_.breadthQueryLines); }
            }
            if (address == last) break;
        }
    }

    size_t addCost(size_t a, size_t b) const {
        return b > std::numeric_limits<size_t>::max() - a ? std::numeric_limits<size_t>::max() : a + b;
    }

    unsigned limit(const BreadthFrame& frame) const {
        return frame.node->size() * (frame.node->leaf ? 2 : 1);
    }

    uint64_t usefulBoundaries(const BreadthFrame& frame) {
        uint64_t mask = 0;
        Node* node = frame.node;
        if (node->maxCV <= frame.minimumBeginRV) return mask;
        for (unsigned r = 0; r < node->size(); ++r) {
            Entry& entry = node->at(r);
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.blockEntriesFiltered);
            if (entry.cv <= tree.validationMinRV_) continue;
            if (node->leaf) {
                if (entry.cv > frame.minimumBeginRV) mask |= uint64_t{3} << (2 * r);
            } else if (tree.refreshSeparator(entry)) mask |= uint64_t{1} << r;
        }
        return mask;
    }

    unsigned nextBoundary(const BreadthFrame& frame, unsigned boundary, uint64_t mask) const {
        const unsigned end = limit(frame);
        if (boundary >= end) return end;
        mask &= ~uint64_t{0} << boundary;
        return mask ? static_cast<unsigned>(__builtin_ctzll(mask)) : end;
    }

    unsigned firstBoundary(const BreadthFrame& frame, uint64_t mask) const {
        unsigned first = nextBoundary(frame, 0, mask);
        return frame.node->leaf || first == limit(frame) ? first : nextBoundary(frame, first + 1, mask);
    }

    bool searchEndpoints(const BreadthFrame& frame) const {
        return !frame.node->leaf && depth <= cfg.breadthBoundaryLevels;
    }

    size_t frameBytes(const BreadthFrame& frame) const {
        size_t bytes = sizeof(Node) + sizeof(BreadthFrame) + sizeof(BlockNodeState);
        bytes = addCost(bytes, 2 * Capacity * sizeof(const uint8_t*));
        bytes = addCost(bytes, sizeof(uint64_t));
        for (size_t e = frame.first; e < frame.end; ++e) {
            bytes = addCost(bytes, sizeof(SortedEndpoint) + sizeof(Query) + sizeof(uint8_t) + sizeof(uint32_t));
            bytes = addCost(bytes, size_t(tree.sortedEndpoints_[e].key.size()) + line - 1);
        }
        for (unsigned r = 0; r < Capacity; ++r)
            bytes = addCost(bytes, 4 * size_t(64));
        return bytes;
    }

    size_t planBlock(size_t first, size_t& bytes) const {
        size_t end = first;
        bytes = 0;
        while (end < tree.breadthCurrent_.size()) {
            size_t cost = frameBytes(tree.breadthCurrent_[end]);
            if (end > first)
                cost = addCost(cost, (tree.breadthCurrent_[end].first -
                                      tree.breadthCurrent_[end - 1].end) * sizeof(uint8_t));
            if (end > first && addCost(bytes, cost) > halfBudget) break;
            bytes = addCost(bytes, cost);
            ++end;
        }
        return end;
    }

    void prefetchInline(size_t first, size_t end) {
        for (size_t f = first; f < end; ++f)
            requestLines(reinterpret_cast<const uint8_t*>(tree.breadthCurrent_[f].node), sizeof(Node), 0);
    }

    void admitPlanned(ReadBlock& block, size_t first, size_t end, size_t bytes) {
        block.addresses.clear();
        block.nodes.assign(end - first, BlockNodeState{});
        block.first = first; block.end = end; block.bytes = bytes;
        block.endpointBase = tree.breadthCurrent_[first].first;
        block.positions.resize(tree.breadthCurrent_[end - 1].end - block.endpointBase);
        BTREE_SEARCH_STAT(++tree.readPipelineStats_.nodeGroups);
        BTREE_SEARCH_STAT(tree.readPipelineStats_.maxL2PlannedBytes =
            std::max<uint64_t>(tree.readPipelineStats_.maxL2PlannedBytes, block.bytes));
        BTREE_SEARCH_STAT(tree.readPipelineStats_.maxActiveNodes =
            std::max<uint64_t>(tree.readPipelineStats_.maxActiveNodes, block.end - block.first));
        if (block.bytes > halfBudget) { BTREE_SEARCH_STAT(++tree.readPipelineStats_.breadthOversizeGroups); }
    }

    void admit(ReadBlock& block, size_t& nextFrame) {
        size_t bytes;
        const size_t end = planBlock(nextFrame, bytes);
        prefetchInline(nextFrame, end);
        admitPlanned(block, nextFrame, end, bytes);
        nextFrame = end;
    }

    // Compare one stored prefix segment at its logical full-key offset.
    int prefixSegment(const uint8_t* bytes, StringRef rhs, size_t offset, size_t length,
                      size_t known, size_t& matched) {
        const size_t skip = std::min(length, known > offset ? known - offset : size_t(0));
        BTREE_SEARCH_STAT(tree.readPipelineStats_.blockLcpInlineBytesSkipped += skip);
        if (skip == length) { matched = offset + length; return 0; }
        const auto result = block_reader_lcp::compare(bytes + skip, rhs.begin() + offset + skip, length - skip);
        matched = offset + skip + result.lcp;
        return result.order;
    }

    template <bool Leaf>
    bool sweepPrefix(Node* node, const Entry& entry, bool end, StringRef rhs, int& order,
                     size_t& matched, size_t known) {
        const HeapRef& ref = Leaf && end ? entry.end : entry.begin;
        const size_t common = std::min<size_t>(ref.length, rhs.size());
        known = std::min(known, common);
        size_t offset = 0;
        matched = 0;
        order = 0;
        if constexpr (!Leaf && NodePrefixEnabled) {
            offset = entry.end.length;
            if (offset) {
                BTREE_SEARCH_STAT(++tree.stats_.nodePrefixChecks);
                ASSERT(!node->leaf && node->navigation.prefix.initialized &&
                       offset == node->navigation.prefix.length && ref.length >= offset);
                const size_t length = std::min(offset, common);
                order = prefixSegment(node->navigation.prefix.bytes.data(), rhs, 0, length, known, matched);
                if (order) return true;
                if (common < offset) { order = 1; return true; }
            }
        }
        const size_t local = std::min(InlineKeyBytes, common - offset);
        const KeyPrefix& bytes = Leaf && end ? tree.endPrefix(entry) : entry.beginInline;
        order = prefixSegment(bytes.data(), rhs, offset, local, known, matched);
        if (order) return true;
        if (common <= matched) {
            order = (ref.length > size_t(rhs.size())) - (ref.length < size_t(rhs.size()));
            return true;
        }
        if (tree.implicitZero(ref)) {
            order = tree.compareImplicitZero(ref, rhs);
            if (!order) matched = common;
            return true;
        }
        return false;
    }

    template <bool Leaf>
    void countComparison(unsigned boundary) {
        BTREE_SEARCH_STAT(++tree.readPipelineStats_.endpointBoundaryComparisons);
        BTREE_SEARCH_STAT(++tree.readPipelineStats_.groupSweepComparisons);
        if constexpr (!Leaf) {
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.sortedInternalComparisons);
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.sortedInternalComparisonsByDepth[depth - 1]);
        } else if (boundary & 1) { BTREE_SEARCH_STAT(++tree.readPipelineStats_.sortedLeafEndComparisons); }
        else { BTREE_SEARCH_STAT(++tree.readPipelineStats_.sortedLeafBeginComparisons); }
    }

    template <bool Leaf>
    bool prefix(const BreadthFrame& frame, unsigned boundary, size_t endpoint, bool& passed, LcpCursor& cursor) {
        Node* node = frame.node;
        const bool end = Leaf && (boundary & 1);
        const Entry& entry = node->at(Leaf ? boundary / 2 : boundary);
        int order = 0;
        size_t matched = 0;
        const size_t known = reusablePrefix(cursor, boundary, endpoint);
        const bool decided = sweepPrefix<Leaf>(node, entry, end, tree.sortedEndpoints_[endpoint].key,
                                              order, matched, known);
        cursor = {endpoint, boundary, matched};
        countComparison<Leaf>(boundary);
        if (decided) { BTREE_SEARCH_STAT(++tree.stats_.inlineKeyDecisions); }
        else { BTREE_SEARCH_STAT(++tree.stats_.keyComparisonFallbacks); }
        passed = Leaf && !end ? order < 0 : order <= 0;
        return decided;
    }

    const HeapRef& reference(const BreadthFrame& frame, unsigned boundary) const {
        const Entry& entry = frame.node->at(frame.node->leaf ? boundary / 2 : boundary);
        return frame.node->leaf && (boundary & 1) ? entry.end : entry.begin;
    }

    void requestHeap(const uint8_t* full, size_t length) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(full) + std::min(InlineKeyBytes, length);
        __builtin_prefetch(reinterpret_cast<const void*>(address), 0, 3);
        __builtin_prefetch(reinterpret_cast<const void*>(address + 64), 0, 3);
        BTREE_SEARCH_STAT(tree.readPipelineStats_.l1Requests += 2);
        BTREE_SEARCH_STAT(tree.readPipelineStats_.breadthHeapLines += 2);
        BTREE_SEARCH_STAT(tree.readPipelineStats_.breadthSuffixLines += 2);
    }

    template <bool Leaf>
    bool fullCompare(ReadBlock& block, const BreadthFrame& frame, BlockNodeState& state,
                     unsigned boundary, size_t endpoint, LcpCursor& cursor) {
        const Entry& entry = frame.node->at(Leaf ? boundary / 2 : boundary);
        const HeapRef& ref = Leaf && (boundary & 1) ? entry.end : entry.begin;
        const StringRef rhs = tree.sortedEndpoints_[endpoint].key;
        auto& full = block.addresses[state.addressOffset + boundary];
        size_t known = reusablePrefix(cursor, boundary, endpoint);
        if (!full) {
            bool passed = false;
            if (prefix<Leaf>(frame, boundary, endpoint, passed, cursor)) return passed;
            known = cursor.matched;
            full = tree.key(ref).begin();
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.breadthHeapResolutions);
        } else {
            if constexpr (InlineKeyBytes > 0) {
                const size_t inlineEnd = InlineKeyBytes + ((!Leaf && NodePrefixEnabled) ? entry.end.length : 0);
                // All endpoints in this frame share these bytes. A cached key
                // tied with one of them, hence with every endpoint over this span.
                known = std::max(known, std::min<size_t>(state.heap >> 32, inlineEnd));
            }
            countComparison<Leaf>(boundary);
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.blockAddressReuses);
        }
        const size_t common = std::min<size_t>(ref.length, rhs.size());
        known = std::min(known, common);
        block_reader_lcp::Result comparison{0, 0};
        if (known < common) comparison = block_reader_lcp::compare(full + known, rhs.begin() + known, common - known);
        int order = comparison.order;
        cursor = {endpoint, boundary, known + comparison.lcp};
        BTREE_SEARCH_STAT(tree.readPipelineStats_.blockLcpFullBytesSkipped += known);
        BTREE_SEARCH_STAT(tree.readPipelineStats_.groupSweepSuffixBytes += common - known);
        if (!order) order = (ref.length > size_t(rhs.size())) - (ref.length < size_t(rhs.size()));
        return Leaf && !(boundary & 1) ? order < 0 : order <= 0;
    }

    template <bool Prepare>
    void searchCuts(ReadBlock& block, const BreadthFrame& frame, BlockNodeState& state) {
        size_t first = frame.first;
        bool write = true;
        const unsigned end = limit(frame);
        for (unsigned boundary = firstBoundary(frame, state.boundaries); boundary < end && first < frame.end;
             boundary = nextBoundary(frame, boundary + 1, state.boundaries)) {
            LcpCursor cursor;
            size_t lo = first, hi = frame.end;
            bool tied = false;
            while (lo < hi) {
                const size_t middle = lo + (hi - lo) / 2;
                bool passed = false;
                if constexpr (Prepare) {
                    if (!prefix<false>(frame, boundary, middle, passed, cursor)) {
                        state.heap |= uint64_t{1} << boundary;
                        tied = true; write = false; break;
                    }
                } else passed = fullCompare<false>(block, frame, state, boundary, middle, cursor);
                if (passed) hi = middle; else lo = middle + 1;
            }
            if (tied) continue;
            if (write) std::fill(block.positions.begin() + (first - block.endpointBase),
                                 block.positions.begin() + (lo - block.endpointBase),
                                 static_cast<uint8_t>(boundary));
            first = lo;
        }
        if (write) std::fill(block.positions.begin() + (first - block.endpointBase),
                             block.positions.begin() + (frame.end - block.endpointBase),
                             static_cast<uint8_t>(end));
    }

    // Called only after preparation has found heap work for this frame.
    // Read existing LCP metadata once; do not compare endpoint keys again.
    uint32_t heapFrameCommonPrefix(const BreadthFrame& frame) const {
        uint32_t common = UINT32_MAX;
        for (size_t e = frame.first + 1; e < frame.end; ++e)
            common = std::min(common, uint32_t(previousLcp(e)));
        return common;
    }

    // One endpoint at a time; the existing state word also locates its cache.
    template <bool Leaf, bool LocalMask>
    void prepareGroup(ReadBlock& block, const BreadthFrame& frame, BlockNodeState& state) {
        ASSERT(!state.heap);
        auto* out = block.positions.data() + (state.group.endpoint - block.endpointBase);
        auto* const stop = block.positions.data() + (frame.end - block.endpointBase);
        const unsigned end = limit(frame);
        const unsigned groupSize = cfg.sweepGroupSize;
        const uint64_t boundaries = state.boundaries;
        LcpCursor cursor;
        unsigned boundary = out != stop ? unsigned(*out & 127u) : end;
        while (out != stop) {
            const size_t endpoint = block.endpointBase + size_t(out - block.positions.data());
            uint64_t pending = 0;
            bool stopped = false;
            unsigned remaining = groupSize;
            while (boundary < end) {
                bool passed = false;
                const bool decided = prefix<Leaf>(frame, boundary, endpoint, passed, cursor);
                if (!decided) pending |= uint64_t{1} << boundary;
                else if (!passed) { stopped = true; break; }
                const unsigned next = boundary + 1;
                const uint64_t later = next < end ? (LocalMask ? boundaries : state.boundaries) & (~uint64_t{0} << next) : 0;
                boundary = later ? static_cast<unsigned>(__builtin_ctzll(later)) : end;
                if (--remaining == 0) {
                    if (pending) break;
                    remaining = groupSize; // No suspension: retain boundary in a local variable.
                }
            }
            if (pending) {
                *out = static_cast<uint8_t>(boundary | ((!stopped && boundary < end) ? 128u : 0));
                state.group.endpoint = static_cast<uint32_t>(endpoint);
                state.heap = pending;
                return;
            }
            *out++ = static_cast<uint8_t>(boundary); // Final cut, once per endpoint.
        }
        state.group.endpoint = static_cast<uint32_t>(frame.end);
    }

    template <bool Leaf>
    void prepareGroupSelected(ReadBlock& block, const BreadthFrame& frame, BlockNodeState& state) {
        if (localMask) prepareGroup<Leaf, true>(block, frame, state);
        else prepareGroup<Leaf, false>(block, frame, state);
    }

    template <bool Leaf>
    void resolveGroupTyped(ReadBlock& block, const BreadthFrame& frame, BlockNodeState& state) {
        if (!state.heap) return;
        if (state.group.cacheFrame == UINT32_MAX) {
            const size_t cacheFrame = block.addresses.size() / (2 * Capacity);
            ASSERT(cacheFrame < UINT32_MAX);
            state.group.cacheFrame = static_cast<uint32_t>(cacheFrame);
            block.addresses.resize(block.addresses.size() + 2 * Capacity, nullptr);
        }
        auto* const addresses = block.addresses.data() + size_t(state.group.cacheFrame) * 2 * Capacity;
        for (uint64_t bits = state.heap; bits; bits &= bits - 1) {
            const unsigned boundary = static_cast<unsigned>(__builtin_ctzll(bits));
            if (addresses[boundary]) continue;
            const Entry& entry = frame.node->at(Leaf ? boundary / 2 : boundary);
            const auto& ref = Leaf && (boundary & 1) ? entry.end : entry.begin;
            addresses[boundary] = tree.key(ref).begin();
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.breadthHeapResolutions);
            requestHeap(addresses[boundary], ref.length);
        }
    }

    void resolveGroup(ReadBlock& block, size_t f, BlockNodeState& state) {
        const auto& frame = tree.breadthCurrent_[f];
        if (frame.node->leaf) resolveGroupTyped<true>(block, frame, state);
        else resolveGroupTyped<false>(block, frame, state);
    }

    template <bool Leaf>
    void finishGroup(ReadBlock& block, size_t f, const BreadthFrame& frame, BlockNodeState& state) {
        ASSERT(state.heap && state.group.endpoint < frame.end);
        const size_t endpoint = state.group.endpoint;
        auto& result = block.positions[endpoint - block.endpointBase];
        const auto rhs = tree.sortedEndpoints_[endpoint].key;
        const auto* const addresses = block.addresses.data() + size_t(state.group.cacheFrame) * 2 * Capacity;
        for (uint64_t bits = state.heap; bits; bits &= bits - 1) {
            const unsigned boundary = static_cast<unsigned>(__builtin_ctzll(bits));
            const Entry& entry = frame.node->at(Leaf ? boundary / 2 : boundary);
            const auto& ref = Leaf && (boundary & 1) ? entry.end : entry.begin;
            // This exact (endpoint,boundary) tied inline in the first pass.
            // No prefix call and no frame-wide proof are necessary here.
            const size_t known = InlineKeyBytes + ((!Leaf && NodePrefixEnabled) ? entry.end.length : 0);
            const size_t common = std::min<size_t>(ref.length, rhs.size());
            ASSERT(addresses[boundary] && known <= common);
            const auto comparison = block_reader_lcp::compare(addresses[boundary] + known,
                                                             rhs.begin() + known, common - known);
            int order = comparison.order;
            if (!order) order = (ref.length > size_t(rhs.size())) - (ref.length < size_t(rhs.size()));
            countComparison<Leaf>(boundary);
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.blockAddressReuses);
            BTREE_SEARCH_STAT(tree.readPipelineStats_.blockLcpFullBytesSkipped += known);
            BTREE_SEARCH_STAT(tree.readPipelineStats_.groupSweepSuffixBytes += common - known);
            const bool passed = Leaf && !(boundary & 1) ? order < 0 : order <= 0;
            if (!passed) { result = static_cast<uint8_t>(boundary); break; }
        }
        state.heap = 0;
        if (!(result & 128u)) {
            ++state.group.endpoint;
            if (state.group.endpoint < frame.end)
                block.positions[state.group.endpoint - block.endpointBase] = static_cast<uint8_t>(result | 128u);
        }
    }

    void resolvePendingGroups(ReadBlock& block) {
        for (size_t word = 0; word < block.pendingFrames.size(); ++word) {
            for (uint64_t bits = block.pendingFrames[word]; bits; bits &= bits - 1) {
                const size_t index = word * 64 + unsigned(__builtin_ctzll(bits));
                resolveGroup(block, block.first + index, block.nodes[index]);
            }
        }
    }

    bool startGroups(ReadBlock& block) {
        block.pendingFrames.clear();
        BTREE_SEARCH_STAT(++tree.readPipelineStats_.groupSweepPasses);
        for (size_t f = block.first; f < block.end; ++f) {
            const auto& frame = tree.breadthCurrent_[f];
            auto& state = block.nodes[f - block.first];
            state.boundaries = usefulBoundaries(frame);
            state.group = {static_cast<uint32_t>(frame.first), UINT32_MAX};
            if (!state.boundaries || frame.first == frame.end) {
                std::fill(block.positions.begin() + (frame.first - block.endpointBase),
                          block.positions.begin() + (frame.end - block.endpointBase), uint8_t(limit(frame)));
                state.group.endpoint = frame.end;
                continue;
            }
            block.positions[frame.first - block.endpointBase] = uint8_t(firstBoundary(frame, state.boundaries) | 128u);
            if (frame.node->leaf) prepareGroupSelected<true>(block, frame, state);
            else prepareGroupSelected<false>(block, frame, state);
            if (state.heap) {
                const size_t index = f - block.first;
                block.pendingFrames.resize(index / 64 + 1, 0);
                block.pendingFrames[index / 64] |= uint64_t{1} << (index % 64);
            }
        }
        resolvePendingGroups(block);
        return block.pendingFrames.empty();
    }

    // Only live frames enter either pass. No scans across completed holes.
    template <bool Leaf>
    void advanceFrame(ReadBlock& block, size_t f, const BreadthFrame& frame, BlockNodeState& state) {
        finishGroup<Leaf>(block, f, frame, state);
        if (state.group.endpoint < frame.end)
            prepareGroupSelected<Leaf>(block, frame, state);
        if (state.heap) resolveGroupTyped<Leaf>(block, frame, state);
    }

    bool advanceGroups(ReadBlock& block) {
        BTREE_SEARCH_STAT(++tree.readPipelineStats_.groupSweepPasses);
        bool pending = false;
        for (size_t word = 0; word < block.pendingFrames.size(); ++word) {
            uint64_t live = block.pendingFrames[word];
            for (uint64_t bits = live; bits; bits &= bits - 1) {
                const size_t index = word * 64 + unsigned(__builtin_ctzll(bits));
                const size_t f = block.first + index;
                const auto& frame = tree.breadthCurrent_[f];
                auto& state = block.nodes[index];
                ASSERT(state.heap && state.group.endpoint < frame.end);
                if (frame.node->leaf) advanceFrame<true>(block, f, frame, state);
                else advanceFrame<false>(block, f, frame, state);
                if (state.group.endpoint == frame.end) live &= ~(uint64_t{1} << (index % 64));
            }
            block.pendingFrames[word] = live;
            pending |= live != 0;
        }
        return !pending;
    }

    void start(ReadBlock& block) {
        BTREE_SEARCH_STAT(++tree.readPipelineStats_.groupSweepPasses);

        for (size_t f = block.first; f < block.end; ++f) {
            const auto& frame = tree.breadthCurrent_[f];
            auto& state = block.nodes[f - block.first];
            state.boundaries = usefulBoundaries(frame);
            if (!state.boundaries || frame.first == frame.end) {
                std::fill(block.positions.begin() + (frame.first - block.endpointBase),
                          block.positions.begin() + (frame.end - block.endpointBase),
                          static_cast<uint8_t>(limit(frame)));
                continue;
            }
            ASSERT(searchEndpoints(frame));
            searchCuts<true>(block, frame, state);
        }
        for (size_t f = block.first; f < block.end; ++f) {
            auto& state = block.nodes[f - block.first];
            if (!state.heap) continue;
            const auto& frame = tree.breadthCurrent_[f];
            uint32_t commonPrefix = 0;
            if constexpr (InlineKeyBytes > 0)
                commonPrefix = heapFrameCommonPrefix(frame);
            state.addressOffset = block.addresses.size();
            block.addresses.resize(state.addressOffset + limit(frame), nullptr);
            for (uint64_t bits = state.heap; bits; bits &= bits - 1) {
                const unsigned boundary = static_cast<unsigned>(__builtin_ctzll(bits));
                const HeapRef& ref = reference(frame, boundary);
                const uint8_t* full = tree.key(ref).begin();
                block.addresses[state.addressOffset + boundary] = full;
                BTREE_SEARCH_STAT(++tree.readPipelineStats_.breadthHeapResolutions);
                requestHeap(full, ref.length);
            }
            // The resolved mask is dead. Retain a frame-wide proof in its word.
            // Every cached address was reached through a tied inline prefix.
            state.heap = (uint64_t(commonPrefix) << 32) | 1;
        }
    }

    void finish(ReadBlock& block) {
        if (block.addresses.empty()) return;
        BTREE_SEARCH_STAT(++tree.readPipelineStats_.groupSweepPasses);
        for (size_t f = block.first; f < block.end; ++f) {
            auto& state = block.nodes[f - block.first];
            if (!state.heap) continue;
            const auto& frame = tree.breadthCurrent_[f];
            ASSERT(searchEndpoints(frame));
            searchCuts<false>(block, frame, state);
            state.heap = 0;
        }
    }

    size_t cut(const ReadBlock& block, const BreadthFrame& frame, size_t from, unsigned boundary) const {
        while (from < frame.end && block.positions[from - block.endpointBase] <= boundary) ++from;
        return from;
    }

    void leaf(const ReadBlock& block, const BreadthFrame& frame, uint64_t mask) {
        for (unsigned b = nextBoundary(frame, 0, mask); b < limit(frame); b = nextBoundary(frame, b + 2, mask)) {
            const Entry& entry = frame.node->at(b / 2);
            consume(cut(block, frame, position, b), NoVersion);
            size_t kept = 0;
            for (uint32_t q : tree.openQueries_) {
                BTREE_SEARCH_STAT(++tree.readPipelineStats_.openQueryChecks);
                if (entry.cv > queries[q].readVersion) queries[q].conflict = true;
                else tree.openQueries_[kept++] = q;
            }
            tree.openQueries_.resize(kept);
            consume(cut(block, frame, position, b + 1), entry.cv);
            BTREE_SEARCH_STAT(++tree.stats_.queryLeafEntries);
        }
    }

    void child(const ReadBlock& block, const BreadthFrame& frame, const Entry& entry, unsigned boundary) {
        const size_t first = position;
        const auto* positions = block.positions.data();
        const auto survives = [&](uint32_t endIndex) {
            return endIndex >= frame.end || positions[endIndex - block.endpointBase] > boundary;
        };
        Version rv = std::numeric_limits<Version>::max();
        size_t kept = 0;
        for (uint32_t q : tree.openQueries_) {
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.openQueryChecks);
            rv = std::min(rv, queries[q].readVersion);
            if (!queries[q].conflict && survives(queries[q].endIndex)) tree.openQueries_[kept++] = q;
        }
        tree.openQueries_.resize(kept);
        for (; position < frame.end && positions[position - block.endpointBase] <= boundary; ++position) {
            const auto& endpoint = tree.sortedEndpoints_[position];
            if (endpoint.end) continue;
            Query& query = queries[endpoint.query];
            if (query.conflict) continue;
            rv = std::min(rv, query.readVersion);
            if (NoVersion > query.readVersion) query.conflict = true;
            else if (survives(query.endIndex)) tree.openQueries_.push_back(endpoint.query);
        }
        BTREE_SEARCH_STAT(tree.readPipelineStats_.maxOpenQueries =
            std::max<uint64_t>(tree.readPipelineStats_.maxOpenQueries, tree.openQueries_.size()));
        if (rv == std::numeric_limits<Version>::max()) {
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.sortedEmptyChildrenSkipped);
        } else if (entry.cv <= rv) {
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.sortedOldChildrenSkipped);
        } else tree.breadthNext_.push_back({entry.payload.link.child, first, position, rv});
    }

    unsigned skipEmptyChildren(const ReadBlock& block, const BreadthFrame& frame, unsigned rank, uint64_t mask) {
        if (!tree.openQueries_.empty()) return rank;
        unsigned next = limit(frame);
        if (position < frame.end) {
            const unsigned boundary = block.positions[position - block.endpointBase];
            const uint64_t before = mask & ((uint64_t{1} << boundary) - 1);
            next = before ? std::max(rank, 63u - static_cast<unsigned>(__builtin_clzll(before))) : rank;
        }
        BTREE_SEARCH_STAT(tree.readPipelineStats_.sortedEmptyChildrenSkipped +=
            __builtin_popcountll(mask & ((uint64_t{1} << next) - 1) & (~uint64_t{0} << rank)));
        return next;
    }

    void consumeBlock(ReadBlock& block) {
        for (size_t f = block.first; f < block.end; ++f) {
            const auto& frame = tree.breadthCurrent_[f];
            ASSERT(block.nodes[f - block.first].heap == 0);
            ASSERT(position <= frame.first);
            consume(frame.first, NoVersion);
            if (frame.node->maxCV <= frame.minimumBeginRV) {
                BTREE_SEARCH_STAT(++tree.readPipelineStats_.sortedNodesSkipped);
                consume(frame.end, NoVersion); continue;
            }
            BTREE_SEARCH_STAT(++tree.stats_.visitedNodes);
            const uint64_t mask = block.nodes[f - block.first].boundaries;
            if (frame.node->leaf) leaf(block, frame, mask);
            else {
                unsigned r = nextBoundary(frame, 0, mask);
                while (r < limit(frame)) {
                    r = skipEmptyChildren(block, frame, r, mask);
                    if (r == limit(frame)) break;
                    const unsigned next = nextBoundary(frame, r + 1, mask);
                    child(block, frame, frame.node->at(r), next);
                    r = next;
                }
            }
            consume(frame.end, NoVersion);
        }
    }

    template <bool Grouped>
    void levelSelected() {
        size_t nextFrame = 0;
        auto& blocks = tree.readBlocks_;
        for (auto& block : blocks) block.first = block.end = 0;
        admit(blocks[0], nextFrame);                         // inline prefetch A
        if (nextFrame < tree.breadthCurrent_.size())
            admit(blocks[1], nextFrame);                    // inline prefetch B
        bool done[2] = {false, false};
        if constexpr (Grouped) done[0] = startGroups(blocks[0]);      // inline A, pending heap A
        else start(blocks[0]);
        if (blocks[1].end) {
            if constexpr (Grouped) done[1] = startGroups(blocks[1]);  // inline B, pending heap B
            else start(blocks[1]);
        }
        unsigned current = 0;
        while (blocks[current].end) {
            // Only the plan and prefetch of C are live; its scratch reuses A
            // after A is consumed. No third ReadBlock or task queue.
            size_t plannedBytes = 0;
            const size_t plannedEnd = planBlock(nextFrame, plannedBytes);
            prefetchInline(nextFrame, plannedEnd);          // inline prefetch C
            const unsigned other = 1 - current;
            if constexpr (Grouped) {
                while (!done[current]) {
                    done[current] = advanceGroups(blocks[current]);
                    if (!done[current] && blocks[other].end && !done[other])
                        done[other] = advanceGroups(blocks[other]);
                }
            } else finish(blocks[current]);                // second pass A
            auto& a = blocks[current];
            consumeBlock(a);
            a.first = a.end = 0;
            if (nextFrame < plannedEnd) {
                admitPlanned(a, nextFrame, plannedEnd, plannedBytes);
                nextFrame = plannedEnd;
                if constexpr (Grouped) done[current] = startGroups(a);     // inline C, pending heap C
                else start(a);
                if constexpr (!Grouped) done[current] = false;
                BTREE_SEARCH_STAT(++tree.readPipelineStats_.blockRotations);
            }
            if (blocks[other].end) current = other;
        }
    }

    void level() {
        const bool lower = tree.breadthCurrent_.front().node->leaf || depth > cfg.breadthBoundaryLevels;
        if (lower) levelSelected<true>();
        else levelSelected<false>();
    }

    void read(size_t count) {
        ASSERT(line > 0 && (line & (line - 1)) == 0);
        if (!initialize(count)) return;
        tree.breadthCurrent_.push_back({tree.root_, 0, tree.sortedEndpoints_.size(), minimumRV});
        while (!tree.breadthCurrent_.empty()) {
            position = 0; tree.openQueries_.clear(); tree.breadthNext_.clear();
            BTREE_SEARCH_STAT(++tree.readPipelineStats_.breadthLevels);
            BTREE_SEARCH_STAT(tree.readPipelineStats_.breadthQueuedNodes += tree.breadthCurrent_.size());
            level();
            consume(tree.sortedEndpoints_.size(), NoVersion);
            ASSERT(tree.openQueries_.empty());
            tree.breadthCurrent_.swap(tree.breadthNext_); ++depth;
        }
        size_t bytes = tree.sortedEndpoints_.capacity() * sizeof(SortedEndpoint) +
            tree.sortedEndpointLcp_.capacity() * sizeof(uint32_t) +
            tree.openQueries_.capacity() * sizeof(uint32_t) +
            (tree.breadthCurrent_.capacity() + tree.breadthNext_.capacity()) * sizeof(BreadthFrame);
        for (const auto& b : tree.readBlocks_)
            bytes += b.pendingFrames.capacity() * sizeof(uint64_t) + b.positions.capacity() * sizeof(uint8_t) +
                b.nodes.capacity() * sizeof(BlockNodeState) + b.addresses.capacity() * sizeof(const uint8_t*);
        BTREE_SEARCH_STAT(tree.readPipelineStats_.scratchBytes = bytes);
    }
};

void PagedBTreeConflictSet::readBlocks(Query* queries, size_t count, const ReadPipelineConfig& cfg,
                                        const OrderedEndpoint* endpoints, size_t endpointCount, bool presorted) {
    BlockReader{*this, queries, cfg, endpoints, endpointCount, presorted}.read(count);
}

bool PagedBTreeConflictSet::hasConflict(Version readVersion, StringRef begin, StringRef end) {
    Query query{readVersion, begin, end};
    hasConflicts(&query, 1);
    return query.conflict;
}

uint64_t PagedBTreeConflictSet::cleanup(unsigned /*leafBudget*/, unsigned /*mergeCandidates*/,
                                       bool /*memoryPressure*/) {
    return 0;
}

unsigned PagedBTreeConflictSet::height() const {
    unsigned result = 0;
    for (Node* node = root_; node; node = node->leaf ? nullptr : node->at(0).payload.link.child) ++result;
    return result;
}

uint64_t PagedBTreeConflictSet::occupancy(bool leaves, unsigned count) const {
    ASSERT(count <= Capacity);
    auto visit = [&](auto&& self, const Node* node) -> uint64_t {
        if (!node) return 0;
        uint64_t result = node->leaf == leaves && node->size() == count;
        if (!node->leaf)
            for (unsigned i = 0; i < node->size(); ++i)
                result += self(self, node->at(i).payload.link.child);
        return result;
    };
    return visit(visit, root_);
}

uint64_t PagedBTreeConflictSet::nodeCount() const {
    return nodeCount_;
}

std::vector<PagedBTreeConflictSet::Level> PagedBTreeConflictSet::levels() const {
    std::vector<Level> result;
    auto visit = [&](auto&& self, const Node* node, unsigned depth) -> void {
        // Indexed on every access: the recursion below can grow the vector and move its elements
        if (result.size() <= depth) result.resize(depth + 1);
        ++result[depth].nodes;
        result[depth].entries += node->size();
        for (unsigned i = 0; i < node->size(); ++i) {
            if (node->at(i).cv > validationMinRV_) ++result[depth].liveEntries;
            if (!node->leaf) self(self, node->at(i).payload.link.child, depth + 1);
        }
    };
    if (root_) visit(visit, root_, 0);
    return result;
}

} // namespace paged_btree
