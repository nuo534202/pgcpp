// heap_tuple.h — Heap tuple header and in-memory tuple representation.
//
// Converted from PostgreSQL 15's src/include/access/htup_details.h.
//
// A heap tuple is a row stored in a heap page. It consists of:
//   1. HeapTupleHeaderData (23 bytes) — MVCC metadata + null bitmap
//   2. User data (the column values, aligned per type)
//
// The header carries the transaction IDs that determine visibility:
//   t_xmin — XID that inserted (created) this tuple version
//   t_xmax — XID that deleted or updated this tuple (0 if not deleted)
//   t_cid  — command ID within the inserting transaction
//   t_ctid — current tuple ID (points to self, or to the updated version)
//
// t_infomask carries hint flags that accelerate visibility checks:
//   HEAP_XMIN_COMMITTED — t_xmin is known committed (hint, not WAL-logged)
//   HEAP_XMIN_INVALID   — t_xmin is known aborted
//   HEAP_XMAX_COMMITTED — t_xmax is known committed
//   HEAP_XMAX_INVALID   — t_xmax is known aborted
// These hints avoid repeated commit-log lookups for the same tuple.
#pragma once

#include <cstdint>

#include "mytoydb/storage/block.hpp"        // BlockNumber
#include "mytoydb/storage/bufpage.hpp"      // OffsetNumber, TransactionId
#include "mytoydb/transaction/transam.hpp"  // TransactionId, MultiXactId

namespace mytoydb::transaction {

// ItemPointerData — a tuple identifier (TID).
// Identifies a tuple by (block number, offset within page).
// Matches PostgreSQL's ItemPointerData layout (6 bytes, packed).
struct __attribute__((packed)) ItemPointerData {
    storage::BlockNumber ip_blkid = 0;   // block number
    storage::OffsetNumber ip_posid = 0;  // offset number (1-based)

    bool operator==(const ItemPointerData&) const = default;
};

// HeapTupleHeaderData — the on-disk header of a heap tuple.
//
// Layout (matches PostgreSQL for on-disk compatibility):
//   Offset  Size  Field
//   0       4     t_xmin (TransactionId)
//   4       4     t_xmax (TransactionId)
//   8       4     t_cid / t_xvac (CommandId or XID, depending on flags)
//   12      6     t_ctid (ItemPointerData: 4-byte block + 2-byte offset)
//   18      2     t_infomask2 (number of attrs + flags)
//   20      2     t_infomask (status flags)
//   22      1     t_hoff (offset to user data, including null bitmap)
//   23      var   t_bits[] (null bitmap, only if HEAP_HASNULL)
//
// Total header size (without null bitmap): 23 bytes.
// User data starts at t_hoff (aligned to MAXALIGN, typically 8 bytes → 24).
struct HeapTupleHeaderData {
    TransactionId t_xmin = 0;  // inserting XID
    TransactionId t_xmax = 0;  // deleting or updating XID
    uint32_t t_cid = 0;        // command ID (or t_xvac for VACUUM)
    ItemPointerData t_ctid;    // current TID (self or updated version)
    uint16_t t_infomask2 = 0;  // number of attrs + flags
    uint16_t t_infomask = 0;   // status flags
    uint8_t t_hoff = 0;        // offset to user data
    // t_bits[] follows if HEAP_HASNULL is set (variable length)
    uint8_t t_bits[1] = {0};  // null bitmap (only present if HASNULL)
};

// HeapTupleHeader — pointer to a HeapTupleHeaderData.
using HeapTupleHeader = HeapTupleHeaderData*;

// Size of the fixed HeapTupleHeaderData (excluding the null bitmap).
// PostgreSQL uses offsetof(HeapTupleHeaderData, t_bits) = 23.
constexpr int kHeapTupleHeaderSize = 23;

// MAXALIGN — align a size to the maximum alignment (8 bytes on 64-bit).
// PostgreSQL uses MAXALIGN(TYPEALIGN(8, ...)).
constexpr int kMaxAlign = 8;
constexpr int MaxAlignOf(int size) {
    return (size + kMaxAlign - 1) & ~(kMaxAlign - 1);
}
constexpr int kHeapTupleHeaderAligned = MaxAlignOf(kHeapTupleHeaderSize);  // 24

// --- t_infomask flags ---
//
// These match PostgreSQL's HEAP_XMIN_* and HEAP_XMAX_* constants exactly.

// t_infomask (16 bits): XMIN/XMAX status hints + other flags
constexpr uint16_t kHeapXminCommitted = 0x0100;      // t_xmin committed (hint)
constexpr uint16_t kHeapXminInvalid = 0x0200;        // t_xmin invalid/aborted (hint)
constexpr uint16_t kHeapXminFrozen = 0x0400;         // t_xmin is FrozenTransactionId
constexpr uint16_t kHeapXmaxLocked = 0x0080;         // t_xmax is a multixact (lock)
constexpr uint16_t kHeapXmaxExclusiveLock = 0x0040;  // FOR UPDATE lock (legacy)
constexpr uint16_t kHeapXmaxKeyshrLock = 0x0010;     // FOR KEY SHARE lock
constexpr uint16_t kHeapXmaxShrLock = 0x0020;        // FOR SHARE lock
constexpr uint16_t kHeapXmaxCommitted = 0x0400;      // t_xmax committed (hint)
constexpr uint16_t kHeapXmaxInvalid = 0x0800;        // t_xmax invalid/aborted (hint)

constexpr uint16_t kHeapHasNull = 0x0001;      // has null bitmap
constexpr uint16_t kHeapHasVarWidth = 0x0002;  // has variable-width attrs
constexpr uint16_t kHeapHasOid = 0x0004;       // has OID column (unused)
constexpr uint16_t kHeapUpdated = 0x2000;      // this is an UPDATEd row
constexpr uint16_t kHeapMovedOff = 0x1000;     // moved by VACUUM (off-page)
constexpr uint16_t kHeapMovedIn = 0x2000;      // moved by VACUUM (in-page)

// Combined masks for checking XMIN/XMAX status
constexpr uint16_t kHeapXminStatusMask = kHeapXminCommitted | kHeapXminInvalid | kHeapXminFrozen;
constexpr uint16_t kHeapXmaxStatusMask = kHeapXmaxCommitted | kHeapXmaxInvalid | kHeapXmaxLocked;

// --- t_infomask2 flags ---
//
// The low 11 bits store the number of attributes (HEAP_NATTS_MASK).
// The high bits store additional flags.
constexpr uint16_t kHeapNattsMask = 0x07FF;    // 11 bits for attr count
constexpr uint16_t kHeapKeysUpdated = 0x2000;  // key columns updated
constexpr uint16_t kHeapHotUpdated = 0x4000;   // HOT update
constexpr uint16_t kHeapOnlyTuple = 0x8000;    // HOT-only tuple (no index)

// --- Accessor functions ---
//
// These mirror PostgreSQL's HeapTupleHeaderGetXmin etc. macros.

inline TransactionId HeapTupleHeaderGetXmin(const HeapTupleHeaderData* tup) {
    return tup->t_xmin;
}

inline TransactionId HeapTupleHeaderGetXmax(const HeapTupleHeaderData* tup) {
    return tup->t_xmax;
}

inline void HeapTupleHeaderSetXmin(HeapTupleHeaderData* tup, TransactionId xid) {
    tup->t_xmin = xid;
}

inline void HeapTupleHeaderSetXmax(HeapTupleHeaderData* tup, TransactionId xid) {
    tup->t_xmax = xid;
}

inline uint32_t HeapTupleHeaderGetCid(const HeapTupleHeaderData* tup) {
    return tup->t_cid;
}

inline void HeapTupleHeaderSetCid(HeapTupleHeaderData* tup, uint32_t cid) {
    tup->t_cid = cid;
}

inline ItemPointerData& HeapTupleHeaderGetTid(HeapTupleHeaderData* tup) {
    return tup->t_ctid;
}

inline const ItemPointerData& HeapTupleHeaderGetTid(const HeapTupleHeaderData* tup) {
    return tup->t_ctid;
}

inline void HeapTupleHeaderSetTid(HeapTupleHeaderData* tup, const ItemPointerData& tid) {
    tup->t_ctid = tid;
}

// HeapTupleHeaderGetXactStatus — return the hint flags for t_xmin.
// Returns one of: COMMITTED, INVALID, FROZEN, or IN_PROGRESS (no hint).
// Used by visibility checks to avoid commit-log lookups.
enum class XactStatus {
    kInProgress,
    kCommitted,
    kAborted,  // maps to INVALID hint
    kFrozen,
};

inline XactStatus HeapTupleHeaderGetXminStatus(const HeapTupleHeaderData* tup) {
    uint16_t info = tup->t_infomask;
    if (info & kHeapXminCommitted)
        return XactStatus::kCommitted;
    if (info & kHeapXminInvalid)
        return XactStatus::kAborted;
    if (info & kHeapXminFrozen)
        return XactStatus::kFrozen;
    return XactStatus::kInProgress;
}

inline XactStatus HeapTupleHeaderGetXmaxStatus(const HeapTupleHeaderData* tup) {
    uint16_t info = tup->t_infomask;
    if (info & kHeapXmaxCommitted)
        return XactStatus::kCommitted;
    if (info & kHeapXmaxInvalid)
        return XactStatus::kAborted;
    return XactStatus::kInProgress;
}

// Set hint flags indicating t_xmin is committed.
inline void HeapTupleHeaderSetXminCommitted(HeapTupleHeaderData* tup) {
    tup->t_infomask |= kHeapXminCommitted;
    tup->t_infomask &= ~kHeapXminInvalid;
}

// Set hint flags indicating t_xmin is aborted (invalid).
inline void HeapTupleHeaderSetXminInvalid(HeapTupleHeaderData* tup) {
    tup->t_infomask |= kHeapXminInvalid;
    tup->t_infomask &= ~kHeapXminCommitted;
}

// Set hint flags indicating t_xmax is committed.
inline void HeapTupleHeaderSetXmaxCommitted(HeapTupleHeaderData* tup) {
    tup->t_infomask |= kHeapXmaxCommitted;
    tup->t_infomask &= ~kHeapXmaxInvalid;
}

// Set hint flags indicating t_xmax is aborted (invalid).
inline void HeapTupleHeaderSetXmaxInvalid(HeapTupleHeaderData* tup) {
    tup->t_infomask |= kHeapXmaxInvalid;
    tup->t_infomask &= ~kHeapXmaxCommitted;
}

// HeapTupleHeaderGetNatts — return the number of attributes.
inline int HeapTupleHeaderGetNatts(const HeapTupleHeaderData* tup) {
    return tup->t_infomask2 & kHeapNattsMask;
}

// HeapTupleHeaderSetNatts — set the number of attributes.
inline void HeapTupleHeaderSetNatts(HeapTupleHeaderData* tup, int natts) {
    tup->t_infomask2 =
        (tup->t_infomask2 & ~kHeapNattsMask) | static_cast<uint16_t>(natts & kHeapNattsMask);
}

// HeapTupleHeaderGetDatumLength — return the total tuple length.
// In PostgreSQL this is stored in HeapTupleData.t_len, not the header.
// We compute it from t_hoff + user data size (caller must provide).
inline int HeapTupleHeaderGetHOff(const HeapTupleHeaderData* tup) {
    return tup->t_hoff;
}

// HeapTupleHeaderHasNulls — true if the tuple has a null bitmap.
inline bool HeapTupleHeaderHasNulls(const HeapTupleHeaderData* tup) {
    return (tup->t_infomask & kHeapHasNull) != 0;
}

// HeapTupleHeaderIsHotUpdated — true if this tuple was HOT-updated.
inline bool HeapTupleHeaderIsHotUpdated(const HeapTupleHeaderData* tup) {
    return (tup->t_infomask2 & kHeapHotUpdated) != 0;
}

// HeapTupleHeaderIsHeapOnly — true if this is a HOT-only tuple.
inline bool HeapTupleHeaderIsHeapOnly(const HeapTupleHeaderData* tup) {
    return (tup->t_infomask2 & kHeapOnlyTuple) != 0;
}

// --- In-memory tuple representation ---
//
// HeapTupleData is the in-memory descriptor for a tuple. It points to the
// header data (which lives in a buffer page or a palloc'd region) and
// records the total length and TID.
struct HeapTupleData {
    uint32_t t_len = 0;                     // length of the tuple (header + data)
    ItemPointerData t_self;                 // TID of this tuple
    HeapTupleHeaderData* t_data = nullptr;  // pointer to the header

    // Convenience accessor
    HeapTupleHeader GetHeader() { return t_data; }
    const HeapTupleHeaderData* GetHeader() const { return t_data; }
};

// HeapTuple — pointer to a HeapTupleData (PostgreSQL convention).
using HeapTuple = HeapTupleData*;

}  // namespace mytoydb::transaction
