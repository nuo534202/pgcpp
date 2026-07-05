#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_tablespace — C++ equivalent of PostgreSQL's catalog/pg_tablespace.h.
//
// Each row describes one tablespace (a named storage location).

struct FormData_pg_tablespace {
    Oid oid = kInvalidOid;            // tablespace OID
    std::string spcname;              // tablespace name
    Oid spcowner = kInvalidOid;       // owner user OID
    bool spcacl = false;              // has ACL (placeholder)
    int32_t spcmaxsize = -1;          // maximum size in blocks (-1 = unlimited)
    std::string spclocation;          // on-disk location (placeholder)
    std::string spcoptions;           // options (placeholder; comma-separated)
};

using Form_pg_tablespace = FormData_pg_tablespace*;

}  // namespace pgcpp::catalog
