#pragma once
#include "PagedBTreeConflictSet.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <istream>
#include <tuple>
#include <unordered_map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

struct TreeStateCapture {
    Version realOffset = 0;
    Version syntheticEnd = 0;
    int64_t captureBatch = -1;
    uint64_t rngPeek = 0;
    uint64_t processSeed = 0;
};

struct TreeStateSummary {
    uint64_t internalNodes = 0, leaves = 0, leafEntries = 0, internalEntries = 0, expiredEntries = 0;
    uint64_t ageMain = 0, ageExceptions = 0, freeNodes = 0, retiredNodes = 0, bytes = 0, hash = 0;
};

struct TreeStateExporter {
    using Tree = paged_btree::PagedBTreeConflictSet;
    using Node = Tree::Node;
    using Entry = Tree::Entry;
    using NodePage = Tree::NodePage;
    static constexpr uint32_t Version1 = 1;
    static constexpr uint32_t NoId = UINT32_MAX;

    struct Sink {
        std::ostream& out;
        uint32_t crc = 0xFFFFFFFFu;
        uint64_t hash = 0, bytes = 0;
        static const std::array<uint32_t, 256>& table() {
            static const std::array<uint32_t, 256> t = [] {
                std::array<uint32_t, 256> r{};
                for (uint32_t i = 0; i < 256; ++i) {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    r[i] = c;
                }
                return r;
            }();
            return t;
        }
        void raw(const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            const auto& t = table();
            for (size_t i = 0; i < n; ++i) crc = t[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
            hash = crc ^ 0xFFFFFFFFu;
            out.write(static_cast<const char*>(p), std::streamsize(n));
            bytes += n;
            if (!out) throw std::runtime_error("tree state write failed");
        }
        template <class T> void put(T v) {
            static_assert(std::is_integral_v<T>);
            uint8_t b[sizeof(T)];
            for (size_t i = 0; i < sizeof(T); ++i) b[i] = uint8_t(uint64_t(v) >> (8 * i));
            raw(b, sizeof(T));
        }
        void tag(const char* t) { raw(t, 4); }
        void key(StringRef k) {
            if (k.size() > 65535) throw std::runtime_error("key too long for export");
            put<uint16_t>(uint16_t(k.size()));
            raw(k.begin(), size_t(k.size()));
        }
    };

    static uint32_t id(const Node* n) { return n ? n->ageIndex : NoId; }
    static uint8_t rankInParent(const Node* n) {
        if (!n->parent) return 255;
        for (unsigned r = 0; r < n->parent->size(); ++r)
            if (n->parent->at(r).payload.link.child == n) return uint8_t(r);
        throw std::runtime_error("child missing from its parent");
    }
    static void ageState(const Tree& t, const Node* n, Version& cv, uint8_t& linked, uint8_t& exception) {
        linked = n->ageLinked;
        cv = linked ? t.pool_.ageEntry(n->ageIndex).maxCV : n->maxCV;
        exception = linked && t.pool_.ageException(n->ageIndex);
    }
    static void heapRef(Sink& s, const HeapRef& r) {
        s.put<uint64_t>(r.pageID);
        s.put<uint32_t>(r.offset);
        s.put<uint32_t>(r.length);
    }

    static TreeStateSummary write(const Tree& t, std::ostream& out, const TreeStateCapture& capture) {
        if (!Tree::InlineOnlyKeys) throw std::runtime_error("tree state export requires inline-only keys");
        Sink s{ out };
        TreeStateSummary sum;
        std::vector<const Node*> internal;
        std::vector<uint16_t> levels;
        std::deque<std::pair<const Node*, uint16_t>> queue;
        if (t.root_ && !t.root_->leaf) queue.push_back({ t.root_, 0 });
        while (!queue.empty()) {
            auto [n, level] = queue.front();
            queue.pop_front();
            internal.push_back(n);
            levels.push_back(level);
            sum.internalEntries += n->size();
            for (unsigned r = 0; r < n->size(); ++r) {
                const Node* c = n->at(r).payload.link.child;
                if (!c->leaf) queue.push_back({ c, uint16_t(level + 1) });
            }
        }
        for (const Node* x = t.firstLeaf_; x; x = x->navigation.leaf.next) {
            ++sum.leaves;
            sum.leafEntries += x->size();
            if (x->fict.fictitious) throw std::runtime_error("tree state export does not support fictitious leaf occupancy");
        }
        sum.internalNodes = internal.size();
        for (uint32_t a = t.pool_.oldestAge_; a != PagePool::NoAge; a = t.pool_.ageEntry(a).next) ++sum.ageMain;
        sum.ageExceptions = t.pool_.exceptionAges_.size();
        for (const Node* x = t.free_; x; x = static_cast<const Node*>(t.pool_.detachedNext(x))) ++sum.freeNodes;
        for (const Node* x = t.retired_; x; x = static_cast<const Node*>(t.pool_.detachedNext(x))) ++sum.retiredNodes;

        s.raw("BY5TREE\0", 8);
        s.put<uint32_t>(Version1);
        s.put<int64_t>(t.validationMinRV_);
        s.put<int64_t>(t.lastWriteCV_);
        s.put<int64_t>(capture.realOffset);
        s.put<int64_t>(capture.syntheticEnd);
        s.put<int64_t>(capture.captureBatch);
        s.put<uint64_t>(capture.rngPeek);
        s.put<uint64_t>(capture.processSeed);
        s.put<uint32_t>(Tree::Capacity);
        s.put<uint32_t>(uint32_t(Tree::InlineKeyBytes));
        s.put<uint8_t>(Tree::InlineOnlyKeys);
        s.put<uint8_t>(Tree::NodePrefixEnabled);
        s.put<uint32_t>(uint32_t(Tree::NodePrefixBytes));
        s.put<uint32_t>(t.probFusion_.maxMergedEntries);
        s.put<uint32_t>(t.probFusion_.maxSourceEntries);
        for (unsigned k = 0; k <= Tree::Capacity; ++k) s.put<uint32_t>(Tree::ProbFusionAttemptPer65536[k]);
        s.put<uint8_t>(PAGED_BTREE_PURGE_FUSION_NEIGHBOUR);
        s.put<uint8_t>(PAGED_BTREE_BITMAP_CLEANUP);
        const PageBudget& b = t.pool_.budget();
        for (uint64_t v : { b.limitPages, b.heapPages, b.heapCapacity, b.indexCapacity, b.unusedPages, b.indexPages, b.freePages }) s.put<uint64_t>(v);
        s.put<uint8_t>(t.pool_.partitioned());
        s.put<uint64_t>(t.nodeCount_);
        s.put<uint64_t>(t.leafCount_);
        s.put<uint64_t>(t.freeNodes_);
        s.put<uint64_t>(t.retiredCount_);
        s.put<uint8_t>(t.writeBatchActive_);
        s.put<uint32_t>(id(t.root_));
        s.put<uint8_t>(t.root_ ? t.root_->leaf : 0);
        s.put<uint32_t>(id(t.firstLeaf_));
        s.put<uint64_t>(t.inlineIdentity_);
        for (uint64_t v : { sum.internalNodes, sum.leaves, sum.leafEntries, sum.internalEntries, sum.ageMain, sum.ageExceptions, sum.freeNodes, sum.retiredNodes }) s.put<uint64_t>(v);

        Tree::KeyScratch scratch;
        s.tag("INTR");
        for (size_t i = 0; i < internal.size(); ++i) {
            const Node* n = internal[i];
            Version cv;
            uint8_t linked, exception;
            ageState(t, n, cv, linked, exception);
            Version computedMax = NoVersionValue(), computedMin = std::numeric_limits<Version>::max();
            for (unsigned r = 0; r < n->size(); ++r) {
                const Version c = n->at(r).payload.link.child->maxCV;
                computedMax = std::max(computedMax, c);
                computedMin = std::min(computedMin, c);
            }
            s.put<uint32_t>(id(n));
            s.put<uint32_t>(n->incarnation);
            s.put<uint16_t>(levels[i]);
            s.put<uint32_t>(id(n->parent));
            s.put<uint8_t>(n->parentSlot);
            s.put<uint8_t>(rankInParent(n));
            s.put<uint8_t>(uint8_t(n->size()));
            s.put<uint32_t>(n->occupied);
            s.put<uint8_t>(n->reclaimHead);
            s.put<uint8_t>(n->cursor);
            s.put<uint8_t>(n->critical);
            s.put<uint8_t>(n->criticalChildren);
            s.put<int64_t>(n->maxCV);
            s.put<int64_t>(cv);
            s.put<int64_t>(computedMax);
            s.put<int64_t>(computedMin);
            s.put<uint8_t>(linked);
            s.put<uint8_t>(exception);
            s.put<uint8_t>(n->navigation.prefix.initialized);
            s.put<uint8_t>(n->navigation.prefix.length);
            s.raw(n->navigation.prefix.bytes.data(), Tree::NodePrefixBytes);
            for (unsigned r = 0; r < n->size(); ++r) {
                const unsigned slot = n->order.slotAt(r);
                const auto& e = n->slots[slot];
                s.put<uint8_t>(uint8_t(r));
                s.put<uint8_t>(uint8_t(slot));
                s.put<int64_t>(e.cv);
                s.put<uint32_t>(id(e.payload.link.child));
                s.put<int64_t>(e.payload.link.firstCV);
                s.key(t.storedKey(e.begin, e.beginInline, scratch));
                heapRef(s, e.begin);
                heapRef(s, e.end);
            }
        }

        s.tag("LEAF");
        uint64_t position = 0;
        for (const Node* x = t.firstLeaf_; x; x = x->navigation.leaf.next, ++position) {
            Version cv;
            uint8_t linked, exception;
            ageState(t, x, cv, linked, exception);
            Version computedMax = NoVersionValue(), computedMin = std::numeric_limits<Version>::max();
            uint8_t expired = 0;
            for (unsigned r = 0; r < x->size(); ++r) {
                const Version c = x->at(r).cv;
                computedMax = std::max(computedMax, c);
                computedMin = std::min(computedMin, c);
                expired += c <= t.validationMinRV_;
            }
            sum.expiredEntries += expired;
            const Node* prev = x->navigation.leaf.previous;
            const Node* next = x->navigation.leaf.next;
            s.put<uint64_t>(position);
            s.put<uint32_t>(id(x));
            s.put<uint32_t>(x->incarnation);
            s.put<uint32_t>(id(x->parent));
            s.put<uint8_t>(x->parentSlot);
            s.put<uint8_t>(rankInParent(x));
            s.put<uint32_t>(id(prev));
            s.put<uint32_t>(id(next));
            s.put<uint8_t>(prev && prev->parent == x->parent);
            s.put<uint8_t>(next && next->parent == x->parent);
            s.put<uint8_t>(uint8_t(x->size()));
            s.put<uint32_t>(x->occupied);
            s.put<uint8_t>(x->reclaimHead);
            s.put<uint8_t>(x->cursor);
            s.put<uint8_t>(x->critical);
            s.put<uint8_t>(expired);
            s.put<int64_t>(x->maxCV);
            s.put<int64_t>(cv);
            s.put<int64_t>(computedMax);
            s.put<int64_t>(computedMin);
            s.put<uint8_t>(linked);
            s.put<uint8_t>(exception);
            for (unsigned r = 0; r < x->size(); ++r) {
                const unsigned slot = x->order.slotAt(r);
                const auto& e = x->slots[slot];
                const bool shared = Tree::endSharesStorage(e);
                s.put<uint8_t>(uint8_t(r));
                s.put<uint8_t>(uint8_t(slot));
                s.put<int64_t>(e.cv);
                s.put<uint8_t>(uint8_t(shared) | uint8_t(Tree::implicitZero(e.end) << 1) | uint8_t((e.cv <= t.validationMinRV_) << 2));
                s.key(t.storedKey(e.begin, e.beginInline, scratch));
                s.key(t.storedKey(e.end, Tree::endPrefix(e), scratch));
                heapRef(s, e.begin);
                heapRef(s, e.end);
            }
        }

        s.tag("AGEM");
        for (uint32_t a = t.pool_.oldestAge_; a != PagePool::NoAge; a = t.pool_.ageEntry(a).next) {
            s.put<uint32_t>(a);
            s.put<uint8_t>(t.pool_.ageObject(a)->ageUse == PageUse::Heap);
            s.put<int64_t>(t.pool_.ageEntry(a).maxCV);
        }
        s.tag("AGEX");
        for (size_t p = 0; p < t.pool_.exceptionAges_.size(); ++p) {
            const uint32_t a = t.pool_.exceptionAges_[p];
            s.put<uint32_t>(uint32_t(p));
            s.put<uint32_t>(a);
            s.put<int64_t>(t.pool_.ageEntry(a).maxCV);
        }
        s.tag("FREE");
        for (const Node* x = t.free_; x; x = static_cast<const Node*>(t.pool_.detachedNext(x))) s.put<uint32_t>(id(x));
        s.tag("RETD");
        for (const Node* x = t.retired_; x; x = static_cast<const Node*>(t.pool_.detachedNext(x))) s.put<uint32_t>(id(x));
        s.tag("STAT");
        const auto& st = t.stats_;
        const auto& ex = t.pool_.exceptionAgeStats();
        for (uint64_t v : { st.splits, st.internalSplits, st.merges, st.internalMerges, st.mergeCandidatesExamined, st.reclaimCalls,
                            st.reclaimAgeCandidates, st.reclaimNodeCandidates, st.expiredIntervals, st.overwrittenIntervals,
                            st.localHeadEntriesReclaimed, st.localHeadChildrenReclaimed, st.emptiedAnchorsRemoved, st.nodesTaken,
                            ex.insertions, ex.removals, ex.updates, ex.swaps, ex.peak, uint64_t(t.pool_.exceptionAges_.capacity()) })
            s.put<uint64_t>(v);
        s.tag("END\0");
        const uint64_t contentHash = s.hash;
        s.put<uint64_t>(contentHash);
        sum.bytes = s.bytes;
        sum.hash = contentHash;
        return sum;
    }

    static constexpr Version NoVersionValue() { return std::numeric_limits<Version>::lowest(); }

    struct Source {
        std::istream& in;
        uint32_t crc = 0xFFFFFFFFu;
        uint64_t bytes = 0;
        void raw(void* p, size_t n) {
            in.read(static_cast<char*>(p), std::streamsize(n));
            if (!in) throw std::runtime_error("tree state truncated");
            const auto* b = static_cast<const uint8_t*>(p);
            const auto& t = Sink::table();
            for (size_t i = 0; i < n; ++i) crc = t[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
            bytes += n;
        }
        template <class T> T get() {
            uint8_t b[sizeof(T)];
            raw(b, sizeof(T));
            uint64_t v = 0;
            for (size_t i = 0; i < sizeof(T); ++i) v |= uint64_t(b[i]) << (8 * i);
            return T(v);
        }
        void tag(const char* t) {
            char b[4];
            raw(b, 4);
            if (std::memcmp(b, t, 4)) throw std::runtime_error(std::string("tree state section ") + std::string(t, 4) + " missing");
        }
        std::string key() {
            const uint16_t n = get<uint16_t>();
            std::string k(n, '\0');
            if (n) raw(k.data(), n);
            return k;
        }
        HeapRef ref() {
            HeapRef r;
            r.pageID = get<uint64_t>();
            r.offset = get<uint32_t>();
            r.length = get<uint32_t>();
            return r;
        }
    };

    struct LoadOptions {
        std::function<const int64_t*(unsigned)> leafOffsets;
        const std::vector<uint8_t>* layoutLeaves = nullptr;
        const std::vector<std::vector<uint8_t>>* layoutLevels = nullptr;
        bool layoutKeepsLeafPartition = false;
    };

    struct LoadSummary {
        Version floor = 0, lastWriteCV = 0, realOffset = 0, syntheticEnd = 0;
        uint64_t internalNodes = 0, leaves = 0, leafEntries = 0, replacedEntries = 0, expiredEntries = 0;
        uint64_t firstCvMatched = 0, firstCvStale = 0, freeNodes = 0, dumpFreeNodes = 0, rngPeek = 0, bytes = 0;
        uint64_t criticalLeaves = 0, criticalInternal = 0, slackViolations = 0, separatorsLive = 0, separatorsExpired = 0, keyStreamCrc = 0;
        bool synthetic = false, layout = false;
    };

    static void prefixFrom(Tree::KeyPrefix& p, const std::string& k) {
        p = Tree::KeyPrefix{};
        if constexpr (Tree::InlineKeyBytes > 0)
            if (!k.empty()) std::memcpy(p.data(), k.data(), std::min<size_t>(Tree::InlineKeyBytes, k.size()));
    }
    static bool sameRef(const HeapRef& a, const HeapRef& b) {
        return a.pageID == b.pageID && a.offset == b.offset && a.length == b.length;
    }

    static LoadSummary load(Tree& t, std::istream& in, const LoadOptions& options) {
        if (!Tree::InlineOnlyKeys) throw std::runtime_error("tree state load requires inline-only keys");
        if (t.root_ || t.nodeCount_ || t.writeBatchActive_ || t.firstLeaf_) throw std::runtime_error("tree state load requires an empty tree");
        if (t.fictitious_.enabled) throw std::runtime_error("tree state load does not support fictitious leaf occupancy");
        Source s{ in };
        LoadSummary sum;
        sum.synthetic = bool(options.leafOffsets);
        char magic[8];
        s.raw(magic, 8);
        if (std::memcmp(magic, "BY5TREE\0", 8) || s.get<uint32_t>() != Version1) throw std::runtime_error("not a version 1 tree state");
        const Version floor = s.get<int64_t>(), lastWrite = s.get<int64_t>();
        sum.floor = floor;
        sum.lastWriteCV = lastWrite;
        sum.realOffset = s.get<int64_t>();
        sum.syntheticEnd = s.get<int64_t>();
        s.get<int64_t>();
        sum.rngPeek = s.get<uint64_t>();
        s.get<uint64_t>();
        if (s.get<uint32_t>() != Tree::Capacity || s.get<uint32_t>() != Tree::InlineKeyBytes || s.get<uint8_t>() != uint8_t(Tree::InlineOnlyKeys))
            throw std::runtime_error("tree state configuration differs");
        s.get<uint8_t>();
        if (s.get<uint32_t>() != Tree::NodePrefixBytes) throw std::runtime_error("node prefix size differs");
        if (s.get<uint32_t>() != t.probFusion_.maxMergedEntries || s.get<uint32_t>() != t.probFusion_.maxSourceEntries)
            throw std::runtime_error("fusion limits differ");
        for (unsigned k = 0; k <= Tree::Capacity; ++k)
            if (s.get<uint32_t>() != Tree::ProbFusionAttemptPer65536[k]) throw std::runtime_error("fusion attempt table differs");
        if (s.get<uint8_t>() != PAGED_BTREE_PURGE_FUSION_NEIGHBOUR || s.get<uint8_t>() != PAGED_BTREE_BITMAP_CLEANUP) throw std::runtime_error("cleanup configuration differs");
        uint64_t budget[7];
        for (auto& b : budget) b = s.get<uint64_t>();
        if (budget[0] != t.pool_.budget().limitPages) throw std::runtime_error("pool size differs");
        s.get<uint8_t>();
        const uint64_t nodeCount = s.get<uint64_t>(), leafCount = s.get<uint64_t>();
        sum.dumpFreeNodes = s.get<uint64_t>();
        if (s.get<uint64_t>() != 0 || s.get<uint8_t>() != 0) throw std::runtime_error("retired nodes or an open write batch are not supported");
        const uint32_t rootId = s.get<uint32_t>();
        s.get<uint8_t>();
        s.get<uint32_t>();
        const uint64_t inlineIdentity = s.get<uint64_t>();
        uint64_t counts[8];
        for (auto& c : counts) c = s.get<uint64_t>();

        std::unordered_map<uint32_t, Node*> byId;
        byId.reserve(nodeCount);
        struct Fix { Node* parent; unsigned slot; uint32_t child; };
        std::vector<Fix> fixes;
        fixes.reserve(counts[3]);
        std::vector<std::pair<Node*, uint32_t>> parents;
        parents.reserve(nodeCount);
        std::vector<Node*> internalOrder;
        internalOrder.reserve(counts[0]);
        auto make = [&](bool leaf, uint32_t incarnation) {
            Node* n = t.allocateNode();
            NodePage* owner = n->owner;
            *n = Node{};
            n->incarnation = incarnation;
            n->ageIndex = t.pool_.nodeAgeIndex(n);
            n->owner = owner;
            n->allocated = true;
            n->leaf = leaf;
            if (!leaf) new (&n->navigation.prefix) Node::ReferencePrefix{};
            ++t.nodeCount_;
            if (leaf) ++t.leafCount_;
            return n;
        };

        s.tag("INTR");
        for (uint64_t i = 0; i < counts[0]; ++i) {
            const uint32_t id = s.get<uint32_t>(), incarnation = s.get<uint32_t>();
            s.get<uint16_t>();
            const uint32_t parent = s.get<uint32_t>();
            Node* n = make(false, incarnation);
            if (!byId.emplace(id, n).second) throw std::runtime_error("duplicate node id");
            internalOrder.push_back(n);
            parents.push_back({ n, parent });
            n->parentSlot = s.get<uint8_t>();
            s.get<uint8_t>();
            const unsigned size = s.get<uint8_t>();
            const uint32_t occupied = s.get<uint32_t>();
            n->reclaimHead = s.get<uint8_t>();
            n->cursor = s.get<uint8_t>();
            n->critical = s.get<uint8_t>();
            n->criticalChildren = s.get<uint8_t>();
            s.get<int64_t>();
            n->maxCV = s.get<int64_t>();
            s.get<int64_t>();
            s.get<int64_t>();
            s.get<uint8_t>();
            s.get<uint8_t>();
            n->navigation.prefix.initialized = s.get<uint8_t>();
            n->navigation.prefix.length = s.get<uint8_t>();
            s.raw(n->navigation.prefix.bytes.data(), Tree::NodePrefixBytes);
            for (unsigned r = 0; r < size; ++r) {
                if (s.get<uint8_t>() != r) throw std::runtime_error("internal rank order");
                const unsigned slot = s.get<uint8_t>();
                Entry& e = n->slots[slot];
                e = Entry{};
                e.cv = s.get<int64_t>();
                const uint32_t child = s.get<uint32_t>();
                e.payload.link.firstCV = s.get<int64_t>();
                prefixFrom(e.beginInline, s.key());
                e.begin = s.ref();
                e.end = s.ref();
                n->order.insert(r, slot);
                fixes.push_back({ n, slot, child });
            }
            n->occupied = occupied;
        }

        s.tag("LEAF");
        Node* previous = nullptr;
        std::vector<Node*> leaves;
        leaves.reserve(counts[1]);
        for (uint64_t i = 0; i < counts[1]; ++i) {
            if (s.get<uint64_t>() != i) throw std::runtime_error("leaf position");
            const uint32_t id = s.get<uint32_t>(), incarnation = s.get<uint32_t>(), parent = s.get<uint32_t>();
            Node* n = make(true, incarnation);
            if (!byId.emplace(id, n).second) throw std::runtime_error("duplicate node id");
            leaves.push_back(n);
            parents.push_back({ n, parent });
            n->parentSlot = s.get<uint8_t>();
            s.get<uint8_t>();
            s.get<uint32_t>();
            s.get<uint32_t>();
            s.get<uint8_t>();
            s.get<uint8_t>();
            const unsigned size = s.get<uint8_t>();
            const uint32_t occupied = s.get<uint32_t>();
            n->reclaimHead = s.get<uint8_t>();
            n->cursor = s.get<uint8_t>();
            n->critical = s.get<uint8_t>();
            s.get<uint8_t>();
            s.get<int64_t>();
            n->maxCV = s.get<int64_t>();
            s.get<int64_t>();
            s.get<int64_t>();
            s.get<uint8_t>();
            s.get<uint8_t>();
            const int64_t* offsets = sum.synthetic ? options.leafOffsets(size) : nullptr;
            for (unsigned r = 0; r < size; ++r) {
                if (s.get<uint8_t>() != r) throw std::runtime_error("leaf rank order");
                const unsigned slot = s.get<uint8_t>();
                Entry& e = n->slots[slot];
                e = Entry{};
                e.cv = s.get<int64_t>();
                s.get<uint8_t>();
                const std::string begin = s.key(), end = s.key();
                prefixFrom(e.beginInline, begin);
                e.begin = s.ref();
                e.end = s.ref();
                if (!Tree::endSharesStorage(e)) {
                    new (&e.payload.endInline) Tree::KeyPrefix{};
                    prefixFrom(e.payload.endInline, end);
                }
                if (offsets) {
                    if (offsets[r] > std::numeric_limits<int64_t>::max() - floor) throw std::overflow_error("synthetic CV overflow");
                    e.cv = floor + offsets[r];
                    if (e.cv > lastWrite) throw std::runtime_error("synthetic CV beyond the last written version");
                    ++sum.replacedEntries;
                }
                sum.expiredEntries += e.cv <= floor;
                n->order.insert(r, slot);
            }
            n->occupied = occupied;
            sum.leafEntries += size;
            n->navigation.leaf.previous = previous;
            if (previous) previous->navigation.leaf.next = n;
            else t.firstLeaf_ = n;
            previous = n;
        }

        std::vector<std::tuple<uint32_t, Version>> mainAges, exceptionAges;
        s.tag("AGEM");
        for (uint64_t i = 0; i < counts[4]; ++i) {
            const uint32_t id = s.get<uint32_t>();
            s.get<uint8_t>();
            mainAges.emplace_back(id, s.get<int64_t>());
        }
        s.tag("AGEX");
        for (uint64_t i = 0; i < counts[5]; ++i) {
            s.get<uint32_t>();
            const uint32_t id = s.get<uint32_t>();
            exceptionAges.emplace_back(id, s.get<int64_t>());
        }
        s.tag("FREE");
        for (uint64_t i = 0; i < counts[6]; ++i) s.get<uint32_t>();
        s.tag("RETD");
        for (uint64_t i = 0; i < counts[7]; ++i) s.get<uint32_t>();
        s.tag("STAT");
        for (unsigned i = 0; i < 20; ++i) s.get<uint64_t>();
        s.tag("END\0");
        const uint32_t computed = s.crc ^ 0xFFFFFFFFu;
        if (s.get<uint64_t>() != computed) throw std::runtime_error("tree state CRC-32 mismatch");
        sum.bytes = s.bytes;

        for (const auto& f : fixes) {
            auto it = byId.find(f.child);
            if (it == byId.end()) throw std::runtime_error("missing child");
            f.parent->slots[f.slot].payload.link.child = it->second;
        }
        for (auto [n, parent] : parents) {
            if (parent == NoId) { n->parent = nullptr; continue; }
            auto it = byId.find(parent);
            if (it == byId.end()) throw std::runtime_error("missing parent");
            n->parent = it->second;
            if (it->second->slots[n->parentSlot].payload.link.child != n) throw std::runtime_error("parent slot mismatch");
        }
        auto root = byId.find(rootId);
        if (root == byId.end() || root->second->parent) throw std::runtime_error("root");
        t.root_ = root->second;
        t.validationMinRV_ = floor;
        t.lastWriteCV_ = lastWrite;
        t.inlineIdentity_ = inlineIdentity;
        sum.internalNodes = internalOrder.size();
        sum.leaves = leaves.size();

        if (sum.synthetic) {
            for (Node* n : leaves) {
                Version m = NoVersionValue();
                for (unsigned r = 0; r < n->size(); ++r) m = std::max(m, n->at(r).cv);
                n->maxCV = m;
            }
            for (auto it = internalOrder.rbegin(); it != internalOrder.rend(); ++it) {
                Node* n = *it;
                Version m = NoVersionValue();
                for (unsigned r = 0; r < n->size(); ++r) {
                    Entry& e = n->at(r);
                    const Node* c = e.payload.link.child;
                    e.cv = c->maxCV;
                    m = std::max(m, e.cv);
                    const Entry* match = nullptr;
                    for (unsigned q = 0; q < c->size() && !match; ++q)
                        if (sameRef(c->at(q).begin, e.begin)) match = &c->at(q);
                    if (match) {
                        e.payload.link.firstCV = c->leaf ? match->cv : match->payload.link.firstCV;
                        ++sum.firstCvMatched;
                    } else {
                        e.payload.link.firstCV = std::min(e.payload.link.firstCV, floor);
                        ++sum.firstCvStale;
                    }
                }
                n->maxCV = m;
            }
            std::vector<Node*> all(internalOrder);
            all.insert(all.end(), leaves.begin(), leaves.end());
            std::stable_sort(all.begin(), all.end(), [](const Node* a, const Node* b) { return a->maxCV < b->maxCV; });
            for (Node* n : all) {
                const Version cv = n->maxCV;
                n->maxCV = NoVersionValue();
                t.pool_.raiseAge(n, cv, PageUse::Index);
            }
        } else {
            for (auto& list : { std::cref(mainAges), std::cref(exceptionAges) })
                for (auto [id, cv] : list.get()) {
                    Node* n = byId.at(id);
                    if (n->maxCV != cv) throw std::runtime_error("age CV differs from the node bound");
                    n->maxCV = NoVersionValue();
                    t.pool_.raiseAge(n, cv, PageUse::Index);
                }
        }
        if (t.nodeCount_ != nodeCount || t.leafCount_ != leafCount) throw std::runtime_error("loaded node counts differ");
        sum.freeNodes = t.freeNodes_;
        return sum;
    }

    struct StreamCrc {
        uint32_t crc = 0xFFFFFFFFu;
        void add(const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            const auto& t = Sink::table();
            for (size_t i = 0; i < n; ++i) crc = t[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
        }
        template <class T> void put(T v) {
            uint8_t b[sizeof(T)];
            for (size_t i = 0; i < sizeof(T); ++i) b[i] = uint8_t(uint64_t(v) >> (8 * i));
            add(b, sizeof(T));
        }
        void key(const std::string& k) { put<uint16_t>(uint16_t(k.size())); add(k.data(), k.size()); }
        void ref(const HeapRef& r) { put<uint64_t>(r.pageID); put<uint32_t>(r.offset); put<uint32_t>(r.length); }
        uint32_t value() const { return crc ^ 0xFFFFFFFFu; }
    };

    static LoadSummary loadLayout(Tree& t, std::istream& in, const LoadOptions& options) {
        if (!Tree::InlineOnlyKeys) throw std::runtime_error("tree state load requires inline-only keys");
        if (t.root_ || t.nodeCount_ || t.writeBatchActive_ || t.firstLeaf_) throw std::runtime_error("tree state load requires an empty tree");
        if (t.fictitious_.enabled) throw std::runtime_error("tree state load does not support fictitious leaf occupancy");
        if (!options.layoutLeaves || !options.layoutLevels || options.layoutLevels->empty()) throw std::runtime_error("layout missing");
        const auto& sizes = *options.layoutLeaves;
        const auto& levels = *options.layoutLevels;
        Source s{ in };
        LoadSummary sum;
        sum.synthetic = bool(options.leafOffsets);
        sum.layout = true;
        char magic[8];
        s.raw(magic, 8);
        if (std::memcmp(magic, "BY5TREE\0", 8) || s.get<uint32_t>() != Version1) throw std::runtime_error("not a version 1 tree state");
        const Version floor = s.get<int64_t>(), lastWrite = s.get<int64_t>();
        sum.floor = floor;
        sum.lastWriteCV = lastWrite;
        sum.realOffset = s.get<int64_t>();
        sum.syntheticEnd = s.get<int64_t>();
        s.get<int64_t>();
        sum.rngPeek = s.get<uint64_t>();
        s.get<uint64_t>();
        if (s.get<uint32_t>() != Tree::Capacity || s.get<uint32_t>() != Tree::InlineKeyBytes || s.get<uint8_t>() != uint8_t(Tree::InlineOnlyKeys))
            throw std::runtime_error("tree state configuration differs");
        s.get<uint8_t>();
        if (s.get<uint32_t>() != Tree::NodePrefixBytes) throw std::runtime_error("node prefix size differs");
        if (s.get<uint32_t>() != t.probFusion_.maxMergedEntries || s.get<uint32_t>() != t.probFusion_.maxSourceEntries)
            throw std::runtime_error("fusion limits differ");
        for (unsigned k = 0; k <= Tree::Capacity; ++k)
            if (s.get<uint32_t>() != Tree::ProbFusionAttemptPer65536[k]) throw std::runtime_error("fusion attempt table differs");
        if (s.get<uint8_t>() != PAGED_BTREE_PURGE_FUSION_NEIGHBOUR || s.get<uint8_t>() != PAGED_BTREE_BITMAP_CLEANUP) throw std::runtime_error("cleanup configuration differs");
        uint64_t budget[7];
        for (auto& b : budget) b = s.get<uint64_t>();
        if (budget[0] != t.pool_.budget().limitPages) throw std::runtime_error("pool size differs");
        s.get<uint8_t>();
        s.get<uint64_t>();
        s.get<uint64_t>();
        sum.dumpFreeNodes = s.get<uint64_t>();
        if (s.get<uint64_t>() != 0 || s.get<uint8_t>() != 0) throw std::runtime_error("retired nodes or an open write batch are not supported");
        s.get<uint32_t>();
        s.get<uint8_t>();
        s.get<uint32_t>();
        const uint64_t inlineIdentity = s.get<uint64_t>();
        uint64_t counts[8];
        for (auto& c : counts) c = s.get<uint64_t>();
        uint64_t layoutEntries = 0, layoutInternal = 0;
        for (uint8_t v : sizes) {
            if (v < 1 || v > Tree::Capacity) throw std::runtime_error("layout leaf occupancy");
            layoutEntries += v;
        }
        for (size_t l = 0; l < levels.size(); ++l) {
            uint64_t children = 0;
            for (uint8_t v : levels[l]) {
                if (v < 1 || v > Tree::Capacity) throw std::runtime_error("layout fanout");
                children += v;
            }
            if (children != (l + 1 < levels.size() ? levels[l + 1].size() : sizes.size())) throw std::runtime_error("layout child count");
            layoutInternal += levels[l].size();
        }
        if (levels[0].size() != 1) throw std::runtime_error("layout root");
        if (sizes.size() != counts[1] || layoutEntries != counts[2] || layoutInternal != counts[0])
            throw std::runtime_error("layout counts differ from the tree state");

        auto make = [&](bool leaf) {
            Node* n = t.allocateNode();
            NodePage* owner = n->owner;
            const uint32_t incarnation = n->incarnation;
            *n = Node{};
            n->incarnation = incarnation;
            n->ageIndex = t.pool_.nodeAgeIndex(n);
            n->owner = owner;
            n->allocated = true;
            n->leaf = leaf;
            if (!leaf) new (&n->navigation.prefix) Node::ReferencePrefix{};
            ++t.nodeCount_;
            if (leaf) ++t.leafCount_;
            return n;
        };

        s.tag("INTR");
        for (uint64_t i = 0; i < counts[0]; ++i) {
            s.get<uint32_t>(); s.get<uint32_t>(); s.get<uint16_t>(); s.get<uint32_t>(); s.get<uint8_t>(); s.get<uint8_t>();
            const unsigned size = s.get<uint8_t>();
            s.get<uint32_t>(); s.get<uint8_t>(); s.get<uint8_t>(); s.get<uint8_t>(); s.get<uint8_t>();
            for (int k = 0; k < 4; ++k) s.get<int64_t>();
            for (int k = 0; k < 4; ++k) s.get<uint8_t>();
            std::vector<uint8_t> prefix(Tree::NodePrefixBytes);
            s.raw(prefix.data(), prefix.size());
            for (unsigned r = 0; r < size; ++r) {
                s.get<uint8_t>(); s.get<uint8_t>(); s.get<int64_t>(); s.get<uint32_t>(); s.get<int64_t>();
                s.key();
                s.ref();
                s.ref();
            }
        }

        s.tag("LEAF");
        std::vector<Node*> leaves;
        leaves.reserve(sizes.size());
        StreamCrc stream;
        Node* current = nullptr;
        Node* previous = nullptr;
        unsigned filled = 0;
        const int64_t* offsets = nullptr;
        for (uint64_t i = 0; i < counts[1]; ++i) {
            if (s.get<uint64_t>() != i) throw std::runtime_error("leaf position");
            s.get<uint32_t>(); s.get<uint32_t>(); s.get<uint32_t>(); s.get<uint8_t>(); s.get<uint8_t>();
            s.get<uint32_t>(); s.get<uint32_t>(); s.get<uint8_t>(); s.get<uint8_t>();
            const unsigned size = s.get<uint8_t>();
            s.get<uint32_t>(); s.get<uint8_t>(); s.get<uint8_t>(); s.get<uint8_t>(); s.get<uint8_t>();
            for (int k = 0; k < 4; ++k) s.get<int64_t>();
            s.get<uint8_t>(); s.get<uint8_t>();
            if (options.layoutKeepsLeafPartition && (sizes[i] != size || leaves.size() != i || (i && filled != sizes[i - 1])))
                throw std::runtime_error("layout does not keep the leaf partition");
            for (unsigned r = 0; r < size; ++r) {
                s.get<uint8_t>(); s.get<uint8_t>();
                const Version cv = s.get<int64_t>();
                const uint8_t flags = s.get<uint8_t>();
                const std::string begin = s.key(), end = s.key();
                const HeapRef beginRef = s.ref(), endRef = s.ref();
                stream.key(begin);
                stream.key(end);
                stream.put<uint8_t>(flags & 3);
                stream.ref(beginRef);
                stream.ref(endRef);
                if (!current || filled == sizes[leaves.size() - 1]) {
                    if (leaves.size() == sizes.size()) throw std::runtime_error("layout has fewer entries than the tree state");
                    current = make(true);
                    leaves.push_back(current);
                    current->navigation.leaf.previous = previous;
                    if (previous) previous->navigation.leaf.next = current;
                    else t.firstLeaf_ = current;
                    previous = current;
                    filled = 0;
                    offsets = sum.synthetic ? options.leafOffsets(sizes[leaves.size() - 1]) : nullptr;
                }
                Entry& e = current->slots[filled];
                e = Entry{};
                prefixFrom(e.beginInline, begin);
                e.begin = beginRef;
                e.end = endRef;
                if (!Tree::endSharesStorage(e)) {
                    new (&e.payload.endInline) Tree::KeyPrefix{};
                    prefixFrom(e.payload.endInline, end);
                }
                e.cv = cv;
                if (offsets) {
                    if (offsets[filled] > std::numeric_limits<int64_t>::max() - floor) throw std::overflow_error("synthetic CV overflow");
                    e.cv = floor + offsets[filled];
                    if (e.cv > lastWrite) throw std::runtime_error("synthetic CV beyond the last written version");
                    ++sum.replacedEntries;
                }
                sum.expiredEntries += e.cv <= floor;
                current->order.insert(filled, filled);
                current->occupied |= uint32_t{ 1 } << filled;
                ++filled;
                ++sum.leafEntries;
            }
        }
        if (leaves.size() != sizes.size() || filled != sizes.back()) throw std::runtime_error("layout has more entries than the tree state");
        s.tag("AGEM");
        for (uint64_t i = 0; i < counts[4]; ++i) { s.get<uint32_t>(); s.get<uint8_t>(); s.get<int64_t>(); }
        s.tag("AGEX");
        for (uint64_t i = 0; i < counts[5]; ++i) { s.get<uint32_t>(); s.get<uint32_t>(); s.get<int64_t>(); }
        s.tag("FREE");
        for (uint64_t i = 0; i < counts[6]; ++i) s.get<uint32_t>();
        s.tag("RETD");
        for (uint64_t i = 0; i < counts[7]; ++i) s.get<uint32_t>();
        s.tag("STAT");
        for (unsigned i = 0; i < 20; ++i) s.get<uint64_t>();
        s.tag("END\0");
        const uint32_t computed = s.crc ^ 0xFFFFFFFFu;
        if (s.get<uint64_t>() != computed) throw std::runtime_error("tree state CRC-32 mismatch");
        sum.bytes = s.bytes;
        sum.keyStreamCrc = stream.value();

        for (Node* n : leaves) {
            Version m = NoVersionValue();
            for (unsigned r = 0; r < n->size(); ++r) m = std::max(m, n->at(r).cv);
            n->maxCV = m;
            n->critical = n->size() == Tree::Capacity;
            sum.criticalLeaves += n->critical;
        }
        std::vector<Node*> children(leaves), internal;
        internal.reserve(layoutInternal);
        for (size_t l = levels.size(); l-- > 0;) {
            std::vector<Node*> parents;
            parents.reserve(levels[l].size());
            size_t next = 0;
            for (uint8_t fanout : levels[l]) {
                Node* p = make(false);
                internal.push_back(p);
                parents.push_back(p);
                Version m = NoVersionValue();
                unsigned critical = 0;
                for (unsigned j = 0; j < fanout; ++j) {
                    Node* c = children[next++];
                    const Entry* first = nullptr;
                    for (unsigned r = 0; r < c->size() && !first; ++r) {
                        const Entry& x = c->at(r);
                        if (c->leaf ? x.cv > floor : x.payload.link.firstCV > floor) first = &x;
                    }
                    const bool live = first;
                    if (!first) first = &c->at(0);
                    Entry& e = p->slots[j];
                    e = Entry{};
                    e.beginInline = first->beginInline;
                    e.begin = first->begin;
                    e.cv = c->maxCV;
                    e.payload.link.child = c;
                    e.payload.link.firstCV = c->leaf ? first->cv : first->payload.link.firstCV;
                    sum.separatorsLive += live;
                    sum.separatorsExpired += !live;
                    p->order.insert(j, j);
                    p->occupied |= uint32_t{ 1 } << j;
                    c->parent = p;
                    c->parentSlot = uint8_t(j);
                    critical += c->critical;
                    m = std::max(m, c->maxCV);
                }
                p->maxCV = m;
                p->criticalChildren = uint8_t(critical);
                if (Tree::Capacity - fanout < critical) ++sum.slackViolations;
                p->critical = Tree::Capacity - fanout == critical;
                sum.criticalInternal += p->critical;
            }
            if (next != children.size()) throw std::runtime_error("layout level does not cover its children");
            children.swap(parents);
        }
        if (children.size() != 1) throw std::runtime_error("layout root");
        t.root_ = children[0];
        t.root_->parent = nullptr;
        t.validationMinRV_ = floor;
        t.lastWriteCV_ = lastWrite;
        t.inlineIdentity_ = inlineIdentity;
        sum.internalNodes = internal.size();
        sum.leaves = leaves.size();
        std::vector<Node*> all(internal);
        all.insert(all.end(), leaves.begin(), leaves.end());
        std::stable_sort(all.begin(), all.end(), [](const Node* a, const Node* b) { return a->maxCV < b->maxCV; });
        for (Node* n : all) {
            const Version cv = n->maxCV;
            n->maxCV = NoVersionValue();
            t.pool_.raiseAge(n, cv, PageUse::Index);
        }
        sum.freeNodes = t.freeNodes_;
        return sum;
    }
};
