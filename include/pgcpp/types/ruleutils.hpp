#pragma once

#include <cstdint>
#include <string>

#include "mytoydb/types/datum.hpp"

namespace mytoydb::types {

// ---------------------------------------------------------------------------
// ruleutils.c — render PostgreSQL parse trees back as SQL text.
//
// We expose a tiny helper API focused on operator/operator-class name
// resolution and the deparse of a few simple node types. This is the surface
// needed by tests and EXPLAIN output.
// ---------------------------------------------------------------------------

// Quote an identifier if necessary (matching PostgreSQL rules).
std::string QuoteIdentifier(const std::string& ident);

// Render a literal datum of the given type as a SQL literal (e.g. '42', 'NULL',
// "'abc'", "E'\\x00\\x01'", "1.5").
std::string DeparseLiteral(uint32_t type_oid, Datum value, bool is_null);

// Convenience: format an operator name for display. Returns "schema.op" if the
// schema is non-empty, otherwise just "op".
std::string FormatOperatorName(const std::string& schema, const std::string& opname);

}  // namespace mytoydb::types
