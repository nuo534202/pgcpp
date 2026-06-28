// xlog.h — Write-Ahead Log (WAL) core types and buffer management.
//
// Converted from PostgreSQL 15's src/include/access/xlog.h and xlog_internal.h.
//
// The WAL is an append-only sequence of XLogRecord entries. Each record is
// identified by a Log Sequence Number (LSN = XLogRecPtr, a uint64 byte offset
// into the logical WAL stream). Records are written by XLogInsert (see
// xloginsert.h) and read back by XLogReader (see xlogreader.h) during crash
// recovery.
//
// In PostgreSQL, WAL is stored in pg_wal/ as a series of segment files. MyToyDB
// is single-process, so we keep an in-memory byte buffer for testability; the
// LSN semantics and record framing are identical.
#pragma once

#include <cstdint>
#include <vector>

#include "pgcpp/transaction/transam.hpp"

namespace mytoydb::transaction {

// XLogRecPtr — Log Sequence Number: byte offset in the WAL stream.
// InvalidXLogRecPtr (0) means "no LSN yet". The first valid record starts
// at the beginning of the first segment (after the page header area).
using XLogRecPtr = uint64_t;

// InvalidXLogRecPtr — sentinel for "no LSN".
constexpr XLogRecPtr kInvalidXLogRecPtr = 0;

// XLogSegNo — WAL segment number (1-based). Each segment is WAL_SEGMENT_SIZE
// bytes. Used for segment-based operations (cleanup, archiving).
using XLogSegNo = uint64_t;

// WAL_SEGMENT_SIZE — matches PostgreSQL's default segment size (16 MB).
// MyToyDB's in-memory buffer is much smaller, but this constant is kept for
// LSN arithmetic fidelity.
constexpr int kWalSegmentSize = 16 * 1024 * 1024;

// RmgrId — Resource Manager ID. Identifies which subsystem owns a WAL record
// (heap, btree, xact, etc.). Each RMGR provides a redo function for recovery.
using RmgrId = uint8_t;

// Standard resource manager IDs (matches PostgreSQL's RM_XLOG_ID etc.).
// Keep these in sync with rmgr.cpp's RmgrTable ordering.
constexpr RmgrId kRmgrXlogId = 0;
constexpr RmgrId kRmgrXactId = 1;
constexpr RmgrId kRmgrSmgrId = 2;
constexpr RmgrId kRmgrDbId = 3;
constexpr RmgrId kRmgrTblspcId = 4;
constexpr RmgrId kRmgrMultiXactId = 5;
constexpr RmgrId kRmgrRelmapId = 6;
constexpr RmgrId kRmgrStandbyId = 7;
constexpr RmgrId kRmgrHeapId = 8;
constexpr RmgrId kRmgrBtreeId = 9;
constexpr RmgrId kRmgrHashId = 10;
constexpr RmgrId kRmgrGinId = 11;
constexpr RmgrId kRmgrGistId = 12;
constexpr RmgrId kRmgrSequenceId = 13;
constexpr RmgrId kRmgrSpGistId = 14;
constexpr RmgrId kRmgrBrinId = 15;
constexpr RmgrId kRmgrCommitTsId = 16;
constexpr RmgrId kRmgrReplicationId = 17;
constexpr RmgrId kRmgrLogicalMsgId = 18;

// XLR_INFO_MASK — bits of xl_info that hold the info byte (low 4 bits).
constexpr uint8_t kXlrInfoMask = 0x0F;
// XLR_RMGR_INFO_MASK — bits of xl_info that hold the RMGR-specific info (high 4 bits).
constexpr uint8_t kXlrRmgrInfoMask = 0xF0;

// XLogRecord — the fixed-size header of every WAL record.
// Matches PostgreSQL's XLogRecord struct (sizeof = 24 on 64-bit).
struct XLogRecord {
    uint32_t xl_tot_len = 0;   // total length including header and data
    TransactionId xl_xid = 0;  // transaction ID that made the change
    XLogRecPtr xl_prev = 0;    // LSN of the previous record (0 for first)
    uint8_t xl_info = 0;       // flag bits + RMGR-specific info
    RmgrId xl_rmid = 0;        // resource manager ID
    uint32_t xl_crc = 0;       // CRC of the record (simplified: not verified)
};

// Size of the XLogRecord header (without data).
constexpr int kSizeofXlogRecord = 24;

// MAX_XLOG_RECORD_LENGTH — maximum total record length (PG: 20MB; we use 1MB).
constexpr uint32_t kMaxXlogRecordLength = 1024 * 1024;

// --- WAL buffer state ---
//
// In PostgreSQL, WAL is buffered in shared memory (wal_buffers, default 3MB)
// and flushed to disk by the WAL writer or at commit. MyToyDB uses a single
// in-memory vector as the "WAL stream"; LSNs are byte offsets into it.

// Initialize the WAL subsystem (clear the buffer, reset the insert pointer).
// Must be called once at startup before any XLogInsert.
void InitializeWal();

// Reset the WAL to an empty state (for testing).
void ResetWal();

// Get the current insert position (the LSN where the next record will go).
XLogRecPtr GetXLogInsertRecPtr();

// Get the current flush position (the LSN up to which WAL is durable).
// In MyToyDB, all inserted records are immediately "flushed", so this
// equals GetXLogInsertRecPtr.
XLogRecPtr GetXLogWriteRecPtr();

// Write raw bytes into the WAL buffer at the current insert position.
// Returns the LSN of the start of the written data. Advances the insert
// pointer. Called by XLogInsert after assembling a record.
XLogRecPtr XLogWriteRaw(const void* data, std::size_t len);

// Read raw bytes from the WAL buffer at a given LSN.
// Returns the number of bytes actually read (may be less than len if the
// LSN is near the end). Used by XLogReader.
std::size_t XLogReadRaw(XLogRecPtr lsn, void* buffer, std::size_t len);

// Get the total size of the WAL buffer (in bytes).
std::size_t GetWalBufferSize();

// Get a read-only pointer to the WAL buffer (for testing/diagnostics).
const std::vector<uint8_t>& GetWalBuffer();

// XLogFlush — flush WAL up to the given LSN. In MyToyDB this is a no-op
// (the in-memory buffer is always "durable"), but it matches the PG API.
void XLogFlush(XLogRecPtr upto);

}  // namespace mytoydb::transaction
