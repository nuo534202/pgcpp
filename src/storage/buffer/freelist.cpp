// freelist.cpp — Buffer pool free list and clock sweep replacement.
//
// Converted from PostgreSQL 15's src/backend/storage/buffer/freelist.c.
//
// Implements the clock sweep buffer replacement algorithm:
//   1. Start from the current clock hand position.
//   2. For each buffer:
//      a. If refcount > 0, skip (buffer is in use).
//      b. If usage_count > 0, decrement usage_count and skip.
//      c. If usage_count == 0, this is our victim.
//   3. If we've gone through the entire pool without finding a victim
//      (all buffers are pinned), return -1 (error).
//
// This gives recently-used buffers a higher chance of survival: each use
// increments usage_count (up to a max of 5), and the clock sweep must
// decrement it to 0 before evicting.

#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "storage/buf_internals.hpp"
#include "storage/bufmgr.hpp"

namespace pgcpp::storage {

// Maximum usage count (PostgreSQL's BM_MAX_USAGE_COUNT).
constexpr int kMaxUsageCount = 5;

// Ring sizes for each strategy type (matches PostgreSQL defaults).
constexpr int kRingSizeBulkRead = 32;
constexpr int kRingSizeBulkWrite = 32;
constexpr int kRingSizeVacuum = 32;

// FindVictimBuffer — find a buffer to evict using the clock sweep algorithm.
//
// Returns the 0-based buffer index, or -1 if all buffers are pinned.
// If the victim is dirty, it is NOT flushed here; the caller must flush
// it before reusing the slot.
int BufferPool::FindVictimBuffer() {
    // First, try the free list (buffers that have never been used or were
    // explicitly freed). This is faster than the clock sweep.
    if (first_free_ >= 0) {
        int victim = first_free_;
        first_free_ = descriptors_[victim].free_next;
        descriptors_[victim].free_next = -1;
        return victim;
    }

    // Clock sweep: scan the buffer pool starting from clock_hand_.
    int start = clock_hand_;
    for (int i = 0; i < n_buffers_ * 2; ++i) {
        int buf_id = (start + i) % n_buffers_;
        BufferDesc& desc = descriptors_[buf_id];

        if (desc.refcount == 0) {
            if (desc.usage_count > 0) {
                // Decrement usage count and continue.
                --desc.usage_count;
            } else {
                // Found a victim: usage_count == 0 and not pinned.
                clock_hand_ = (buf_id + 1) % n_buffers_;
                return buf_id;
            }
        }
    }

    // All buffers are pinned — this is an error in PostgreSQL too.
    return -1;
}

// InitFreeList — initialize the free list with all buffers.
// Called during BufferPool construction.
void BufferPool::InitFreeList() {
    first_free_ = -1;
    for (int i = n_buffers_ - 1; i >= 0; --i) {
        descriptors_[i].free_next = first_free_;
        first_free_ = i;
    }
}

// --- M6 P0 extensions (Task 15.7.3): access strategy ring ---

BufferAccessStrategyHandle GetAccessStrategy(BufferAccessStrategy btype) {
    int ring_size;
    switch (btype) {
        case BufferAccessStrategy::kBulkRead:
            ring_size = kRingSizeBulkRead;
            break;
        case BufferAccessStrategy::kBulkWrite:
            ring_size = kRingSizeBulkWrite;
            break;
        case BufferAccessStrategy::kVacuum:
            ring_size = kRingSizeVacuum;
            break;
        case BufferAccessStrategy::kNormal:
        default:
            ring_size = 0;
            break;
    }

    auto* strategy = pgcpp::nodes::makePallocNode<BufferAccessStrategyData>();
    strategy->type = btype;
    strategy->ring_size = ring_size;
    strategy->current = 0;
    if (ring_size > 0) {
        strategy->ring.resize(ring_size, kInvalidBuffer);
    }
    return strategy;
}

void FreeAccessStrategy(BufferAccessStrategyHandle strategy) {
    if (strategy == nullptr)
        return;
    pgcpp::nodes::destroyPallocNode(strategy);
}

void StrategyFreeBuffer(Buffer /*buffer*/) {
    // pgcpp is single-process: there is no shared freelist to return the
    // buffer to. The clock sweep will reclaim the slot on the next victim
    // search. PostgreSQL uses this hook to push the buffer back onto the
    // shared free list for fast reuse by InvalidateBuffer.
}

}  // namespace pgcpp::storage
