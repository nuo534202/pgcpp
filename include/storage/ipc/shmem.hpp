// shmem.h — Shared memory region management.
//
// Converted from PostgreSQL 15's src/include/storage/shmem.h and
// src/backend/storage/ipc/shmem.c.
//
// PostgreSQL allocates a large shared-memory segment at server startup and
// sub-allocates named regions from it via a ShmemIndex hash table. Each
// subsystem (BufferPool, ProcArray, LWLock array, etc.) calls
// ShmemInitStruct() during IPC initialization to claim its region.
//
// pgcpp uses mmap(MAP_SHARED|MAP_ANONYMOUS) for the shared segment. This
// is inherited across fork() automatically, so child backends see the same
// memory as the postmaster without needing shm_open. A fallback
// process-local allocation (std::map) is used when ShmemInit() has not been
// called (unit-test mode).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pgcpp::storage {

// Maximum number of named regions in the shared-memory index.
constexpr int kMaxShmemRegions = 64;

// Maximum length of a region name (including NUL).
constexpr int kShmemNameLen = 64;

// ShmemRegionEntry — one entry in the shared-memory named-region index.
// This struct lives inside the shared segment itself.
struct ShmemRegionEntry {
    char name[kShmemNameLen];
    std::size_t offset;
    std::size_t size;
};

// ShmemHeader — the header at the start of the shared-memory segment.
// Contains the bump-allocator pointer and the named-region index.
struct ShmemHeader {
    uint64_t magic;
    std::size_t total_size;
    std::size_t free_offset;
    int region_count;
    ShmemRegionEntry regions[kMaxShmemRegions];
};

// Magic number to verify that a shared segment is initialized.
constexpr uint64_t kShmemMagic = 0x5047435053484D45ULL;  // "PGCPSHME"

// ShmemAddrIsValid — true if the pointer is non-null.
bool ShmemAddrIsValid(const void* addr);

// ShmemInit — create the shared-memory segment (called by postmaster before
// fork). Uses mmap(MAP_SHARED|MAP_ANONYMOUS) so the segment is inherited by
// fork'd children. Returns true on success. If already initialized, returns
// true without re-creating.
bool ShmemInit(std::size_t total_size);

// ShmemAttach — attach to an existing shared-memory segment (called by a
// fork'd child backend). With MAP_ANONYMOUS|MAP_SHARED, the mapping is
// inherited automatically via fork(); this function verifies the magic
// number and returns true if the segment is valid. Returns false if no
// segment is present (single-process/test mode).
bool ShmemAttach();

// ShmemDetach — unmap the shared-memory segment (called on shutdown).
void ShmemDetach();

// IsShmemActive — returns true if ShmemInit has been called and the shared
// segment is active (i.e., we are in multi-process mode).
bool IsShmemActive();

// ShmemInitStruct — find or create a named shared-memory region of `size` bytes.
// Returns a pointer to the region. If found_in_ptr is non-null, *found_in_ptr
// is set to true if the region already existed (PG's ShmemInitStruct semantic).
// In multi-process mode, allocates from the shared segment. In single-process
// (test) mode, falls back to a process-local std::map.
void* ShmemInitStruct(const char* name, std::size_t size, bool* found_in_ptr);

// ShmemInitHash — convenience wrapper; identical to ShmemInitStruct.
void* ShmemInitHash(const char* name, std::size_t size);

// InitShmemIndex — initialize the named-region index. Idempotent.
void InitShmemIndex();

// ResetShmem — drop all regions (used by tests / re-init).
// In multi-process mode, this unmaps and re-initializes the segment.
// In test mode, clears the process-local map.
void ResetShmem();

// ShmemSize — total bytes currently held across all named regions.
std::size_t ShmemSize();

// NumShmemRegions — number of currently-registered regions.
int NumShmemRegions();

// CreateSharedMemoryAndSemaphores is defined in ipci.hpp (the dispatcher).
// shmem.cpp's role is to manage the named-region index (InitShmemIndex).

}  // namespace pgcpp::storage
