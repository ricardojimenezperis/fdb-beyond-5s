#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace paged_btree {

// Queues share reusable blocks, while their entries and cursor positions stay independent
// Erasing leaves a hole and never moves another entry, so live positions remain stable
// The owner synchronizes mutations and ensures that queued values remain valid until erased
// This container does not own objects referenced by its values
// All queue and position handles belong to this pool and expire when it is destroyed
template <class T, unsigned BlockCapacity = 64>
class BlockQueuePool {
    static_assert(BlockCapacity > 0 && BlockCapacity <= 64);
    static_assert(std::is_trivially_copyable_v<T> && std::is_nothrow_default_constructible_v<T>);
public:
    struct alignas(64) Block {
        Block* previous = nullptr;
        Block* next = nullptr;
        Block* allNext = nullptr;
        uint64_t live = 0;
        unsigned used = 0;
        std::array<T, BlockCapacity> entries{};
    };
    struct Queue {
        Block* head = nullptr;
        Block* tail = nullptr;
        uint64_t count = 0;
    };
    struct Cursor {
        Block* block = nullptr;
        unsigned slot = 0;
    };

    BlockQueuePool() = default;
    BlockQueuePool(const BlockQueuePool&) = delete;
    BlockQueuePool& operator=(const BlockQueuePool&) = delete;
    ~BlockQueuePool() {
        while (all_) {
            Block* next = all_->allNext;
            delete all_;
            all_ = next;
        }
    }

    Cursor append(Queue& queue, T value) {
        if (!queue.tail || queue.tail->used == BlockCapacity) {
            Block* block = acquire();
            block->previous = queue.tail;
            if (queue.tail) queue.tail->next = block;
            else queue.head = block;
            queue.tail = block;
        }
        Block* block = queue.tail;
        const unsigned slot = block->used++;
        block->entries[slot] = value;
        block->live |= uint64_t{1} << slot;
        ++queue.count;
        return {block, slot};
    }

    // The position must still be live in this queue
    // An optional scan cursor is redirected before its empty block can be reused
    void erase(Queue& queue, Cursor position, Cursor* scan = nullptr) {
        Block* block = position.block;
        block->live &= ~(uint64_t{1} << position.slot);
        block->entries[position.slot] = T{};
        --queue.count;
        if (block->live) {
            // Reuse trailing holes without changing the order of remaining queue entries
            block->used = 64 - __builtin_clzll(block->live);
            return;
        }
        Block* next = block->next;
        if (block->previous) block->previous->next = next;
        else queue.head = next;
        if (next) next->previous = block->previous;
        else queue.tail = block->previous;
        if (scan && scan->block == block) *scan = {next ? next : queue.head, 0};
        block->previous = nullptr;
        block->next = free_;
        free_ = block;
    }

    // Starts at the head for an empty cursor, returns at most one full turn of live entries
    // The caller consumes each window before mutating entries that its values refer to
    // The callback can prefetch referenced data without teaching this container its type
    template <bool PrefetchBlocks, class Prepare>
    unsigned readWindow(const Queue& queue, Cursor& cursor, T* values,
                        unsigned limit, bool wrap, Prepare prepare) const {
        if (!queue.count) { cursor = {}; return 0; }
        if (!cursor.block) cursor = {queue.head, 0};
        limit = std::min<uint64_t>(limit, queue.count);
        unsigned count = 0;
        while (cursor.block && count < limit) {
            Block* block = cursor.block;
            prefetchBlock<PrefetchBlocks>(block->next ? block->next : (wrap ? queue.head : nullptr));
            uint64_t live = cursor.slot < BlockCapacity
                ? block->live & (~uint64_t{0} << cursor.slot) : 0;
            while (live && count < limit) {
                const unsigned slot = __builtin_ctzll(live);
                const T value = block->entries[slot];
                values[count++] = value;
                prepare(value);
                cursor.slot = slot + 1;
                live &= live - 1;
            }
            if (!live) cursor = {block->next ? block->next : (wrap ? queue.head : nullptr), 0};
        }
        return count;
    }

    template <bool Enabled>
    static void prefetchBlock(Block* block) {
        if constexpr (Enabled) {
            if (block) for (size_t offset = 0; offset < sizeof(Block); offset += 64)
                __builtin_prefetch(reinterpret_cast<const char*>(block) + offset, 0, 1);
        }
    }

    uint64_t allocatedBlocks() const { return allocated_; }
    uint64_t reusedBlocks() const { return reused_; }
    uint64_t allocatedBytes() const { return allocated_ * sizeof(Block); }
    // Diagnostic traversal only, including blocks retained for reuse
    Block* allBlocks() const { return all_; }
    Block* freeBlocks() const { return free_; }

private:
    Block* free_ = nullptr;
    Block* all_ = nullptr;
    uint64_t allocated_ = 0;
    uint64_t reused_ = 0;

    Block* acquire() {
        Block* block = free_;
        if (block) {
            free_ = block->next;
            ++reused_;
        } else {
            block = new Block;
            block->allNext = all_;
            all_ = block;
            ++allocated_;
        }
        block->previous = block->next = nullptr;
        block->live = 0;
        block->used = 0;
        return block;
    }
};

} // namespace paged_btree
