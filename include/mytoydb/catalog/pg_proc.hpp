#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mytoydb/catalog/catalog.hpp"

namespace mytoydb::catalog {

// FormData_pg_proc — C++ equivalent of PostgreSQL's catalog/pg_proc.h.
//
// Each row describes one function/procedure/aggregate. Field names and
// semantics are preserved from PostgreSQL; NameData uses std::string,
// oidvector uses std::vector<Oid>.

// prokind values (PostgreSQL 'char' codes).
enum class ProKind : char {
    kFunction = 'f',
    kAggregate = 'a',
    kWindow = 'w',
    kProcedure = 'p',
};

// provolatile values (PostgreSQL 'char' codes).
enum class ProVolatile : char {
    kImmutable = 'i',  // never changes for given input
    kStable = 's',     // does not change within a scan
    kVolatile = 'v',   // can change even within a scan
};

// proparallel values (PostgreSQL 'char' codes).
enum class ProParallel : char {
    kSafe = 's',        // can run in worker or leader
    kRestricted = 'r',  // can run in parallel leader only
    kUnsafe = 'u',      // banned while in parallel mode
};

// proargmodes values (must agree with FunctionParameterMode in parsenodes.h).
enum class ProArgMode : char {
    kIn = 'i',
    kOut = 'o',
    kInOut = 'b',
    kVariadic = 'v',
    kTable = 't',
};

struct FormData_pg_proc {
    Oid oid = kInvalidOid;           // procedure OID
    std::string proname;             // procedure name
    Oid pronamespace = kInvalidOid;  // namespace OID
    Oid proowner = kInvalidOid;      // owner OID
    Oid prolang = kInvalidOid;       // language OID
    float procost = 1.0f;            // estimated execution cost
    float prorows = 0.0f;            // estimated # of rows out (if proretset)
    Oid provariadic = kInvalidOid;   // element type of variadic array
    Oid prosupport = kInvalidOid;    // planner support function (regproc)
    ProKind prokind = ProKind::kFunction;
    bool prosecdef = false;     // security definer?
    bool proleakproof = false;  // leak-proof function?
    bool proisstrict = true;    // strict w.r.t. NULLs?
    bool proretset = false;     // returns a set?
    ProVolatile provolatile = ProVolatile::kImmutable;
    ProParallel proparallel = ProParallel::kSafe;
    int16_t pronargs = 0;          // number of arguments
    int16_t pronargdefaults = 0;   // number of arguments with defaults
    Oid prorettype = kInvalidOid;  // OID of result type
    std::vector<Oid> proargtypes;  // parameter types (IN only)
    // Variable-length fields (NULL = empty in MyToyDB):
    std::vector<Oid> proallargtypes;     // all param types (empty if IN only)
    std::string proargmodes;             // parameter modes (empty if IN only)
    std::string proargnames;             // parameter names (empty if no names)
    std::string proargdefaults;          // expression trees for arg defaults
    std::vector<Oid> protrftypes;        // types for transforms
    std::string prosrc;                  // procedure source text
    std::string probin;                  // secondary procedure info
    std::string prosqlbody;              // pre-parsed SQL function body
    std::vector<std::string> proconfig;  // procedure-local GUC settings
    std::vector<std::string> proacl;     // access permissions
};

using Form_pg_proc = FormData_pg_proc*;

}  // namespace mytoydb::catalog
