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

#include "pgcpp/storage/block.hpp"
#include "pgcpp/storage/buf_internals.hpp"
#include "pgcpp/storage/bufpage.hpp"
#include "pgcpp/storage/relfilenode.hpp"
#include "pgcpp/storage/smgr.hpp"

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

// --- M6 P0 extensions (Task 15.7.1) ---
//
// These functions extend the buffer manager to cover the remaining P0 API
// surface from PostgreSQL's bufmgr.c. MyToyDB simplifies them for the
// single-process, no-WAL model: hint dirty == dirty, and DROP/FLUSH by
// database falls back to scanning the whole pool.

// MarkBufferDirtyHint — mark a buffer as "hint dirty" (hint-bit writeback).
// In PostgreSQL this generates a special WAL record only if the page LSN is
// newer than the redo pointer. MyToyDB has no WAL, so a hint dirty is
// equivalent to a normal dirty mark. If `release` is true, the buffer is
// unpinned after being marked (matching PG's MarkBufferDirtyHint signature).
void MarkBufferDirtyHint(Buffer buffer, bool release);

// ReleaseAndReadBuffer — release (unpin) `buffer` and read the page
// (reln, forknum, blocknum). Optimization: if the old buffer's tag matches
// the new (reln, forknum, blocknum), the buffer is reused (no release/read).
Buffer ReleaseAndReadBuffer(Buffer buffer, SmgrRelation reln, ForkNumber forknum,
                            BlockNumber blocknum, ReadBufferMode mode);

// IncrBufferRefCount — increment the private (per-backend) refcount of a
// buffer handle. MyToyDB stores refcount directly in BufferDesc, so this
// is a thin wrapper around PinBuffer.
void IncrBufferRefCount(Buffer buffer);

// BufferGetTag — fill in (rnode, forknum, blocknum) for the given buffer.
void BufferGetTag(Buffer buffer, RelFileNode* rnode, ForkNumber* forknum, BlockNumber* blocknum);

// DropRelFileNodeBuffers — drop all buffers matching the given
// RelFileNodeBackend + forknum. Used by TRUNCATE / per-fork DDL.
void DropRelFileNodeBuffers(RelFileNodeBackend rnode, ForkNumber forknum);

// DropDatabaseBuffers — drop all buffers belonging to the given database.
// MyToyDB simplification: scan the entire pool and drop matching buffers.
void DropDatabaseBuffers(Oid dbid);

// FlushDatabaseBuffers — flush all dirty buffers belonging to the given
// database to disk.
void FlushDatabaseBuffers(Oid dbid);

// ReadBufferWithoutRelcache — read a buffer bypassing the relcache (used by
// WAL redo / recovery). MyToyDB simplification: open the SmgrRelation for
// the rnode and call ReadBuffer.
Buffer ReadBufferWithoutRelcache(RelFileNodeBackend rnode, ForkNumber forknum, BlockNumber blocknum,
                                 ReadBufferMode mode, BufferAccessStrategy strategy);

// --- M6 P0 extensions (Task 15.7.3): access strategy ring ---

// BufferAccessStrategyData — ring buffer for bulk access strategies
// (BAS_BULKREAD / BAS_BULKWRITE / BAS_VACUUM). MyToyDB simplification: the
// ring is allocated for API compatibility but the clock sweep is still used
// for victim selection (single-process, the ring reuse optimization is not
// needed for correctness). PostgreSQL uses the ring to reuse a small set of
// buffers during sequential scans / COPY / VACUUM to avoid evicting the
// entire cache.
struct BufferAccessStrategyData {
    BufferAccessStrategy type = BufferAccessStrategy::kNormal;
    int ring_size = 0;
    std::vector<Buffer> ring;
    int current = 0;
};
using BufferAccessStrategyHandle = BufferAccessStrategyData*;

// GetAccessStrategy — create a BufferAccessStrategy of the given type.
// Ring sizes: BULKREAD=32, BULKWRITE=32, VACUUM=32, NORMAL=0 (no ring).
BufferAccessStrategyHandle GetAccessStrategy(BufferAccessStrategy btype);

// FreeAccessStrategy — release a strategy allocated by GetAccessStrategy.
void FreeAccessStrategy(BufferAccessStrategyHandle strategy);

// StrategyFreeBuffer — return a buffer to the freelist. MyToyDB is
// single-process, so this is a no-op (the clock sweep reclaims the slot).
void StrategyFreeBuffer(Buffer buffer);

}  // namespace mytoydb::storage
