// syncrep.h — Synchronous replication.
//
// Converted from PostgreSQL 15's src/backend/replication/syncrep.c.
//
// Sync replication configures a list of standbys (named in
// `synchronous_standby_names`) that must acknowledge a transaction's WAL
// before the committing backend can proceed. PG parses
// `synchronous_standby_names` into a SyncRepConfigData tree and walks
// it in SyncRepWaitForLSN.
//
// pgcpp keeps a flat list of standby names + a count of how many must
// ack. The wait itself is a stub: SyncRepWaitForLSN returns immediately.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pgcpp/transaction/xlog.hpp"

namespace pgcpp::replication {

// SyncRepSyncMethod — how to count acks (PG: SYNC_REP_PRIORITY vs
// SYNC_REP_QUORUM).
enum class SyncRepSyncMethod : uint8_t {
    kPriority = 0,  // any one of the priority standbys
    kQuorum = 1,    // any N-of-M standbys
};

// SyncRepConfig — the parsed `synchronous_standby_names` value.
struct SyncRepConfig {
    std::vector<std::string> standby_names;
    int num_sync = 1;  // how many standbys must ack
    SyncRepSyncMethod method = SyncRepSyncMethod::kPriority;
    bool initialized = false;
};

// SyncRepConfigInit — initialize to "no sync standbys".
void SyncRepConfigInit();

// SyncRepConfigReset — alias for tests.
void SyncRepConfigReset();

// SyncRepConfigUpdate — replace the config with a new standby list.
// `num_sync` is the number that must acknowledge. Returns false if
// num_sync is out of range.
bool SyncRepConfigUpdate(std::vector<std::string> standby_names, int num_sync,
                         SyncRepSyncMethod method);

// SyncRepConfigGet — return a pointer to the current config.
const SyncRepConfig* SyncRepConfigGet();

// SyncRepConfigParse — parse the textual form of `synchronous_standby_names`
// into the active config. Supports:
//   "n (a, b, c)"  -> quorum, n-of-M
//   "ANY n (a, b)" -> quorum, explicit
//   "a, b"         -> first 1 (priority mode)
// Returns true on success. On parse failure, calls ereport(ERROR) and
// returns false.
bool SyncRepConfigParse(const std::string& text);

// SyncRepWaitForLSN — block until enough standbys have acknowledged `lsn`.
// In pgcpp this is a stub that just records the wait and returns.
// Returns the LSN we waited for.
transaction::XLogRecPtr SyncRepWaitForLSN(transaction::XLogRecPtr lsn);

// SyncRepGetWaiters — return the number of backends currently blocked
// in SyncRepWaitForLSN (always 0 in the stub).
int SyncRepGetWaiters();

// SyncRepIsSyncStandby — true if the given standby name is part of the
// current config.
bool SyncRepIsSyncStandby(const std::string& application_name);

}  // namespace pgcpp::replication
