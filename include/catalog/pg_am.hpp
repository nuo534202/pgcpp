#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_am — C++ equivalent of PostgreSQL's catalog/pg_am.h.
//
// Each row describes one access method (index AM or table AM). Field names
// and semantics are preserved from PostgreSQL.

enum class AmType : char {
    kIndex = 'i',  // index access method
    kTable = 't',  // table access method
};

struct FormData_pg_am {
    Oid oid = kInvalidOid;            // access method OID
    std::string amname;               // access method name
    AmType amtype = AmType::kIndex;   // 'i' = index, 't' = table
    Oid amhandler = kInvalidOid;      // OID of handler function
    bool amcanorder = false;          // can produce sorted output?
    bool amcanorderbyop = false;      // ORDER BY op(...)?
    bool amcanbackward = false;       // backward scan?
    bool amcanunique = false;         // unique index support?
    bool amcanmulticol = false;       // multi-column index?
    bool amoptionalkey = false;       // is first key optional?
    bool amsearcharray = false;       // ScalarArrayOpExpr search?
    bool amsearchnulls = false;       // IS NULL / IS NOT NULL search?
    bool amstorage = false;           // can store data?
    bool amclusterable = false;       // CLUSTER support?
    bool ampredlocks = false;         // predicate locks?
    bool amkeytype = false;           // has a key type?
    bool amsummarizing = false;       // summarizing AM?
    bool amcaninclude = false;        // INCLUDE non-key columns?
    bool amusemaintenanceworkmem = false;  // uses maintenance_work_mem?
};

using Form_pg_am = FormData_pg_am*;

}  // namespace pgcpp::catalog
