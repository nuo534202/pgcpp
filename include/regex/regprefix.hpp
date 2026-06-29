// regprefix.hpp — pg_regprefix: extract the constant prefix of a pattern.
//
//   int pg_regprefix(const regex_t* re, char** prefix, size_t* prefix_size);
//
// Used for index optimization (range scans): returns the longest literal prefix
// that every match of `re` must begin with, *when the regex is anchored at the
// start of the searched string*. Returns -1 if no such fixed prefix exists
// (e.g. the pattern is not anchored with '^', or begins with a metacharacter).
//
// On success, returns the prefix length (>= 0), sets `*prefix` to a palloc'd
// NUL-terminated buffer in CurrentMemoryContext, and `*prefix_size` to the
// prefix length (excluding the NUL). The caller is not responsible for freeing
// the buffer (it is owned by CurrentMemoryContext).

#pragma once

#include <cstddef>

#include "regex/regex_internal.hpp"

namespace pgcpp::regex {

int pg_regprefix(const regex_t* re, char** prefix, std::size_t* prefix_size);

}  // namespace pgcpp::regex
