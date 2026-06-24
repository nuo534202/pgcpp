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
// In PostgreSQL, these operations involve spinlocks, LWLocks, and
// inter-process coordination. MyToyDB is single-process, so we use
// plain fields and no locking. The refcount still matters (a pinned
// buffer cannot be evicted).

#include "mytoydb/storage/bufmgr.h"

#include <cstring>
#include <new>

#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/memory_context.h"

namespace mytoydb::storage {

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

BufferPool::BufferPool(int n_buffers)
    : n_buffers_(n_buffers), descriptors_(n_buffers), blocks_(n_buffers, nullptr) {
    // Allocate page memory for each buffer slot.
    for (int i = 0; i < n_buffers; ++i) {
        blocks_[i] = static_cast<char*>(mytoydb::memory::palloc(kBlckSz));
        std::memset(blocks_[i], 0, kBlckSz);

        descriptors_[i].buf_id = i;
        descriptors_[i].state = 0;
        descriptors_[i].refcount = 0;
        descriptors_[i].usage_count = 0;
        descriptors_[i].free_next = -1;
    }

    InitFreeList();
}

BufferPool::~BufferPool() {
    // Flush all dirty buffers before destroying.
    for (int i = 0; i < n_buffers_; ++i) {
        if (descriptors_[i].IsDirty() && descriptors_[i].IsValid()) {
            FlushBuffer(i, false);
        }
    }

    // Free page memory.
    for (int i = 0; i < n_buffers_; ++i) {
        if (blocks_[i] != nullptr) {
            mytoydb::memory::pfree(blocks_[i]);
        }
    }
}

// --- Buffer access ---

Page BufferPool::GetBufferPage(Buffer buffer) {
    int buf_id = buffer - 1;
    if (buf_id < 0 || buf_id >= n_buffers_) {
        ereport(mytoydb::error::LogLevel::kError, "invalid buffer handle");
    }
    return blocks_[buf_id];
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
    auto it = hash_table_.find(tag);
    if (it == hash_table_.end()) {
        return kInvalidBuffer;
    }
    return it->second + 1;  // 0-based → 1-based
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

    // If the victim was tagged (had a valid tag), remove it from the hash.
    if (desc.IsTagged()) {
        hash_table_.erase(desc.tag);
        desc.ClearTagged();
    }

    // Set up the new tag.
    desc.tag = tag;
    desc.SetTagged();
    desc.state &= ~kBMValid;  // not valid until data is read
    desc.refcount = 1;        // pin it for the caller
    desc.usage_count = 1;     // recently used

    // Insert into hash table.
    hash_table_[tag] = victim_id;

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
    smgrwrite(reln, desc.tag.fork_num, desc.tag.block_num, blocks_[buf_id], false);

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

// --- Stats ---

int BufferPool::NumPinned() const {
    int count = 0;
    for (const auto& d : descriptors_) {
        if (d.refcount > 0)
            ++count;
    }
    return count;
}

int BufferPool::NumDirty() const {
    int count = 0;
    for (const auto& d : descriptors_) {
        if (d.IsDirty())
            ++count;
    }
    return count;
}

// --- Public API ---

Buffer ReadBuffer(SmgrRelation smgr_reln, ForkNumber fork_num, BlockNumber block_num,
                  ReadBufferMode mode, BufferAccessStrategy /*strategy*/) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr) {
        ereport(mytoydb::error::LogLevel::kError, "buffer pool not initialized");
    }

    // Build the buffer tag.
    BufferTag tag;
    tag.rnode = smgr_reln->smgr_rnode.node;
    tag.fork_num = fork_num;
    tag.block_num = block_num;

    // Step 1: check if the page is already in the pool (buffer hit).
    Buffer buffer = pool->LookupBuffer(tag);
    if (buffer != kInvalidBuffer) {
        pool->PinBuffer(buffer);
        return buffer;
    }

    // Step 2: buffer miss — find a victim.
    int victim_id = pool->FindVictimBuffer();
    if (victim_id < 0) {
        ereport(mytoydb::error::LogLevel::kError,
                "no buffer available in buffer pool (all pinned)");
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
    pool->UnpinBuffer(buffer);
}

void MarkBufferDirty(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    pool->MarkBufferDirty(buffer);
}

Page BufferGetPage(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return nullptr;
    return pool->GetBufferPage(buffer);
}

BlockNumber BufferGetBlockNumber(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return kInvalidBlockNumber;
    BufferDesc* desc = pool->GetBufferDesc(buffer);
    if (desc == nullptr)
        return kInvalidBlockNumber;
    return desc->tag.block_num;
}

void FlushBuffer(Buffer buffer) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    int buf_id = buffer - 1;
    pool->FlushBuffer(buf_id, false);
}

void FlushRelationBuffers(SmgrRelation smgr_reln) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;
    pool->FlushRelationBuffers(smgr_reln->smgr_rnode.node);
}

void DropRelationBuffers(RelFileNode rnode) {
    BufferPool* pool = GetBufferPool();
    if (pool == nullptr)
        return;

    // Iterate through all buffers and remove matching ones.
    // We need to access the hash table and descriptors directly.
    // Since DropRelationBuffers is less performance-critical, we
    // scan all descriptors.
    for (int i = 0; i < pool->NumBuffers(); ++i) {
        BufferDesc* desc = pool->GetBufferDesc(i + 1);
        if (desc->IsTagged() && desc->tag.rnode == rnode) {
            // Flush if dirty.
            if (desc->IsDirty() && desc->IsValid()) {
                pool->FlushBuffer(i, false);
            }
            // Remove from hash table.
            // We need to clear the tag and mark as untagged.
            // The buffer goes back to the free list.
            desc->ClearTagged();
            desc->ClearValid();
            desc->ClearDirty();
            desc->tag = BufferTag{};
            desc->usage_count = 0;
            // Add to free list.
            desc->free_next = -1;  // simplified: just mark as free
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
    return desc->refcount > 0;
}

void InitBufferPool(int n_buffers) {
    if (g_buffer_pool != nullptr) {
        delete g_buffer_pool;
    }
    g_buffer_pool = new BufferPool(n_buffers);
}

void ShutdownBufferPool() {
    if (g_buffer_pool != nullptr) {
        delete g_buffer_pool;
        g_buffer_pool = nullptr;
    }
}

}  // namespace mytoydb::storage
