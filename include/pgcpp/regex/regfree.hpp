// regfree.hpp — pg_regfree: release a regex_t compiled by pg_regcomp.
//
//   void pg_regfree(regex_t* re);
//
// Calls the regex_t destructor (releasing the internal std::regex/std::string
// state), unregisters it from its MemoryContext, and pfrees the block. Safe to
// call with nullptr (no-op). Also safe to call on a failed regex_t (one whose
// re_errno != 0).

#pragma once

#include "mytoydb/regex/regex_internal.hpp"

namespace mytoydb::regex {

void pg_regfree(regex_t* re);

}  // namespace mytoydb::regex
