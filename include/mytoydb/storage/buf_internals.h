// buf_internals.h — Internal buffer manager data structures.
//
// Converted from PostgreSQL 15's src/include/storage/buf_internals.h.
//
// Defines the core structures of the buffer pool:
//   - BufferTag: identifies a page by (relation, fork, block number)
//   - BufferDesc: per-buffer metadata (tag, state, refcount, usage count)
//   - BufferPool: the array of descriptors + page data + lookup hash table
//
// In PostgreSQL, these structures are shared across processes in shared
// memory and protected by spinlocks. MyToyDB is single-process, so:
//   - No spinlocks or atomics needed (plain int fields suffice)
//   - No shared memory (the buffer pool lives in a regular heap allocation)
//   - Refcount still matters (can't evict a pinned buffer)
//   - Usage count drives the clock sweep replacement algorithm
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "mytoydb/storage/block.h"
#include "mytoydb/storage/bufpage.h"
#include "mytoydb/storage/relfilenode.h"

namespace mytoydb::storage {

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
constexpr uint32_t kBMDirty = 0x000001;       // buffer needs writing
constexpr uint32_t kBMValid = 0x000002;       // buffer has valid data
constexpr uint32_t kBMTagged = 0x000004;      // buffer is in the hash table
constexpr uint32_t kBMPermanent = 0x000008;   // buffer belongs to permanent rel

// BufferTag — uniquely identifies a page in the buffer pool.
// Matches PostgreSQL's BufferTag struct.
struct BufferTag {
    RelFileNode rnode;       // relation file identifier
    ForkNumber fork_num = ForkNumber::kInvalid;  // fork number
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
// MyToyDB (single-process), plain fields suffice. The structure preserves
// PostgreSQL's fields for architectural fidelity.
struct BufferDesc {
    BufferTag tag;              // page identifier (valid if state & kBMTagged)
    BufferId buf_id = 0;        // index in the buffer pool (0-based)
    uint32_t state = 0;         // state flags (kBMDirty, kBMValid, etc.)
    int refcount = 0;           // pin count (can't evict if > 0)
    int usage_count = 0;        // clock sweep usage count (0-5)

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
//   - Hash table lookup: BufferTag → buffer index
//   - Clock sweep replacement: find a victim buffer to evict
//   - Page read/write through the storage manager
//
// In PostgreSQL, the buffer pool is allocated in shared memory at server
// startup. In MyToyDB, it's a heap-allocated object owned by the storage
// layer, created at initialization time.
class BufferPool {
public:
    // Create a buffer pool with `n_buffers` slots.
    // Each slot holds one 8KB page. The pool allocates all page memory
    // up front (matching PostgreSQL's shared_buffers model).
    explicit BufferPool(int n_buffers);
    ~BufferPool();

    // Non-copyable, non-movable (owns raw page memory).
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // --- Buffer access ---

    // Get the raw page pointer for a buffer (for reading/writing page data).
    // The buffer must be pinned (refcount > 0).
    Page GetBufferPage(Buffer buffer);

    // Get the BufferDesc for a buffer (for internal use).
    BufferDesc* GetBufferDesc(Buffer buffer);

    // --- Lookup and pinning ---

    // Look up a page in the buffer pool. If found, pin it and return the
    // buffer. If not found, return kInvalidBuffer.
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
    int FindVictimBuffer();

    // --- Insertion ---

    // Insert a new page into the buffer pool. The victim buffer must have
    // been selected by FindVictimBuffer() and its old content flushed.
    // Sets the tag, marks as valid, and pins the buffer.
    // Returns the buffer handle.
    Buffer InsertBuffer(const BufferTag& tag, int victim_id);

    // --- Dirty management ---

    // Mark a buffer as dirty (needs writing to disk).
    void MarkBufferDirty(Buffer buffer);

    // Flush a dirty buffer to disk. If release is true, also unpin it.
    void FlushBuffer(int buf_id, bool release);

    // Flush all dirty buffers for a relation.
    void FlushRelationBuffers(RelFileNode rnode);

    // --- Stats ---

    int NumBuffers() const { return n_buffers_; }
    int NumPinned() const;
    int NumDirty() const;

private:
    int n_buffers_;

    // Array of buffer descriptors.
    std::vector<BufferDesc> descriptors_;

    // Array of page data (each is kBlckSz bytes, palloc'd).
    std::vector<char*> blocks_;

    // Hash table: BufferTag → buffer index (0-based).
    std::unordered_map<BufferTag, int, BufferTagHash> hash_table_;

    // Clock sweep state.
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

}  // namespace mytoydb::storage
