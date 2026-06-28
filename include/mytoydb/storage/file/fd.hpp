// fd.h — Virtual File Descriptor (VFD) cache.
//
// Converted from PostgreSQL 15's src/include/storage/fd.h and
// src/backend/storage/file/fd.c.
//
// PostgreSQL opens more files than the OS FD limit allows by maintaining a
// LRU pool of VFDs (virtual file descriptors). When a real fd is needed,
// the LRU VFD is closed (with its position saved) and the fd is reused.
//
// MyToyDB is single-process and typically under the OS fd limit, so VFDs
// are mostly a thin wrapper around real fds. The API preserves PG's
// AllocateFile/PathNameOpenFile/etc. surface.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mytoydb::storage {

// File — opaque VFD handle (1-based; 0 = invalid).
using File = int;

constexpr File kInvalidFile = 0;

// FileAccessFlags — open() mode bits (subset; matches PG's O_RDONLY / O_RDWR etc).
enum FileAccessFlags : int {
    kOReadOnly = 0,
    kOReadWrite = 1 << 1,
    kOCreate = 1 << 2,
    kOExclusive = 1 << 3,
    kOAppend = 1 << 4,
};

// AllocateFile — open a transient file (auto-closed at transaction end).
// Returns a FILE* or nullptr on error.
void* AllocateFile(const char* name, const char* mode);

// FreeFile — close a file opened by AllocateFile.
int FreeFile(void* file);

// PathNameOpenFile — open a permanent file (returns a VFD handle).
// Returns kInvalidFile on error.
File PathNameOpenFile(const char* name, int flags, int mode = 0644);

// FileClose — close a VFD (and its underlying fd).
int FileClose(File file);

// FileRead — read up to nbytes from the file at *offset. On entry *offset
// is the file position; on return it is updated to the new position.
// Returns bytes read or -1 on error.
int FileRead(File file, void* buffer, std::size_t nbytes, int64_t* offset);

// FileWrite — write nbytes to the file at *offset, updating *offset.
// Returns bytes written or -1 on error.
int FileWrite(File file, const void* buffer, std::size_t nbytes, int64_t* offset);

// FileSync — fsync the underlying fd.
int FileSync(File file);

// FileSeek — seek to offset; returns the new position or -1 on error.
int64_t FileSeek(File file, int64_t offset, int whence);

// FileTruncate — truncate the file to the given length.
int FileTruncate(File file, int64_t length);

// FileName — return the path that PathNameOpenFile was called with.
const char* FileName(File file);

// FileFd — return the underlying OS fd (for low-level use).
int FileFd(File file);

// InitFileAccess — initialize the VFD pool (idempotent).
void InitFileAccess();

// CloseTransientFiles — close all currently-open transient files
// (called at transaction abort).
void CloseTransientFiles();

// ResetVfdCache — drop all VFDs without closing files (used by tests).
void ResetVfdCache();

// NumOpenFiles — number of currently-open VFDs.
int NumOpenFiles();

}  // namespace mytoydb::storage
