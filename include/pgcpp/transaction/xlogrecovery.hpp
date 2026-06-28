// xlogrecovery.h — Crash recovery: read WAL from start and replay each record.
//
// Converted from PostgreSQL 15's src/backend/access/transam/xlogrecovery.c.
//
// Crash recovery reads every WAL record from the beginning of the log and
// dispatches each to the appropriate resource manager's redo function. The
// redo function re-applies the logged change to the data page or in-memory
// state, reconstructing the database to a consistent state.
//
// In MyToyDB, the "redo" functions are registered per-RMGR and called in
// record order. This provides a testable crash-recovery loop.
#pragma once

#include <cstdint>
#include <functional>

#include "pgcpp/transaction/transam.hpp"
#include "pgcpp/transaction/xlog.hpp"
#include "pgcpp/transaction/xlogreader.hpp"

namespace mytoydb::transaction {

// RecoveryStats — statistics from a recovery pass.
struct RecoveryStats {
    uint64_t records_replayed = 0;             // total records processed
    uint64_t records_skipped = 0;              // records with no redo function
    RmgrId last_rmid = 0;                      // RMGR of the last replayed record
    XLogRecPtr last_lsn = kInvalidXLogRecPtr;  // LSN of the last replayed record
};

// RedoFn — a resource manager's redo function. Called once per record.
// `rec` is the record header; `data` is the payload; `lsn` is the record LSN.
using RedoFn = std::function<void(const XLogRecord& rec, const uint8_t* data, std::size_t len,
                                  XLogRecPtr lsn)>;

// RegisterRmgrRedo — register a redo function for the given resource manager.
// Replaces any previously-registered function for that RMGR.
void RegisterRmgrRedo(RmgrId rmid, RedoFn fn);

// ClearRmgrRedo — remove all registered redo functions (for testing).
void ClearRmgrRedo();

// PerformCrashRecovery — read every WAL record from the beginning and call
// the matching redo function. Returns recovery statistics.
//
// If a record's RMGR has no registered redo function, it is counted as
// skipped (not an error). Recovery stops at end-of-WAL.
RecoveryStats PerformCrashRecovery();

// PerformCrashRecoveryFrom — like PerformCrashRecovery but starts reading
// from `start_lsn` instead of the beginning of the WAL.
RecoveryStats PerformCrashRecoveryFrom(XLogRecPtr start_lsn);

// Get the redo function registered for a RMGR (returns nullptr if none).
RedoFn GetRmgrRedo(RmgrId rmid);

}  // namespace mytoydb::transaction
