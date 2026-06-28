// inv_api.h — Large object (inversion) API.
//
// Converted from PostgreSQL 15's src/include/storage/large_object.h and
// src/backend/storage/large_object/inv_api.c.
//
// PG stores large objects ("BLOBs") in a system catalog pg_largeobject,
// whose rows are (loid, pageno, data) tuples. Each LO is a sequence of
// up-to-LOBLKSIZE-byte pages. inv_api provides a file-like API:
// inv_create / inv_open / inv_read / inv_write / inv_seek / inv_truncate.
//
// MyToyDB keeps large objects in an in-memory std::map<oid, LargeObject>
// to avoid the catalog machinery. The API is preserved so callers can
// exercise the LO semantics (read/write/seek/truncate).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace mytoydb::storage {

// Oid — large object identifier (uint32_t in PG).
using LargeObjectOid = uint32_t;

// InvalidOid — sentinel for "no such LO".
constexpr LargeObjectOid kInvalidLargeObjectOid = 0;

// LOBLKSIZE — page size for large object data (matches PG's BLCKSZ - 24).
constexpr int kLoblkSize = 8168;

// INV_READ / INV_WRITE / INV_RDWR — open flags (PG's lo_open mode bits).
constexpr int kInvRead = 0x00040000;
constexpr int kInvWrite = 0x00020000;
constexpr int kInvRdwr = kInvRead | kInvWrite;

// LargeObject — in-memory representation of one large object.
struct LargeObject {
    LargeObjectOid oid = kInvalidLargeObjectOid;
    uint32_t owner_oid = 0;     // owner role oid
    std::vector<uint8_t> data;  // entire blob (concatenated pages)
};

// LargeObjectDesc — handle returned by inv_open (PG's LargeObjectDesc).
struct LargeObjectDesc {
    LargeObjectOid oid = kInvalidLargeObjectOid;
    int flags = 0;        // INV_READ / INV_WRITE
    uint64_t offset = 0;  // current seek position
};

// inv_create — create a new large object with the given oid (or pick one
// if oid == kInvalidLargeObjectOid). Returns the new oid.
LargeObjectOid inv_create(LargeObjectOid oid);

// inv_open — open an existing LO by oid. Returns nullptr if not found.
LargeObjectDesc* inv_open(LargeObjectOid oid, int flags);

// inv_close — close an LO descriptor (frees the LargeObjectDesc).
int inv_close(LargeObjectDesc* desc);

// inv_read — read up to nbytes from the LO at *desc->offset, updating offset.
// Returns bytes read (0 at EOF).
int inv_read(LargeObjectDesc* desc, uint8_t* buffer, int nbytes);

// inv_write — write nbytes to the LO at *desc->offset, updating offset and
// extending the LO as needed. Returns bytes written.
int inv_write(LargeObjectDesc* desc, const uint8_t* buffer, int nbytes);

// inv_seek — seek to offset; returns the new offset or -1 on error.
// whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END.
int64_t inv_seek(LargeObjectDesc* desc, int64_t offset, int whence);

// inv_truncate — truncate the LO to the given length.
int inv_truncate(LargeObjectDesc* desc, int64_t length);

// inv_drop — delete the large object from the catalog. Returns 0 on success.
int inv_drop(LargeObjectOid oid);

// ResetLargeObjects — drop all large objects (used by tests).
void ResetLargeObjects();

// GetLargeObject — direct access for tests; returns nullptr if not found.
LargeObject* GetLargeObject(LargeObjectOid oid);

// NumLargeObjects — count of currently-stored LOs.
int NumLargeObjects();

// inv_tell — return the current offset (PG's inv_tell).
int64_t inv_tell(LargeObjectDesc* desc);

}  // namespace mytoydb::storage
