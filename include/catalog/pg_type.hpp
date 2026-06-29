#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_type — C++ equivalent of PostgreSQL's catalog/pg_type.h.
//
// Each row describes one data type. Field names and semantics are preserved
// from PostgreSQL; char* name fields use std::string.

// typtype values (PostgreSQL 'char' codes).
enum class TypeType : char {
    kBase = 'b',        // base type
    kComposite = 'c',   // composite type (pg_class entry exists)
    kDomain = 'd',      // domain
    kEnum = 'e',        // enum type
    kPseudo = 'p',      // pseudo-type
    kRange = 'r',       // range type
    kMultirange = 'm',  // multirange type
};

// typcategory values (PostgreSQL 'char' codes, see catalog/pg_type_d.h).
enum class TypeCategory : char {
    kInvalid = '\0',
    kArray = 'A',
    kBitstring = 'B',
    kComposite = 'C',
    kDateTime = 'D',
    kEnum = 'E',
    kGeometric = 'G',
    kNetworkAddress = 'I',
    kNumeric = 'N',
    kPseudo = 'P',
    kString = 'S',
    kTimespan = 'T',
    kUserDefined = 'U',
    kBitstringVariant = 'V',
    kUnknown = 'X',
};

// typalign values (shared with AttAlign).
enum class TypeAlign : char {
    kChar = 'c',
    kShort = 's',
    kInt = 'i',
    kDouble = 'd',
};

// typstorage values (shared with AttStorage).
enum class TypeStorage : char {
    kPlain = 'p',
    kExternal = 'e',
    kExtended = 'x',
    kMain = 'm',
};

struct FormData_pg_type {
    Oid oid = kInvalidOid;           // type OID
    std::string typname;             // type name
    Oid typnamespace = kInvalidOid;  // namespace OID
    Oid typowner = kInvalidOid;      // owner OID
    int16_t typlen = 0;              // fixed length, or -1 for varlena
    bool typbyval = false;           // pass-by-value?
    TypeType typtype = TypeType::kBase;
    TypeCategory typcategory = TypeCategory::kInvalid;
    bool typispreferred = false;   // preferred type in its category?
    bool typisdefined = true;      // type is defined (not placeholder)?
    char typdelim = ',';           // array element delimiter
    Oid typrelid = kInvalidOid;    // composite type's pg_class OID
    Oid typelem = kInvalidOid;     // array element type OID
    Oid typarray = kInvalidOid;    // array type OID for this type
    Oid typinput = kInvalidOid;    // input function (regproc OID)
    Oid typoutput = kInvalidOid;   // output function
    Oid typreceive = kInvalidOid;  // binary input function
    Oid typsend = kInvalidOid;     // binary output function
    Oid typmodin = kInvalidOid;    // typmod input function
    Oid typmodout = kInvalidOid;   // typmod output function
    Oid typanalyze = kInvalidOid;  // analyze function
    TypeAlign typalign = TypeAlign::kInt;
    TypeStorage typstorage = TypeStorage::kPlain;
    bool typnotnull = false;         // not-null constraint (domain only)
    Oid typbasetype = kInvalidOid;   // base type OID (domain only)
    int32_t typtypmod = -1;          // typmod applied to base (domain only)
    int32_t typndims = 0;            // array dimensions (domain over array)
    Oid typcollation = kInvalidOid;  // type's collation
    std::string typdefault;          // default expression text
    std::string typdefaultbin;       // default expression internal form
};

using Form_pg_type = FormData_pg_type*;

}  // namespace pgcpp::catalog
