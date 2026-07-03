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
                       LWLock* mapping_lock, bool init)
    : n_buffers_(n_buffers),
      descriptors_(descriptors),
      blocks_base_(blocks_base),
      mapping_lock_(mapping_lock) {
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
        InitFreeList();
    }
}

BufferPool::~BufferPool() {
    // Flush dirty buffers before destroying. The descriptors/blocks memory
    // is owned by the shm framework (mmap segment or test-mode std::map),
    // not by this object, so we don't free it here.
    for (int i = 0; i < n_buffers_; ++i) {
        if (descriptors_[i].IsDirty() && descriptors_[i].IsValid()) {
            FlushBuffer(i, false);
        }
    }
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

// --- Lookup and pinning ---

Buffer BufferPool::LookupBuffer(const BufferTag& tag) {
    // Linear scan over all descriptors. For n_buffers ≤ 4096 this is
    // L1-cache-resident and negligible. Caller must hold mapping_lock_.
    for (int i = 0; i < n_buffers_; ++i) {
        if (descriptors_[i].IsTagged() && descriptors_[i].tag == tag) {
            return i + 1;  // 0-based → 1-based
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

    // The victim's old tag is simply overwritten. With linear-scan lookup,
    // there is no hash table to update — the old tag disappears and the new
    // tag becomes visible. The victim was already flushed by the caller.
    desc.tag = tag;
    desc.SetTagged();
    desc.state &= ~kBMValid;  // not valid until data is read
    desc.refcount = 1;        // pin it for the caller
    desc.usage_count = 1;     // recently used
    desc.free_next = -1;      // remove from free list

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

    // Clear the tag (no hash table to update with linear-scan lookup).
    desc.ClearTagged();
    desc.ClearValid();
    desc.ClearDirty();
    desc.tag = BufferTag{};
    desc.usage_count = 0;
    desc.free_next = -1;
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

    // Acquire the buffer mapping lock for the entire operation. This
    // serializes lookup + victim selection + insertion, preventing races
    // between concurrent ReadBuffer calls for the same tag. Disk I/O
    // (smgrread) happens under the lock — acceptable for pgcpp's
    // low-concurrency target.
    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);

    // Step 1: check if the page is already in the pool (buffer hit).
    Buffer buffer = pool->LookupBuffer(tag);
    if (buffer != kInvalidBuffer) {
        pool->PinBuffer(buffer);
        LWLockRelease(pool->mapping_lock());
        return buffer;
    }

    // Step 2: buffer miss — find a victim.
    int victim_id = pool->FindVictimBuffer();
    if (victim_id < 0) {
        LWLockRelease(pool->mapping_lock());
        ereport(pgcpp::error::LogLevel::kError, "no buffer available in buffer pool (all pinned)");
    }

    BufferDesc* victim = pool->GetBufferDesc(victim_id + 1);

    // Step 3: if the victim is dirty, flush it to disk first.
    if (victim->IsDirty() && victim->IsValid()) {
        pool->FlushBuffer(victim_id, false);
    }

    // Step 4: insert the new tag into the victim slot and pin it.
    buffer = pool->InsertBuffer(tag, victim_id);

    // Step 5: read the page data.
    if (mode == ReadBufferMode::kZero) {
        // Zeroed page — no disk I/O. Used for new pages.
        Page page = pool->GetBufferPage(buffer);
        std::memset(page, 0, kBlckSz);
    } else {
        // Read from disk via the storage manager.
        Page page = pool->GetBufferPage(buffer);
        smgrread(smgr_reln, fork_num, block_num, page);
    }

    // Step 6: mark as valid.
    victim = pool->GetBufferDesc(buffer);
    victim->SetValid();

    LWLockRelease(pool->mapping_lock());
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
    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);
    pool->UnpinBuffer(buffer);
    LWLockRelease(pool->mapping_lock());
}

void MarkBufferDirty(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);
    pool->MarkBufferDirty(buffer);
    LWLockRelease(pool->mapping_lock());
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
    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);
    pool->FlushBuffer(buf_id, false);
    LWLockRelease(pool->mapping_lock());
}

void FlushRelationBuffers(SmgrRelation smgr_reln) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);
    pool->FlushRelationBuffers(smgr_reln->smgr_rnode.node);
    LWLockRelease(pool->mapping_lock());
}

void DropRelationBuffers(RelFileNode rnode) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;

    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);
    for (int i = 0; i < pool->NumBuffers(); ++i) {
        BufferDesc* desc = pool->GetBufferDesc(i + 1);
        if (desc->IsTagged() && desc->tag.rnode == rnode) {
            if (desc->IsDirty() && desc->IsValid()) {
                pool->FlushBuffer(i, false);
            }
            desc->ClearTagged();
            desc->ClearValid();
            desc->ClearDirty();
            desc->tag = BufferTag{};
            desc->usage_count = 0;
            desc->free_next = -1;
        }
    }
    LWLockRelease(pool->mapping_lock());
}

bool BufferIsPinned(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return false;
    LWLockAcquire(pool->mapping_lock(), LWLockMode::kShared);
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    bool pinned = (desc != nullptr && desc->refcount > 0);
    LWLockRelease(pool->mapping_lock());
    return pinned;
}

std::size_t BufferPoolShmemSize(int n_buffers) {
    return sizeof(BufferDesc) * static_cast<std::size_t>(n_buffers) +
           static_cast<std::size_t>(kBlckSz) * static_cast<std::size_t>(n_buffers);
}

void InitBufferPool(int n_buffers) {
    if (g_buffer_pool != nullptr) {
        delete g_buffer_pool;
        g_buffer_pool = nullptr;
    }

    // Allocate descriptors and page blocks via ShmemInitStruct. In
    // multi-process mode this draws from the mmap'd shared segment; in
    // test mode (no ShmemInit called) it falls back to process-local
    // std::map allocation.
    bool found_desc = false;
    bool found_blocks = false;
    auto* descriptors = static_cast<BufferDesc*>(
        ShmemInitStruct("BufferPoolDescriptors",
                        sizeof(BufferDesc) * static_cast<std::size_t>(n_buffers),
                        &found_desc));
    auto* blocks_base = static_cast<char*>(
        ShmemInitStruct("BufferPoolBlocks",
                        static_cast<std::size_t>(kBlckSz) * static_cast<std::size_t>(n_buffers),
                        &found_blocks));

    // The mapping lock is a named LWLock allocated in shm (or test-mode
    // fallback). LookupNamedLock initializes the array on first call.
    LWLock* mapping_lock = LookupNamedLock(LWLockId::kBufferMappingLock);

    // Create the BufferPool controller object on the heap. The object is
    // small (just pointers + ints); the heavy data (descriptors, blocks)
    // lives in shm. `init` controls whether to zero/init the arrays.
    bool init = !found_desc;
    g_buffer_pool = new BufferPool(n_buffers, descriptors, blocks_base, mapping_lock, init);
}

void ShutdownBufferPool() {
    if (g_buffer_pool != nullptr) {
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
    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc == nullptr) {
        LWLockRelease(pool->mapping_lock());
        return;
    }
    if (!desc->IsDirty()) {
        pool->MarkBufferDirty(buffer);
    }
    if (release) {
        pool->UnpinBuffer(buffer);
    }
    LWLockRelease(pool->mapping_lock());
}

Buffer ReleaseAndReadBuffer(Buffer buffer, SmgrRelation reln, ForkNumber forknum,
                            BlockNumber blocknum, ReadBufferMode mode) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "buffer pool not initialized");
    }

    // Optimization: if the old buffer's tag matches the new (reln, forknum,
    // blocknum), reuse it without releasing.
    LWLockAcquire(pool->mapping_lock(), LWLockMode::kShared);
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    bool reuse = (desc != nullptr && desc->IsTagged() &&
                  desc->tag.rnode == reln->smgr_rnode.node &&
                  desc->tag.fork_num == forknum &&
                  desc->tag.block_num == blocknum);
    LWLockRelease(pool->mapping_lock());

    if (reuse) {
        return buffer;
    }

    // Different page: release the old buffer and read the new one.
    // ReleaseBuffer and ReadBuffer acquire the lock themselves.
    ReleaseBuffer(buffer);
    return ReadBuffer(reln, forknum, blocknum, mode, BufferAccessStrategy::kNormal);
}

void IncrBufferRefCount(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);
    pool->PinBuffer(buffer);
    LWLockRelease(pool->mapping_lock());
}

void BufferGetTag(Buffer buffer, RelFileNode* rnode, ForkNumber* forknum, BlockNumber* blocknum) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    LWLockAcquire(pool->mapping_lock(), LWLockMode::kShared);
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc != nullptr) {
        if (rnode != nullptr)
            *rnode = desc->tag.rnode;
        if (forknum != nullptr)
            *forknum = desc->tag.fork_num;
        if (blocknum != nullptr)
            *blocknum = desc->tag.block_num;
    }
    LWLockRelease(pool->mapping_lock());
}

void DropRelFileNodeBuffers(RelFileNodeBackend rnode, ForkNumber forknum) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;

    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);
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
    LWLockRelease(pool->mapping_lock());
}

void DropDatabaseBuffers(Oid dbid) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;

    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);
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
    LWLockRelease(pool->mapping_lock());
}

void FlushDatabaseBuffers(Oid dbid) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;

    LWLockAcquire(pool->mapping_lock(), LWLockMode::kExclusive);
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
    LWLockRelease(pool->mapping_lock());
}

Buffer ReadBufferWithoutRelcache(RelFileNodeBackend rnode, ForkNumber forknum, BlockNumber blocknum,
                                 ReadBufferMode mode, BufferAccessStrategy strategy) {
    // pgcpp simplification: bypass-relcache read is just smgropen + ReadBuffer.
    SmgrRelation reln = smgropen(rnode);
    return ReadBuffer(reln, forknum, blocknum, mode, strategy);
}

}  // namespace pgcpp::storage
