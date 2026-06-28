// procarray.h — Process array: tracks all running transactions.
//
// Converted from PostgreSQL 15's src/include/storage/procarray.h.
//
// ProcArray is the authoritative list of all currently-running backend
// transactions. It is used to compute snapshots (which XIDs are visible),
// determine the oldest running XID (for VACUUM), and build the snapshot of
// running XIDs for standby.
//
// In MyToyDB (single-process), ProcArray tracks the current transaction
// plus any concurrent "virtual" backends for testing.
#pragma once

#include <cstdint>
#include <vector>

#include "mytoydb/transaction/transam.hpp"

namespace mytoydb::transaction {

// InitializeProcArray — set up the process array (clear it).
void InitializeProcArray();

// ResetProcArray — clear the process array (for testing).
void ResetProcArray();

// ProcArrayAdd — register a running transaction with the given XID.
// In PG, this is done by ProcArrayAdd (called at transaction start).
void ProcArrayAdd(TransactionId xid);

// ProcArrayRemove — remove a transaction from the array (called at commit/abort).
void ProcArrayRemove(TransactionId xid);

// GetOldestXmin — the oldest XID that could still be running, used by VACUUM
// as the cutoff for removing dead tuples. Returns FrozenTransactionId if
// no transactions are running. If `ignore` is non-zero, it is excluded.
TransactionId GetOldestXmin(TransactionId ignore = kInvalidTransactionId);

// GetRunningTransactionData — return all currently-running XIDs.
std::vector<TransactionId> GetRunningTransactionData();

// CountRunningXacts — number of running transactions.
int CountRunningXacts();

// ProcArrayContains — true if `xid` is currently in the array (running).
bool ProcArrayContains(TransactionId xid);

}  // namespace mytoydb::transaction
