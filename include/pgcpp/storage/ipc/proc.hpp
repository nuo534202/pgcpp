// proc.h — PGPROC struct (per-backend state) and process array helpers.
//
// Converted from PostgreSQL 15's src/include/storage/proc.h and
// src/backend/storage/ipc/proc.c.
//
// A PGPROC entry is the shared-memory state for one backend: it carries the
// backend's XID, lock-wait state, latch, and a slot in the ProcArray. PG
// allocates a fixed-size array of PGPROCs at server startup.
//
// MyToyDB is single-process, so there is typically only one PGPROC at a
// time, but we keep the structure and an array API for fidelity (and for
// the auxiliary-process / WAL-recovery use cases).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pgcpp/transaction/transam.hpp"

namespace mytoydb::storage {

class Latch;  // forward decl from latch.hpp

// ProcStatus — mirrors PG's PROC_WAIT_STATUS / backend state.
enum class ProcStatus {
    kIdle,     // PROC_WAIT_STATUS_OK (not waiting)
    kWaiting,  // waiting for a lock
    kBlocked,  // deadlock detected
};

// PGPROC — per-backend state.
struct PGPROC {
    int pid = 0;  // OS process id (0 = unused slot)
    mytoydb::transaction::TransactionId xid =
        mytoydb::transaction::kInvalidTransactionId;  // current top-level XID
    mytoydb::transaction::TransactionId xmin =
        mytoydb::transaction::kInvalidTransactionId;  // snapshot xmin
    int lxid = 0;                                     // local XID (in-process counter)
    ProcStatus status = ProcStatus::kIdle;
    Latch* procLatch = nullptr;  // owned externally; PGPROC does not own it
    std::string backend_id;      // logical backend identifier
    bool is_aux = false;         // true if this is an auxiliary process
};

// InitProcess — initialize a PGPROC for the current backend.
// Returns a pointer to the PGPROC; nullptr if no slot is available.
PGPROC* InitProcess();

// ProcGlobalInit — initialize the PGPROC pool (idempotent).
void ProcGlobalInit();

// ProcGlobalReset — drop all PGPROC slots (used by tests).
void ProcGlobalReset();

// GetMyProc — return the PGPROC for the current backend (set by InitProcess).
// Returns nullptr if InitProcess has not been called yet.
PGPROC* GetMyProc();

// SetMyProc — install a PGPROC pointer for the current backend (test hook).
void SetMyProc(PGPROC* proc);

// ProcArrayShmemSize — PG's reserved-shared-memory estimate; in MyToyDB this
// is just the in-memory size of the pool.
int ProcArrayShmemSize();

// NumProcs — number of currently-active PGPROC slots.
int NumProcs();

// AllProcs — return the list of all currently-active PGPROCs (for inspection).
std::vector<PGPROC*> AllProcs();

}  // namespace mytoydb::storage
