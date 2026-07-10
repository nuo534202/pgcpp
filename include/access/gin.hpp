// gin.h — GIN (Generalized Inverted Index) access method API.
//
// Converted from PostgreSQL 15's src/include/access/gin.h.
//
// GIN is an inverted index: it maps individual key elements (e.g. array
// members, full-text tokens) to posting lists of heap TIDs. It excels at
// "contains" queries (array @> element, full-text @@ tsquery).
//
// pgcpp simplifications:
//   - Entry tree: a single flat page of (key, posting-list-block) entries
//   - Posting lists: pages of packed TIDs
//   - Supports int32 element keys (for array containment)
//   - No VACUUM, no fast update, no pending list
#pragma once

#include <cstdint>
#include <vector>

#include "access/nbtree.hpp"
#include "access/rel.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::access {

// --- GIN index operations ---

void ginbuild(Relation index, BTKeyKind key_kind);

bool gininsert(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
               const pgcpp::transaction::ItemPointerData& tid);

BTScanDesc ginbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key);

bool gingettuple(BTScanDesc scan);

void ginrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key);

void ginendscan(BTScanDesc scan);

bool gincanreturn(Relation index);

int64_t gingetbitmap(BTScanDesc scan, std::vector<pgcpp::transaction::ItemPointerData>* tids);

}  // namespace pgcpp::access
