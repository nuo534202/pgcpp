#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_database — C++ equivalent of PostgreSQL's catalog/pg_database.h.
//
// Each row describes one database. Field names and semantics are preserved
// from PostgreSQL; char* name fields use std::string.

// datconnlimit / datfrozenxid placeholders use uint32_t until transaction
// types are introduced (Phase 7).

struct FormData_pg_database {
    Oid oid = kInvalidOid;            // database OID
    std::string datname;              // database name
    Oid datdba = kInvalidOid;         // owner user OID
    int32_t encoding = 0;             // character encoding (pg_enc)
    std::string datcollate;           // LC_COLLATE
    std::string datctype;             // LC_CTYPE
    bool datistemplate = false;       // allowed to be used as CREATE DATABASE template
    bool datallowconn = true;         // allowed to connect?
    int32_t datconnlimit = -1;        // max connections allowed (-1 = no limit)
    Oid datlastsysoid = kInvalidOid;  // highest OID of a system object
    int32_t datsize = 0;              // database size in pages (placeholder)
    bool datacl = false;              // has ACL (placeholder)
    uint32_t datfrozenxid = 0;        // TransactionId placeholder
    uint32_t datminmxid = 0;          // MultiXactId placeholder
};

using Form_pg_database = FormData_pg_database*;

}  // namespace pgcpp::catalog
