#pragma once

#include <cstdint>
#include <vector>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_inherits — C++ equivalent of PostgreSQL's catalog/pg_inherits.h.
//
// Each row records one parent-child partition/inheritance relationship.

struct FormData_pg_inherits {
    Oid inhrelid = kInvalidOid;    // OID of child relation
    Oid inhparent = kInvalidOid;   // OID of parent relation
    int16_t inhseqnum = 0;         // sequence number for ordering child tables
};

using Form_pg_inherits = FormData_pg_inherits*;

}  // namespace pgcpp::catalog
