// namespace.h — schema/namespace resolution.
//
// Converted from PostgreSQL 15's src/include/catalog/namespace.h and
// src/backend/catalog/namespace.c.
//
// MyToyDB simplification: there is no schema concept yet. All relations live
// in a single implicit "public" namespace. schemaname on RangeVar is ignored,
// and get_namespace_name always returns "public".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pgcpp/catalog/catalog.hpp"

// Forward declaration to avoid a hard parser dependency in the header.
namespace mytoydb::parser {
class RangeVar;
}  // namespace mytoydb::parser

namespace mytoydb::catalog {

// RangeVarGetRelid — resolve a relation name (RangeVar) to an OID.
//
// PG semantics: when failOK is false, ereport(ERROR) if the relation does
// not exist. When failOK is true, return InvalidOid on miss.
// MyToyDB simplification: schemaname is ignored (single-namespace model).
Oid RangeVarGetRelid(mytoydb::parser::RangeVar* rangevar, bool failOK);

// RelnameGetRelid — convenience wrapper: resolve a bare relation name to OID.
// Returns InvalidOid on miss when failOK is true; otherwise ereport(ERROR).
Oid RelnameGetRelid(const std::string& relname, bool failOK = true);

// makeRangeVarFromNameList — construct a RangeVar from a qualified-name list.
// PG semantics: the last element is the relname; if there are two, the first
// is the schemaname; if three, the first is the catalog (database) name.
// MyToyDB simplification: only relname is preserved.
mytoydb::parser::RangeVar* makeRangeVarFromNameList(const std::vector<std::string>& names);

// get_namespace_name — return the namespace name for an OID.
// MyToyDB has a single namespace; always returns "public".
const char* get_namespace_name(Oid nspoid);

}  // namespace mytoydb::catalog
