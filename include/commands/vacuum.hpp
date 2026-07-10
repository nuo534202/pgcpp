// vacuum.h — VACUUM command (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/vacuum.c.
// pgcpp's MVCC implementation marks dead tuples in-place during DML
// (heap_delete sets t_xmax; heap_update sets t_xmax + inserts a new
// version). VACUUM reclaims physical space by compacting pages whose
// dead tuples are no longer visible to any running transaction.
//
// VACUUM FREEZE additionally replaces the inserting XID (t_xmin) of old
// committed tuples with FrozenTransactionId, so the commit log no longer
// needs to be consulted for those tuples. This prevents XID wraparound:
// once every tuple on a relation is frozen past a given XID, the
// relation's relfrozenxid is advanced in pg_class, and XIDs below that
// value can be reused safely (the 32-bit XID space wraps around).
#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"  // Oid

namespace pgcpp::parser {
class VacuumStmt;
}  // namespace pgcpp::parser

namespace pgcpp::transaction {
using TransactionId = uint32_t;
}  // namespace pgcpp::transaction

namespace pgcpp::commands {

// VacuumStats — statistics collected during a VACUUM run.
// Returned to the caller for inspection (autovacuum logging, tests).
struct VacuumStats {
    int pages_scanned = 0;               // number of heap pages scanned
    int tuples_frozen = 0;               // tuples whose t_xmin was frozen
    int tuples_dead_reclaimed = 0;       // dead tuples marked LP_DEAD + compacted
    int tuples_live = 0;                 // live tuples observed
    bool relfrozenxid_advanced = false;  // true if pg_class.relfrozenxid was bumped
};

// ExecVacuum — execute VACUUM (and ANALYZE when stmt->is_vacuumcmd is
// false). Returns the command tag ("VACUUM" or "ANALYZE").
// If `stats` is non-null, it is populated with run statistics.
std::string ExecVacuum(parser::VacuumStmt* stmt, VacuumStats* stats = nullptr);

// --- Freeze-limit computation (XID wraparound protection) ---
//
// VACUUM freezes tuples whose t_xmin is committed and older than the
// freeze limit. The limit is derived from OldestXmin (no snapshot can
// see tuples older than this) minus an age threshold:
//   - normal VACUUM: freeze_limit = OldestXmin - vacuum_freeze_min_age
//   - VACUUM FREEZE (aggressive): freeze_limit = OldestXmin
//
// vacuum_freeze_min_age / vacuum_freeze_max_age default to PostgreSQL's
// defaults (50M / 2B). pgcpp keeps them as constants for simplicity.

// Default age (in XIDs) before VACUUM starts freezing tuples.
constexpr int64_t kVacuumFreezeMinAge = 50000000;
// Age (in XIDs) at which a relation is in imminent wraparound danger
// and must be vacuumed urgently (autovacuum triggers).
constexpr int64_t kVacuumFreezeMaxAge = 2000000000;

// VacuumGetFreezeLimit — compute the XID below which tuples may be frozen.
// `oldest_xmin` is the OldestXmin from GetOldestXmin().
// `aggressive` is true for VACUUM FREEZE (freeze everything older than
// OldestXmin) and false for normal VACUUM (apply kVacuumFreezeMinAge).
pgcpp::transaction::TransactionId VacuumGetFreezeLimit(
    pgcpp::transaction::TransactionId oldest_xmin, bool aggressive);

// RelationNeedsVacuumForWraparound — return true if the relation's
// relfrozenxid is so old that (nextXid - relfrozenxid) exceeds
// kVacuumFreezeMaxAge, meaning an emergency VACUUM FREEZE is required
// to prevent XID wraparound data loss.
bool RelationNeedsVacuumForWraparound(pgcpp::catalog::Oid relid);

}  // namespace pgcpp::commands
