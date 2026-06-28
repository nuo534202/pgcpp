#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pgcpp/types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// Full-text search types (PostgreSQL utils/adt/tsvector.c, tsquery.c).
//
// Simplified in-memory representation. We expose:
//   - TsVectorData: a list of (lexeme, positions[]) pairs sorted by lexeme.
//   - TsQueryData:  a flat list of terms (with optional NOT) combined by
//                   AND/OR. We do not implement phrase queries or weighted
//                   lexemes — sufficient for unit testing in/out and basic
//                   boolean matching.
// ---------------------------------------------------------------------------

struct TsWordEntry {
    std::string lexeme;
    std::vector<int32_t> positions;
};

struct TsVectorData {
    std::vector<TsWordEntry> entries;  // sorted ascending by lexeme
};

enum class TsQueryNodeType : uint8_t {
    kTerm,
    kNot,
    kAnd,
    kOr,
};

struct TsQueryNode {
    TsQueryNodeType type;
    std::string lexeme;                 // valid for kTerm
    std::vector<TsQueryNode> children;  // for kAnd/kOr
};

struct TsQueryData {
    TsQueryNode root;
};

// --- tsvector ---
Datum tsvector_in(const char* str);
char* tsvector_out(Datum value);

// --- tsquery ---
Datum tsquery_in(const char* str);
char* tsquery_out(Datum value);

// ts_match — tsvector matches tsquery?
Datum ts_match(Datum tsvector, Datum tsquery);

// Helpers.
Datum MakeTsVectorDatum(const TsVectorData& data);
Datum MakeTsQueryDatum(const TsQueryData& data);
inline TsVectorData* DatumGetTsVector(Datum x) {
    return reinterpret_cast<TsVectorData*>(x);
}
inline TsQueryData* DatumGetTsQuery(Datum x) {
    return reinterpret_cast<TsQueryData*>(x);
}

}  // namespace pgcpp::types
