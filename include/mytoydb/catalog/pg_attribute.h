#pragma once

#include <cstdint>
#include <string>

#include "mytoydb/catalog/catalog.h"

namespace mytoydb::catalog {

// FormData_pg_attribute — C++ equivalent of PostgreSQL's catalog/pg_attribute.h.
//
// Each row describes one column of one relation. Field names and semantics are
// preserved from PostgreSQL; char* name fields use std::string.

// attstorage / attalign values (PostgreSQL 'char' codes).
enum class AttStorage : char {
    kPlain = 'p',     // stored uncompressed
    kExternal = 'e',  // stored externally (TOAST)
    kExtended = 'x',  // try compress, then external
    kMain = 'm',      // try compress, but keep inline
};

enum class AttAlign : char {
    kChar = 'c',    // 1-byte alignment
    kShort = 's',   // 2-byte alignment
    kInt = 'i',     // 4-byte alignment
    kDouble = 'd',  // 8-byte alignment
};

struct FormData_pg_attribute {
    Oid attrelid = kInvalidOid;  // relation this column belongs to
    std::string attname;         // column name
    Oid atttypid = kInvalidOid;  // type OID of this column
    int32_t attstattarget = -1;  // statistics target (-1 = default)
    int16_t attlen = 0;          // type's length (see pg_type.typlen)
    int16_t attnum = 0;          // column number (1-based for user cols)
    int16_t attndims = 0;        // number of dimensions if array
    int32_t attcacheoff = -1;    // cached offset (-1 = unknown)
    int32_t atttypmod = -1;      // type-specific modifier (-1 = none)
    bool attbyval = false;       // type is pass-by-value?
    AttStorage attstorage = AttStorage::kPlain;
    AttAlign attalign = AttAlign::kInt;
    bool attnotnull = false;  // has NOT NULL constraint
    bool atthasdef = false;   // has DEFAULT expression
    bool atthasmissing = false;
    char attidentity = '\0';         // generated identity ('' = none)
    char attgenerated = '\0';        // generated stored ('' = none)
    bool attisdropped = false;       // column has been dropped
    bool attislocal = true;          // column is local to relation
    int16_t attinhcount = 0;         // number of times inherited
    Oid attcollation = kInvalidOid;  // column's collation OID
};

using Form_pg_attribute = FormData_pg_attribute*;

}  // namespace mytoydb::catalog
