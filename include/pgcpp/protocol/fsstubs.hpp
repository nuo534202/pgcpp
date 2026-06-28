// fsstubs.h — Large-object file-descriptor stubs (be-fsstubs.c).
//
// Converted from PostgreSQL 15's src/backend/libpq/be-fsstubs.c.
//
// The frontend protocol exposes large objects through a file-descriptor API:
// the client sends a Fastpath or SQL call (lo_open, lo_read, ...) and the
// backend returns an integer "fd" that the client passes to subsequent
// lo_read/lo_write/lo_lseek/lo_close calls.
//
// Internally the fd is a small integer (an index into a session-local
// fd table) that maps to a LargeObjectDesc* (from inv_api.c). The fd table
// is per-backend (PG keeps it in a global array in be-fsstubs.c).
//
// pgcpp preserves this design: the fd table is a process-wide array of
// LargeObjectDesc* pointers, sized to PG's MAX_LARGE_OBJECTS (currently 1024).
// All operations are synchronous and single-threaded (matching PG's
// per-backend model).
#pragma once

#include <cstdint>

#include "pgcpp/storage/large_object/inv_api.hpp"

namespace pgcpp::protocol {

// MAX_LARGE_OBJECTS — maximum number of simultaneously open LOs per backend.
// Matches PG's MAX_LARGE_OBJECTS in be-fsstubs.c.
constexpr int kMaxLargeObjects = 1024;

// InvalidLargeObjectFd — sentinel returned by lo_open on failure.
constexpr int kInvalidLargeObjectFd = -1;

// lo_create — create a new large object. Returns the new OID.
// (Wraps inv_create; matches PG's lo_create Fastpath signature.)
storage::LargeObjectOid lo_create(int32_t lobjId);

// lo_open — open an existing LO by OID. Returns a non-negative fd on success
// or -1 on failure. `mode` is INV_READ / INV_WRITE / INV_RDWR.
int lo_open(storage::LargeObjectOid lobjId, int mode);

// lo_close — close an LO fd. Returns 0 on success, -1 on error.
int lo_close(int fd);

// lo_read — read up to `len` bytes from the LO referenced by `fd` into `buf`.
// Returns bytes read (>=0) or -1 on error.
int lo_read(int fd, char* buf, int len);

// lo_write — write `len` bytes from `buf` to the LO referenced by `fd`.
// Returns bytes written (>=0) or -1 on error.
int lo_write(int fd, const char* buf, int len);

// lo_lseek — seek to `offset` (whence: 0=SET, 1=CUR, 2=END).
// Returns the new offset (>=0) or -1 on error.
int lo_lseek(int fd, int32_t offset, int32_t whence);

// lo_lseek64 — 64-bit variant of lo_lseek.
int64_t lo_lseek64(int fd, int64_t offset, int32_t whence);

// lo_tell — return the current offset of `fd`, or -1 on error.
int32_t lo_tell(int fd);

// lo_tell64 — 64-bit variant of lo_tell.
int64_t lo_tell64(int fd);

// lo_truncate — truncate the LO to `len` bytes. Returns 0 on success, -1 on error.
int lo_truncate(int fd, int32_t len);

// lo_truncate64 — 64-bit variant of lo_truncate.
int lo_truncate64(int fd, int64_t len);

// lo_unlink — delete a large object by OID. Returns 1 on success, -1 on error.
int lo_unlink(storage::LargeObjectOid lobjId);

// ResetLargeObjectFds — close all open LO fds (used by tests).
void ResetLargeObjectFds();

// NumOpenLargeObjectFds — count of currently-open fds (for testing).
int NumOpenLargeObjectFds();

}  // namespace pgcpp::protocol
