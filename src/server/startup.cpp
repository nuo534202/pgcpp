// startup.cpp — Startup process: performs crash recovery on server startup.
//
// Converted from PostgreSQL 15's src/backend/postmaster/startup.c.
//
// The startup process is the first auxiliary process forked by the
// postmaster after a crash. It reads the WAL from the start (or from the
// last checkpoint) and replays all records to restore the database to
// a consistent state. MyToyDB wraps PerformCrashRecovery (xlogrecovery.h).
#include "pgcpp/server/startup.hpp"

#include <chrono>
#include <cstdint>

#include "pgcpp/server/interrupt.hpp"
#include "pgcpp/transaction/xlog.hpp"
#include "pgcpp/transaction/xlogrecovery.hpp"

namespace mytoydb::server {

namespace {

StartupState& State() {
    static StartupState s = StartupState::kNotStarted;
    return s;
}

StartupStats& Stats() {
    static StartupStats s;
    return s;
}

transaction::XLogRecPtr& StartLsn() {
    static transaction::XLogRecPtr lsn = transaction::kSizeofXlogRecord;
    return lsn;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void InitializeStartupProcess() {
    State() = StartupState::kNotStarted;
    Stats() = StartupStats{};
    StartLsn() = transaction::kSizeofXlogRecord;
}

void ResetStartupProcess() {
    InitializeStartupProcess();
}

int StartupProcessMain() {
    auto& s = Stats();
    int64_t start_ms = NowMs();

    State() = StartupState::kInitializing;
    State() = StartupState::kRecovering;

    // Perform crash recovery: read WAL from start, replay each record via
    // the registered redo functions.
    transaction::RecoveryStats recovery = transaction::PerformCrashRecoveryFrom(StartLsn());

    s.records_replayed = recovery.records_replayed;
    s.records_skipped = recovery.records_skipped;
    s.last_rmid = recovery.last_rmid;
    s.last_lsn = static_cast<uint64_t>(recovery.last_lsn);
    s.recovery_duration_ms = NowMs() - start_ms;
    s.recovery_end_lsn = static_cast<uint64_t>(transaction::GetXLogInsertRecPtr());

    State() = StartupState::kConsistent;
    State() = StartupState::kDone;
    return 0;
}

StartupState GetStartupState() {
    return State();
}

bool IsRecoveryInProgress() {
    return State() == StartupState::kRecovering;
}

StartupStats GetStartupStats() {
    return Stats();
}

void SetRecoveryStartLsn(transaction::XLogRecPtr lsn) {
    StartLsn() = lsn;
}

}  // namespace mytoydb::server
