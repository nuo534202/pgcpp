#pragma once

#include <cstdint>
#include <string>

#include "pgcpp/catalog/catalog.hpp"

namespace mytoydb::catalog {

// FormData_pg_operator — C++ equivalent of PostgreSQL's catalog/pg_operator.h.
//
// Each row describes one operator. Field names and semantics are preserved
// from PostgreSQL; NameData fields use std::string.

// oprkind values: 'b' for binary (infix) operator, 'l' for left (prefix).
enum class OperatorKind : char {
    kBinary = 'b',  // infix: left OP right
    kLeft = 'l',    // prefix: OP right
};

struct FormData_pg_operator {
    Oid oid = kInvalidOid;                         // operator OID
    std::string oprname;                           // operator name (e.g., "=", "+")
    Oid oprnamespace = kInvalidOid;                // namespace OID
    Oid oprowner = kInvalidOid;                    // owner OID
    OperatorKind oprkind = OperatorKind::kBinary;  // 'b' or 'l'
    bool oprcanmerge = false;                      // can be used in merge join?
    bool oprcanhash = false;                       // can be used in hash join?
    Oid oprleft = kInvalidOid;                     // left arg type OID (0 if prefix)
    Oid oprright = kInvalidOid;                    // right arg type OID
    Oid oprresult = kInvalidOid;                   // result type OID (0 = shell)
    Oid oprcom = kInvalidOid;                      // commutator operator OID
    Oid oprnegate = kInvalidOid;                   // negator operator OID
    Oid oprcode = kInvalidOid;                     // underlying function OID (regproc)
    Oid oprrest = kInvalidOid;                     // restriction estimator (regproc)
    Oid oprjoin = kInvalidOid;                     // join estimator (regproc)
};

using Form_pg_operator = FormData_pg_operator*;

}  // namespace mytoydb::catalog
