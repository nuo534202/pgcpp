// large_object.hpp — Large object (lo_*) API (P3-11).
//
// Mirrors PostgreSQL libpq's large-object client API. In pgcpp these
// are thin wrappers around the in-process fsstubs module
// (src/protocol/fsstubs.cpp), which provides the per-process file
// descriptor table and OID→Relation mapping. A real libpq would
// forward the lo_* calls as function-call ('F') messages over the wire;
// pgcpp instead invokes the fsstub functions directly because the
// client library is linked into the same binary as the server (M13
// tooling model).
//
// The API surface matches libpq so that client code that uses lo_* can
// be written identically to libpq-based code.
#pragma once

#include <cstdint>
#include <string>

namespace pgcpp::libpq {

class PgConn;

// LoMode — access flags for LoOpen (mirrors INV_READ / INV_WRITE in libpq).
namespace LoMode {
constexpr int kRead = 0x00040000;   // INV_READ
constexpr int kWrite = 0x00020000;  // INV_WRITE
}  // namespace LoMode

// LoCreate — create a new large object with the given OID (or
// kInvalidOid to let the server assign one). Returns the assigned OID
// or kInvalidOid on failure.
uint32_t LoCreate(PgConn& conn, uint32_t lobjId);

// LoImport — import a file as a new large object. Returns the new OID
// or kInvalidOid on failure.
uint32_t LoImport(PgConn& conn, const std::string& filename);

// LoImportWithOid — import with a specific OID.
uint32_t LoImportWithOid(PgConn& conn, const std::string& filename, uint32_t lobjId);

// LoExport — export a large object to a file. Returns 1 on success,
// -1 on failure (matching libpq's lo_export return convention).
int LoExport(PgConn& conn, uint32_t lobjId, const std::string& filename);

// LoOpen — open an existing large object. Returns a file-descriptor
// (>= 0) on success or -1 on failure.
int LoOpen(PgConn& conn, uint32_t lobjId, int mode);

// LoClose — close a large object descriptor. Returns 0 on success,
// -1 on failure.
int LoClose(PgConn& conn, int fd);

// LoRead — read up to `len` bytes into `buf`. Returns the number of
// bytes read (>= 0) or -1 on failure.
int LoRead(PgConn& conn, int fd, char* buf, int len);

// LoWrite — write `len` bytes from `buf`. Returns the number of bytes
// written (>= 0) or -1 on failure.
int LoWrite(PgConn& conn, int fd, const char* buf, int len);

// LoSeek — seek to `offset` from `whence` (0=SEEK_SET, 1=SEEK_CUR,
// 2=SEEK_END). Returns 0 on success, -1 on failure.
int LoSeek(PgConn& conn, int fd, int offset, int whence);

// LoSeek64 — 64-bit variant of LoSeek.
int LoSeek64(PgConn& conn, int fd, int64_t offset, int whence);

// LoTell — return the current offset, or -1 on failure.
int LoTell(PgConn& conn, int fd);

// LoTell64 — 64-bit variant of LoTell.
int64_t LoTell64(PgConn& conn, int fd);

// LoTruncate — truncate the large object to `len` bytes. Returns 0 on
// success, -1 on failure.
int LoTruncate(PgConn& conn, int fd, int len);

// LoTruncate64 — 64-bit variant of LoTruncate.
int LoTruncate64(PgConn& conn, int fd, int64_t len);

// LoUnlink — delete a large object. Returns 1 on success, -1 on failure.
int LoUnlink(PgConn& conn, uint32_t lobjId);

}  // namespace pgcpp::libpq
