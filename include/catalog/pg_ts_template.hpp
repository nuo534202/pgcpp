#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_ts_template — C++ equivalent of PostgreSQL's
// catalog/pg_ts_template.h.
//
// Each row describes a text-search template: the C functions that initialize
// a dictionary and perform lexicalization. Field names and semantics are
// preserved from PostgreSQL.

struct FormData_pg_ts_template {
    Oid oid = kInvalidOid;            // template OID
    std::string tmplname;             // template name (e.g. "snowball")
    Oid tmplnamespace = kInvalidOid;  // namespace OID
    Oid tmplinit = kInvalidOid;       // OID of init function (pg_proc)
    Oid tmpllexize = kInvalidOid;     // OID of lexize function (pg_proc)
};

using Form_pg_ts_template = FormData_pg_ts_template*;

}  // namespace pgcpp::catalog
