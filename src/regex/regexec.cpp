// regexec.cpp — pg_regexec implementation.
//
// Runs a compiled regex_t against a string using std::regex_search. PG
// eflags are mapped onto std::regex match flags where possible:
//   REG_NOTBOL -> std::regex_constants::match_not_bol
//   REG_NOTEOL -> std::regex_constants::match_not_eol
//
// REG_NOSUB (set at compile time) suppresses filling of pmatch.
// REG_NEWLINE (set at compile time) is best-effort: std::regex has no exact
// POSIX-newline mode, so the flag is recorded but not re-applied here.

#include "pgcpp/regex/regexec.hpp"

#include <regex>
#include <string>

namespace pgcpp::regex {

int pg_regexec(const regex_t* re, const char* string, std::size_t nmatch, regmatch_t pmatch[],
               int eflags) {
    if (re == nullptr || string == nullptr) {
        return kRegNomatch;
    }
    if (re->re_magic != kRegexMagic || re->re_errno != kRegOk) {
        return kRegBadpat;
    }

    std::smatch m;
    std::string subject(string);
    std::regex_constants::match_flag_type mflags = std::regex_constants::match_default;
    if ((eflags & kRegNotbol) != 0) {
        mflags |= std::regex_constants::match_not_bol;
    }
    if ((eflags & kRegNoteol) != 0) {
        mflags |= std::regex_constants::match_not_eol;
    }

    bool matched = std::regex_search(subject, m, re->cpp_re, mflags);
    if (!matched) {
        return kRegNomatch;
    }

    // Fill pmatch unless REG_NOSUB was requested at compile time.
    if ((re->re_flags & kRegNosub) == 0 && nmatch > 0 && pmatch != nullptr) {
        // Group 0 = whole match. m.size() == mark_count()+1 when matched.
        for (std::size_t i = 0; i < nmatch; ++i) {
            if (i < m.size() && m[i].matched) {
                pmatch[i].rm_so = static_cast<int>(m[i].first - subject.begin());
                pmatch[i].rm_eo = static_cast<int>(m[i].second - subject.begin());
            } else {
                pmatch[i].rm_so = -1;
                pmatch[i].rm_eo = -1;
            }
        }
    }
    return kRegOk;
}

}  // namespace pgcpp::regex
