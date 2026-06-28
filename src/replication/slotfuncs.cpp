// slotfuncs.cpp — SQL-facing wrappers around the replication-slot store.
//
// Converted from PostgreSQL 15's src/backend/replication/slotfuncs.c.
// These mirror pg_create_replication_slot / pg_drop_replication_slot /
// pg_replication_slot_advance. Errors go through ereport(ERROR).
#include "pgcpp/replication/slotfuncs.hpp"

#include <string>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/replication/replutil.hpp"
#include "pgcpp/transaction/xlog.hpp"

namespace pgcpp::replication {

using pgcpp::error::LogLevel;

PgCreateReplicationSlotResult PgCreateReplicationSlot(const std::string& name,
                                                      const std::string& plugin, bool is_logical,
                                                      const std::string& database) {
    PgCreateReplicationSlotResult result;
    result.slot_name = name;

    SlotType type = is_logical ? SlotType::kLogical : SlotType::kPhysical;
    SlotPersistence persistence = SlotPersistence::kPersistent;
    std::string effective_plugin = is_logical ? plugin : "";
    if (is_logical && effective_plugin.empty()) {
        effective_plugin = DefaultOutputPluginName();
    }

    // PG_TRY to convert the ereport(ERROR) from ReplicationSlotCreate into a
    // false-but-still-returned result. The caller is responsible for checking
    // `result.start_lsn` (0 on failure).
    bool ok = false;
    PG_TRY() {
        ok = ReplicationSlotCreate(name, type, persistence, effective_plugin, database);
    }
    PG_CATCH() {
        ok = false;
    }
    PG_END_TRY();

    if (!ok) {
        return result;
    }

    const ReplicationSlot* s = ReplicationSlotLookup(name);
    if (s == nullptr) {
        // Should not happen: we just created it.
        ereport(LogLevel::kError, "pg_create_replication_slot: slot vanished");
        return result;
    }
    result.start_lsn = s->restart_lsn;
    if (is_logical) {
        // PG exports a snapshot for logical slots so the subscriber can clone
        // the catalog state. We synthesize a name; the actual snapshot is a
        // no-op in pgcpp.
        result.snapshot_name = "mytoydb_logical_snap_" + name;
    }
    return result;
}

bool PgDropReplicationSlot(const std::string& name) {
    bool ok = false;
    PG_TRY() {
        ok = ReplicationSlotDrop(name);
    }
    PG_CATCH() {
        ok = false;
    }
    PG_END_TRY();
    return ok;
}

transaction::XLogRecPtr PgReplicationSlotAdvance(const std::string& name,
                                                 transaction::XLogRecPtr upto) {
    transaction::XLogRecPtr r = 0;
    PG_TRY() {
        r = ReplicationSlotAdvance(name, upto);
    }
    PG_CATCH() {
        r = 0;
    }
    PG_END_TRY();
    return r;
}

transaction::XLogRecPtr PgReplicationSlotAdvanceToCurrent(const std::string& name) {
    return PgReplicationSlotAdvance(name, transaction::GetXLogInsertRecPtr());
}

}  // namespace pgcpp::replication
