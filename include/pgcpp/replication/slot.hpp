// slot.h — Replication slots.
//
// Converted from PostgreSQL 15's src/backend/replication/slot.c and
// src/backend/replication/slotfuncs.c (the latter's SQL-facing wrappers
// live in slotfuncs.h to keep this file focused on the slot store).
//
// A replication slot preserves resources (WAL, catalog tuples) needed by
// a downstream consumer. PG stores slots in shared memory; pgcpp keeps
// an in-process std::map<std::string, ReplicationSlot> keyed by slot name.
//
// Two flavors of slot exist:
//   - physical: preserves WAL past the standby's confirmed_flush_lsn so
//     the standby can resume streaming after a disconnect.
//   - logical: builds on a physical slot but additionally tracks the
//     catalog_xmin required to keep old row versions alive while the
//     output plugin decodes them.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "pgcpp/transaction/transam.hpp"
#include "pgcpp/transaction/xlog.hpp"

namespace pgcpp::replication {

// SlotType — physical vs. logical (PG: RS_LOGICAL vs. RS_PHYSICAL).
enum class SlotType : uint8_t {
    kPhysical = 0,
    kLogical = 1,
};

// SlotPersistence — whether the slot survives a server restart.
enum class SlotPersistence : uint8_t {
    kTemporary = 0,   // RS_TEMPORARY: dropped on crash
    kPersistent = 1,  // RS_PERSISTENT: preserved across restarts
};

// ReplicationSlot — one slot. Field names match PG's ReplicationSlotPersistentData
// (subset relevant to pgcpp).
struct ReplicationSlot {
    std::string name;  // unique slot name
    SlotType type = SlotType::kPhysical;
    SlotPersistence persistence = SlotPersistence::kPersistent;
    std::string plugin;    // logical output plugin name (e.g. "pgoutput")
    std::string database;  // logical slots are per-database
    // Local short-form names retained for compatibility with PG's code:
    transaction::TransactionId xmin = 0;              // oldest running xid held
    transaction::TransactionId catalog_xmin = 0;      // oldest xid for catalog access
    transaction::XLogRecPtr restart_lsn = 0;          // oldest WAL we still need
    transaction::XLogRecPtr confirmed_flush_lsn = 0;  // consumer's confirmed LSN
    int32_t active_pid = 0;                           // pid of the walsender/apply worker using it
    bool active = false;                              // true if a pid currently holds the slot
    bool just_started = false;                        // PG RS_INUSE transition marker
    bool dirty = false;                               // state changed and not yet persisted
};

// ReplicationSlotCtlData — the global slot store.
struct ReplicationSlotCtlData {
    std::map<std::string, ReplicationSlot> slots;
};

// ReplicationSlotInit — initialize the slot subsystem (clear the store).
void ReplicationSlotInit();

// ReplicationSlotReset — alias used in tests.
void ReplicationSlotReset();

// ReplicationSlotCreate — create and register a slot. Returns false if a
// slot with the same name already exists.
bool ReplicationSlotCreate(std::string name, SlotType type, SlotPersistence persistence,
                           std::string plugin, std::string database);

// ReplicationSlotAcquire — look up a slot by name and mark it active for
// the given pid. Returns false if the slot is already active or missing.
bool ReplicationSlotAcquire(const std::string& name, int32_t pid);

// ReplicationSlotRelease — release the slot held by `pid`. Returns the
// name of the released slot, or empty string if `pid` holds none.
std::string ReplicationSlotRelease(int32_t pid);

// ReplicationSlotDrop — remove a slot by name. Returns false if the slot
// does not exist or is currently active.
bool ReplicationSlotDrop(const std::string& name);

// ReplicationSlotPersist — mark a temporary slot as persistent. Returns
// false if the slot is missing.
bool ReplicationSlotPersist(const std::string& name);

// ReplicationSlotAdvance — advance confirmed_flush_lsn / restart_lsn.
// For physical slots, advances restart_lsn; for logical, advances
// confirmed_flush_lsn. Returns the new LSN, or 0 on error.
transaction::XLogRecPtr ReplicationSlotAdvance(const std::string& name,
                                               transaction::XLogRecPtr upto);

// ReplicationSlotLookup — return a pointer to a slot (or nullptr).
const ReplicationSlot* ReplicationSlotLookup(const std::string& name);

// ReplicationSlotCount — number of registered slots.
int ReplicationSlotCount();

// GetReplicationSlotCtl — pointer to the global slot store.
ReplicationSlotCtlData* GetReplicationSlotCtl();

// SlotTypeName / SlotPersistenceName — diagnostics.
const char* SlotTypeName(SlotType t);
const char* SlotPersistenceName(SlotPersistence p);

}  // namespace pgcpp::replication
