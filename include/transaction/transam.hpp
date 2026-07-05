// transam.h — Transaction ID type and commit-log status tracking.
//
// Converted from PostgreSQL 15's src/include/access/transam.h.
//
// A TransactionId (XID) is a 32-bit monotonically increasing identifier
// assigned to each transaction. XIDs wrap around at 2^32, requiring careful
// comparison using modular arithmetic (see TransactionIdPrecedes).
//
// Special XIDs (reserved values that never appear in tuple headers):
//   0 — InvalidTransactionId (sentinel for "no transaction")
//   1 — BootstrapTransactionId (initial catalog setup; always committed)
//   2 — FrozenTransactionId (replaces very old XIDs during VACUUM; always
//        considered committed and older than every normal XID)
//   3 — FirstNormalTransactionId (first XID assigned to a user transaction)
//
// The commit log (CLOG) records the status of each normal XID:
//   IN_PROGRESS, COMMITTED, ABORTED, or SUB_COMMITTED (for subtransactions).
//
// pgcpp allocates the CLOG and VariableCache in shared memory (via
// ShmemInitStruct) so fork'd backends share the same transaction state.
// kCLogControlLock serializes all mutations. In test mode (no ShmemInit
// called), ShmemInitStruct falls back to process-local allocation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::transaction {

// MaxXids — upper bound on the number of XIDs the CLOG can hold.
// 16M XIDs × 1 byte = 16MB. Plenty for pgcpp's target workload.
constexpr int kMaxXids = 1 << 24;

// TransactionId — the fundamental transaction identifier.
// PostgreSQL typedefs this globally; pgcpp keeps it in the transaction
// namespace (see rules/04-naming-and-namespace.md).
using TransactionId = uint32_t;

// MultiXactId — identifier for a set of multixact members (used for row
// locking). pgcpp does not implement multixact yet; this placeholder
// matches PostgreSQL's type so struct layouts are stable.
using MultiXactId = uint32_t;

// MultiXactOffset — offset into the multixact members file.
using MultiXactOffset = uint32_t;

// --- Special transaction IDs ---
//
// These reserved values are never assigned to a real transaction and never
// appear in tuple headers (except FrozenTransactionId, which replaces very
// old committed XIDs during VACUUM FREEZE).

// InvalidTransactionId — sentinel meaning "no transaction".
constexpr TransactionId kInvalidTransactionId = 0;

// BootstrapTransactionId — used during catalog bootstrap. Always committed.
constexpr TransactionId kBootstrapTransactionId = 1;

// FrozenTransactionId — used to freeze old tuples. Always considered
// committed and older than every normal transaction.
constexpr TransactionId kFrozenTransactionId = 2;

// FirstNormalTransactionId — the first XID assigned to a user transaction.
constexpr TransactionId kFirstNormalTransactionId = 3;

// VariableCacheData — shared cache of transaction ID limits (PostgreSQL's
// ShmemVariableCache). Lives in shared memory; protected by kXactGenLock.
struct VariableCacheData {
    TransactionId nextXid = kFirstNormalTransactionId;
    TransactionId oldestXid = kFirstNormalTransactionId;
};

// FirstBootstrapObjectId — first OID assigned during bootstrap (matches
// PostgreSQL's FirstBootstrapObjectId). Kept here for catalog init.
constexpr TransactionId kFirstBootstrapObjectId = 16384;

// MaxTransactionId — the largest possible XID (before wraparound).
constexpr TransactionId kMaxTransactionId = 0xFFFFFFFF;

// TransactionIdPrecedes — modular comparison.
//
// XIDs wrap around at 2^32, so we use modular arithmetic: a < b if
// (b - a) < 2^31 (i.e., b is "less than half a circle ahead" of a).
// This matches PostgreSQL's TransactionIdPrecedes macro.
inline bool TransactionIdPrecedes(TransactionId id1, TransactionId id2) {
    int32_t diff = static_cast<int32_t>(id1 - id2);
    return diff < 0;
}

// TransactionIdFollows — modular comparison (inverse of Precedes).
inline bool TransactionIdFollows(TransactionId id1, TransactionId id2) {
    int32_t diff = static_cast<int32_t>(id1 - id2);
    return diff > 0;
}

// TransactionIdPrecedesOrEquals — modular <=.
inline bool TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2) {
    int32_t diff = static_cast<int32_t>(id1 - id2);
    return diff <= 0;
}

// TransactionIdFollowsOrEquals — modular >=.
inline bool TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2) {
    int32_t diff = static_cast<int32_t>(id1 - id2);
    return diff >= 0;
}

// TransactionIdEquals — exact equality (not modular; XIDs are unique until
// wraparound, at which point wraparound handling is required).
inline bool TransactionIdEquals(TransactionId id1, TransactionId id2) {
    return id1 == id2;
}

// TransactionIdIsValid — true if the XID is not InvalidTransactionId.
inline bool TransactionIdIsValid(TransactionId xid) {
    return xid != kInvalidTransactionId;
}

// TransactionIdIsNormal — true if the XID is a normal user transaction
// (i.e., >= FirstNormalTransactionId). Special XIDs (bootstrap, frozen)
// have hardcoded visibility and are never looked up in the commit log.
inline bool TransactionIdIsNormal(TransactionId xid) {
    return xid >= kFirstNormalTransactionId;
}

// TransactionIdIsCurrentTransactionId — forward declaration; implemented
// in xact.cpp (where the current-transaction stack lives).
bool TransactionIdIsCurrentTransactionId(TransactionId xid);

// --- Commit log (CLOG) status ---
//
// XidStatus records the state of a transaction in the commit log.
// Matches PostgreSQL's XidStatus enum values.
enum class XidStatus : uint8_t {
    kInProgress = 0,    // TRANSACTION_STATUS_IN_PROGRESS
    kCommitted = 1,     // TRANSACTION_STATUS_COMMITTED
    kAborted = 2,       // TRANSACTION_STATUS_ABORTED
    kSubCommitted = 3,  // TRANSACTION_STATUS_SUB_COMMITTED
};

// --- Commit log API ---
//
// In PostgreSQL, these read/write the CLOG pages in shared memory.
// pgcpp uses an in-memory vector that grows as XIDs are assigned.

// Initialize the commit log. Must be called once at startup before any
// transaction is started.
void InitializeCommitLog();

// Get the status of a transaction from the commit log.
// Special XIDs (bootstrap, frozen) return kCommitted without lookup.
XidStatus TransactionIdGetStatus(TransactionId xid);

// TransactionLogFetch — return the commit-log status of `xid` with a
// single-entry cache (PostgreSQL's TransactionLogFetch equivalent).
// Special XIDs return kCommitted without consulting or polluting the cache.
// TransactionIdGetStatus forwards here, and the commit/abort/in-progress
// mutators invalidate the cached entry when they modify it.
XidStatus TransactionLogFetch(TransactionId xid);

// Did the transaction commit? (true for committed or frozen/bootstrap XIDs).
bool TransactionIdDidCommit(TransactionId xid);

// Did the transaction abort? (false for special XIDs).
bool TransactionIdDidAbort(TransactionId xid);

// Record a transaction as committed in the commit log.
void TransactionIdCommit(TransactionId xid);

// Record a transaction as aborted in the commit log.
void TransactionIdAbort(TransactionId xid);

// Record a transaction as in-progress (used when starting a transaction).
void TransactionIdSetInProgress(TransactionId xid);

// --- XID assignment ---
//
// Assigns the next normal XID. In PostgreSQL, this is done by the
// transaction manager (procArray + ShmemVariableCache->nextXid).
// pgcpp uses a simple monotonic counter.

// Allocate the next transaction ID (does not record status).
TransactionId AllocateNextTransactionId();

// Get the last allocated transaction ID (the "next XID" minus one).
TransactionId GetNextTransactionId();

// Reset the transaction ID counter and commit log (for testing).
void ResetTransactionState();

// CLogShmemSize — shared-memory bytes needed for the CLOG + VariableCache.
std::size_t CLogShmemSize();

// --- CLOG persistence (pg_xact/) ---
//
// pgcpp persists the CLOG to <data_dir>/pg_xact/ in PostgreSQL-compatible
// format: 2 bits per XID, 8 KB pages, file name = hex page number.
// In memory, pgcpp keeps 1 byte per XID (XidStatus) for simplicity; the
// 2-bit encoding is applied only on disk. Dirty pages are tracked and
// flushed by the checkpointer and on shutdown.

// ClogXidsPerPage — XIDs per CLOG page (8 KB × 4 XIDs/byte = 32768).
constexpr int kClogXidsPerPage = 32768;

// SetClogDirectory — set the directory where CLOG pages are persisted.
// Called once at server startup with <data_dir>/pg_xact.
void SetClogDirectory(const std::string& dir);

// LoadClogFiles — load CLOG from disk into the shared-memory array.
// Called at startup. A missing directory is not an error (fresh initdb).
void LoadClogFiles();

// FlushClogFiles — write dirty CLOG pages to disk and fsync.
// Called by the checkpointer and on shutdown.
void FlushClogFiles();

// ShutdownClog — flush dirty pages (no file descriptors to close).
void ShutdownClog();

// ResetClogPersistence — clear the CLOG directory and dirty-page tracking
// (for testing).
void ResetClogPersistence();

}  // namespace pgcpp::transaction
