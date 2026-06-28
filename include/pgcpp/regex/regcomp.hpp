// regcomp.hpp — pg_regcomp: compile a POSIX-style regular expression.
//
// Converts PostgreSQL's regcomp.c API to a std::regex-backed implementation.
//
//   regex_t* pg_regcomp(const char* pattern, int flags);
//
// Returns a regex_t* allocated in CurrentMemoryContext (via palloc + placement
// new + RegisterDestructor). The caller is responsible for releasing it via
// pg_regfree. The returned regex_t is always non-null; on compilation failure
// its `re_errno` field holds an error code (and `cpp_re` is left empty), so the
// caller can pass the code to pg_regerror. A failed regex_t still must be
// pg_regfree'd.

#pragma once

#include "pgcpp/regex/regex_internal.hpp"

namespace mytoydb::regex {

// Compile `pattern` (a null-terminated C string) with the given `flags`
// (REG_EXTENDED | REG_ICASE | REG_NOSUB | REG_NEWLINE). Returns a regex_t*
// allocated in CurrentMemoryContext. On failure the returned handle has
// re_errno != 0.
regex_t* pg_regcomp(const char* pattern, int flags);

}  // namespace mytoydb::regex
