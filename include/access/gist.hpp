// gist.h — GiST (Generalized Search Tree) access method API.
//
// Converted from PostgreSQL 15's src/include/access/gist.h.
//
// GiST is a balanced search tree that supports extensible predicates.
// Each internal node stores bounding keys (e.g. min/max ranges) that
// describe the subtree's contents. Insertion descends to the best leaf
// and propagates splits upward.
//
// pgcpp simplifications:
//   - Supports int32 range keys (internal nodes store [min, max])
//   - Root starts as leaf; splits into two leaves + new internal root
//   - No VACUUM, no buffering build, no coverage splits
#pragma once

#include <cstdint>
#include <vector>

#include "access/nbtree.hpp"
#include "access/rel.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::access {

// --- GiST index operations ---

void gistbuild(Relation index, BTKeyKind key_kind);

bool gistinsert(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
                const pgcpp::transaction::ItemPointerData& tid);

BTScanDesc gistbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key);

bool gistgettuple(BTScanDesc scan);

void gistrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key);

void gistendscan(BTScanDesc scan);

bool gistcanreturn(Relation index);

int64_t gistgetbitmap(BTScanDesc scan, std::vector<pgcpp::transaction::ItemPointerData>* tids);

}  // namespace pgcpp::access
