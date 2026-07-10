// hash.h — Hash index access method API.
//
// Converted from PostgreSQL 15's src/include/access/hash.h.
//
// The hash access method maps keys to bucket pages via a hash function.
// It supports only equality lookups (point queries). Each bucket page
// holds (key, tid) entries; overflow pages chain from full buckets.
//
// pgcpp simplifications (mirroring nbtree):
//   - Fixed bucket count (no split / linear hashing)
//   - Supports int32, int64, and text keys
//   - No VACUUM, no page deletion
#pragma once

#include <cstdint>
#include <vector>

#include "access/nbtree.hpp"  // BTScanDesc, BTKeyKind, BTScanKeyData
#include "access/rel.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::access {

// --- Hash index operations ---

// hashbuild — create a new (empty) hash index.
// Initializes the meta page (block 0) with the bucket count and a single
// bucket page (block 1).
void hashbuild(Relation index, BTKeyKind key_kind);

// hashinsert — insert a (key, tid) entry into the hash index.
bool hashinsert(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
                const pgcpp::transaction::ItemPointerData& tid);

// hashbeginscan — start a hash scan. Only BTStrategy::kEqual is meaningful;
// other strategies fall back to a full bucket scan.
BTScanDesc hashbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key);

// hashgettuple — fetch the next matching tid from the hash scan.
bool hashgettuple(BTScanDesc scan);

// hashrescan — restart the scan.
void hashrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key);

// hashendscan — release scan resources.
void hashendscan(BTScanDesc scan);

// hashcanreturn — hash indexes cannot return heap tuples (no amcanreturn).
bool hashcanreturn(Relation index);

// hashgetbitmap — fetch all matching tids into a vector.
int64_t hashgetbitmap(BTScanDesc scan, std::vector<pgcpp::transaction::ItemPointerData>* tids);

// --- Hash helpers (exposed for testing) ---

// hashcalc — compute the bucket number for a key.
// Returns a bucket index in [0, nbuckets-1].
uint32_t hashcalc(BTKeyKind kind, const void* key, uint16_t key_len, uint32_t nbuckets);

}  // namespace pgcpp::access
