#pragma once

#include <cstdint>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_cast — C++ equivalent of PostgreSQL's catalog/pg_cast.h.
//
// Each row describes a cast path between two data types.

// castcontext values: contexts in which the cast can be used.
enum class CastContext : char {
    kImplicit = 'i',    // coercion in context of expression
    kAssignment = 'a',  // coercion in context of assignment
    kExplicit = 'e',    // explicit cast operation
};

// castmethod values: how the cast is performed.
enum class CastMethod : char {
    kFunction = 'f',  // use a cast function
    kBinary = 'b',    // types are binary-compatible
    kInOut = 'i',     // use type input/output functions
};

struct FormData_pg_cast {
    Oid oid = kInvalidOid;         // cast OID
    Oid castsource = kInvalidOid;  // source datatype OID
    Oid casttarget = kInvalidOid;  // destination datatype OID
    Oid castfunc = kInvalidOid;    // cast function OID (0 = binary coercible)
    CastContext castcontext = CastContext::kExplicit;
    CastMethod castmethod = CastMethod::kFunction;
};

using Form_pg_cast = FormData_pg_cast*;

}  // namespace pgcpp::catalog
