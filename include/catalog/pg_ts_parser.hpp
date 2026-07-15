#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_ts_parser — C++ equivalent of PostgreSQL's
// catalog/pg_ts_parser.h.
//
// Each row describes a text-search parser: the C functions that tokenize
// input text into typed tokens. Field names and semantics are preserved
// from PostgreSQL.

struct FormData_pg_ts_parser {
    Oid oid = kInvalidOid;           // parser OID
    std::string prsname;             // parser name (e.g. "default")
    Oid prsnamespace = kInvalidOid;  // namespace OID
    Oid prsstart = kInvalidOid;      // OID of start function (pg_proc)
    Oid prstoken = kInvalidOid;      // OID of next-token function (pg_proc)
    Oid prsend = kInvalidOid;        // OID of end function (pg_proc)
    Oid prsheadline = kInvalidOid;   // OID of headline function (pg_proc)
    Oid prslextype = kInvalidOid;    // OID of lextype function (pg_proc)
};

using Form_pg_ts_parser = FormData_pg_ts_parser*;

}  // namespace pgcpp::catalog
