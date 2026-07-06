#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_constraint — C++ equivalent of PostgreSQL's catalog/pg_constraint.h.
//
// Each row describes one table constraint (CHECK, FOREIGN KEY, UNIQUE,
// PRIMARY KEY, EXCLUDE). Field names and semantics are preserved from PG.

// contype values (PostgreSQL CONSTRAINT_* constants).
enum class ConstraintType : char {
    kCheck = 'c',       // CHECK constraint
    kForeignKey = 'f',  // FOREIGN KEY constraint
    kPrimaryKey = 'p',  // PRIMARY KEY constraint
    kUnique = 'u',      // UNIQUE constraint
    kTrigger = 't',     // constraint represented by a trigger (deprecated)
    kExclusion = 'x',   // EXCLUDE constraint
};

// confupdtype / confdeltype / confmatchtype values.
enum class ConstraintAction : char {
    kNoAction = 'a',
    kRestrict = 'r',
    kCascade = 'c',
    kNull = 'n',
    kSetNull = 'n',
    kSetDefault = 'd',
};

enum class ConstraintMatch : char {
    kFull = 'f',
    kPartial = 'p',
    kSimple = 's',
};

struct FormData_pg_constraint {
    Oid oid = kInvalidOid;           // constraint OID
    std::string conname;             // constraint name (may be empty)
    Oid connamespace = kInvalidOid;  // OID of namespace containing constraint
    ConstraintType contype = ConstraintType::kCheck;
    bool condeferrable = false;       // deferrable?
    bool condeferred = false;         // initially deferred?
    bool convalidated = false;        // constraint has been validated?
    Oid conrelid = kInvalidOid;       // relation this constraint is on
    Oid contypid = kInvalidOid;       // type this constraint is on (domain)
    Oid conindid = kInvalidOid;       // supporting index, if any
    Oid conparentid = kInvalidOid;    // parent constraint, if partitioned
    std::vector<int16_t> conkey;      // column numbers of the constrained columns
    std::vector<int16_t> confkey;     // referenced column numbers (foreign key)
    std::vector<Oid> conpfeqop;       // PK = FK equality operators (foreign key)
    std::vector<Oid> conppeqop;       // PK = PK equality operators (foreign key)
    std::vector<Oid> conffeqop;       // FK = FK equality operators (foreign key)
    std::vector<Oid> confdelsetcols;  // FK columns set to NULL on reference row delete
    ConstraintAction confupdtype = ConstraintAction::kNoAction;  // ON UPDATE action
    ConstraintAction confdeltype = ConstraintAction::kNoAction;  // ON DELETE action
    ConstraintMatch confmatchtype = ConstraintMatch::kSimple;    // FULL / PARTIAL / SIMPLE
    bool conislocal = true;     // constraint is local to this relation
    int16_t coninhcount = 0;    // number of times inherited
    bool connoinherit = false;  // cannot be inherited?
    std::string conbin;         // serialized check expression tree (pg_node_tree)
    std::string consrc;         // human-readable check expression (placeholder)
};

using Form_pg_constraint = FormData_pg_constraint*;

}  // namespace pgcpp::catalog
