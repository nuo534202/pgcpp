#pragma once

#include <cstdint>
#include <string_view>

namespace pgcpp::parser {

// Keyword categories. Matches PostgreSQL's keyword categories.
// UNRESERVED_KEYWORD: can be used as a column/function name
// COL_NAME_KEYWORD: can be used as a column name but not a function name
// TYPE_FUNC_NAME_KEYWORD: can be used as a type or function name
// RESERVED_KEYWORD: fully reserved, cannot be used as an identifier
enum class KeywordCategory : uint8_t {
    kUnreserved = 0,
    kColName,
    kTypeFuncName,
    kReserved,
};

// Label status — whether the keyword can appear as a bare label in
// SELECT output (e.g., `SELECT 1 AS foo` vs `SELECT 1 foo`).
enum class KeywordLabel : uint8_t {
    kBareLabel = 0,
    kAsLabel,
};

// A single keyword entry in the keyword table.
struct KeywordEntry {
    std::string_view name;
    int token;  // Bison token code (from gram.tab.hpp)
    KeywordCategory category;
    KeywordLabel label;
};

// Total number of keywords in the table.
constexpr int kKeywordCount = 460;

// The keyword table, sorted by name for binary search.
extern const KeywordEntry kKeywordTable[];

// Look up a keyword by name. Returns nullptr if not a keyword.
const KeywordEntry* ScanKeywordLookup(std::string_view name);

}  // namespace pgcpp::parser
