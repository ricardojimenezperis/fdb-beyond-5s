#pragma once

#include "PagedTemporalSkipList.h"
#include "CleanupBeforeStructure.h"
#include "LocalFictitious.h"
#include "Redistribution.h"
#include "PairIndex.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <limits>
#include <type_traits>
#include <vector>

// Local cleanup: 0 circular head and slot cursor; 1 bitmap cleanup; 2 bitmap cleanup, first free slot;
// 3 (default) as 2, with the write's anchor leaf left to overwrite's rank sweep.
#ifndef PAGED_BTREE_BITMAP_CLEANUP
#define PAGED_BTREE_BITMAP_CLEANUP 3
#endif
static_assert(PAGED_BTREE_BITMAP_CLEANUP >= 0 && PAGED_BTREE_BITMAP_CLEANUP <= 3,
              "Invalid bitmap cleanup arm");

#ifndef PAGED_BTREE_NODE_PREFIX
#define PAGED_BTREE_NODE_PREFIX 0
#endif

// Keys <= InlineKeyBytes are stored only in nodes. Longer keys retain heap records.
// Disabling this policy stores a heap copy of every key.
#ifndef PAGED_BTREE_INLINE_ONLY_KEYS
#define PAGED_BTREE_INLINE_ONLY_KEYS 1
#endif

#ifndef PAGED_BTREE_INLINE_KEY_BYTES
#define PAGED_BTREE_INLINE_KEY_BYTES 16
#endif
static_assert(PAGED_BTREE_INLINE_KEY_BYTES >= 0, "Inline key size must be nonnegative");

#ifndef PAGED_BTREE_WRITE_PREFETCH
#define PAGED_BTREE_WRITE_PREFETCH 1
#endif
#ifndef PAGED_BTREE_CLEANUP_PREFETCH
#define PAGED_BTREE_CLEANUP_PREFETCH 1
#endif
#ifndef PAGED_BTREE_NODE_CAPACITY
#define PAGED_BTREE_NODE_CAPACITY 23
#endif
#ifndef PAGED_BTREE_PERMUTATION_SIMD
#define PAGED_BTREE_PERMUTATION_SIMD 1
#endif

#ifndef PAGED_BTREE_PROB_FUSION_MAX_RESULT
#define PAGED_BTREE_PROB_FUSION_MAX_RESULT (PAGED_BTREE_NODE_CAPACITY * 3 / 4)
#endif

#ifndef PAGED_BTREE_PROB_FUSION_MAX_SOURCE
#define PAGED_BTREE_PROB_FUSION_MAX_SOURCE 6
#endif

namespace paged_btree {

// Empty at K=0, so disabling the prefix need not grow the entry layout.
template <size_t N> struct InlineKeyPrefix {
    std::array<uint8_t, N> bytes{};
    uint8_t* data() { return bytes.data(); }
    const uint8_t* data() const { return bytes.data(); }
};
template <> struct InlineKeyPrefix<0> {
    uint8_t* data() { return nullptr; }
    const uint8_t* data() const { return nullptr; }
};

// Logical ranks hold byte-sized physical slot numbers, leaving entries in place
class KeyPermutation {
public:
    static constexpr unsigned Capacity = PAGED_BTREE_NODE_CAPACITY;
    static_assert(Capacity == 7 || Capacity == 15 || Capacity == 19 || Capacity == 23 || Capacity == 26 || Capacity == 31,
                  "Supported node capacities are 7, 15, 19, 23, 26 and 31");
    unsigned size() const { return order_[Capacity]; }
    unsigned slotAt(unsigned rank) const;
    unsigned rankOfSlot(unsigned slot) const;
    void insert(unsigned rank, unsigned slot);
    void erase(unsigned rank);
private:
    // The size occupies byte Capacity, outside the ranks; padding bounds each SIMD load
    static constexpr unsigned StorageBytes = ((Capacity + 1 + 15) / 16) * 16;
    std::array<uint8_t, StorageBytes> order_{};
};

class PagedBTreeConflictSet {
    friend struct ::TreeStateExporter;
public:
    // ABI/layout parameter: every translation unit must use the same value.
    static constexpr size_t InlineKeyBytes = PAGED_BTREE_INLINE_KEY_BYTES;
    static constexpr bool NodePrefixEnabled = PAGED_BTREE_NODE_PREFIX != 0;
    static constexpr size_t NodePrefixBytes = 12;
    static constexpr bool InlineOnlyKeys = PAGED_BTREE_INLINE_ONLY_KEYS != 0;
    static constexpr unsigned Capacity = KeyPermutation::Capacity;
    static constexpr uint32_t SlotMask = (uint32_t{1} << Capacity) - 1;
    static constexpr unsigned WriteSearchWidth = 16;
    static constexpr unsigned MaxHeight = 64;

    struct Write {
        StringRef begin;
        StringRef end;
        ConflictKind kind = ConflictKind::Range;
    };
    struct PrefixMatch {
        int order = 0;
        bool ready = false;
    };
    struct Query {
        Version readVersion;
        StringRef begin;
        StringRef end;
        bool conflict = false;
        // Position of this query's end in the sorted endpoint vector. Set by
        // endpoint preparation; existing read paths leave it unused.
        uint32_t endIndex = UINT32_MAX;
    };
    // FDB's StringRef is 12 bytes with 4-byte alignment (40-byte queries); 64-bit stubs give 16 and 48
    struct QueryWithoutEndIndex {
        Version readVersion;
        StringRef begin;
        StringRef end;
        bool conflict;
    };
    static_assert(sizeof(Query) == sizeof(QueryWithoutEndIndex),
                  "Query::endIndex must fit in the existing tail padding");
    // Structural counters are unconditional. Search counters require PAGED_RESOLVER_SEARCH_STATS;
    // disabling them preserves the layout and leaves their values zero.
    struct Stats {
        static constexpr bool searchEnabled = PAGED_RESOLVER_SEARCH_STATS;
        uint64_t splits = 0;
        uint64_t internalSplits = 0;
        uint64_t intervalInsertions = 0;
        uint64_t merges = 0;
        uint64_t internalMerges = 0;
        uint64_t mergeCandidatesExamined = 0;
        uint64_t cleanupLeavesVisited = 0; // Legacy statistic: no global leaf sweep; remains zero.
        uint64_t indexPagesReleased = 0;
        // Fragments trimmed in place and the extra entry of a split interval
        uint64_t leftFragmentsTrimmed = 0;
        uint64_t rightFragmentsTrimmed = 0;
        uint64_t splitFragmentsInserted = 0;
        uint64_t localHeadEntriesReclaimed = 0, localHeadChildrenReclaimed = 0;
        uint64_t emptiedAnchorsRemoved = 0;
        uint64_t reclaimCalls = 0;
        uint64_t reclaimAgeCandidates = 0;
        uint64_t reclaimNodeCandidates = 0;
        uint64_t reclaimLeavesVisited = 0;
        uint64_t heapPagesReclaimed = 0;
        uint64_t expiredIntervals = 0;
        uint64_t overwrittenIntervals = 0;
        uint64_t discardedWrites = 0;
        uint64_t separatorRefreshes = 0;
        // Includes all read/write comparisons, gated by PAGED_RESOLVER_SEARCH_STATS.
        uint64_t inlineKeyDecisions = 0;
        uint64_t keyComparisonFallbacks = 0;
        uint64_t heapKeyLoads = 0;
        uint64_t nodePrefixChecks = 0, nodePrefixReuses = 0;
        uint64_t separatorEncodeHeapLoads = 0, separatorEncodedReuses = 0;
        uint64_t visitedNodes = 0;
        uint64_t queries = 0;
        // Diagnostic per-write work counts, enabled by PAGED_RESOLVER_SEARCH_STATS.
        uint64_t writes = 0;
        uint64_t descents = 0;
        uint64_t descentNodes = 0;
        uint64_t childRankProbes = 0;
        uint64_t liveRankEntries = 0;
        uint64_t refreshes = 0;
        uint64_t refreshEntries = 0;
        uint64_t minimumCalls = 0;
        uint64_t minimumEntries = 0;
        uint64_t overwriteLeaves = 0;
        uint64_t overwriteEntries = 0;
        uint64_t leafSearchProbes = 0;
        uint64_t slotInserts = 0;
        uint64_t slotErases = 0;
        uint64_t nodesReserved = 0;
        uint64_t nodesTaken = 0;
        uint64_t fictitiousCreated = 0, fictitiousReleased = 0, fictitiousFusionConsumed = 0, fictitiousRetired = 0, fictitiousSplits = 0;
        uint64_t pressureEvents = 0, pressureLocalAttempts = 0, pressureGlobalAttempts = 0, pressureLocalFusions = 0, pressureGlobalFusions = 0;
        uint64_t pressureNodesMadeAvailable = 0, pressureFallbacks = 0, pressureCandidatesInspected = 0, pressureCandidatesMax = 0;
        uint64_t pressureSkippedNotSameParent = 0, pressureSkippedLimits = 0, pressureFusionNotApplied = 0;
        uint64_t redistLeftFree = 0, redistLeftFictitious = 0, redistRightFree = 0, redistRightFictitious = 0, redistIncomingMoved = 0;
        uint64_t redistCrossParent = 0, redistRejectedFull = 0, redistRejectedSlack = 0, redistNoNeighbour = 0;
        uint64_t pairIndexUpdates = 0, pairIndexFlushes = 0, pairIndexDirtyLeaves = 0, pairIndexDirtyMax = 0;
        uint64_t pairIndexStale = 0, pairIndexBlocked = 0, pairIndexEmpty = 0;
        uint64_t queryLeafEntries = 0;
        // The grouped write search, compiled only with PAGED_RESOLVER_SEARCH_STATS
        uint64_t writeSearchPasses = 0;
        uint64_t writeSearchNodes = 0;
        uint64_t writeSearchLeaves = 0;
        uint64_t writeSearchMaxActive = 0;
        uint64_t writeHintHits = 0;
        uint64_t writeHintRightMoves = 0;
        uint64_t writeHintRestarts = 0;
        // Hints rejected because their node changed incarnation or saturated
        uint64_t writeHintStale = 0;
        uint64_t writeRootSearches = 0;
        // Most tree nodes retired at once within one write batch, between drains (batch end or a reclamation)
        uint64_t writeBatchMaxRetired = 0;
    };
    // Node and entry counts by depth; root depth is zero. Diagnostic traversal only.
    struct Level {
        uint64_t nodes = 0;
        uint64_t entries = 0;
        uint64_t liveEntries = 0;
    };

    // Pool occupancy depends on key lengths, rewrite frequency, insertion order and node layout.
    // Measurements exclude auxiliary storage and unused pages; they are not allocation quotas.
    // Shares are permanent. Automatic assignment reserves minima even for inline-only workloads;
    // index pages never transfer to the key share.
    static constexpr unsigned ProbFusionMaxMergedEntries = PAGED_BTREE_PROB_FUSION_MAX_RESULT;
    static_assert(ProbFusionMaxMergedEntries >= 2 && ProbFusionMaxMergedEntries <= Capacity,
                  "probFusion result limit must be between 2 and Capacity");
    static constexpr unsigned ProbFusionMaxSourceEntries = PAGED_BTREE_PROB_FUSION_MAX_SOURCE;
    static_assert(ProbFusionMaxSourceEntries >= 1 && ProbFusionMaxSourceEntries <= Capacity,
                  "probFusion source limit must be between 1 and Capacity");
    // Index is the CURRENT occupied-entry count; free entries = Capacity - count.
    // Rounded HP same-parent success rates at result limit 17; attempt heuristic p=q.
    // maxSourceEntries (default 6) independently caps this table before any RNG call.
    // Other result limits may be tested, but these rates then cease to describe q.
    static constexpr std::array<uint32_t, Capacity + 1> ProbFusionAttemptPer65536 = [] {
        constexpr unsigned percent[] = {0,78,73,68,64,59,52,47,41,35,29,22,17,12,7,3,1};
        std::array<uint32_t, Capacity + 1> attempts{};
        for (unsigned occupied = 1; occupied <= Capacity && occupied < sizeof(percent) / sizeof(percent[0]); ++occupied)
            attempts[occupied] = (percent[occupied] * 65536u + 50u) / 100u;
        return attempts;
    }();
    struct ProbFusionConfig {
        unsigned maxMergedEntries = ProbFusionMaxMergedEntries;
        unsigned maxSourceEntries = ProbFusionMaxSourceEntries;
    };
    // Configuration is per tree, never per node. Configure outside write batches.
    void configureProbFusion(const ProbFusionConfig& config);
    struct FictitiousLeafConfig {
        bool enabled = false;
        unsigned rootReserve = 11;
        unsigned splitReserve = 11;
        uint64_t choiceSeed = 0;
        unsigned pressureMergedLimit = 0;
        bool prioritizeReclaimable = false;
        bool startupOnly = false;
        Version realHistoryStart = 0;
    };
    void configureFictitiousLeaves(const FictitiousLeafConfig& config);
    const FictitiousLeafConfig& fictitiousLeaves() const { return fictitious_; }
    bool fictitiousReplenishmentStopped() const { return fictitiousReplenishmentStopped_; }
    Version fictitiousStopFloor() const { return fictitiousStopFloor_; }
    std::function<void(bool global, uint64_t inspected, unsigned removeLeft, unsigned removeRight, bool success)> pressureObserver;
    bool checkPairIndex();
    uint64_t pairIndexSize() const { return pairIndexSize_; }
    void leafOccupancy(std::array<uint64_t, KeyPermutation::Capacity + 1>& real, std::array<uint64_t, KeyPermutation::Capacity + 1>& effective,
                       uint64_t& fictitiousPresent) const;
    uint64_t freeNodeCount() const { return freeNodes_; }

    struct MemoryConfig {
        uint64_t heapPages = UINT64_MAX;
        uint64_t indexPages = UINT64_MAX;
        unsigned minimumPercent = 10; // Floor to whole pages, at least one each in auto mode.
        // Zero heap pages require InlineOnlyKeys and an exclusively inline workload.
        PoolBacking backing = PoolBacking::Normal;
    };
    explicit PagedBTreeConflictSet(uint64_t limitPages, Version initialValidationMinRV);
    PagedBTreeConflictSet(uint64_t limitPages, Version initialValidationMinRV, MemoryConfig memory);
    ~PagedBTreeConflictSet();
    PagedBTreeConflictSet(const PagedBTreeConflictSet&) = delete;
    PagedBTreeConflictSet& operator=(const PagedBTreeConflictSet&) = delete;

    void addRange(Version cv, StringRef begin, StringRef end);
    void addPoint(Version cv, StringRef key);
    // Writes and key bytes must remain valid throughout the call. Search uses key order;
    // modification preserves input order. Reclamation may checkpoint between insertions:
    // discard hints, recycle nodes and rebuild search. Allocation failure is fatal.
    void addWrites(Version cv, const Write* writes, size_t count);
    bool hasConflict(Version readVersion, StringRef begin, StringRef end);

    // Callers must admit every query before calling the batch API
    // An inadmissible query throws before any result is changed
    void hasConflicts(Query* queries, size_t count);
    struct OrderedEndpoint {
        uint32_t query;
        uint32_t lcp; // Exact byte LCP with the previous supplied endpoint; first is zero.
        bool end;
    };
    // Input must contain each nonempty query's two endpoints exactly once, ordered
    // by key and end-before-begin. Equal-key/same-kind query IDs may be in any order.
    void hasConflictsOrdered(Query* queries, size_t count,
                             const OrderedEndpoint* endpoints, size_t endpointCount);

    struct ReadPipelineConfig {
        // Explicit per-machine configuration. No cache space is physically reserved.
        size_t l2Bytes = 1024 * 1024;
        size_t l2ReserveBytes = 64 * 1024;
        size_t lineBytes = 64;
        unsigned breadthBoundaryLevels = 2; // Root is level 1; zero selects linear cuts everywhere.
        unsigned sweepGroupSize = 3;
    };
    struct ReadPipelineStats {
        uint64_t nodeGroups = 0, rootQueries = 0;
        uint64_t l1Requests = 0, l2Requests = 0;
        uint64_t maxActiveNodes = 0, maxL2PlannedBytes = 0;
        uint64_t scratchBytes = 0;
        uint64_t sortedEndpointCount = 0, endpointSortComparisons = 0;
        uint64_t endpointLcpUpdates = 0, presortedEndpointCount = 0;
        uint64_t blockLcpInlineBytesSkipped = 0, blockLcpFullBytesSkipped = 0;
        uint64_t endpointBoundaryComparisons = 0, openQueryChecks = 0;
        uint64_t maxOpenQueries = 0, sortedNodesSkipped = 0;
        uint64_t breadthLevels = 0, breadthQueuedNodes = 0;
        uint64_t breadthNodeLines = 0, breadthSuffixLines = 0, breadthHeapResolutions = 0, breadthOversizeGroups = 0;
        uint64_t breadthHeapLines = 0, breadthQueryLines = 0;
        uint64_t blockRotations = 0;
        uint64_t blockEntriesFiltered = 0, blockAddressReuses = 0;
        uint64_t groupSweepPasses = 0, groupSweepComparisons = 0;
        uint64_t groupSweepSuffixBytes = 0;
        // Sum of these three equals endpointBoundaryComparisons (sort excluded).
        uint64_t sortedInternalComparisons = 0, sortedLeafBeginComparisons = 0, sortedLeafEndComparisons = 0;
        uint64_t sortedEmptyChildrenSkipped = 0, sortedOldChildrenSkipped = 0;
        // Depth zero is the root. Populated only in statistics builds.
        std::array<uint64_t, MaxHeight> sortedInternalComparisonsByDepth{};
    };
    // Validate configuration before modifying
    // query flags; query bytes must remain valid throughout the call.
    void hasConflicts(Query* queries, size_t count, const ReadPipelineConfig& config);
    const ReadPipelineStats& readPipelineStats() const { return readPipelineStats_; }
    void configureReader(unsigned sweepGroupSize);
    ReadPipelineConfig readerConfig() const;

    // Compatibility entry point: local expiry and capacity reclamation do the work.
    // No global sweep or node merging; budgets are ignored.
    uint64_t cleanup(unsigned leafBudget = 64, unsigned mergeCandidates = 8,
                     bool memoryPressure = false);

    Version validationMinRV() const { return validationMinRV_; }
    const PageBudget& budget() const { return pool_.budget(); }
    const PagePool::BackingReport& poolBacking() const { return pool_.backing(); }
    // History pages reclaimed under pool pressure so far, for the Resolver's metrics
    PageID reclaimedHistoryPages() const { return heap_.lastReclaimedPageID(); }
    const Stats& stats() const { return stats_; }
    // No auxiliary cleanup queues remain. CV metadata is accounted separately.
    uint64_t cleanupMetadataBytes() const { return 0; }
    uint64_t by5AgeBytes() const { return pool_.ageMetadataBytes(); }
    uint64_t by5PartitionBytes() const { return pool_.partitionMetadataBytes(); }
    uint64_t by5PageTableBytes() const { return heap_.by5PageTableBytes(); }
    uint64_t by5MetadataBytes() const { return by5AgeBytes() + by5PartitionBytes() + by5PageTableBytes(); }
    uint64_t by5PoolPhysicalBytes() const {
        const auto& b = pool_.backing();
        return b.heap.mappedBytes + b.index.mappedBytes + b.shared.mappedBytes;
    }
    uint64_t by5WriteScratchBytes() const {
        return writeOrder_.capacity() * sizeof(size_t) + writeHints_.capacity() * sizeof(LeafHint) +
               writePending_.capacity() * sizeof(WriteSearchWork) + writePrefixMatches_.capacity() * sizeof(WritePrefix);
    }
    unsigned height() const;
    // Diagnostic tree traversal; do not call inside timed batch processing.
    uint64_t occupancy(bool leaves, unsigned count) const;
    uint64_t leafCount() const { return leafCount_; }
    uint64_t nodeCount() const;
    // Diagnostic traversal of every node; excludes batch processing.
    std::vector<Level> levels() const;
    // Physical sizes of this build's node layout, for reports
    struct Layout {
        size_t permutationBytes, entryBytes, nodeBytes, nodeAlignment, nodesPerPage;
    };
    static Layout layout();

private:
    static constexpr Version NoVersion = std::numeric_limits<Version>::lowest();
    struct Node;
    struct NodePage;
    using KeyPrefix = InlineKeyPrefix<InlineKeyBytes>;
    struct Entry {
        [[no_unique_address]] KeyPrefix beginInline{};
        // Full endpoint lengths. Point endpoints share one heap address, with
        // end.length == begin.length + 1 (the additional byte is zero).
        HeapRef begin{};
        // Leaf: end key. Internal: end.length is the inline suffix offset
        // (zero = full-key prefix); the other end fields are unused/zero.
        HeapRef end{};
        Version cv = NoVersion;
        struct Link { Node* child = nullptr; Version firstCV = NoVersion; };
        // Node::leaf selects the interpretation. Leaf ranges use endInline;
        // leaf points reuse beginInline and never store a second prefix.
        union Payload {
            Link link;
            KeyPrefix endInline;
            Payload() : link{} {}
        } payload;
    };
    static_assert(std::is_trivially_copyable_v<Entry>);
    struct alignas(64) Node : ReclaimLink {
        KeyPermutation order;
        NodePage* owner = nullptr;
        Node* parent = nullptr;
        struct LeafLinks { Node* previous = nullptr; Node* next = nullptr; };
        struct ReferencePrefix {
            std::array<uint8_t, NodePrefixBytes> bytes{};
            uint8_t length = 0;
            bool initialized = false;
        };
        // Internal nodes do not use leaf links. Reuse their 16 bytes rather
        // than enlarging nodes and reducing the number of nodes per page.
        union Navigation {
            LeafLinks leaf;
            ReferencePrefix prefix;
            Navigation() : leaf{} {}
        } navigation;
        uint32_t occupied = 0;
        uint8_t cursor = 0;
        uint8_t parentSlot = 0;
        // Children whose critical flag is set; kept by refresh and by refreshEntry
        uint8_t criticalChildren = 0;
        bool leaf = true;
        bool critical = false;
        bool allocated = false;
        uint8_t reclaimHead = 0; // physical circular head; fits existing header padding
        struct Fictitious {
            uint8_t fictitious = 0;
            bool pairDirty = false;
            by5v5::PairLink pair{};
        } fict{};
        std::array<Entry, Capacity> slots{};
        Entry& at(unsigned rank) { return slots[order.slotAt(rank)]; }
        const Entry& at(unsigned rank) const { return slots[order.slotAt(rank)]; }
        unsigned size() const { return order.size(); }
    };
    struct NodePage {
        NodePage* previous;
        NodePage* next;
        // Allocated nodes, including unpublished reservations; excludes retired nodes
        uint32_t live;
        NodePage* emptyPrevious = nullptr;
        NodePage* emptyNext = nullptr;
    };
    struct Reservation {
        std::array<Node*, 3> nodes{};
        unsigned size = 0;
    };
    // Hints identify leaves by address and incarnation. Validate identity, membership and
    // key range before insertion; permanent index pages keep addresses readable after reuse.
    struct LeafHint {
        Node* node = nullptr;
        uint32_t incarnation = 0;
    };
    static constexpr uint32_t SaturatedIncarnation = std::numeric_limits<uint32_t>::max();
    struct InsertCursor {
        Node* leaf;
        unsigned rank;
    };
    struct WriteSearchWork {
        Node* node = nullptr;
        size_t first = 0;
        size_t end = 0;
        bool prepare = true;
    };

    ProbFusionConfig probFusion_{};
    FictitiousLeafConfig fictitious_{};
    uint64_t fictitiousChoice_ = 0;
    bool fictitiousReplenishmentStopped_ = false;
    Version fictitiousStopFloor_ = 0;
    void observeFictitiousFloor();
    by5v5::PairIndex<KeyPermutation::Capacity> pairIndex_{};
    std::vector<Node*> pairDirty_;
    uint64_t pairIndexSize_ = 0;
    void setFictitious(Node* node, unsigned count);
    void markPair(Node* node) {
        if (!fictitious_.enabled || node->fict.pairDirty) return;
        node->fict.pairDirty = true;
        pairDirty_.push_back(node);
    }
    unsigned pressureMergedLimit() const { return fictitious_.pressureMergedLimit ? fictitious_.pressureMergedLimit : probFusion_.maxMergedEntries; }
    unsigned pressureSourceLimit() const { return fictitious_.pressureMergedLimit ? Capacity : probFusion_.maxSourceEntries; }
    unsigned pairPriority(const Node* left) const;
    void setPair(Node* left, unsigned priority);
    void flushPairs();
    bool pressureFusion(Node* target);
    bool tryPressurePair(Node* left, Node* right, bool global);
    bool promotionSafe(const Node* node) const;
    bool redistribute(InsertCursor& cursor, const Entry& interval);
    struct FusionCandidate {
        Node* node = nullptr;
        uint32_t incarnation = 0;
        unsigned before = 0;
    };
    void maybeProbFusion(const FusionCandidate& candidate);
    bool fuseLeafNeighbour(Node* node, bool previous, unsigned mergedLimit = 0);
    Version validationMinRV_;
    Version lastWriteCV_ = NoVersion;
    PagePool pool_;
    TemporalHeap heap_;
    NodePage* pages_ = nullptr;
    NodePage* emptyPages_ = nullptr;
    Node* free_ = nullptr;
    uint64_t freeNodes_ = 0;
    Node* root_ = nullptr;
    Node* firstLeaf_ = nullptr;
    uint64_t inlineIdentity_ = 0;
    uint64_t leafCount_ = 0;
    uint64_t nodeCount_ = 0; // Published/taken nodes; excludes unused reservations.
    mutable Stats stats_;
    // Reclamation may drain retired nodes without closing the batch or discarding hints.
    // Retired and free nodes reuse their detached maxCV link.
    Node* retired_ = nullptr;
    Node* retiredTail_ = nullptr;
    uint64_t retiredCount_ = 0;
    bool writeBatchActive_ = false;
    // Set while reclaim() runs: nodes it removes go straight to the free queue, not to retired_
    bool reclaiming_ = false;
    // Scope exit returns unpublished reservations, discards hints and enables retired-node reuse.
    struct WriteBatchScope {
        PagedBTreeConflictSet& tree;
        Reservation* reservation = nullptr;
        explicit WriteBatchScope(PagedBTreeConflictSet& owner) : tree(owner) {
            ASSERT(!tree.writeBatchActive_);
            tree.writeBatchActive_ = true;
        }
        ~WriteBatchScope() {
            if (reservation) tree.releaseReservation(*reservation);
            tree.finishWriteBatch();
        }
        WriteBatchScope(const WriteBatchScope&) = delete;
        WriteBatchScope& operator=(const WriteBatchScope&) = delete;
    };
    // Reused between batches, with no growth during the grouped traversal
    std::vector<size_t> writeOrder_;
    std::vector<LeafHint> writeHints_;
    std::vector<WriteSearchWork> writePending_;
    struct WritePrefix { const Node* node = nullptr; PrefixMatch match; };
    std::vector<WritePrefix> writePrefixMatches_;

    struct SortedEndpoint {
        StringRef key;
        uint32_t query;
        bool end;
    };
    std::vector<SortedEndpoint> sortedEndpoints_;
    // Indexed by 2 * original query id + endpoint kind, stable across sort moves.
    // After sorting, each live endpoint stores its LCP with its immediate predecessor.
    std::vector<uint32_t> sortedEndpointLcp_;
    std::vector<uint32_t> openQueries_;
    struct BlockNodeState {
        uint64_t boundaries = 0, heap = 0;
        // Ordinary path: address-cache offset; heap becomes (common LCP << 32)|1.
        // Grouped path uses the same storage for two indices. initialize()
        // already bounds endpoints to UINT32_MAX; cache frames cannot exceed
        // the number of nonempty endpoint frames. heap marks exact inline ties.
        struct GroupPosition { uint32_t endpoint, cacheFrame; };
        union {
            size_t addressOffset = std::numeric_limits<size_t>::max();
            GroupPosition group;
        };
    };
    struct ReadBlock {
        size_t first = 0, end = 0, endpointBase = 0, bytes = 0;
        std::vector<uint8_t> positions;
        std::vector<uint64_t> pendingFrames; // Grouped path only.
        std::vector<BlockNodeState> nodes;
        std::vector<const uint8_t*> addresses;
    };
    std::array<ReadBlock, 2> readBlocks_;
    struct BlockReader;
    static constexpr size_t MaxBlockQueries = UINT32_MAX / 2;
    void readBlocks(Query* queries, size_t count, const ReadPipelineConfig& cfg,
                    const OrderedEndpoint* endpoints = nullptr, size_t endpointCount = 0,
                    bool presorted = false);
    struct BreadthFrame {
        Node* node = nullptr;
        size_t first = 0, end = 0;
        Version minimumBeginRV = std::numeric_limits<Version>::max();
    };
    std::vector<BreadthFrame> breadthCurrent_, breadthNext_;
    ReadPipelineStats readPipelineStats_{};
    unsigned readerGroupSize_ = 3;

    static size_t nodeOffset();
    static size_t nodesPerPage();
    Node* allocateNode();
    void releaseNode(Node* node);
    void recycleNode(Node* node);
    void finishWriteBatch();
    void drainRetired();
    void addFree(Node* node);
    void removeFree(Node* node);
    void releaseEmptyPages();
    void linkEmptyPage(NodePage* page);
    void unlinkEmptyPage(NodePage* page);
    Node* take(Reservation& reservation, bool leaf);
    bool reserve(Reservation& reservation, unsigned required);
    void releaseReservation(Reservation& reservation);
    static void reclaimThunk(void* context, PageUse use);
    void reclaim(PageUse use);

    static void prefetchCleanupNode(Node* node, bool wholeNode);
    void insertSlot(Node* node, unsigned rank, const Entry& entry);
    void eraseSlot(Node* node, unsigned rank);
    void refresh(Node* node);
    Version entriesMaximum(const Node* node) const;
    void refreshUp(Node* node);
    void refreshEntry(Node* parent, Node* child, bool childWasCritical);
    bool refreshSeparator(Entry& entry);
    Entry minimum(Node* node, const Node** encodedAt = nullptr);
    StringRef key(const HeapRef& ref) const;
    static bool isInlineOnly(const HeapRef& ref) {
        return InlineOnlyKeys && ref.offset == std::numeric_limits<uint32_t>::max();
    }
    HeapRef inlineReference(size_t length);
    // A K-byte point's successor is prefix + one implicit zero. Copies of
    // that endpoint (e.g. a surviving range boundary) keep this representation.
    static bool implicitZero(const HeapRef& ref) {
        return isInlineOnly(ref) && ref.length > InlineKeyBytes;
    }
    static int compareImplicitZero(const HeapRef& ref, StringRef rhs);
    using KeyScratch = std::array<uint8_t, InlineKeyBytes + 1>;
    void reclaimNode(Node* node);
    void discardExpiredRoot();
    bool purgeHead(Node* leaf);
    bool purgeInsertionHead(Node* node);
    static void cacheKey(KeyPrefix& prefix, StringRef value);
    static bool endSharesStorage(const Entry& entry) {
        return entry.begin.pageID == entry.end.pageID && entry.begin.offset == entry.end.offset;
    }
    static const KeyPrefix& endPrefix(const Entry& entry) {
        return endSharesStorage(entry) ? entry.beginInline : entry.payload.endInline;
    }
    static void setEnd(Entry& entry, const HeapRef& ref, const KeyPrefix& prefix);
    static void setBegin(Entry& entry, const HeapRef& ref, const KeyPrefix& prefix);
    StringRef storedKey(const HeapRef& ref, const KeyPrefix& prefix, KeyScratch& scratch) const;
    // Return the ordering of the stored key versus rhs. A cached heap address is
    // optional and only valid during a read visit with no writes/reclamation.
    int compareKey(const HeapRef& ref, const KeyPrefix& prefix, StringRef rhs,
                   const uint8_t** cachedFull = nullptr) const;
    int compareBegin(const Entry& entry, StringRef rhs, const uint8_t** cachedFull = nullptr) const {
        return compareKey(entry.begin, entry.beginInline, rhs, cachedFull);
    }
    int compareEnd(const Entry& entry, StringRef rhs) const {
        return compareKey(entry.end, endPrefix(entry), rhs);
    }
    void initializeReferencePrefix(Node* node);
    void encodeSeparator(Node* node, Entry& entry);
    void assignSeparator(Node* node, Entry& entry, const Entry& first, const Node* encodedAt = nullptr);
    Entry plainSeparator(const Node* node, const Entry& entry) const;
    static size_t separatorInlineBytes(const Entry& entry) {
        return InlineKeyBytes + (NodePrefixEnabled ? entry.end.length : 0);
    }
    // matched is the first byte beyond inline coverage; only a tied prefix needs the heap.
    __attribute__((always_inline)) inline bool separatorPrefix(
        const Node* node, const Entry& entry, StringRef rhs, int& order, size_t& matched, PrefixMatch* saved = nullptr) const {
        const auto& ref = entry.begin;
        const size_t common = std::min<size_t>(ref.length, rhs.size());
        size_t skip = 0;
        order = 0;
        if constexpr (NodePrefixEnabled) {
            skip = entry.end.length;
            if (skip) {
                ASSERT(!node->leaf && node->navigation.prefix.initialized &&
                       skip == node->navigation.prefix.length && ref.length >= skip);
                const size_t n = std::min(skip, common);
                if (saved && saved->ready) {
                    order = saved->order;
                    if constexpr (Stats::searchEnabled) ++stats_.nodePrefixReuses;
                } else {
                    if constexpr (Stats::searchEnabled) ++stats_.nodePrefixChecks;
                    if (n) order = std::memcmp(node->navigation.prefix.bytes.data(), rhs.begin(), n);
                    if (!order && common < skip) order = 1; // query ends inside the reference
                    if (saved) { saved->order = order; saved->ready = true; }
                }
                matched = n;
                if (order) return true;
            }
        } else { (void)node; }
        const size_t local = std::min(InlineKeyBytes, common - skip);
        if constexpr (InlineKeyBytes)
            if (local) order = std::memcmp(entry.beginInline.data(), rhs.begin() + skip, local);
        matched = skip + local;
        if (!order && common > matched) {
            if (!implicitZero(ref)) return false;
            ASSERT(!skip); // compressed implicit-zero keys fit completely
            order = compareImplicitZero(ref, rhs);
        }
        if (!order) order = (ref.length > size_t(rhs.size())) - (ref.length < size_t(rhs.size()));
        return true;
    }
    int compareSeparator(const Node* node, const Entry& entry, StringRef rhs,
                         const uint8_t** cachedFull = nullptr, PrefixMatch* saved = nullptr) const;
    unsigned liveRanks(Node* node, std::array<unsigned, Capacity>& ranks);
    unsigned childRank(Node* node, StringRef begin);
    Node* findLeaf(StringRef begin);
    void removeEmpty(Node* node);
    bool attachedLeaf(const Node* leaf) const;
    void closeEmptiedAnchor(Node* anchor, uint32_t incarnation);
    void collapseRoot();
    void destroySubtree(Node* node);
    Node* split(Node* node, Reservation& reservation, unsigned incomingRank = 0);
    // Returns the position immediately after the inserted entry, adjusted by any split
    InsertCursor insertAt(InsertCursor cursor, const Entry& interval, Reservation& reservation);
    // Optional input keys must match incoming. Use their begin and range end directly;
    // resolve the implicit point end from incoming.
    void overwrite(const Entry& incoming, Reservation& reservation, const LeafHint* hint = nullptr,
                   const Write* write = nullptr);
    // Appends only the key record; node reservation is sized at the actual insertion.
    Entry prepareWrite(Version cv, StringRef begin, StringRef end, bool point = false);
    void findWriteLeaves(const Write* writes, size_t first, size_t count);
    Node* resolveWriteHint(StringRef begin, const LeafHint& hint);
    static void prefetchWriteNode(Node* node);
};

} // namespace paged_btree
