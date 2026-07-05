// procarray.h — Process array: tracks all running transactions.
//
// Converted from PostgreSQL 15's src/include/storage/procarray.h.
//
// ProcArray is the authoritative list of all currently-running backend
// transactions. It is used to compute snapshots (which XIDs are visible),
// determine the oldest running XID (for VACUUM), and build the snapshot of
// running XIDs for standby.
//
// pgcpp allocates the ProcArray in shared memory as an array of PGPROC slot
// indices (pgprocnos), parallel to the PGXACT compact array. InitProcess
// registers a backend by calling ProcArrayAdd(PGPROC*); ProcKill deregisters
// via ProcArrayRemove(PGPROC*). GetSnapshotData scans the PGXACT entries
// (via pgprocnos) for cache-line efficiency. All mutations are serialized
// by kProcArrayLock. In test mode (no ShmemInit called), ShmemInitStruct
// falls back to process-local allocation.
#pragma once

#include <cstdint>
#include <vector>

#include "transaction/transam.hpp"

namespace pgcpp::storage {
struct PGPROC;  // forward decl from storage/ipc/proc.hpp
}  // namespace pgcpp::storage

namespace pgcpp::transaction {

// InitializeProcArray — allocate the ProcArray in shared memory.
// Idempotent; called by postmaster at startup.
void InitializeProcArray();

// ResetProcArray — clear the ProcArray (for testing).
void ResetProcArray();

// ProcArrayAdd — register a backend (by its PGPROC slot) in the ProcArray.
// Called by InitProcess at backend startup. Stores the PGPROC's pool index
// (pgprocno) so snapshot scans can read the parallel PGXACT entry.
// Acquires kProcArrayLock exclusive.
void ProcArrayAdd(pgcpp::storage::PGPROC* proc);

// ProcArrayRemove — deregister a backend from the ProcArray.
// Called by ProcKill at backend exit.
// Acquires kProcArrayLock exclusive.
void ProcArrayRemove(pgcpp::storage::PGPROC* proc);

// GetOldestXmin — the oldest XID that could still be running, used by VACUUM
// as the cutoff for removing dead tuples. Returns FrozenTransactionId if
// no transactions are running. If `ignore` is non-zero, it is excluded.
// Acquires kProcArrayLock shared.
TransactionId GetOldestXmin(TransactionId ignore = kInvalidTransactionId);

// GetRunningTransactionData — return all currently-running XIDs.
// Acquires kProcArrayLock shared.
std::vector<TransactionId> GetRunningTransactionData();

// GetRunningTransactionData — fill `xip_out` with running XIDs, set
// `xmax_out` to nextXid and `xmin_out` to oldestXmin. More efficient than
// the vector overload when the caller already has a buffer.
// Acquires kProcArrayLock shared.
void GetRunningTransactionData(std::vector<TransactionId>& xip_out, TransactionId* xmax_out,
                               TransactionId* xmin_out);

// CountRunningXacts — number of running transactions (backends with a valid
// XID in their PGXACT entry). Acquires kProcArrayLock shared.
int CountRunningXacts();

// ProcArrayContains — true if `xid` is currently in any registered backend's
// PGXACT entry (i.e., a backend with that XID is running).
// Acquires kProcArrayLock shared.
bool ProcArrayContains(TransactionId xid);

// ProcArrayShmemSize — shared-memory bytes needed for the ProcArray
// (the pgprocnos index array).
std::size_t ProcArrayShmemSize();

}  // namespace pgcpp::transaction
