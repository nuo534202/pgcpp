// twophase.hpp — Two-phase commit (2PC) prepared transaction support.
//
// Converted from PostgreSQL 15's src/backend/access/transam/twophase.c.
//
// pgcpp implements the SQL interface for 2PC:
//   PREPARE TRANSACTION 'gid'
//   COMMIT PREPARED 'gid'
//   ROLLBACK PREPARED 'gid'
//
// A prepared transaction is "in limbo": neither committed nor aborted.
// Its XID remains "in-progress" in the CLOG until COMMIT PREPARED or
// ROLLBACK PREPARED is issued. The transaction state (gid, xid, isolation
// level, read-only/deferrable flags) is persisted to pg_twophase/ so it
// survives crashes and restarts.
//
// pgcpp simplifications (single-process mode):
//   - Catalog (DDL) changes are committed at PREPARE time, not at COMMIT
//     PREPARED time. ROLLBACK PREPARED does NOT undo DDL changes. In real
//     PostgreSQL, WAL replay handles this. This is a documented limitation.
//   - No distributed commit coordination (single backend).
//   - No lock conflict checks against prepared transactions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "transaction/transam.hpp"
#include "transaction/xact.hpp"

namespace pgcpp::transaction {

// TwoPhaseState — saved state of a prepared transaction.
//
// Captured at PREPARE TRANSACTION time and persisted to disk so the
// transaction can be later committed or rolled back by GID.
struct TwoPhaseState {
    std::string gid;                            // user-supplied global identifier
    TransactionId xid = kInvalidTransactionId;  // assigned XID (stays in-progress)
    IsolationLevel isolation_level = IsolationLevel::kReadCommitted;
    bool read_only = false;
    bool deferrable = false;
};

// --- Prepared transaction lifecycle ---

// SaveTwoPhaseState — record a prepared transaction. Called by
// PrepareTransactionBlock after capturing the current transaction's state.
// Persists to disk immediately. Throws ereport(ERROR) if the GID already
// exists.
void SaveTwoPhaseState(const TwoPhaseState& state);

// RemoveTwoPhaseState — remove a prepared transaction record by GID.
// Returns true if found and removed, false if not found.
bool RemoveTwoPhaseState(const std::string& gid);

// LookupTwoPhaseState — return the prepared transaction state for the
// given GID, or nullptr if not found.
const TwoPhaseState* LookupTwoPhaseState(const std::string& gid);

// CommitPreparedTransaction — commit a previously prepared transaction.
// Marks the XID committed in the CLOG and removes the record.
// Throws ereport(ERROR) if the GID is not found.
// Returns true on success.
bool CommitPreparedTransaction(const std::string& gid);

// RollbackPreparedTransaction — abort a previously prepared transaction.
// Marks the XID aborted in the CLOG and removes the record.
// Throws ereport(ERROR) if the GID is not found.
// Returns true on success.
bool RollbackPreparedTransaction(const std::string& gid);

// NumTwoPhaseStates — return the count of in-memory prepared transactions.
std::size_t NumTwoPhaseStates();

// --- Persistence (pg_twophase/) ---

// SetTwoPhaseDirectory — set the directory where prepared transactions
// are persisted. Called once at server startup with <data_dir>/pg_twophase.
// If the directory does not exist, it is created on first save.
void SetTwoPhaseDirectory(const std::string& dir);

// LoadTwoPhaseFiles — load prepared transactions from disk into memory.
// Called at startup. A missing directory is not an error (fresh initdb).
void LoadTwoPhaseFiles();

// FlushTwoPhaseFiles — persist all in-memory prepared transactions
// to disk. Called on shutdown.
void FlushTwoPhaseFiles();

// ResetTwoPhaseState — clear in-memory state and remove on-disk files
// (for testing).
void ResetTwoPhaseState();

}  // namespace pgcpp::transaction
