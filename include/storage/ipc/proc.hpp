// proc.h — PGPROC struct (per-backend state) and process array helpers.
//
// Converted from PostgreSQL 15's src/include/storage/proc.h and
// src/backend/storage/ipc/proc.c.
//
// A PGPROC entry is the shared-memory state for one backend: it carries the
// backend's XID, lock-wait state, latch, and a slot in the ProcArray. PG
// allocates a fixed-size array of PGPROCs at server startup.
//
// pgcpp allocates the PGPROC pool in shared memory (via ShmemInitStruct) so
// fork'd backends share the same pool. `InitProcess` claims a slot from the
// freelist; `ProcKill` returns it. In test mode (no ShmemInit called),
// ShmemInitStruct falls back to process-local allocation, so the pool works
// identically in single-process unit tests.
#pragma once

#include <cstdint>
#include <vector>

#include "transaction/transam.hpp"

namespace pgcpp::storage {

class Latch;  // forward decl from latch.hpp

// MaxBackends — upper bound on the number of concurrent backends. Used to
// size the PGPROC pool and ProcArray in shared memory. Matches the default
// max_connections in postmaster.
constexpr int kMaxBackends = 100;

// ProcStatus — mirrors PG's PROC_WAIT_STATUS / backend state.
enum class ProcStatus {
    kIdle,     // PROC_WAIT_STATUS_OK (not waiting)
    kWaiting,  // waiting for a lock
    kBlocked,  // deadlock detected
};

// PGPROC — per-backend state.
//
// All fields are plain types (no std::string / std::vector) so the struct
// can live in shared memory. The `next` field links PGPROCs in the freelist.
struct PGPROC {
    int pid = 0;  // OS process id (0 = unused slot)
    pgcpp::transaction::TransactionId xid =
        pgcpp::transaction::kInvalidTransactionId;  // current top-level XID
    pgcpp::transaction::TransactionId xmin =
        pgcpp::transaction::kInvalidTransactionId;  // snapshot xmin
    int lxid = 0;                                   // local XID (in-process counter)
    ProcStatus status = ProcStatus::kIdle;
    Latch* procLatch = nullptr;  // owned externally; PGPROC does not own it
    char backend_id[16] = {};    // logical backend identifier (fixed-size for shm)
    int backend_id_num = 0;      // numeric backend id
    bool is_aux = false;         // true if this is an auxiliary process

    // Freelist linkage (next free PGPROC, or nullptr if none).
    PGPROC* next = nullptr;
};

// InitProcess — initialize a PGPROC for the current backend.
// Claims a slot from the shared freelist (under kProcArrayLock).
// Returns a pointer to the PGPROC; nullptr if no slot is available.
PGPROC* InitProcess();

// ProcKill — release the current backend's PGPROC back to the freelist.
// Called when a backend exits.
void ProcKill();

// ProcGlobalInit — initialize the PGPROC pool in shared memory.
// Allocates the pool via ShmemInitStruct and builds the freelist if
// this is the first caller (postmaster). Idempotent.
void ProcGlobalInit();

// ProcGlobalReset — drop all PGPROC slots (used by tests).
// Clears the freelist and all slots.
void ProcGlobalReset();

// GetMyProc — return the PGPROC for the current backend (set by InitProcess).
// Returns nullptr if InitProcess has not been called yet.
PGPROC* GetMyProc();

// SetMyProc — install a PGPROC pointer for the current backend (test hook).
void SetMyProc(PGPROC* proc);

// ProcArrayShmemSize — shared-memory bytes needed for the PGPROC pool.
std::size_t ProcArrayShmemSize();

// NumProcs — number of currently-active PGPROC slots (pid != 0).
int NumProcs();

// AllProcs — return the list of all currently-active PGPROCs (for inspection).
std::vector<PGPROC*> AllProcs();

}  // namespace pgcpp::storage
