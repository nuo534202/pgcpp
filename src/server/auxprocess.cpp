// auxprocess.cpp — Auxiliary process type enumeration and dispatch.
//
// Converted from PostgreSQL 15's src/backend/postmaster/auxprocess.c.
//
// In PostgreSQL, auxprocess.c provides the AuxiliaryProcessMain entry point
// that dispatches to the specific auxiliary process's main loop based on
// the AuxiliaryProcessType argument. pgcpp preserves this dispatch table.
#include "server/auxprocess.hpp"

#include <string>
#include <string_view>

#include "server/autovacuum.hpp"
#include "server/bgwriter.hpp"
#include "server/checkpointer.hpp"
#include "server/interrupt.hpp"
#include "server/pgarch.hpp"
#include "server/startup.hpp"
#include "server/syslogger.hpp"
#include "server/walwriter.hpp"

namespace pgcpp::server {

const char* AuxProcessTypeToString(AuxiliaryProcessType type) {
    switch (type) {
        case AuxiliaryProcessType::kNoProcess:
            return "none";
        case AuxiliaryProcessType::kBgWriter:
            return "bgwriter";
        case AuxiliaryProcessType::kCheckPointer:
            return "checkpointer";
        case AuxiliaryProcessType::kStartupProcess:
            return "startup";
        case AuxiliaryProcessType::kAutoVacuumLauncher:
            return "autovacuum launcher";
        case AuxiliaryProcessType::kAutoVacuumWorker:
            return "autovacuum worker";
        case AuxiliaryProcessType::kWalWriter:
            return "walwriter";
        case AuxiliaryProcessType::kPgArch:
            return "archiver";
        case AuxiliaryProcessType::kSysLogger:
            return "syslogger";
        case AuxiliaryProcessType::kBgWorker:
            return "bgworker";
    }
    return "unknown";
}

AuxiliaryProcessType LookupAuxProcessType(std::string_view name) {
    if (name == "bgwriter")
        return AuxiliaryProcessType::kBgWriter;
    if (name == "checkpointer")
        return AuxiliaryProcessType::kCheckPointer;
    if (name == "startup")
        return AuxiliaryProcessType::kStartupProcess;
    if (name == "autovacuum launcher" || name == "autovacuum_launcher")
        return AuxiliaryProcessType::kAutoVacuumLauncher;
    if (name == "autovacuum worker" || name == "autovacuum_worker")
        return AuxiliaryProcessType::kAutoVacuumWorker;
    if (name == "walwriter")
        return AuxiliaryProcessType::kWalWriter;
    if (name == "archiver" || name == "pgarch")
        return AuxiliaryProcessType::kPgArch;
    if (name == "syslogger")
        return AuxiliaryProcessType::kSysLogger;
    if (name == "bgworker")
        return AuxiliaryProcessType::kBgWorker;
    return AuxiliaryProcessType::kNoProcess;
}

const char* AuxProcessMainName(AuxiliaryProcessType type) {
    switch (type) {
        case AuxiliaryProcessType::kNoProcess:
            return "none";
        case AuxiliaryProcessType::kBgWriter:
            return "BgWriterMain";
        case AuxiliaryProcessType::kCheckPointer:
            return "CheckpointerMain";
        case AuxiliaryProcessType::kStartupProcess:
            return "StartupProcessMain";
        case AuxiliaryProcessType::kAutoVacuumLauncher:
            return "AutoVacuumLauncherMain";
        case AuxiliaryProcessType::kAutoVacuumWorker:
            return "AutoVacuumWorkerMain";
        case AuxiliaryProcessType::kWalWriter:
            return "WalWriterMain";
        case AuxiliaryProcessType::kPgArch:
            return "PgArchiverMain";
        case AuxiliaryProcessType::kSysLogger:
            return "SysLoggerMain";
        case AuxiliaryProcessType::kBgWorker:
            return "BgWorkerMain";
    }
    return "none";
}

int AuxProcessMain(AuxiliaryProcessType type) {
    // Install aux process signal handlers (SIGHUP, SIGTERM only).
    InstallAuxProcessSignalHandlers();

    switch (type) {
        case AuxiliaryProcessType::kNoProcess:
            return -1;
        case AuxiliaryProcessType::kBgWriter:
            return BgWriterMain(/*max_iterations=*/100);
        case AuxiliaryProcessType::kCheckPointer:
            return CheckpointerMain(/*max_iterations=*/100);
        case AuxiliaryProcessType::kStartupProcess:
            return StartupProcessMain();
        case AuxiliaryProcessType::kAutoVacuumLauncher:
            return AutoVacuumLauncherMain(/*max_workers=*/10);
        case AuxiliaryProcessType::kAutoVacuumWorker:
            // A worker needs a work item; in AuxProcessMain we have none to give.
            return -1;
        case AuxiliaryProcessType::kWalWriter:
            return WalWriterMain(/*max_iterations=*/100);
        case AuxiliaryProcessType::kPgArch:
            return PgArchiverMain(/*max_iterations=*/100);
        case AuxiliaryProcessType::kSysLogger:
            return SysLoggerMain(/*max_iterations=*/100);
        case AuxiliaryProcessType::kBgWorker:
            // Generic bgworker dispatch requires a worker ID; from AuxProcessMain
            // we have no way to know which one. Return an error.
            return -1;
    }
    return -1;
}

}  // namespace pgcpp::server
