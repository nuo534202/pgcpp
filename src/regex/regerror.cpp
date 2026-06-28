// regerror.cpp — pg_regerror implementation.
//
// Maps a PG-compatible REG_* error code to a fixed message string and copies
// it into the caller's buffer (strerror_r semantics). The `re` parameter is
// ignored — messages depend only on `errcode`, matching PostgreSQL's behavior
// where pg_regerror produces a generic message per code.

#include "mytoydb/regex/regerror.hpp"

#include <cstring>
#include <string_view>

namespace mytoydb::regex {

namespace {

const char* ErrorMessage(int errcode) {
    switch (errcode) {
        case kRegOk:
            return "success";
        case kRegNomatch:
            return "regular expression did not match";
        case kRegBadpat:
            return "invalid regular expression";
        case kRegEcollate:
            return "invalid collating element";
        case kRegEctype:
            return "invalid character class name";
        case kRegEescape:
            return "trailing backslash";
        case kRegEsubreg:
            return "invalid back reference";
        case kRegEbrack:
            return "brackets [] not balanced";
        case kRegEparen:
            return "parentheses () not balanced";
        case kRegEbrace:
            return "braces {} not balanced";
        case kRegBadbr:
            return "invalid repetition count";
        case kRegErange:
            return "invalid character range";
        case kRegEspace:
            return "out of memory in regex compilation";
        case kRegBadrpt:
            return "quantifier operand invalid";
        default:
            return "unknown regex error";
    }
}

}  // namespace

std::size_t pg_regerror(int errcode, const regex_t* /*re*/, char* errbuf, std::size_t errbuf_size) {
    std::string_view msg(ErrorMessage(errcode));
    std::size_t len = msg.size();
    if (errbuf != nullptr && errbuf_size > 0) {
        std::size_t copy = (len < errbuf_size - 1) ? len : (errbuf_size - 1);
        std::memcpy(errbuf, msg.data(), copy);
        errbuf[copy] = '\0';
    }
    return len;
}

}  // namespace mytoydb::regex
