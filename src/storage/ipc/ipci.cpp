// ipci.cpp — IPC initialization dispatcher.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/ipci.c.
#include "mytoydb/storage/ipc/ipci.hpp"

#include "mytoydb/storage/ipc/shmem.hpp"

namespace mytoydb::storage {

namespace {

struct NamedInitFn {
    std::string name;
    IPCInitFn fn;
};

std::vector<NamedInitFn>& InitFns() {
    static std::vector<NamedInitFn> fns;
    return fns;
}

}  // namespace

void RegisterIPCInitFn(const std::string& name, IPCInitFn fn) {
    auto& fns = InitFns();
    for (auto& entry : fns) {
        if (entry.name == name) {
            entry.fn = std::move(fn);
            return;
        }
    }
    fns.push_back({name, std::move(fn)});
}

int CreateSharedMemoryAndSemaphores() {
    InitShmemIndex();
    int succeeded = 0;
    for (const auto& entry : InitFns()) {
        if (entry.fn && entry.fn()) {
            ++succeeded;
        }
    }
    return succeeded;
}

void ResetIPCInitFns() {
    InitFns().clear();
}

std::vector<std::string> RegisteredIPCInitFns() {
    std::vector<std::string> names;
    for (const auto& entry : InitFns()) {
        names.push_back(entry.name);
    }
    return names;
}

}  // namespace mytoydb::storage
