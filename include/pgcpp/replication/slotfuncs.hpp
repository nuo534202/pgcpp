// slotfuncs.h — SQL-facing wrappers around the replication-slot store.
//
// Converted from PostgreSQL 15's src/backend/replication/slotfuncs.c.
//
// These mirror the pg_create_replication_slot / pg_drop_replication_slot /
// pg_replication_slot_advance SQL functions. In PG they're SQL-callable
// (returning a tuple); here they are plain C++ functions that perform the
// same logic and return the same pieces of information.
//
// `ereport` is used for error cases (duplicate name, missing slot,
// active slot) so callers can use PG_TRY/PG_CATCH.
#pragma once

#include <cstdint>
#include <string>

#include "pgcpp/replication/slot.hpp"
#include "pgcpp/transaction/xlog.hpp"

namespace mytoydb::replication {

// PgCreateReplicationSlotResult — return value of pg_create_replication_slot.
struct PgCreateReplicationSlotResult {
    std::string slot_name;
    transaction::XLogRecPtr start_lsn = 0;  // LSN where the slot starts
    // Logical-only: snapshot exported so the slot's catalog state can be
    // imported on the subscriber side. Empty for physical slots.
    std::string snapshot_name;
};

// PgCreateReplicationSlot — SQL-facing wrapper around
// ReplicationSlotCreate. Returns the slot's start LSN and (for logical
// slots) a synthetic snapshot name. On error, calls ereport(ERROR).
PgCreateReplicationSlotResult PgCreateReplicationSlot(const std::string& name,
                                                      const std::string& plugin, bool is_logical,
                                                      const std::string& database);

// PgDropReplicationSlot — SQL-facing wrapper around ReplicationSlotDrop.
// On error (missing / active slot), calls ereport(ERROR). Returns true
// on success.
bool PgDropReplicationSlot(const std::string& name);

// PgReplicationSlotAdvance — SQL-facing wrapper around
// ReplicationSlotAdvance. Returns the new end LSN, or 0 on error.
// On missing slot, calls ereport(ERROR).
transaction::XLogRecPtr PgReplicationSlotAdvance(const std::string& name,
                                                 transaction::XLogRecPtr upto);

// PgReplicationSlotAdvanceToCurrent — convenience: advance a slot to the
// current WAL insert pointer. Returns the new LSN.
transaction::XLogRecPtr PgReplicationSlotAdvanceToCurrent(const std::string& name);

}  // namespace mytoydb::replication
