// regexport.cpp — pg_regexport stub implementation.
//
// See regexport.hpp for rationale. The stub validates its argument and returns
// 0 to indicate "no data exported". A non-zero argument-status return is used
// only for obviously invalid input.

#include "pgcpp/regex/regexport.hpp"

namespace mytoydb::regex {

int pg_regexport(const regex_t* re) {
    if (re == nullptr || re->re_magic != kRegexMagic) {
        return -1;
    }
    return 0;
}

}  // namespace mytoydb::regex
