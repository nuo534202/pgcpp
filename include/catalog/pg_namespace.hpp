#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_namespace — C++ equivalent of PostgreSQL's catalog/pg_namespace.h.
//
// Each row describes one namespace (schema). Field names and semantics are
// preserved from PostgreSQL; char* name fields use std::string.

struct FormData_pg_namespace {
    Oid oid = kInvalidOid;        // namespace OID
    std::string nspname;          // name of the namespace
    Oid nspowner = kInvalidOid;   // owner user OID
    bool nspacl = false;          // has ACL (placeholder; real ACL is a list)
};

using Form_pg_namespace = FormData_pg_namespace*;

}  // namespace pgcpp::catalog
