#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_class — C++ equivalent of PostgreSQL's catalog/pg_class.h.
//
// Each row of the pg_class catalog describes one table-like object (table,
// index, view, sequence, etc.). Field names and semantics are preserved from
// PostgreSQL; only the C-style typedef and char* name fields are converted to
// C++ struct + std::string (see rules/01-conversion-strategy.md).
//
// Note: TransactionId / MultiXactId are not yet defined (Phase 7). We use
// uint32_t placeholders so the struct layout is stable for later phases.

// relkind values (PostgreSQL RELKIND_* constants, kept as enum class).
enum class RelKind : char {
    kRelation = 'r',          // ordinary table
    kIndex = 'i',             // index
    kSequence = 'S',          // sequence
    kToastValue = 't',        // toast table
    kView = 'v',              // view
    kMaterializedView = 'm',  // materialized view
    kCompositeType = 'c',     // composite type
    kForeignTable = 'f',      // foreign table
    kPartitionedTable = 'p',  // partitioned table
};

// relpersistence values (PostgreSQL RELPERSISTENCE_* constants).
enum class RelPersistence : char {
    kPermanent = 'p',  // permanent relation
    kTemporary = 't',  // temporary relation
    kUnlogged = 'u',   // unlogged relation
};

struct FormData_pg_class {
    Oid oid = kInvalidOid;            // relation OID
    std::string relname;              // relation name
    Oid relnamespace = kInvalidOid;   // OID of namespace containing this rel
    Oid reltype = kInvalidOid;        // OID of composite type, if any
    Oid reloftype = kInvalidOid;      // OID of typed table's type, if any
    Oid relowner = kInvalidOid;       // owner user OID
    Oid relam = kInvalidOid;          // access method OID
    Oid relfilenode = kInvalidOid;    // physical file node OID
    Oid reltablespace = kInvalidOid;  // tablespace OID
    int32_t relpages = 0;             // estimated size in pages
    float reltuples = 0.0F;           // estimated number of rows
    Oid reltoastrelid = kInvalidOid;  // toast table OID, if any
    bool relhasindex = false;         // has (or had) any indexes
    bool relisshared = false;         // visible across databases
    RelPersistence relpersistence = RelPersistence::kPermanent;
    RelKind relkind = RelKind::kRelation;
    int16_t relnatts = 0;  // number of user attributes
    int16_t relchecks = 0;
    bool relhasrules = false;
    bool relhastriggers = false;
    bool relrowsecurity = false;
    bool relforcerowsecurity = false;
    bool relispopulated = true;
    char relreplident = 'd';  // replica identity
    bool relispartition = false;
    uint32_t relfrozenxid = 0;  // TransactionId placeholder
    uint32_t relminmxid = 0;    // MultiXactId placeholder
};

// PostgreSQL uses FormData_pg_class as the struct type and Form_pg_class as a
// pointer-to-struct typedef. In C++ we keep FormData_pg_class as the value
// type and use raw pointers / references where PostgreSQL used Form_pg_class.
using Form_pg_class = FormData_pg_class*;

}  // namespace pgcpp::catalog
