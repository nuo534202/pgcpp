#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_language — C++ equivalent of PostgreSQL's catalog/pg_language.h.
//
// Each row describes one procedural language available for CREATE FUNCTION.
// Field names and semantics are preserved from PostgreSQL; NameData uses
// std::string instead of fixed-size char arrays.

// lanpltrusted: true if the language is considered "trusted" (safe for
// unprivileged users). Internal, C, and SQL are untrusted; plpgsql is
// trusted.
struct FormData_pg_language {
    Oid oid = kInvalidOid;            // language OID
    std::string lanname;              // language name (e.g. "plpgsql")
    bool lanpltrusted = false;        // trusted (unprivileged-safe)?
    Oid lanplcallfoid = kInvalidOid;  // OID of call handler function
    Oid laninlinefoid = kInvalidOid;  // OID of anonymous-code (DO) handler
    Oid lanvalidator = kInvalidOid;   // OID of validator function
    bool lanispl = false;             // true if this is a PL (not internal/c/sql)
    std::vector<std::string> lanacl;  // access permissions
};

using Form_pg_language = FormData_pg_language*;

}  // namespace pgcpp::catalog
