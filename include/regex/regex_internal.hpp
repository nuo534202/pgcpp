// regex_internal.hpp — internal types and constants for the POSIX regex
// engine wrapper.
//
// Converts PostgreSQL's src/backend/regex/ internal type definitions to C++20.
// The full PostgreSQL regex engine (~17k lines) is replaced by a std::regex
// wrapper exposing a PG-compatible API surface.
//
// This header defines:
//   - regex_t     : compiled regex handle (wraps std::regex)
//   - regmatch_t  : match offset pair (rm_so, rm_eo)
//   - compile flags: REG_EXTENDED, REG_ICASE, REG_NOSUB, REG_NEWLINE
//   - exec flags  : REG_NOTBOL, REG_NOTEOL
//   - error codes : REG_NOMATCH, REG_BADPAT, ... (PG-compatible)
//
// All public API functions live in namespace pgcpp::regex.

#pragma once

#include <cstddef>
#include <regex>
#include <string>

namespace pgcpp::regex {

// --- Compile-time flags (passed to pg_regcomp) ---
// Values match PostgreSQL's regex.h flag values for API compatibility.

constexpr int kRegExtended = 0x0001;  // REG_EXTENDED — use POSIX ERE syntax
constexpr int kRegIcase = 0x0002;     // REG_ICASE — case-insensitive match
constexpr int kRegNosub = 0x0004;     // REG_NOSUB — suppress submatch reporting
constexpr int kRegNewline = 0x0008;   // REG_NEWLINE — see notes below

// --- Execution-time flags (passed to pg_regexec as eflags) ---

constexpr int kRegNotbol = 0x0001;  // REG_NOTBOL — ^ does not match start
constexpr int kRegNoteol = 0x0002;  // REG_NOTEOL — $ does not match end

// --- Result / error codes ---
// REG_NOMATCH (1) signals "no match" from pg_regexec. All other error codes
// are non-zero and indicate compilation/execution errors. 0 means success.

constexpr int kRegOk = 0;        // success
constexpr int kRegNomatch = 1;   // regular expression did not match
constexpr int kRegBadpat = 2;    // invalid regular expression
constexpr int kRegEcollate = 3;  // invalid collating element
constexpr int kRegEctype = 4;    // invalid character class name
constexpr int kRegEescape = 5;   // trailing backslash
constexpr int kRegEsubreg = 6;   // invalid back reference
constexpr int kRegEbrack = 7;    // brackets [] not balanced
constexpr int kRegEparen = 8;    // parentheses () not balanced
constexpr int kRegEbrace = 9;    // braces {} not balanced
constexpr int kRegBadbr = 10;    // invalid repetition count
constexpr int kRegErange = 11;   // invalid character range
constexpr int kRegEspace = 12;   // out of memory / internal error
constexpr int kRegBadrpt = 13;   // quantifier operand invalid

// Magic number stored in regex_t.re_magic to distinguish a valid (compiled)
// regex_t from uninitialized memory. Matches PostgreSQL's REGEX_MAGIC.
constexpr int kRegexMagic = 0x52454758;  // "REGX"

// regmatch_t — offset pair describing one submatch.
//   rm_so : start offset of the match in the searched string (inclusive),
//           or -1 if this group did not participate.
//   rm_eo : end offset of the match (exclusive), or -1.
//
// Offsets are byte offsets into the searched C string, matching PostgreSQL's
// regoff_t semantics (regoff_t is int in PostgreSQL).
struct regmatch_t {
    int rm_so = -1;
    int rm_eo = -1;
};

// regex_t — the compiled-regex handle returned by pg_regcomp.
//
// This struct holds a std::regex internally (cpp_re) plus metadata retained
// from compilation: the original pattern text (for prefix extraction), the
// capture-group count, and the compile flags. Because std::regex and std::string
// are non-trivial C++ objects, regex_t instances MUST be allocated via palloc +
// placement new and registered for destructor invocation on their owning
// MemoryContext (see pg_regcomp). pg_regfree invokes the destructor explicitly
// and pfrees the block.
//
// Field naming mirrors PostgreSQL's regex_t for familiarity:
//   re_magic : validity marker (kRegexMagic after successful init)
//   re_nsub  : number of capture groups (excluding the whole-match group 0)
//   re_flags  : compile flags used at pg_regcomp time
//   re_errno  : 0 if compilation succeeded, else an error code (REG_BADPAT...)
struct regex_t {
    int re_magic = 0;
    int re_nsub = 0;
    int re_flags = 0;
    int re_errno = 0;
    std::string pattern;  // original pattern text (NUL-free)
    std::regex cpp_re;    // compiled std::regex (empty if re_errno != 0)
};

}  // namespace pgcpp::regex
