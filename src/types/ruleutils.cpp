// ruleutils.cpp — rule utility display helpers.

#include "pgcpp/types/ruleutils.hpp"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "pgcpp/types/builtins.hpp"
#include "pgcpp/types/datum.hpp"

namespace mytoydb::types {

namespace {

bool IsIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsIdentCont(char c) {
    return IsIdentStart(c) || std::isdigit(static_cast<unsigned char>(c));
}

bool NeedsQuoting(const std::string& ident) {
    if (ident.empty()) {
        return true;
    }
    if (!IsIdentStart(ident[0])) {
        return true;
    }
    for (char c : ident) {
        if (!IsIdentCont(c)) {
            return true;
        }
    }
    // Reserved keyword check — minimal subset.
    static const char* kReserved[] = {
        "select", "from",  "where", "insert", "update", "delete", "create", "drop",   "table",
        "index",  "view",  "and",   "or",     "not",    "null",   "as",     "join",   "on",
        "group",  "order", "by",    "having", "limit",  "offset", "into",   "values", nullptr};
    std::string lower;
    lower.reserve(ident.size());
    for (char c : ident) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (int i = 0; kReserved[i] != nullptr; ++i) {
        if (lower == kReserved[i]) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string QuoteIdentifier(const std::string& ident) {
    if (!NeedsQuoting(ident)) {
        return ident;
    }
    std::string out = "\"";
    for (char c : ident) {
        if (c == '"') {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string DeparseLiteral(uint32_t type_oid, Datum value, bool is_null) {
    if (is_null) {
        return "NULL";
    }
    switch (type_oid) {
        case kBoolOid: {
            char* s = bool_out(value);
            return std::string(s);
        }
        case kInt2Oid:
        case kInt4Oid:
        case kInt8Oid:
        case kFloat8Oid: {
            char* s = (type_oid == kInt2Oid)   ? int2_out(value)
                      : (type_oid == kInt4Oid) ? int4_out(value)
                      : (type_oid == kInt8Oid) ? int8_out(value)
                                               : float8_out(value);
            return std::string(s);
        }
        case kTextOid:
        case kVarcharOid: {
            char* s = text_out(value);
            std::string out = "'";
            for (char c : std::string(s)) {
                if (c == '\'') {
                    out.push_back('\'');
                }
                out.push_back(c);
            }
            out.push_back('\'');
            return out;
        }
        default:
            return "NULL";
    }
}

std::string FormatOperatorName(const std::string& schema, const std::string& opname) {
    if (schema.empty()) {
        return opname;
    }
    return QuoteIdentifier(schema) + "." + opname;
}

}  // namespace mytoydb::types
