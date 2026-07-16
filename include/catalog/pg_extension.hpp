#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_extension — C++ equivalent of PostgreSQL's catalog/pg_extension.h.
//
// Each row describes one installed extension. Field names and semantics are
// preserved from PostgreSQL; NameData uses std::string instead of fixed-size
// char arrays.
//
// extconfig: OIDs of "configuration tables" whose data should be dumped with
//   the extension (simplified: we store the OIDs but don't implement
//   pg_dump integration).
// extcondition: WHERE clauses (one per extconfig entry) restricting which
//   rows of each configuration table are dumped.
struct FormData_pg_extension {
    Oid oid = kInvalidOid;                  // extension OID
    std::string extname;                    // extension name (e.g. "pgcrypto")
    Oid extowner = kInvalidOid;             // owner role OID (not enforced)
    Oid extnamespace = kInvalidOid;         // namespace the extension lives in
    bool extrelocatable = false;            // can the extension be relocated?
    std::string extversion;                 // installed version string
    std::vector<Oid> extconfig;             // configuration table OIDs
    std::vector<std::string> extcondition;  // per-config-table filters
};

using Form_pg_extension = FormData_pg_extension*;

}  // namespace pgcpp::catalog
