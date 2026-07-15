#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_ts_cfg — C++ equivalent of PostgreSQL's catalog/pg_ts_cfg.h.
//
// Each row describes one text-search configuration. A configuration binds a
// parser to a mapping of token types -> dictionary chains (cfgmap). Field
// names and semantics are preserved from PostgreSQL.

struct FormData_pg_ts_cfg {
    Oid oid = kInvalidOid;           // configuration OID
    std::string cfgname;             // configuration name (e.g. "english")
    Oid cfgnamespace = kInvalidOid;  // namespace OID
    Oid cfgowner = kInvalidOid;      // owner OID
    Oid cfgparser = kInvalidOid;     // OID of the parser (pg_ts_parser)
    // cfgmap: maps token-type integer -> comma-separated dictionary OID list.
    // In PostgreSQL this is an int2vector; here we use a map for clarity.
    std::map<int32_t, std::string> cfgmap;
};

using Form_pg_ts_cfg = FormData_pg_ts_cfg*;

}  // namespace pgcpp::catalog
