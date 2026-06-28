// xlogrecovery.cpp — Crash recovery: replay WAL records from the start.
//
// Converted from PostgreSQL 15's src/backend/access/transam/xlogrecovery.c
// (simplified: no redo point tracking, no backup block filtering).
//
// PerformCrashRecovery reads every WAL record from the beginning of the log
// (LSN kSizeofXlogRecord) and dispatches each to the matching resource
// manager's redo function. Records whose RMGR has no registered redo
// function are counted as skipped (not an error). Recovery stops at
// end-of-WAL.
#include "pgcpp/transaction/xlogrecovery.hpp"

#include <array>
#include <functional>

#include "pgcpp/transaction/xlog.hpp"
#include "pgcpp/transaction/xlogreader.hpp"

namespace mytoydb::transaction {

namespace {

// Redo functions indexed by RmgrId (0..255). A default-constructed (empty)
// std::function means "not registered".
std::array<RedoFn, 256>& RedoTable() {
    static std::array<RedoFn, 256> table;
    return table;
}

}  // namespace

void RegisterRmgrRedo(RmgrId rmid, RedoFn fn) {
    RedoTable()[rmid] = std::move(fn);
}

void ClearRmgrRedo() {
    for (auto& fn : RedoTable()) {
        fn = nullptr;
    }
}

RedoFn GetRmgrRedo(RmgrId rmid) {
    return RedoTable()[rmid];
}

RecoveryStats PerformCrashRecovery() {
    return PerformCrashRecoveryFrom(kSizeofXlogRecord);
}

RecoveryStats PerformCrashRecoveryFrom(XLogRecPtr start_lsn) {
    RecoveryStats stats;

    XLogReaderState* reader = XLogReaderAlloc();
    XLogRecPtr lsn = start_lsn;

    while (XLogReadRecord(reader, &lsn)) {
        const XLogRecord& rec = reader->record;
        RedoFn fn = GetRmgrRedo(rec.xl_rmid);
        if (fn) {
            fn(rec, reader->main_data.data(), reader->main_data.size(), reader->current_lsn);
            ++stats.records_replayed;
            stats.last_rmid = rec.xl_rmid;
            stats.last_lsn = reader->current_lsn;
        } else {
            ++stats.records_skipped;
        }
    }

    XLogReaderFree(reader);
    return stats;
}

}  // namespace mytoydb::transaction
