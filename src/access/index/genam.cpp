// genam.cpp — Generic index scan-key helpers.
//
// Converted from PostgreSQL 15's src/backend/access/index/genam.c.
//
// Implements ScanKeyInit and ScanKeyEntryInitialize, which populate a
// ScanKeyData with the comparison-qualification metadata used by the AM
// during a scan. pgcpp extends ScanKeyData with sk_key_kind / sk_key_len
// so the B-tree AM can compare without a function manager.

#include "pgcpp/access/genam.hpp"

namespace pgcpp::access {

void ScanKeyInit(ScanKey key, int attno, int strategy, pgcpp::catalog::Oid subtype,
                 pgcpp::types::Datum argument) {
    if (key == nullptr)
        return;
    key->sk_flags = 0;
    key->sk_attno = attno;
    key->sk_strategy = strategy;
    key->sk_subtype = subtype;
    key->sk_argument = argument;
    // Defaults: caller overrides for non-int32 keys.
    key->sk_key_kind = BTKeyKind::kInt32;
    key->sk_key_len = 0;
}

void ScanKeyEntryInitialize(ScanKey key, int flags, int attno, int strategy,
                            pgcpp::catalog::Oid subtype, BTKeyKind key_kind, uint16_t key_len,
                            pgcpp::types::Datum argument) {
    if (key == nullptr)
        return;
    key->sk_flags = flags;
    key->sk_attno = attno;
    key->sk_strategy = strategy;
    key->sk_subtype = subtype;
    key->sk_argument = argument;
    key->sk_key_kind = key_kind;
    key->sk_key_len = key_len;
}

}  // namespace pgcpp::access
