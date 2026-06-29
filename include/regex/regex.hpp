// regex.hpp — umbrella header for the POSIX regex engine wrapper.
//
// Includes the full PG-compatible regex API surface for pgcpp:
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
//   #include "regex/regex.hpp"
//   using namespace pgcpp::regex;  // or explicit pgcpp::regex::pg_regcomp
//   regex_t* re = pg_regcomp("^abc", REG_EXTENDED);
//   regmatch_t m[2];
//   int rc = pg_regexec(re, "abcdef", 2, m, 0);
//   ...
//   pg_regfree(re);

#pragma once

#include "regex/regcomp.hpp"
#include "regex/regerror.hpp"
#include "regex/regex_internal.hpp"
#include "regex/regexec.hpp"
#include "regex/regexport.hpp"
#include "regex/regfree.hpp"
#include "regex/regprefix.hpp"
