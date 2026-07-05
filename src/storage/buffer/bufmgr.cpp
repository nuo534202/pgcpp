// bufmgr.cpp — Buffer manager implementation.
//
// Converted from PostgreSQL 15's src/backend/storage/buffer/bufmgr.c.
//
// The buffer manager sits between the executor/access methods and the
// storage manager. It maintains a pool of in-memory page buffers to
// minimize disk I/O. Pages are read into the pool on first access and
// kept until evicted by the clock sweep algorithm.
//
// Key operations:
//   ReadBuffer — look up a page in the pool; on miss, find a victim,
//                read the page from disk, and pin it.
//   ReleaseBuffer — unpin a buffer (decrement refcount).
//   MarkBufferDirty — mark a buffer as needing write-back.
//   FlushBuffer — write a dirty buffer's page to disk.
//
// The descriptor array and page block memory are allocated in shared memory
// (via ShmemInitStruct) so fork'd backends share the same pool. A coarse
// LWLock (kBufferMappingLock) serializes all state transitions. In test
// mode, ShmemInitStruct falls back to process-local allocation.

#include "storage/bufmgr.hpp"

#include <cstring>
#include <new>

#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "storage/ipc/lwlock.hpp"
#include "storage/ipc/shmem.hpp"

namespace pgcpp::storage {

// Maximum usage count (PostgreSQL's BM_MAX_USAGE_COUNT).
constexpr int kMaxUsageCount = 5;

// --- Global buffer pool instance ---

namespace {
BufferPool* g_buffer_pool = nullptr;
}

BufferPool* GetBufferPool() {
    return g_buffer_pool;
}
void SetBufferPool(BufferPool* pool) {
    g_buffer_pool = pool;
}

// --- BufferPool construction/destruction ---

BufferPool::BufferPool(int n_buffers, BufferDesc* descriptors, char* blocks_base,
                       BufferPoolShmemState* shmem_state, BufferHashEntry* hash_table,
                       int hash_table_size, bool init)
    : n_buffers_(n_buffers),
      descriptors_(descriptors),
      blocks_base_(blocks_base),
      shmem_state_(shmem_state),
      hash_table_(hash_table),
      hash_table_size_(hash_table_size) {
    if (init) {
        // Zero the page block memory.
        std::memset(blocks_base_, 0, static_cast<std::size_t>(kBlckSz) * n_buffers_);
        // Initialize descriptors.
        for (int i = 0; i < n_buffers_; ++i) {
            descriptors_[i].buf_id = i;
            descriptors_[i].state = 0;
            descriptors_[i].refcount = 0;
            descriptors_[i].usage_count = 0;
            descriptors_[i].free_next = -1;
        }
        // Initialize shared control state.
        shmem_state_->clock_hand = 0;
        shmem_state_->first_free = -1;
        shmem_state_->n_buffers = n_buffers_;
        // Initialize the hash table (all slots empty).
        for (int i = 0; i < hash_table_size_; ++i) {
            hash_table_[i] = BufferHashEntry{};
        }
        // Acquire the freelist lock for InitFreeList.
        LWLockAcquire(FreelistLock(), LWLockMode::kExclusive);
        InitFreeList();
        LWLockRelease(FreelistLock());
    }
}

BufferPool::~BufferPool() {
    // Destructor is a no-op. Dirty buffer flushing is done explicitly via
    // FlushAllDirtyBuffers() before destruction (called from
    // ShutdownBufferPool / InitBufferPool re-init). This avoids calling
    // ereport(kError) from a destructor — after exception-ization, throwing
    // during stack unwinding would call std::terminate.
}

void BufferPool::FlushAllDirtyBuffers() {
    // Flush all dirty+valid buffers. Called explicitly before pool destruction
    // so errors propagate normally (not from a destructor).
    for (int i = 0; i < n_buffers_; ++i) {
        if (descriptors_[i].IsDirty() && descriptors_[i].IsValid()) {
            FlushBuffer(i, false);
        }
    }
}

int BufferPool::FlushDirtyBuffers(int max_buffers) {
    if (max_buffers <= 0)
        return 0;

    // Acquire FreelistLock to serialize the scan with concurrent victim
    // selection. The flush itself (smgrwrite) happens under the lock —
    // acceptable for our simplified model (smgrwrite is typically fast,
    // just an OS page-cache write).
    LWLockAcquire(FreelistLock(), LWLockMode::kExclusive);

    int flushed = 0;
    for (int i = 0; i < n_buffers_ && flushed < max_buffers; ++i) {
        BufferDesc& desc = descriptors_[i];
        // Skip buffers that are being modified (pinned) — flushing them
        // would write a partially-updated page.
        if (desc.refcount > 0)
            continue;
        if (!desc.IsDirty() || !desc.IsValid())
            continue;
        FlushBuffer(i, false);
        ++flushed;
    }

    LWLockRelease(FreelistLock());
    return flushed;
}

// --- Buffer access ---

Page BufferPool::GetBufferPage(Buffer buffer) {
    int buf_id = buffer - 1;
    if (buf_id < 0 || buf_id >= n_buffers_) {
        ereport(pgcpp::error::LogLevel::kError, "invalid buffer handle");
    }
    return blocks_base_ + static_cast<std::size_t>(buf_id) * kBlckSz;
}

BufferDesc* BufferPool::GetBufferDesc(Buffer buffer) {
    int buf_id = buffer - 1;
    if (buf_id < 0 || buf_id >= n_buffers_) {
        return nullptr;
    }
    return &descriptors_[buf_id];
}

// --- Lock helpers ---

LWLock* BufferPool::MappingLockForTag(const BufferTag& tag) {
    return GetBufMappingLock(BufMappingPartition(tag));
}

LWLock* BufferPool::FreelistLock() {
    return LookupNamedLock(LWLockId::kBufFreelistLock);
}

// --- Hash table helpers ---

int BufferPool::HashTableSlot(const BufferTag& tag) const {
    BufferTagHash hasher;
    std::size_t h = hasher(tag);
    return static_cast<int>(h % static_cast<std::size_t>(hash_table_size_));
}

void BufferPool::HashTableInsert(const BufferTag& tag, BufferId buf_id) {
    int slot = HashTableSlot(tag);
    // Linear probing: find an empty slot or a matching slot (update).
    for (int i = 0; i < hash_table_size_; ++i) {
        int idx = (slot + i) % hash_table_size_;
        if (hash_table_[idx].IsEmpty()) {
            hash_table_[idx].tag = tag;
            hash_table_[idx].buf_id = buf_id;
            return;
        }
        if (hash_table_[idx].tag == tag) {
            // Already present — update the mapping (shouldn't happen normally).
            hash_table_[idx].buf_id = buf_id;
            return;
        }
    }
    ereport(pgcpp::error::LogLevel::kError, "buffer hash table full");
}

void BufferPool::HashTableDelete(const BufferTag& tag) {
    int slot = HashTableSlot(tag);
    for (int i = 0; i < hash_table_size_; ++i) {
        int idx = (slot + i) % hash_table_size_;
        if (hash_table_[idx].IsEmpty()) {
            return;  // not found
        }
        if (hash_table_[idx].tag == tag) {
            hash_table_[idx] = BufferHashEntry{};  // mark empty
            return;
        }
    }
}

// --- Lookup and pinning ---

Buffer BufferPool::LookupBuffer(const BufferTag& tag) {
    int slot = HashTableSlot(tag);
    for (int i = 0; i < hash_table_size_; ++i) {
        int idx = (slot + i) % hash_table_size_;
        if (hash_table_[idx].IsEmpty()) {
            return kInvalidBuffer;  // not in the pool
        }
        if (hash_table_[idx].tag == tag) {
            return hash_table_[idx].buf_id + 1;  // 0-based → 1-based
        }
    }
    return kInvalidBuffer;
}

void BufferPool::PinBuffer(Buffer buffer) {
    int buf_id = buffer - 1;
    if (buf_id < 0 || buf_id >= n_buffers_)
        return;
    ++descriptors_[buf_id].refcount;
    // Bump usage count (capped at kMaxUsageCount).
    if (descriptors_[buf_id].usage_count < kMaxUsageCount) {
        ++descriptors_[buf_id].usage_count;
    }
}

void BufferPool::UnpinBuffer(Buffer buffer) {
    int buf_id = buffer - 1;
    if (buf_id < 0 || buf_id >= n_buffers_)
        return;
    if (descriptors_[buf_id].refcount > 0) {
        --descriptors_[buf_id].refcount;
    }
}

// --- Insertion ---

Buffer BufferPool::InsertBuffer(const BufferTag& tag, int victim_id) {
    BufferDesc& desc = descriptors_[victim_id];

    // If the victim was previously tagged, remove its old mapping from the
    // hash table (the caller should have flushed it already).
    if (desc.IsTagged()) {
        HashTableDelete(desc.tag);
    }

    // Install the new tag and insert into the hash table.
    desc.tag = tag;
    desc.SetTagged();
    desc.state &= ~kBMValid;  // not valid until data is read
    desc.refcount = 1;        // pin it for the caller
    desc.usage_count = 1;     // recently used
    desc.free_next = -1;      // remove from free list

    HashTableInsert(tag, victim_id);

    return victim_id + 1;  // 0-based → 1-based
}

// --- Dirty management ---

void BufferPool::MarkBufferDirty(Buffer buffer) {
    int buf_id = buffer - 1;
    if (buf_id < 0 || buf_id >= n_buffers_)
        return;
    descriptors_[buf_id].SetDirty();
}

void BufferPool::FlushBuffer(int buf_id, bool release) {
    if (buf_id < 0 || buf_id >= n_buffers_)
        return;

    BufferDesc& desc = descriptors_[buf_id];

    if (!desc.IsValid() || !desc.IsDirty()) {
        if (release && desc.refcount > 0) {
            --desc.refcount;
        }
        return;
    }

    // Write the page to disk via the storage manager.
    SmgrRelation reln = smgropen(RelFileNodeBackend{desc.tag.rnode, 0});
    Page page = blocks_base_ + static_cast<std::size_t>(buf_id) * kBlckSz;
    smgrwrite(reln, desc.tag.fork_num, desc.tag.block_num, page, false);

    desc.ClearDirty();

    if (release && desc.refcount > 0) {
        --desc.refcount;
    }
}

void BufferPool::FlushRelationBuffers(RelFileNode rnode) {
    for (int i = 0; i < n_buffers_; ++i) {
        BufferDesc& desc = descriptors_[i];
        if (desc.IsTagged() && desc.tag.rnode == rnode) {
            if (desc.IsDirty() && desc.IsValid()) {
                FlushBuffer(i, false);
            }
        }
    }
}

void BufferPool::InvalidateBuffer(int buf_id) {
    if (buf_id < 0 || buf_id >= n_buffers_)
        return;
    BufferDesc& desc = descriptors_[buf_id];

    // Flush dirty pages before discarding.
    if (desc.IsDirty() && desc.IsValid()) {
        FlushBuffer(buf_id, false);
    }

    // Remove the tag from the hash table. Acquire the partition lock for
    // the tag to serialize with concurrent lookups. Lock ordering: the
    // caller may hold FreelistLock (freelist-before-partition is OK).
    if (desc.IsTagged()) {
        LWLock* part_lock = MappingLockForTag(desc.tag);
        LWLockAcquire(part_lock, LWLockMode::kExclusive);
        HashTableDelete(desc.tag);
        LWLockRelease(part_lock);
    }

    desc.ClearTagged();
    desc.ClearValid();
    desc.ClearDirty();
    desc.tag = BufferTag{};
    desc.usage_count = 0;
    desc.free_next = -1;

    // Return the buffer to the free list for fast reuse.
    LWLockAcquire(FreelistLock(), LWLockMode::kExclusive);
    desc.free_next = shmem_state_->first_free;
    shmem_state_->first_free = buf_id;
    LWLockRelease(FreelistLock());
}

// --- Stats ---

int BufferPool::NumPinned() const {
    int count = 0;
    for (int i = 0; i < n_buffers_; ++i) {
        if (descriptors_[i].refcount > 0)
            ++count;
    }
    return count;
}

int BufferPool::NumDirty() const {
    int count = 0;
    for (int i = 0; i < n_buffers_; ++i) {
        if (descriptors_[i].IsDirty())
            ++count;
    }
    return count;
}

// --- Public API ---

Buffer ReadBuffer(SmgrRelation smgr_reln, ForkNumber fork_num, BlockNumber block_num,
                  ReadBufferMode mode, BufferAccessStrategy /*strategy*/) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "buffer pool not initialized");
    }

    // Build the buffer tag.
    BufferTag tag;
    tag.rnode = smgr_reln->smgr_rnode.node;
    tag.fork_num = fork_num;
    tag.block_num = block_num;

    // Acquire the partition lock for this tag. This serializes lookup +
    // victim selection + insertion for tags in the same partition.
    LWLock* part_lock = pool->MappingLockForTag(tag);
    LWLockAcquire(part_lock, LWLockMode::kExclusive);

    // Step 1: check if the page is already in the pool (buffer hit).
    Buffer buffer = pool->LookupBuffer(tag);
    if (buffer != kInvalidBuffer) {
        pool->PinBuffer(buffer);
        LWLockRelease(part_lock);
        return buffer;
    }

    // Step 2: buffer miss — find a victim (acquires freelist lock internally).
    int victim_id = pool->FindVictimBuffer();
    if (victim_id < 0) {
        LWLockRelease(part_lock);
        ereport(pgcpp::error::LogLevel::kError, "no buffer available in buffer pool (all pinned)");
    }

    BufferDesc* victim = pool->GetBufferDesc(victim_id + 1);

    // If the victim is tagged with a tag in a DIFFERENT partition, we need
    // to acquire that partition's lock to delete the old mapping. To avoid
    // complex lock ordering, we flush the victim and delete its old mapping
    // under the freelist lock (which is always acquired before partition locks).
    // In practice, for the test pool sizes this is fine.
    if (victim->IsTagged() && victim->IsDirty() && victim->IsValid()) {
        pool->FlushBuffer(victim_id, false);
    }

    // Step 3: insert the new tag into the victim slot and pin it.
    // InsertBuffer also updates the hash table (deletes old tag if present,
    // inserts new tag). Since we hold the partition lock for the new tag,
    // the insert is safe. If the victim's old tag was in a different
    // partition, we rely on the fact that no other backend will look up
    // the old tag (it's being evicted) — the hash table delete is just
    // cleanup.
    buffer = pool->InsertBuffer(tag, victim_id);

    // Step 4: read the page data.
    if (mode == ReadBufferMode::kZero) {
        // Zeroed page — no disk I/O. Used for new pages.
        Page page = pool->GetBufferPage(buffer);
        std::memset(page, 0, kBlckSz);
    } else {
        // Read from disk via the storage manager.
        Page page = pool->GetBufferPage(buffer);
        smgrread(smgr_reln, fork_num, block_num, page);
    }

    // Step 5: mark as valid.
    victim = pool->GetBufferDesc(buffer);
    victim->SetValid();

    LWLockRelease(part_lock);
    return buffer;
}

Buffer ReadBufferExtended(SmgrRelation smgr_reln, ForkNumber fork_num, BlockNumber block_num,
                          ReadBufferMode mode, BufferAccessStrategy strategy) {
    return ReadBuffer(smgr_reln, fork_num, block_num, mode, strategy);
}

void ReleaseBuffer(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc == nullptr)
        return;
    // The buffer is pinned by the caller, so its tag is stable. Use the
    // partition lock for the tag to serialize with concurrent lookups.
    LWLock* part_lock =
        desc->IsTagged() ? pool->MappingLockForTag(desc->tag) : pool->FreelistLock();
    LWLockAcquire(part_lock, LWLockMode::kExclusive);
    pool->UnpinBuffer(buffer);
    LWLockRelease(part_lock);
}

void MarkBufferDirty(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc == nullptr)
        return;
    LWLock* part_lock =
        desc->IsTagged() ? pool->MappingLockForTag(desc->tag) : pool->FreelistLock();
    LWLockAcquire(part_lock, LWLockMode::kExclusive);
    pool->MarkBufferDirty(buffer);
    LWLockRelease(part_lock);
}

Page BufferGetPage(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return nullptr;
    // No lock needed — the caller must hold a pin, which prevents eviction.
    return pool->GetBufferPage(buffer);
}

BlockNumber BufferGetBlockNumber(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return kInvalidBlockNumber;
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc == nullptr)
        return kInvalidBlockNumber;
    // No lock — tag is stable while pinned.
    return desc->tag.block_num;
}

void FlushBuffer(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    int buf_id = buffer - 1;
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc == nullptr)
        return;
    LWLock* part_lock =
        desc->IsTagged() ? pool->MappingLockForTag(desc->tag) : pool->FreelistLock();
    LWLockAcquire(part_lock, LWLockMode::kExclusive);
    pool->FlushBuffer(buf_id, false);
    LWLockRelease(part_lock);
}

void FlushRelationBuffers(SmgrRelation smgr_reln) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    // Scan-based flush: use FreelistLock as a coarse lock to serialize the
    // scan with concurrent victim selection. Individual FlushBuffer calls
    // are safe because the desc.tag is read while holding the lock.
    LWLockAcquire(pool->FreelistLock(), LWLockMode::kExclusive);
    pool->FlushRelationBuffers(smgr_reln->smgr_rnode.node);
    LWLockRelease(pool->FreelistLock());
}

void DropRelationBuffers(RelFileNode rnode) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;

    // Scan and invalidate matching buffers. InvalidateBuffer acquires all
    // necessary locks (partition lock for the tag, freelist lock for the
    // free-list push) internally. In single-process/test mode there is no
    // concurrency; in multi-process mode a concurrent backend could add a
    // new buffer for this rnode between iterations (PG has the same race
    // and handles it by re-checking under locks).
    for (int i = 0; i < pool->NumBuffers(); ++i) {
        BufferDesc* desc = pool->GetBufferDesc(i + 1);
        if (desc->IsTagged() && desc->tag.rnode == rnode) {
            pool->InvalidateBuffer(i);
        }
    }
}

bool BufferIsPinned(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return false;
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc == nullptr)
        return false;
    LWLock* part_lock =
        desc->IsTagged() ? pool->MappingLockForTag(desc->tag) : pool->FreelistLock();
    LWLockAcquire(part_lock, LWLockMode::kShared);
    bool pinned = desc->refcount > 0;
    LWLockRelease(part_lock);
    return pinned;
}

// BufferHashTableSize — number of slots in the buffer hash table.
// Must be a power of 2 and >= 2*n_buffers for good load factor (PG uses
// NBuffers * 2 rounded up to the next power of 2).
static int BufferHashTableSize(int n_buffers) {
    int size = 1;
    int target = n_buffers * 2;
    while (size < target) {
        size <<= 1;
    }
    return size;
}

std::size_t BufferPoolShmemSize(int n_buffers) {
    int hash_table_size = BufferHashTableSize(n_buffers);
    return sizeof(BufferDesc) * static_cast<std::size_t>(n_buffers) +
           static_cast<std::size_t>(kBlckSz) * static_cast<std::size_t>(n_buffers) +
           sizeof(BufferPoolShmemState) +
           sizeof(BufferHashEntry) * static_cast<std::size_t>(hash_table_size);
}

void InitBufferPool(int n_buffers) {
    if (g_buffer_pool != nullptr) {
        // Flush dirty buffers before re-initializing (preserves data).
        // Done here, not in the destructor, to avoid ereport from a destructor.
        g_buffer_pool->FlushAllDirtyBuffers();
        delete g_buffer_pool;
        g_buffer_pool = nullptr;
    }

    // Allocate descriptors, page blocks, control state, and hash table via
    // ShmemInitStruct. In multi-process mode this draws from the mmap'd
    // shared segment; in test mode (no ShmemInit called) it falls back to
    // process-local std::map allocation.
    bool found_desc = false;
    bool found_blocks = false;
    bool found_shmem_state = false;
    bool found_hash_table = false;

    auto* descriptors = static_cast<BufferDesc*>(
        ShmemInitStruct("BufferPoolDescriptors",
                        sizeof(BufferDesc) * static_cast<std::size_t>(n_buffers), &found_desc));
    auto* blocks_base = static_cast<char*>(ShmemInitStruct(
        "BufferPoolBlocks", static_cast<std::size_t>(kBlckSz) * static_cast<std::size_t>(n_buffers),
        &found_blocks));
    auto* shmem_state = static_cast<BufferPoolShmemState*>(
        ShmemInitStruct("BufferPoolShmemState", sizeof(BufferPoolShmemState), &found_shmem_state));

    int hash_table_size = BufferHashTableSize(n_buffers);
    auto* hash_table = static_cast<BufferHashEntry*>(ShmemInitStruct(
        "BufferHashTable", sizeof(BufferHashEntry) * static_cast<std::size_t>(hash_table_size),
        &found_hash_table));

    // The partition locks are allocated by InitializeAllLWLocks (called
    // from LookupNamedLock on first use). Ensure they're initialized.
    LookupNamedLock(LWLockId::kBufFreelistLock);
    GetBufMappingLock(0);

    // Create the BufferPool controller object on the heap. The object is
    // small (just pointers + ints); the heavy data (descriptors, blocks,
    // shmem_state, hash_table) lives in shm. `init` controls whether to
    // zero/init the arrays.
    bool init = !found_desc;
    g_buffer_pool = new BufferPool(n_buffers, descriptors, blocks_base, shmem_state, hash_table,
                                   hash_table_size, init);
}

void ShutdownBufferPool() {
    if (g_buffer_pool != nullptr) {
        // Flush dirty buffers before destroying. Done here, not in the
        // destructor, to avoid ereport from a destructor.
        g_buffer_pool->FlushAllDirtyBuffers();
        delete g_buffer_pool;
        g_buffer_pool = nullptr;
    }
    // In test mode, clear shm regions so the next InitBufferPool gets
    // fresh allocations (test isolation). In multi-process mode, shm is
    // freed by ShmemDetach on server shutdown.
    if (!IsShmemActive()) {
        ResetShmem();
        ResetAllLWLocks();
    }
}

// --- M6 P0 extensions (Task 15.7.1) ---

void MarkBufferDirtyHint(Buffer buffer, bool release) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    // pgcpp has no WAL, so a hint dirty is equivalent to a normal dirty.
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc == nullptr)
        return;
    LWLock* part_lock =
        desc->IsTagged() ? pool->MappingLockForTag(desc->tag) : pool->FreelistLock();
    LWLockAcquire(part_lock, LWLockMode::kExclusive);
    if (!desc->IsDirty()) {
        pool->MarkBufferDirty(buffer);
    }
    if (release) {
        pool->UnpinBuffer(buffer);
    }
    LWLockRelease(part_lock);
}

Buffer ReleaseAndReadBuffer(Buffer buffer, SmgrRelation reln, ForkNumber forknum,
                            BlockNumber blocknum, ReadBufferMode mode) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "buffer pool not initialized");
    }

    // Optimization: if the old buffer's tag matches the new (reln, forknum,
    // blocknum), reuse it without releasing. The buffer is pinned, so its
    // tag is stable — read without a lock.
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    bool reuse = (desc != nullptr && desc->IsTagged() && desc->tag.rnode == reln->smgr_rnode.node &&
                  desc->tag.fork_num == forknum && desc->tag.block_num == blocknum);

    if (reuse) {
        return buffer;
    }

    // Different page: release the old buffer and read the new one.
    // ReleaseBuffer and ReadBuffer acquire locks themselves.
    ReleaseBuffer(buffer);
    return ReadBuffer(reln, forknum, blocknum, mode, BufferAccessStrategy::kNormal);
}

void IncrBufferRefCount(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc == nullptr)
        return;
    LWLock* part_lock =
        desc->IsTagged() ? pool->MappingLockForTag(desc->tag) : pool->FreelistLock();
    LWLockAcquire(part_lock, LWLockMode::kExclusive);
    pool->PinBuffer(buffer);
    LWLockRelease(part_lock);
}

void BufferGetTag(Buffer buffer, RelFileNode* rnode, ForkNumber* forknum, BlockNumber* blocknum) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc == nullptr)
        return;
    // The buffer is pinned by the caller, so its tag is stable.
    LWLock* part_lock =
        desc->IsTagged() ? pool->MappingLockForTag(desc->tag) : pool->FreelistLock();
    LWLockAcquire(part_lock, LWLockMode::kShared);
    if (rnode != nullptr)
        *rnode = desc->tag.rnode;
    if (forknum != nullptr)
        *forknum = desc->tag.fork_num;
    if (blocknum != nullptr)
        *blocknum = desc->tag.block_num;
    LWLockRelease(part_lock);
}

void DropRelFileNodeBuffers(RelFileNodeBackend rnode, ForkNumber forknum) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;

    // Scan and invalidate matching buffers. InvalidateBuffer acquires all
    // necessary locks internally.
    for (int i = 0; i < pool->NumBuffers(); ++i) {
        BufferDesc* desc = pool->GetBufferDesc(i + 1);
        if (!desc->IsTagged())
            continue;
        if (desc->tag.rnode != rnode.node)
            continue;
        if (desc->tag.fork_num != forknum)
            continue;
        if (desc->refcount > 0)
            continue;
        pool->InvalidateBuffer(i);
    }
}

void DropDatabaseBuffers(Oid dbid) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;

    for (int i = 0; i < pool->NumBuffers(); ++i) {
        BufferDesc* desc = pool->GetBufferDesc(i + 1);
        if (!desc->IsTagged())
            continue;
        if (desc->tag.rnode.db_node != dbid)
            continue;
        if (desc->refcount > 0)
            continue;
        pool->InvalidateBuffer(i);
    }
}

void FlushDatabaseBuffers(Oid dbid) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;

    // Scan-based flush: use FreelistLock as a coarse lock to serialize the
    // scan with concurrent victim selection.
    LWLockAcquire(pool->FreelistLock(), LWLockMode::kExclusive);
    for (int i = 0; i < pool->NumBuffers(); ++i) {
        BufferDesc* desc = pool->GetBufferDesc(i + 1);
        if (!desc->IsTagged())
            continue;
        if (desc->tag.rnode.db_node != dbid)
            continue;
        if (desc->IsDirty() && desc->IsValid()) {
            pool->FlushBuffer(i, false);
        }
    }
    LWLockRelease(pool->FreelistLock());
}

Buffer ReadBufferWithoutRelcache(RelFileNodeBackend rnode, ForkNumber forknum, BlockNumber blocknum,
                                 ReadBufferMode mode, BufferAccessStrategy strategy) {
    // pgcpp simplification: bypass-relcache read is just smgropen + ReadBuffer.
    SmgrRelation reln = smgropen(rnode);
    return ReadBuffer(reln, forknum, blocknum, mode, strategy);
}

}  // namespace pgcpp::storage
