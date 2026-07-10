// spgist.h — SP-GiST (Space-Partitioned GiST) access method API.
//
// Converted from PostgreSQL 15's src/include/access/spgist.h.
//
// SP-GiST supports space-partitioned trees (tries, quadtrees, etc.).
// For int32 keys, pgcpp implements a binary radix trie: inner nodes
// partition by a single bit, leaf nodes hold (key, tid) entries.
//
// pgcpp simplifications:
//   - Supports int32 keys with binary radix partitioning
//   - Fixed 32-bit depth (one bit per level)
//   - No VACUUM, no pickout, no compression
#pragma once

#include <cstdint>
#include <vector>

#include "access/nbtree.hpp"
#include "access/rel.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::access {

// --- SP-GiST index operations ---

void spgistbuild(Relation index, BTKeyKind key_kind);

bool spgistinsert(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
                  const pgcpp::transaction::ItemPointerData& tid);

BTScanDesc spgistbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key);

bool spgistgettuple(BTScanDesc scan);

void spgistrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key);

void spgistendscan(BTScanDesc scan);

bool spgistcanreturn(Relation index);

int64_t spgistgetbitmap(BTScanDesc scan, std::vector<pgcpp::transaction::ItemPointerData>* tids);

}  // namespace pgcpp::access
