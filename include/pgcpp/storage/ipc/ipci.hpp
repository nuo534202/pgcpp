// ipci.h — IPC initialization dispatcher.
//
// Converted from PostgreSQL 15's src/include/storage/ipc.h and
// src/backend/storage/ipc/ipci.c.
//
// PostgreSQL registers subsystem initialization functions (e.g., for
// BufferPool, ProcArray, LWLock array) at static init time via the
// shmem_startup_hook / pqsignoinit mechanism, then calls them in order
// during CreateSharedMemoryAndSemaphores().
//
// pgcpp keeps the same registry-and-dispatch design: subsystems call
// RegisterIPCInitFn() with a name and a function pointer, then
// CreateSharedMemoryAndSemaphores() iterates the registry.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace pgcpp::storage {

// IPCInitFn — signature for an initialization function. Returns true on
// success, false on failure (which aborts startup).
using IPCInitFn = std::function<bool()>;

// RegisterIPCInitFn — register a subsystem init function.
// Idempotent: registering the same name twice replaces the prior entry.
void RegisterIPCInitFn(const std::string& name, IPCInitFn fn);

// CreateSharedMemoryAndSemaphores — initialize shared memory and call all
// registered init functions in registration order. Returns the number of
// functions that succeeded.
int CreateSharedMemoryAndSemaphores();

// ResetIPCInitFns — clear the registry (used by tests).
void ResetIPCInitFns();

// RegisteredIPCInitFns — return the names of all currently-registered init
// functions in registration order.
std::vector<std::string> RegisteredIPCInitFns();

}  // namespace pgcpp::storage
