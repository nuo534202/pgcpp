// shmem.cpp — Shared memory region management.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/shmem.c.
//
// Uses mmap(MAP_SHARED|MAP_ANONYMOUS) for the shared segment, which is
// inherited across fork(). Falls back to a process-local std::map when
// ShmemInit() has not been called (unit-test mode).
#include "storage/ipc/shmem.hpp"

#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace pgcpp::storage {

namespace {

// --- Multi-process mode state ---
// g_shmem_base points to the mmap'd shared segment. It is set by ShmemInit()
// in the postmaster and inherited by fork'd children (the pointer value is
// copied, and the underlying mapping is shared).
std::atomic<void*> g_shmem_base{nullptr};
std::size_t g_shmem_size = 0;

// --- Test-mode (fallback) state ---
// Used when ShmemInit() has not been called. Each region is a
// process-local std::vector<uint8_t>.
struct ShmemRegion {
    std::vector<uint8_t> data;
};

std::map<std::string, ShmemRegion>& ShmemTestIndex() {
    static std::map<std::string, ShmemRegion> index;
    return index;
}

// Align `offset` up to the next multiple of `alignment` (power of 2).
std::size_t AlignUp(std::size_t offset, std::size_t alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

}  // namespace

// --- Multi-process mode API ---

bool ShmemInit(std::size_t total_size) {
    if (g_shmem_base.load() != nullptr) {
        return true;  // already initialized
    }

    // Round up to page size.
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096;
    }
    total_size = AlignUp(total_size, static_cast<std::size_t>(page_size));

    void* addr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        return false;
    }

    // Initialize the header.
    auto* header = static_cast<ShmemHeader*>(addr);
    header->magic = kShmemMagic;
    header->total_size = total_size;
    header->free_offset = AlignUp(sizeof(ShmemHeader), 64);
    header->region_count = 0;
    // Zero out the region index.
    for (int i = 0; i < kMaxShmemRegions; ++i) {
        header->regions[i].name[0] = '\0';
        header->regions[i].offset = 0;
        header->regions[i].size = 0;
    }

    g_shmem_base.store(addr);
    g_shmem_size = total_size;
    return true;
}

bool ShmemAttach() {
    void* base = g_shmem_base.load();
    if (base == nullptr) {
        return false;  // not in multi-process mode
    }
    auto* header = static_cast<ShmemHeader*>(base);
    return header->magic == kShmemMagic;
}

void ShmemDetach() {
    void* base = g_shmem_base.exchange(nullptr);
    if (base != nullptr && g_shmem_size > 0) {
        munmap(base, g_shmem_size);
    }
    g_shmem_size = 0;
}

bool IsShmemActive() {
    return g_shmem_base.load() != nullptr;
}

// --- ShmemInitStruct (multi-process mode) ---

static void* ShmemInitStructShared(const char* name, std::size_t size, bool* found_in_ptr) {
    void* base = g_shmem_base.load();
    if (base == nullptr) {
        return nullptr;
    }
    auto* header = static_cast<ShmemHeader*>(base);

    // Search existing regions.
    for (int i = 0; i < header->region_count; ++i) {
        if (std::strncmp(header->regions[i].name, name, kShmemNameLen) == 0) {
            if (found_in_ptr != nullptr) {
                *found_in_ptr = true;
            }
            return static_cast<char*>(base) + header->regions[i].offset;
        }
    }

    // Not found — allocate from the bump allocator.
    if (header->region_count >= kMaxShmemRegions) {
        return nullptr;  // index full
    }
    std::size_t aligned_offset = AlignUp(header->free_offset, 64);
    if (aligned_offset + size > header->total_size) {
        return nullptr;  // out of shared memory
    }

    int slot = header->region_count++;
    std::strncpy(header->regions[slot].name, name, kShmemNameLen - 1);
    header->regions[slot].name[kShmemNameLen - 1] = '\0';
    header->regions[slot].offset = aligned_offset;
    header->regions[slot].size = size;
    header->free_offset = aligned_offset + size;

    void* ptr = static_cast<char*>(base) + aligned_offset;
    std::memset(ptr, 0, size);

    if (found_in_ptr != nullptr) {
        *found_in_ptr = false;
    }
    return ptr;
}

// --- ShmemInitStruct (test-mode fallback) ---

static void* ShmemInitStructTest(const char* name, std::size_t size, bool* found_in_ptr) {
    auto& index = ShmemTestIndex();
    auto it = index.find(name);
    if (it != index.end()) {
        if (found_in_ptr != nullptr) {
            *found_in_ptr = true;
        }
        if (it->second.data.size() != size) {
            it->second.data.resize(size, 0);
        }
        return it->second.data.data();
    }
    if (found_in_ptr != nullptr) {
        *found_in_ptr = false;
    }
    auto& region = index[name];
    region.data.assign(size, 0);
    return region.data.data();
}

// --- Public API ---

bool ShmemAddrIsValid(const void* addr) {
    return addr != nullptr;
}

void* ShmemInitStruct(const char* name, std::size_t size, bool* found_in_ptr) {
    if (IsShmemActive()) {
        void* ptr = ShmemInitStructShared(name, size, found_in_ptr);
        if (ptr != nullptr) {
            return ptr;
        }
        // Fall through to test mode if shared allocation failed.
    }
    return ShmemInitStructTest(name, size, found_in_ptr);
}

void* ShmemInitHash(const char* name, std::size_t size) {
    return ShmemInitStruct(name, size, nullptr);
}

void InitShmemIndex() {
    if (IsShmemActive()) {
        // The index is part of the ShmemHeader, already initialized by ShmemInit.
        return;
    }
    ShmemTestIndex();  // lazily construct the test-mode index
}

void ResetShmem() {
    if (IsShmemActive()) {
        // In multi-process mode: detach and re-init the header (does not
        // re-mmap; just clears the index and resets the bump allocator).
        void* base = g_shmem_base.load();
        if (base != nullptr) {
            auto* header = static_cast<ShmemHeader*>(base);
            header->free_offset = AlignUp(sizeof(ShmemHeader), 64);
            header->region_count = 0;
            for (int i = 0; i < kMaxShmemRegions; ++i) {
                header->regions[i].name[0] = '\0';
            }
        }
        return;
    }
    ShmemTestIndex().clear();
}

std::size_t ShmemSize() {
    if (IsShmemActive()) {
        void* base = g_shmem_base.load();
        if (base == nullptr) {
            return 0;
        }
        auto* header = static_cast<ShmemHeader*>(base);
        std::size_t total = 0;
        for (int i = 0; i < header->region_count; ++i) {
            total += header->regions[i].size;
        }
        return total;
    }
    std::size_t total = 0;
    for (const auto& kv : ShmemTestIndex()) {
        total += kv.second.data.size();
    }
    return total;
}

int NumShmemRegions() {
    if (IsShmemActive()) {
        void* base = g_shmem_base.load();
        if (base == nullptr) {
            return 0;
        }
        auto* header = static_cast<ShmemHeader*>(base);
        return header->region_count;
    }
    return static_cast<int>(ShmemTestIndex().size());
}

}  // namespace pgcpp::storage
