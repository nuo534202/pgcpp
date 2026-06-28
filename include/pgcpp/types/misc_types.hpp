#pragma once

#include <cstdint>

#include "pgcpp/types/datum.hpp"

namespace mytoydb::types {

// ---------------------------------------------------------------------------
// "char" — single-byte character type (PostgreSQL internal type OID 18).
// Despite the name, this is essentially a tiny int8 storing one ASCII byte.
// Used heavily by the catalog for enum-like classification fields.
// ---------------------------------------------------------------------------

Datum char_in(const char* str);  // takes first character of str
char* char_out(Datum value);
int char_cmp(Datum a, Datum b);
Datum char_eq(Datum a, Datum b);
Datum char_lt(Datum a, Datum b);

// ---------------------------------------------------------------------------
// name — 64-byte fixed-length identifier (PostgreSQL type OID 19).
// Used for table/column/sequence/etc. names in catalog rows. Stores up to
// 63 bytes of identifier plus a NUL terminator (NAMEDATALEN-1).
// ---------------------------------------------------------------------------

constexpr int kNameDataLen = 64;  // NAMEDATALEN

struct NameData {
    char data[kNameDataLen];
};

Datum name_in(const char* str);
char* name_out(Datum value);
int name_cmp(Datum a, Datum b);
Datum name_eq(Datum a, Datum b);
Datum name_ne(Datum a, Datum b);
Datum name_lt(Datum a, Datum b);
Datum name_le(Datum a, Datum b);
Datum name_gt(Datum a, Datum b);
Datum name_ge(Datum a, Datum b);

// Construct a NameData datum from a C string (truncating to NAMEDATALEN-1).
Datum MakeNameDatum(const char* str);
// Borrow a NameData datum as a C string view.
const char* NameDatumToCString(Datum value);

// ---------------------------------------------------------------------------
// xid — TransactionId (uint32, OID 28). xid8 — FullTransactionId (uint64, OID 5069).
// ---------------------------------------------------------------------------

Datum xid_in(const char* str);
char* xid_out(Datum value);
Datum xid8_in(const char* str);
char* xid8_out(Datum value);
int xid_cmp(Datum a, Datum b);
Datum xid_eq(Datum a, Datum b);

// ---------------------------------------------------------------------------
// tid — ItemPointerData (BlockNumber, OffsetNumber) (OID 27).
// Stored as a 6-byte structure on disk; Datum is a pointer to a copy.
// ---------------------------------------------------------------------------

struct ItemPointerData {
    uint32_t block_num;   // BlockIdData (uint16 bi_hi + uint16 bi_lo) collapsed
    uint16_t offset_num;  // OffsetNumber
};

Datum tid_in(const char* str);  // "(block,offset)"
char* tid_out(Datum value);
int tid_cmp(Datum a, Datum b);
Datum tid_eq(Datum a, Datum b);
Datum tid_lt(Datum a, Datum b);
Datum tid_gt(Datum a, Datum b);

// ---------------------------------------------------------------------------
// pg_lsn — Log Sequence Number (uint64, OID 3220).
// Displayed as two hex uint32 values separated by '/'.
// ---------------------------------------------------------------------------

Datum pg_lsn_in(const char* str);  // "FFFFFFFF/FFFFFFFF"
char* pg_lsn_out(Datum value);
int pg_lsn_cmp(Datum a, Datum b);
Datum pg_lsn_eq(Datum a, Datum b);
Datum pg_lsn_lt(Datum a, Datum b);
Datum pg_lsn_add(Datum a, Datum b);  // pg_lsn + numeric -> pg_lsn

// ---------------------------------------------------------------------------
// bytea — variable-length binary string (OID 17).
// Stores a varlena with raw bytes. Display uses PostgreSQL "escape" format
// with backslash escapes for non-printable bytes.
// ---------------------------------------------------------------------------

Datum bytea_in(const char* str);  // accepts "abc" and "\\000\\001" escapes
char* bytea_out(Datum value);
int bytea_cmp(Datum a, Datum b);
Datum bytea_eq(Datum a, Datum b);
Datum bytea_concat(Datum a, Datum b);
Datum bytea_length(Datum value);

}  // namespace mytoydb::types
