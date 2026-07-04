// buf_internals.h — Internal buffer manager data structures.
//
// Converted from PostgreSQL 15's src/include/storage/buf_internals.h.
//
// Defines the core structures of the buffer pool:
//   - BufferTag: identifies a page by (relation, fork, block number)
//   - BufferDesc: per-buffer metadata (tag, state, refcount, usage count)
//   - BufferPool: the array of descriptors + page data
//
// In PostgreSQL, these structures are shared across processes in shared
// memory and protected by spinlocks. pgcpp allocates the descriptor array
// and page block memory in shared memory (via ShmemInitStruct) so fork'd
// backends share the same pool. A coarse LWLock (kBufferMappingLock)
// serializes all state transitions. In test mode (no ShmemInit called),
// ShmemInitStruct falls back to process-local std::map allocation, so the
// buffer pool works identically in single-process unit tests.
#pragma once

#include <cstdint>

#include "storage/block.hpp"
#include "storage/bufpage.hpp"
#include "storage/ipc/lwlock.hpp"
#include "storage/relfilenode.hpp"

namespace pgcpp::storage {

// Buffer — an integer handle identifying a buffer slot (1-based, matching
// PostgreSQL's convention where 0 = InvalidBuffer).
using Buffer = int;

// InvalidBuffer — sentinel value meaning "no buffer".
constexpr Buffer kInvalidBuffer = 0;

// Buffer ID is 0-based (Buffer is 1-based: buffer_id = buffer - 1).
using BufferId = int;

// --- Buffer state flags ---
//
// These correspond to PostgreSQL's BM_LOCKED, BM_DIRTY, BM_VALID, etc.
// In PostgreSQL they're packed into a single atomic uint32; here we use
// a plain uint32_t since there's no concurrency.
constexpr uint32_t kBMDirty = 0x000001;      // buffer needs writing
constexpr uint32_t kBMValid = 0x000002;      // buffer has valid data
constexpr uint32_t kBMTagged = 0x000004;     // buffer is in the hash table
constexpr uint32_t kBMPermanent = 0x000008;  // buffer belongs to permanent rel

// BufferTag — uniquely identifies a page in the buffer pool.
// Matches PostgreSQL's BufferTag struct.
struct BufferTag {
    RelFileNode rnode;                            // relation file identifier
    ForkNumber fork_num = ForkNumber::kInvalid;   // fork number
    BlockNumber block_num = kInvalidBlockNumber;  // block number within fork

    bool operator==(const BufferTag&) const = default;
};

// BufferTagHash — hash function for BufferTag (for std::unordered_map).
struct BufferTagHash {
    std::size_t operator()(const BufferTag& t) const {
        return std::hash<uint32_t>()(t.rnode.spc_node) ^
               (std::hash<uint32_t>()(t.rnode.db_node) << 1) ^
               (std::hash<uint32_t>()(t.rnode.rel_node) << 2) ^
               (std::hash<int>()(static_cast<int>(t.fork_num)) << 3) ^
               (std::hash<uint32_t>()(t.block_num) << 4);
    }
};

// BufferDesc — per-buffer metadata.
//
// In PostgreSQL this is a shared-memory struct with atomic fields. In
// pgcpp (single-process), plain fields suffice. The structure preserves
// PostgreSQL's fields for architectural fidelity.
struct BufferDesc {
    BufferTag tag;        // page identifier (valid if state & kBMTagged)
    BufferId buf_id = 0;  // index in the buffer pool (0-based)
    uint32_t state = 0;   // state flags (kBMDirty, kBMValid, etc.)
    int refcount = 0;     // pin count (can't evict if > 0)
    int usage_count = 0;  // clock sweep usage count (0-5)

    // Linked list of free buffers (next free buffer index, or -1 if none).
    int free_next = -1;

    // --- Convenience predicates ---

    bool IsDirty() const { return (state & kBMDirty) != 0; }
    bool IsValid() const { return (state & kBMValid) != 0; }
    bool IsTagged() const { return (state & kBMTagged) != 0; }
    bool IsPinned() const { return refcount > 0; }

    void SetDirty() { state |= kBMDirty; }
    void ClearDirty() { state &= ~kBMDirty; }
    void SetValid() { state |= kBMValid; }
    void ClearValid() { state &= ~kBMValid; }
    void SetTagged() { state |= kBMTagged; }
    void ClearTagged() { state &= ~kBMTagged; }
};

// BufferPool — the shared buffer pool.
//
// Manages a fixed-size array of buffer descriptors and their associated
// page data. Provides:
//   - Linear-scan lookup: BufferTag → buffer index
//   - Clock sweep replacement: find a victim buffer to evict
//   - Page read/write through the storage manager
//
// The descriptor array and page block memory are allocated in shared memory
// (via ShmemInitStruct) so fork'd backends share the same pool. A coarse
// LWLock (kBufferMappingLock) serializes all state transitions. In test
// mode (no ShmemInit called), ShmemInitStruct falls back to process-local
// allocation, so the buffer pool works identically in single-process tests.
//
// Lookup is a linear scan over the descriptor array (O(n_buffers)). For the
// expected pool sizes (64–4096) this is L1-cache-resident and negligible,
// and avoids the complexity of placing a hash table in shared memory.
class BufferPool {
public:
    // Create a buffer pool with `n_buffers` slots using the given
    // shm-allocated arrays. If `init` is true, zero the blocks and
    // initialize descriptors/free list (postmaster first init). If false,
    // the pool is being attached to existing shm (fork'd child).
    BufferPool(int n_buffers, BufferDesc* descriptors, char* blocks_base, LWLock* mapping_lock,
               bool init);
    ~BufferPool();

    // Non-copyable, non-movable (references shm-allocated arrays).
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // --- Buffer access ---

    // Get the raw page pointer for a buffer (for reading/writing page data).
    // The buffer must be pinned (refcount > 0).
    Page GetBufferPage(Buffer buffer);

    // Get the BufferDesc for a buffer (for internal use).
    BufferDesc* GetBufferDesc(Buffer buffer);

    // --- Lookup and pinning ---

    // Look up a page in the buffer pool. If found, return the buffer
    // handle (pinned by caller). If not found, return kInvalidBuffer.
    // Caller must hold mapping_lock_.
    Buffer LookupBuffer(const BufferTag& tag);

    // Pin a buffer (increment refcount). The buffer must already be in the pool.
    void PinBuffer(Buffer buffer);

    // Unpin a buffer (decrement refcount). If refcount reaches 0 and the
    // buffer is dirty, it remains in the pool (will be flushed by the
    // clock sweep or an explicit flush).
    void UnpinBuffer(Buffer buffer);

    // --- Victim selection (clock sweep) ---

    // Find a victim buffer to evict. The victim must have refcount == 0.
    // If the victim is dirty, it is flushed to disk first.
    // Returns the buffer index, or -1 if all buffers are pinned.
    // Caller must hold mapping_lock_.
    int FindVictimBuffer();

    // --- Insertion ---

    // Insert a new page into the buffer pool. The victim buffer must have
    // been selected by FindVictimBuffer() and its old content flushed.
    // Sets the tag, marks as valid, and pins the buffer.
    // Returns the buffer handle. Caller must hold mapping_lock_.
    Buffer InsertBuffer(const BufferTag& tag, int victim_id);

    // --- Dirty management ---

    // Mark a buffer as dirty (needs writing to disk).
    void MarkBufferDirty(Buffer buffer);

    // Flush a dirty buffer to disk. If release is true, also unpin it.
    void FlushBuffer(int buf_id, bool release);

    // Flush all dirty buffers for a relation.
    void FlushRelationBuffers(RelFileNode rnode);

    // Flush all dirty+valid buffers. Called explicitly before pool
    // destruction (not from the destructor) so errors propagate normally.
    void FlushAllDirtyBuffers();

    // Invalidate a buffer: flush if dirty, clear the tag, and reset the
    // descriptor to the free state. Used by the Drop* family.
    // The buffer must not be pinned (caller's responsibility).
    void InvalidateBuffer(int buf_id);

    // --- Stats ---

    int NumBuffers() const { return n_buffers_; }
    int NumPinned() const;
    int NumDirty() const;

    // --- Lock access (for public API functions in bufmgr.cpp) ---

    LWLock* mapping_lock() { return mapping_lock_; }

private:
    int n_buffers_;

    // Array of buffer descriptors (shm-allocated, not owned by this object).
    BufferDesc* descriptors_;

    // Contiguous page block memory (shm-allocated, not owned by this object).
    // Page i is at blocks_base_ + i * kBlckSz.
    char* blocks_base_;

    // Coarse lock protecting all state transitions (tag changes, refcount,
    // clock sweep, free list). Acquired exclusive for state-modifying ops,
    // shared for lookups/stats.
    LWLock* mapping_lock_;

    // Clock sweep state (protected by mapping_lock_).
    int clock_hand_ = 0;

    // Free list head (index of first free buffer, or -1 if none).
    int first_free_ = -1;

    // Initialize the free list (all buffers start free).
    void InitFreeList();
};

// Get the global BufferPool instance.
BufferPool* GetBufferPool();

// Set the global BufferPool instance (takes ownership).
void SetBufferPool(BufferPool* pool);

}  // namespace pgcpp::storage
