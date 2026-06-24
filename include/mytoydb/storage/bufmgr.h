// bufmgr.h — Buffer manager public API.
//
// Converted from PostgreSQL 15's src/include/storage/bufmgr.h.
//
// The buffer manager provides transparent page caching between the
// executor/access methods and the storage manager. Pages are read into
// the buffer pool on demand and kept until evicted by the clock sweep
// algorithm.
//
// Key functions:
//   ReadBuffer — read a page into the buffer pool (pin it)
//   ReleaseBuffer — unpin a buffer (may be evicted later)
//   MarkBufferDirty — mark a buffer's page as modified
//   BufferGetPage — get the raw page pointer for a buffer
//
// Buffer handles are 1-based integers (Buffer = 1..N, 0 = InvalidBuffer).
#pragma once

#include "mytoydb/storage/block.h"
#include "mytoydb/storage/buf_internals.h"
#include "mytoydb/storage/bufpage.h"
#include "mytoydb/storage/relfilenode.h"
#include "mytoydb/storage/smgr.h"

namespace mytoydb::storage {

// Buffer access strategy (mirrors PostgreSQL's BufferAccessStrategyType).
// MyToyDB implements only BAS_NORMAL (clock sweep). The enum is preserved
// for API compatibility.
enum class BufferAccessStrategy {
    kNormal,     // BAS_NORMAL — normal clock sweep
    kBulkRead,   // BAS_BULKREAD — bulk read (reuse ring)
    kBulkWrite,  // BAS_BULKWRITE — bulk write (reuse ring)
    kVacuum,     // BAS_VACUUM — vacuum (reuse ring)
};

// ReadBufferMode — controls how ReadBuffer handles missing pages.
enum class ReadBufferMode {
    kNormal,  // RBM_NORMAL — read existing page, error if missing
    kZero,    // RBM_ZERO — return zeroed page without I/O
    kNoLock,  // RBM_NO_LOG — don't log (not used in MyToyDB)
};

// ReadBuffer — read a page into the buffer pool and pin it.
//
// If the page is already in the pool, just pins it. Otherwise, finds a
// victim buffer (evicting a dirty one if necessary), reads the page from
// disk via the storage manager, and pins it.
//
// Parameters:
//   smgr_reln — the SmgrRelation for the relation
//   fork_num — which fork to read from
//   block_num — which block to read
//   mode — RBM_NORMAL (read from disk) or RBM_ZERO (zeroed page, no I/O)
//   strategy — buffer access strategy (currently unused, always normal)
//
// Returns the buffer handle (1-based), or kInvalidBuffer on error.
Buffer ReadBuffer(SmgrRelation smgr_reln, ForkNumber fork_num, BlockNumber block_num,
                  ReadBufferMode mode,
                  BufferAccessStrategy strategy = BufferAccessStrategy::kNormal);

// ReadBufferExtended — same as ReadBuffer but with explicit strategy.
// (In MyToyDB, this is the same as ReadBuffer with the strategy parameter.)
Buffer ReadBufferExtended(SmgrRelation smgr_reln, ForkNumber fork_num, BlockNumber block_num,
                          ReadBufferMode mode, BufferAccessStrategy strategy);

// ReleaseBuffer — unpin a buffer (decrement refcount).
// The buffer remains in the pool and may be evicted later by the clock sweep.
void ReleaseBuffer(Buffer buffer);

// MarkBufferDirty — mark a buffer's page as modified.
// The page will be written to disk when the buffer is evicted or when
// FlushBuffer is called explicitly.
void MarkBufferDirty(Buffer buffer);

// BufferGetPage — return the raw page pointer for a buffer.
// The buffer must be pinned.
Page BufferGetPage(Buffer buffer);

// BufferGetBlockNumber — return the block number of the page in a buffer.
BlockNumber BufferGetBlockNumber(Buffer buffer);

// FlushBuffer — flush a dirty buffer to disk (internal, but exposed for tests).
void FlushBuffer(Buffer buffer);

// FlushRelationBuffers — flush all dirty buffers for a relation.
void FlushRelationBuffers(SmgrRelation smgr_reln);

// DropRelationBuffers — remove all buffers for a relation from the pool.
// Used when a relation is dropped or truncated.
void DropRelationBuffers(RelFileNode rnode);

// BufferIsPinned — check if a buffer is currently pinned.
bool BufferIsPinned(Buffer buffer);

// Initialize the buffer pool with the given number of buffers.
// Must be called once at startup before any ReadBuffer call.
void InitBufferPool(int n_buffers);

// Shutdown the buffer pool (flush all dirty buffers and free memory).
void ShutdownBufferPool();

}  // namespace mytoydb::storage
