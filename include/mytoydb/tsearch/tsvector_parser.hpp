#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mytoydb::tsearch {

// ---------------------------------------------------------------------------
// tsvector parser (PostgreSQL src/backend/tsearch/tsvector_parser.c).
//
// Parses a tsvector string literal of the form
//     "lexeme:posA,posB lexeme2:posC"
// into a list of WordEntry. Each entry carries the lexeme, the list of
// positions, and the per-position weight (A, B, C, or D).
//
// Note: we define a tsearch-specific WordEntry because the existing
// mytoydb::types::TsWordEntry (in types/ts_types.hpp) does not carry weights.
// ---------------------------------------------------------------------------

struct WordEntry {
    std::string lexeme;
    std::vector<int32_t> positions;
    std::vector<char> weights;  // 'A','B','C','D' — one per position
};

// Parse a tsvector string literal. Throws (ereport) on malformed input.
// The returned list is NOT sorted — callers may sort as needed.
std::vector<WordEntry> TsVectorParse(std::string_view str);

}  // namespace mytoydb::tsearch
