// auxprocess.h — Auxiliary process type enumeration and dispatch.
//
// Converted from PostgreSQL 15's src/backend/postmaster/auxprocess.c.
//
// PostgreSQL runs several "auxiliary" background processes alongside the
// postmaster: bgwriter, checkpointer, startup, autovacuum launcher/worker,
// walwriter, pgarch, syslogger. Each is forked by the postmaster and runs
// a fixed main loop. In pgcpp (single-process), auxiliary processes are
// represented by an enum and dispatched via function calls rather than
// actual forked processes.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pgcpp::server {

// AuxiliaryProcessType — identifies which auxiliary process is running.
// Matches PostgreSQL's AuxiliaryProcessType enum order.
enum class AuxiliaryProcessType : uint8_t {
    kNoProcess = 0,
    kBgWriter,            // Background writer
    kCheckPointer,        // Checkpointer
    kStartupProcess,      // Startup / crash recovery
    kAutoVacuumLauncher,  // Autovacuum launcher
    kAutoVacuumWorker,    // Autovacuum worker
    kWalWriter,           // WAL writer
    kPgArch,              // WAL archiver
    kSysLogger,           // System logger
    kBgWorker,            // Generic background worker
};

// AuxProcessTypeToString — return a string name for the given type.
const char* AuxProcessTypeToString(AuxiliaryProcessType type);

// LookupAuxProcessType — parse a name into an AuxiliaryProcessType.
// Returns AuxiliaryProcessType::kNoProcess if the name is unknown.
AuxiliaryProcessType LookupAuxProcessType(std::string_view name);

// AuxProcessMainName — the human-readable name shown in process title.
const char* AuxProcessMainName(AuxiliaryProcessType type);

// AuxProcessMain — dispatch to the appropriate auxiliary process main loop.
// Returns 0 on clean exit, non-zero on error.
int AuxProcessMain(AuxiliaryProcessType type);

}  // namespace pgcpp::server
