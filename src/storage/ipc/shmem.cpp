// shmem.cpp — Shared memory region management (in-process simulation).
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/shmem.c.
#include "mytoydb/storage/ipc/shmem.hpp"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace mytoydb::storage {

namespace {

// ShmemRegion — one named region in the simulated shared memory.
struct ShmemRegion {
    std::vector<uint8_t> data;
};

// ShmemIndex — the map of name → region. Implemented as a function-local
// static to avoid static-initialization-order issues (matches procarray.cpp).
std::map<std::string, ShmemRegion>& ShmemIndex() {
    static std::map<std::string, ShmemRegion> index;
    return index;
}

}  // namespace

bool ShmemAddrIsValid(const void* addr) {
    return addr != nullptr;
}

void* ShmemInitStruct(const char* name, std::size_t size, bool* found_in_ptr) {
    auto& index = ShmemIndex();
    auto it = index.find(name);
    if (it != index.end()) {
        if (found_in_ptr != nullptr) {
            *found_in_ptr = true;
        }
        // If the caller asks for a different size, resize (PG would ereport ERROR).
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

void* ShmemInitHash(const char* name, std::size_t size) {
    return ShmemInitStruct(name, size, nullptr);
}

void InitShmemIndex() {
    ShmemIndex();  // lazily constructed on first access; nothing else to do.
}

void ResetShmem() {
    ShmemIndex().clear();
}

std::size_t ShmemSize() {
    std::size_t total = 0;
    for (const auto& kv : ShmemIndex()) {
        total += kv.second.data.size();
    }
    return total;
}

int NumShmemRegions() {
    return static_cast<int>(ShmemIndex().size());
}

// CreateSharedMemoryAndSemaphores is implemented in ipci.cpp (the dispatcher
// iterates the registry of init functions, including shmem's InitShmemIndex).

}  // namespace mytoydb::storage
