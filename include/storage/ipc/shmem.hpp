// shmem.h — Shared memory region management (in-process simulation).
//
// Converted from PostgreSQL 15's src/include/storage/shmem.h and
// src/backend/storage/ipc/shmem.c.
//
// PostgreSQL allocates a large shared-memory segment at server startup and
// sub-allocates named regions from it via a ShmemIndex hash table. Each
// subsystem (BufferPool, ProcArray, LWLock array, etc.) calls
// ShmemInitStruct() during IPC initialization to claim its region.
//
// pgcpp is single-process, so we keep a std::map<string, vector<uint8_t>>
// instead of a SysV/POSIX shared-memory segment. The API preserves PG's
// "named region" semantics for architectural fidelity.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pgcpp::storage {

// ShmemAddrIsValid — true if the pointer is within the shared-memory region.
// In pgcpp we always return true for non-null pointers (no contiguous
// shared-memory segment).
bool ShmemAddrIsValid(const void* addr);

// ShmemInitStruct — find or create a named shared-memory region of `size` bytes.
// Returns a pointer to the region. If found_in_ptr is non-null, *found_in_ptr
// is set to true if the region already existed (PG's ShmemInitStruct semantic).
void* ShmemInitStruct(const char* name, std::size_t size, bool* found_in_ptr);

// ShmemInitHash — convenience wrapper; in pgcpp this is identical to
// ShmemInitStruct since we use std::unordered_map rather than PG's dynahash.
void* ShmemInitHash(const char* name, std::size_t size);

// InitShmemIndex — initialize the named-region index. Idempotent.
void InitShmemIndex();

// ResetShmem — drop all regions (used by tests / re-init).
void ResetShmem();

// ShmemSize — total bytes currently held across all named regions.
std::size_t ShmemSize();

// NumShmemRegions — number of currently-registered regions.
int NumShmemRegions();

// CreateSharedMemoryAndSemaphores is defined in ipci.hpp (the dispatcher).
// shmem.cpp's role is to manage the named-region index (InitShmemIndex).

}  // namespace pgcpp::storage
