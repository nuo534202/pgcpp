// regerror.hpp — pg_regerror: return a human-readable message for an error
// code.
//
//   size_t pg_regerror(int errcode, const regex_t* re, char* errbuf,
//                      size_t errbuf_size);
//
// Returns the full length of the error message (in bytes, excluding the NUL
// terminator) like strerror_r. If `errbuf` is non-null and `errbuf_size` > 0,
// copies up to errbuf_size-1 bytes into `errbuf` and NUL-terminates it.
// `re` is accepted for PG API compatibility but currently unused (the messages
// depend only on `errcode`).

#pragma once

#include <cstddef>

#include "pgcpp/regex/regex_internal.hpp"

namespace pgcpp::regex {

std::size_t pg_regerror(int errcode, const regex_t* re, char* errbuf, std::size_t errbuf_size);

}  // namespace pgcpp::regex
