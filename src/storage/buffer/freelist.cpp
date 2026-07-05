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
//
// Acquires the freelist lock (kBufFreelistLock) internally to protect the
// shared clock_hand and first_free fields. The caller must NOT hold any
// BufMappingLock partition when calling this (lock ordering: freelist lock
// is always acquired before partition locks, never the reverse).
int BufferPool::FindVictimBuffer() {
    LWLockAcquire(FreelistLock(), LWLockMode::kExclusive);

    // First, try the free list (buffers that have never been used or were
    // explicitly freed). This is faster than the clock sweep.
    if (shmem_state_->first_free >= 0) {
        int victim = shmem_state_->first_free;
        shmem_state_->first_free = descriptors_[victim].free_next;
        descriptors_[victim].free_next = -1;
        LWLockRelease(FreelistLock());
        return victim;
    }

    // Clock sweep: scan the buffer pool starting from clock_hand.
    int start = shmem_state_->clock_hand;
    for (int i = 0; i < n_buffers_ * 2; ++i) {
        int buf_id = (start + i) % n_buffers_;
        BufferDesc& desc = descriptors_[buf_id];

        if (desc.refcount == 0) {
            if (desc.usage_count > 0) {
                // Decrement usage count and continue.
                --desc.usage_count;
            } else {
                // Found a victim: usage_count == 0 and not pinned.
                shmem_state_->clock_hand = (buf_id + 1) % n_buffers_;
                LWLockRelease(FreelistLock());
                return buf_id;
            }
        }
    }

    LWLockRelease(FreelistLock());
    // All buffers are pinned — this is an error in PostgreSQL too.
    return -1;
}

// InitFreeList — initialize the free list with all buffers.
// Called during BufferPool construction. Caller must hold the freelist lock.
void BufferPool::InitFreeList() {
    shmem_state_->first_free = -1;
    for (int i = n_buffers_ - 1; i >= 0; --i) {
        descriptors_[i].free_next = shmem_state_->first_free;
        shmem_state_->first_free = i;
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
    // The shared free list (first_free_, protected by mapping_lock_) is
    // managed by InvalidateBuffer, which pushes evicted buffers back onto
    // it for fast reuse. This strategy-ring hook is a no-op: the clock
    // sweep reclaims the slot on the next victim search if the free list
    // is empty.
}

}  // namespace pgcpp::storage
