// xact.h — Transaction state machine and lifecycle management.
//
// Converted from PostgreSQL 15's src/include/access/xact.h.
//
// PostgreSQL uses a stack of TransactionStateData records to track:
//   - Top-level transactions (BEGIN/COMMIT)
//   - Subtransactions (SAVEPOINT)
//   - Per-command state (autocommit vs. explicit transaction)
//
// The state machine has two orthogonal dimensions:
//   TransState  — low-level state (START, INPROGRESS, COMMIT, ABORT)
//   TBlockState — high-level block state (DEFAULT, BEGIN, INPROGRESS, END, ...)
//
// pgcpp preserves this structure for fidelity. Subtransactions are
// supported at the API level but the implementation is simplified
// (no resource owners, no GUC nesting).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "transaction/transam.hpp"

namespace pgcpp::transaction {

// SubTransactionId — identifier for a subtransaction (savepoint).
// 0 = InvalidSubTransactionId, 1 = TopSubTransactionId.
using SubTransactionId = uint32_t;

constexpr SubTransactionId kInvalidSubTransactionId = 0;
constexpr SubTransactionId kTopSubTransactionId = 1;

// CommandId — command counter within a transaction.
// Used to distinguish successive commands (INSERT, UPDATE, ...) within the
// same transaction so that a query can see its own earlier changes.
using CommandId = uint32_t;

constexpr CommandId kFirstCommandId = 0;
constexpr CommandId kInvalidCommandId = 0xFFFFFFFF;

// TransState — low-level transaction state.
enum class TransState {
    kDefault,     // idle (no transaction active)
    kStart,       // transaction starting
    kInProgress,  // transaction in progress
    kCommit,      // commit in progress
    kAbort,       // abort in progress
    kPrepare,     // prepare in progress (2PC; unused in pgcpp)
};

// TBlockState — high-level transaction block state.
// Tracks where we are in the BEGIN/COMMIT/SAVEPOINT state machine.
enum class TBlockState {
    kDefault,          // idle (no transaction block)
    kStarted,          // single-query transaction (autocommit)
    kBegin,            // BEGIN received, starting transaction block
    kInProgress,       // inside a transaction block
    kEnd,              // COMMIT received, ending transaction block
    kAbort,            // ROLLBACK received, aborting transaction block
    kAbortEnd,         // abort completed
    kAbortPending,     // abort pending (e.g., error during commit)
    kSubBegin,         // SAVEPOINT received
    kSubInProgress,    // inside a subtransaction
    kSubRelease,       // RELEASE SAVEPOINT received
    kSubCommit,        // committing a subtransaction
    kSubAbort,         // ROLLBACK TO SAVEPOINT received
    kSubAbortPending,  // abort pending in subtransaction
    kSubRestart,       // ROLLBACK TO + re-entering subtransaction
    kSubAbortRestart,  // abort during subtransaction restart
};

// IsolationLevel — transaction isolation level (PostgreSQL's XactIsoLevel).
// pgcpp defaults to READ COMMITTED, matching PostgreSQL.
enum class IsolationLevel {
    kReadUncommitted,  // effectively same as READ COMMITTED
    kReadCommitted,
    kRepeatableRead,
    kSerializable,  // SSI — requires predicate locks + conflict detection
};

// TransactionStateData — per-transaction state record.
//
// These form a stack: the top of the stack is the current (sub)transaction.
// Pushing occurs at SAVEPOINT; popping at RELEASE/ROLLBACK TO.
struct TransactionStateData {
    TransactionId transaction_id = kInvalidTransactionId;
    SubTransactionId sub_transaction_id = kInvalidSubTransactionId;
    std::string name;  // savepoint name (empty for top-level)
    int savepoint_level = 0;
    TransState state = TransState::kDefault;
    TBlockState block_state = TBlockState::kDefault;
    int nesting_level = 0;
    CommandId command_id = kFirstCommandId;
    CommandId command_id_before_subxact = kFirstCommandId;
    IsolationLevel isolation_level = IsolationLevel::kReadCommitted;
    bool read_only = false;
    bool deferrable = false;
    TransactionStateData* parent = nullptr;
};

// TransactionState — pointer to the current transaction state.
using TransactionState = TransactionStateData*;

// --- Public API (PostgreSQL-compatible names) ---

// IsTransactionBlock — true if we're inside an explicit BEGIN/COMMIT block.
bool IsTransactionBlock();

// IsAbortedTransactionBlock — true if we're inside a transaction block that
// has failed (an error occurred and the user must issue ROLLBACK to exit).
// Used to send the 'E' (in failed transaction) status byte in ReadyForQuery.
bool IsAbortedTransactionBlock();

// IsTransactionOrTransactionBlock — true if any transaction is active
// (either autocommit single-statement or explicit block).
bool IsTransactionOrTransactionBlock();

// TransactionBlockStateAsString — return a human-readable state name
// (for debugging/error messages).
const char* TransactionBlockStateAsString();

// GetCurrentTransactionId — return the current transaction's XID.
// Allocates a new XID if this is the first use in the transaction
// (PostgreSQL defers XID assignment until first write).
TransactionId GetCurrentTransactionId();

// GetCurrentTransactionIdIfAny — return the current XID, or
// InvalidTransactionId if no transaction is active.
TransactionId GetCurrentTransactionIdIfAny();

// GetCurrentSubTransactionId — return the current subtransaction ID.
SubTransactionId GetCurrentSubTransactionId();

// GetCurrentTransactionNestingLevel — return the nesting depth
// (0 = no transaction, 1 = top-level, 2+ = subtransactions).
int GetCurrentTransactionNestingLevel();

// GetCurrentCommandId — return the current command ID.
// If used_in_trigger is true, returns a frozen command ID (for triggers).
CommandId GetCurrentCommandId(bool used_in_trigger = false);

// CommandCounterIncrement — increment the command counter.
// Called after each SQL command within a transaction so that subsequent
// commands can see the effects of earlier ones.
void CommandCounterIncrement();

// --- Transaction block control (BEGIN/COMMIT/ROLLBACK) ---

// BeginTransactionBlock — called when a BEGIN is received.
// Returns true if the block was started, false if already in a block.
bool BeginTransactionBlock();

// EndTransactionBlock — called when a COMMIT is received.
// Returns true if the commit succeeded, false on failure (will abort).
bool EndTransactionBlock();

// AbortTransactionBlock — called when a ROLLBACK is received.
void AbortTransactionBlock();

// PrepareTransactionBlock — 2PC prepare. Captures the current transaction's
// state (XID, isolation level, flags) into a TwoPhaseState record, persists
// it to pg_twophase/, and pops the transaction state stack WITHOUT committing
// or aborting. The XID remains "in-progress" in the CLOG until COMMIT
// PREPARED or ROLLBACK PREPARED is issued.
// Returns true on success, false on validation failure (no active block,
// subtransactions open, duplicate GID).
bool PrepareTransactionBlock(const std::string& gid);

// --- Per-command control (autocommit) ---

// StartTransactionCommand — called at the start of each query.
// If not in an explicit block, starts an autocommit transaction.
void StartTransactionCommand();

// CommitTransactionCommand — called at the end of each query.
// If in an autocommit transaction, commits it. If in an explicit block,
// just increments the command counter.
void CommitTransactionCommand();

// AbortCurrentTransaction — called when a query fails.
// Aborts the current command, rolling back any changes.
void AbortCurrentTransaction();

// --- Savepoint / subtransaction control ---

// BeginSavepoint — define a new savepoint within the current transaction.
void BeginSavepoint(const std::string& name);

// ReleaseSavepoint — release a savepoint (commit the subtransaction).
void ReleaseSavepoint(const std::string& name);

// RollbackToSavepoint — roll back to a savepoint.
void RollbackToSavepoint(const std::string& name);

// --- Initialization ---

// InitializeTransactionSystem — set up the transaction state stack.
// Must be called once at startup.
void InitializeTransactionSystem();

// --- Internal (used by visibility checks) ---

// TransactionIdIsCurrentTransactionId — true if the XID belongs to the
// current transaction or any of its subtransactions. Declared in transam.h
// but implemented here.

// GetTopLevelTransactionId — return the top-level transaction's XID.
TransactionId GetTopLevelTransactionId();

// GetTopLevelTransactionIdIfAny — return the top-level XID, or
// InvalidTransactionId if none.
TransactionId GetTopLevelTransactionIdIfAny();

// --- Isolation level ---

// GetTransactionIsolationLevel — return the current transaction's isolation
// level (defaults to READ COMMITTED).
IsolationLevel GetTransactionIsolationLevel();

// SetTransactionIsolationLevel — set the isolation level for the current
// transaction. Must be called before any data access (PostgreSQL allows
// SET TRANSACTION ISOLATION LEVEL only at the start of a transaction block).
void SetTransactionIsolationLevel(IsolationLevel level);

// SetTransactionReadOnly — mark the current transaction READ ONLY (or not).
// Applied from BEGIN TRANSACTION READ ONLY.
void SetTransactionReadOnly(bool read_only);

// SetTransactionDeferrable — mark the current transaction DEFERRABLE (or
// not). Applied from BEGIN TRANSACTION DEFERRABLE. Only meaningful for
// SERIALIZABLE READ ONLY DEFERRABLE transactions.
void SetTransactionDeferrable(bool deferrable);

// GetTransactionReadOnly — return the current transaction's READ ONLY flag.
bool GetTransactionReadOnly();

// GetTransactionDeferrable — return the current transaction's DEFERRABLE flag.
bool GetTransactionDeferrable();

// ParseIsolationLevelName — convert a string name (e.g. "serializable",
// "repeatable read") to an IsolationLevel enum value. Returns
// kReadCommitted for unrecognized names (matching PostgreSQL's default).
IsolationLevel ParseIsolationLevelName(const std::string& name);

// IsolationLevelName — convert an IsolationLevel to its string name.
std::string IsolationLevelName(IsolationLevel level);

}  // namespace pgcpp::transaction
