// brin.h — BRIN (Block Range Index) access method API.
//
// Converted from PostgreSQL 15's src/include/access/brin.h.
//
// BRIN stores summary information (min, max) for ranges of heap blocks.
// It is extremely compact — one summary entry per N heap pages. Scan
// skips ranges whose min/max cannot satisfy the scan key, then fetches
// candidate tuples from the heap for residual checking.
//
// pgcpp simplifications:
//   - Summary: {min, max} per range (int32 keys)
//   - Fixed pages_per_range (default 16)
//   - No VACUUM, no auto-summarize
#pragma once

#include <cstdint>
#include <vector>

#include "access/nbtree.hpp"
#include "access/rel.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::access {

// Default number of heap pages per BRIN range.
constexpr uint32_t kBrinDefaultPagesPerRange = 16;

// --- BRIN index operations ---

void brinbuild(Relation index, BTKeyKind key_kind);

bool brininsert(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
                const pgcpp::transaction::ItemPointerData& tid);

BTScanDesc brinbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key);

bool bringettuple(BTScanDesc scan);

void brinrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key);

void brinendscan(BTScanDesc scan);

bool brincanreturn(Relation index);

int64_t bringetbitmap(BTScanDesc scan, std::vector<pgcpp::transaction::ItemPointerData>* tids);

}  // namespace pgcpp::access
