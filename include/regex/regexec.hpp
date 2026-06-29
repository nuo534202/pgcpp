// regexec.hpp — pg_regexec: execute a compiled regex against a string.
//
// Converts PostgreSQL's regexec.c API to a std::regex-backed implementation.
//
//   int pg_regexec(const regex_t* re, const char* string, size_t nmatch,
//                  regmatch_t pmatch[], int eflags);
//
// Returns 0 if a match is found, REG_NOMATCH otherwise. On a successful
// match and when nmatch > 0 and pmatch != nullptr, pmatch[0] receives the
// whole-match span and pmatch[1..nmatch-1] receive the capture groups
// (rm_so/rm_eo == -1 for groups that did not participate).

#pragma once

#include <cstddef>

#include "regex/regex_internal.hpp"

namespace pgcpp::regex {

int pg_regexec(const regex_t* re, const char* string, std::size_t nmatch, regmatch_t pmatch[],
               int eflags);

}  // namespace pgcpp::regex
