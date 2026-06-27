// genam.h — Generic index access method API (scan keys).
//
// Converted from PostgreSQL 15's src/include/access/genam.h and skey.h.
//
// Provides the generic scan-key structure (ScanKeyData) used by all index
// access methods, plus the strategy-number constants for B-tree comparison
// operators. In PostgreSQL, ScanKeyData carries an FmgrInfo so the AM can
// invoke the comparison function via fmgr. MyToyDB simplifies this: the
// ScanKey carries the key value (as a Datum pointer) and the key kind/length,
// and the AM (nbtree) compares directly via _bt_compare_keys.
#pragma once

#include <cstdint>

#include "mytoydb/access/nbtpage.hpp"  // BTKeyKind
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/types/datum.hpp"

namespace mytoydb::access {

// --- Strategy numbers (PostgreSQL BTreeStrategyNumber constants) ---
//
// Each B-tree comparison operator maps to one of these strategy numbers.
// The AM uses the strategy to decide which entries match a scan key.
constexpr int kBTLessStrategyNumber = 1;          // key < sk_argument
constexpr int kBTLessEqualStrategyNumber = 2;     // key <= sk_argument
constexpr int kBTEqualStrategyNumber = 3;         // key == sk_argument
constexpr int kBTGreaterEqualStrategyNumber = 4;  // key >= sk_argument
constexpr int kBTGreaterStrategyNumber = 5;       // key > sk_argument

// Total number of B-tree strategies.
constexpr int kBTMaxStrategyNumber = 5;

// --- ScanKey flags (PostgreSQL SK_* constants, simplified) ---
constexpr int kSKSearchNull = 0x0001;     // search for NULLs
constexpr int kSKSearchNotNull = 0x0002;  // search for NOT NULLs
constexpr int kSKIsPartial = 0x0004;      // partial-match scan key

// ScanKeyData — a single scan qualification.
//
// Fields mirror PostgreSQL's ScanKeyData:
//   sk_flags    — search flags (SK_*)
//   sk_attno    — the index column number (1-based)
//   sk_strategy — operator strategy number (BTLessStrategyNumber etc.)
//   sk_subtype  — the operator's argument subtype OID (kInvalidOid if none)
//   sk_argument — the comparison value (Datum). For varlena types this is a
//                 pointer to a palloc'd value.
//
// MyToyDB additions (so the AM can compare without fmgr):
//   sk_key_kind — the key type (int32/int64/text) for direct comparison
//   sk_key_len  — length of the key data (for text; ignored for fixed types)
struct ScanKeyData {
    int sk_flags = 0;
    int sk_attno = 0;
    int sk_strategy = 0;
    mytoydb::catalog::Oid sk_subtype = mytoydb::catalog::kInvalidOid;
    mytoydb::types::Datum sk_argument = 0;
    BTKeyKind sk_key_kind = BTKeyKind::kInt32;
    uint16_t sk_key_len = 0;
};

// ScanKey — pointer to a ScanKeyData (PostgreSQL convention).
using ScanKey = ScanKeyData*;

// ScanKeyInit — initialize a scan key.
//
// Fills in the key with the given attribute number, strategy, subtype, and
// comparison argument. The key kind defaults to kInt32; the caller must set
// sk_key_kind / sk_key_len explicitly for non-int32 keys.
void ScanKeyInit(ScanKey key, int attno, int strategy, mytoydb::catalog::Oid subtype,
                 mytoydb::types::Datum argument);

// ScanKeyEntryInitialize — initialize a scan key with explicit flags.
//
// Mirrors PostgreSQL's ScanKeyEntryInitialize. The key kind and key length
// are set from the additional parameters (MyToyDB extension so the AM can
// compare without fmgr).
void ScanKeyEntryInitialize(ScanKey key, int flags, int attno, int strategy,
                            mytoydb::catalog::Oid subtype, BTKeyKind key_kind, uint16_t key_len,
                            mytoydb::types::Datum argument);

}  // namespace mytoydb::access
