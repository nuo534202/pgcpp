// regcomp.cpp — pg_regcomp implementation.
//
// Compiles a POSIX-style regular expression by wrapping std::regex.
// PG-compatible flags are mapped onto std::regex_constants where possible:
//   REG_ICASE   -> std::regex_constants::icase
//   REG_NOSUB   -> recorded (pg_regexec will skip filling pmatch)
//   REG_NEWLINE -> recorded (best-effort: PG newline semantics are approximated
//                  by using ECMAScript syntax; std::regex has no exact multiline
//                  mode, so the flag is stored for pg_regexec to apply as match
//                  flags)
//   REG_EXTENDED-> treated as the default (ECMAScript syntax is used, which is a
//                  superset of ERE for the constructs pgcpp exercises)

#include "pgcpp/regex/regcomp.hpp"

#include <regex>
#include <string>
#include <string_view>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

namespace pgcpp::regex {

using pgcpp::error::LogLevel;
using pgcpp::memory::MemoryContext;
using pgcpp::memory::palloc;

namespace {

// Translate a std::regex_error code into a PG-compatible REG_* error code.
int TranslateRegexError(const std::regex_error& e) {
    using E = std::regex_constants::error_type;
    switch (e.code()) {
        case E::_S_error_badbrace:
        case E::_S_error_badrepeat:
            return kRegBadrpt;
        case E::_S_error_brace:
            return kRegEbrace;
        case E::_S_error_brack:
            return kRegEbrack;
        case E::_S_error_collate:
            return kRegEcollate;
        case E::_S_error_ctype:
            return kRegEctype;
        case E::_S_error_escape:
            return kRegEescape;
        case E::_S_error_paren:
            return kRegEparen;
        case E::_S_error_range:
            return kRegErange;
        case E::_S_error_space:
            return kRegEspace;
        case E::_S_error_backref:
            return kRegEsubreg;
        default:
            return kRegBadpat;
    }
}

}  // namespace

regex_t* pg_regcomp(const char* pattern, int flags) {
    if (pattern == nullptr) {
        ereport(LogLevel::kError, "pg_regcomp: pattern is NULL");
    }

    // Allocate the regex_t shell via palloc + placement new, then register its
    // destructor so std::regex/std::string internals are released when the
    // MemoryContext is reset/deleted.
    auto* re = static_cast<regex_t*>(palloc(sizeof(regex_t)));
    new (re) regex_t();
    MemoryContext* ctx = pgcpp::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        ctx->RegisterDestructor(re, [](void* o) { static_cast<regex_t*>(o)->~regex_t(); });
    }

    re->re_magic = kRegexMagic;
    re->re_flags = flags;
    re->pattern = pattern;

    // Build std::regex flags. ECMAScript is the project-wide default (matches
    // string_funcs.cpp). REG_EXTENDED is treated as ECMAScript since it covers
    // the ERE constructs pgcpp relies on.
    std::regex_constants::syntax_option_type syn = std::regex_constants::ECMAScript;
    if ((flags & kRegIcase) != 0) {
        syn |= std::regex_constants::icase;
    }
    // std::regex has no flag to disable submatch capture; REG_NOSUB is simply
    // recorded so pg_regexec can skip filling pmatch.

    try {
        re->cpp_re.assign(pattern, syn);
        re->re_errno = kRegOk;
        re->re_nsub = static_cast<int>(re->cpp_re.mark_count());
    } catch (const std::regex_error& e) {
        re->re_errno = TranslateRegexError(e);
        re->re_nsub = 0;
    }
    return re;
}

}  // namespace pgcpp::regex
