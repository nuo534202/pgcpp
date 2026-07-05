#pragma once

#include <cstdint>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_depend — C++ equivalent of PostgreSQL's catalog/pg_depend.h.
//
// Each row records a dependency between two database objects, enabling
// DROP CASCADE / RESTRICT semantics. Field names and semantics are
// preserved from PostgreSQL.

// deptype values (PostgreSQL DEPENDENCY_* constants).
enum class DependencyType : char {
    kNormal = 'n',         // normal dependency — drop dependent object with referenced
    kAuto = 'a',           // auto-dependency — silently drop dependent with referenced
    kInternal = 'i',       // internal dependency — dependent cannot be dropped separately
    kExtension = 'e',      // extension dependency — auto-drop with extension
    kAutoExtension = 'x',  // auto extension dependency
    kPin = 'p',            // pinned dependency — cannot drop referenced object
};

// classid constants for common pinned objects (PostgreSQL uses the OID of
// the catalog itself; we keep a small enum for readability in tests).
struct DependReference {
    Oid classid = kInvalidOid;  // OID of catalog containing the dependent object
    Oid objid = kInvalidOid;    // OID of the dependent object itself
    int32_t objsubid = 0;       // column number, or 0 for whole object
};

struct FormData_pg_depend {
    Oid classid = kInvalidOid;     // OID of catalog containing the dependent object
    Oid objid = kInvalidOid;       // OID of the dependent object itself
    int32_t objsubid = 0;          // column number, or 0 for whole object
    Oid refclassid = kInvalidOid;  // OID of catalog containing the referenced object
    Oid refobjid = kInvalidOid;    // OID of the referenced object
    int32_t refobjsubid = 0;       // column number, or 0 for whole object
    DependencyType deptype = DependencyType::kNormal;
};

using Form_pg_depend = FormData_pg_depend*;

}  // namespace pgcpp::catalog
