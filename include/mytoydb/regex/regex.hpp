// regex.hpp — umbrella header for the POSIX regex engine wrapper.
//
// Includes the full PG-compatible regex API surface for MyToyDB:
//   - pg_regcomp   : compile a pattern into a regex_t
//   - pg_regexec   : execute a compiled regex against a string
//   - pg_regfree   : release a regex_t
//   - pg_regerror  : convert an error code to a message
//   - pg_regprefix : extract the constant prefix of an anchored pattern
//   - pg_regexport : (stub) NFA export entry point
//
// Also re-exports the PG-compatible constants and types from regex_internal.hpp.
//
// Usage:
//   #include "mytoydb/regex/regex.hpp"
//   using namespace mytoydb::regex;  // or explicit mytoydb::regex::pg_regcomp
//   regex_t* re = pg_regcomp("^abc", REG_EXTENDED);
//   regmatch_t m[2];
//   int rc = pg_regexec(re, "abcdef", 2, m, 0);
//   ...
//   pg_regfree(re);

#pragma once

#include "mytoydb/regex/regcomp.hpp"
#include "mytoydb/regex/regerror.hpp"
#include "mytoydb/regex/regex_internal.hpp"
#include "mytoydb/regex/regexec.hpp"
#include "mytoydb/regex/regexport.hpp"
#include "mytoydb/regex/regfree.hpp"
#include "mytoydb/regex/regprefix.hpp"
