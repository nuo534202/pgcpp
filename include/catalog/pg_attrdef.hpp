#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_attrdef — C++ equivalent of PostgreSQL's catalog/pg_attrdef.h.
//
// Each row stores a default-value expression for one column of one relation.

struct FormData_pg_attrdef {
    Oid oid = kInvalidOid;            // attrdef OID
    Oid adrelid = kInvalidOid;        // OID of the relation containing the column
    int16_t adnum = 0;                // attnum of the column
    Oid adbin = kInvalidOid;          // OID of default expression tree (pg_node_tree)
    std::string adsrc;                // human-readable default expression (placeholder)
};

using Form_pg_attrdef = FormData_pg_attrdef*;

}  // namespace pgcpp::catalog
