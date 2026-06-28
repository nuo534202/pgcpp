// regexport.hpp — pg_regexport: extract sub-pattern information.
//
//   int pg_regexport(const regex_t* re, int start, int stop, ...);
//
// In PostgreSQL, pg_regexport walks the compiled NFA to produce a textual
// representation of a sub-portion of the regex tree, used by debug/diagnostic
// tooling. pgcpp wraps std::regex which does not expose its internal tree,
// so this entry point is provided as a stub. It returns 0 and writes nothing,
// signalling "no exported data". Callers that genuinely need NFA inspection
// should be migrated to a different approach.
//
// The stub is intentionally minimal: it accepts a regex_t* and returns 0
// (no data exported). The variadic tail is omitted to keep the signature simple.

#pragma once

#include "pgcpp/regex/regex_internal.hpp"

namespace pgcpp::regex {

// Returns 0 always (no data exported). The regex_t is consulted only to verify
// validity; the stub performs no real export.
int pg_regexport(const regex_t* re);

}  // namespace pgcpp::regex
