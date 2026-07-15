#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_ts_dict — C++ equivalent of PostgreSQL's catalog/pg_ts_dict.h.
//
// Each row describes one text-search dictionary. A dictionary is an instance
// of a template (pg_ts_template) with optional init options. Field names and
// semantics are preserved from PostgreSQL.

struct FormData_pg_ts_dict {
    Oid oid = kInvalidOid;            // dictionary OID
    std::string dictname;             // dictionary name (e.g. "english_stem")
    Oid dictnamespace = kInvalidOid;  // namespace OID
    Oid dictowner = kInvalidOid;      // owner OID
    Oid dicttemplate = kInvalidOid;   // OID of the template (pg_ts_template)
    std::string dictinitoption;       // init options string (e.g. stop words)
};

using Form_pg_ts_dict = FormData_pg_ts_dict*;

}  // namespace pgcpp::catalog
