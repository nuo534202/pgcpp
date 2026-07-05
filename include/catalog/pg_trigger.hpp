#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_trigger — C++ equivalent of PostgreSQL's catalog/pg_trigger.h.
//
// Each row describes one trigger on a table.

// tgenabled values (TRIGGER_FIRES_*).
enum class TriggerEnabled : char {
    kOrigin = 'O',    // fires in original role
    kReplica = 'R',   // fires in replica role
    kAlways = 'A',    // always fires
    kDisabled = 'D',  // never fires
};

// tgtype bit flags (PostgreSQL TRIGGER_TYPE_*).
constexpr uint16_t kTriggerTypeBefore = 1U << 0;
constexpr uint16_t kTriggerTypeInsert = 1U << 1;
constexpr uint16_t kTriggerTypeDelete = 1U << 2;
constexpr uint16_t kTriggerTypeUpdate = 1U << 3;
constexpr uint16_t kTriggerTypeTruncate = 1U << 4;
constexpr uint16_t kTriggerTypeInstead = 1U << 5;
constexpr uint16_t kTriggerTypeRow = 1U << 6;

struct FormData_pg_trigger {
    Oid oid = kInvalidOid;                               // trigger OID
    Oid tgrelid = kInvalidOid;                           // relation this trigger is on
    Oid tgparentid = kInvalidOid;                        // parent trigger, if cloned from partition
    std::string tgname;                                  // trigger name
    Oid tgfoid = kInvalidOid;                            // OID of function to be called
    TriggerEnabled tgenabled = TriggerEnabled::kOrigin;  // when to fire
    bool tgisinternal = false;                           // trigger is system-generated
    int16_t tgnargs = 0;              // number of argument strings passed to trigger
    std::string tgargs;               // argument strings (placeholder: joined)
    std::vector<int16_t> tgattr;      // column numbers updated (UPDATE triggers)
    Oid tgconstrrelid = kInvalidOid;  // relation referenced by constraint trigger
    Oid tgconstrindid = kInvalidOid;  // index supporting the constraint
    Oid tgconstraint = kInvalidOid;   // pg_constraint entry, if constraint trigger
    bool tgdeferrable = false;        // deferrable?
    bool tginitdeferred = false;      // initially deferred?
    Oid tgqual = kInvalidOid;         // WHEN expression tree (pg_node_tree)
    std::string tgnewtable;           // transition table name for NEW rows, if any
    std::string tgoldtable;           // transition table name for OLD rows, if any
};

using Form_pg_trigger = FormData_pg_trigger*;

}  // namespace pgcpp::catalog
