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
// In PostgreSQL, WAL is stored in pg_wal/ as a series of segment files. pgcpp
// stores WAL in a single append-only file (<data_dir>/pg_wal/wal.log) backed
// by an in-memory buffer for fast reads during recovery. When no WAL directory
// is configured (test mode), WAL is purely in-memory.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "transaction/transam.hpp"

namespace pgcpp::transaction {

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
// pgcpp's in-memory buffer is much smaller, but this constant is kept for
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
// and flushed to disk by the WAL writer or at commit. pgcpp uses an in-memory
// vector as the "WAL stream" (LSNs are byte offsets into it) that is also
// mirrored to <wal_dir>/wal.log when a WAL directory is configured.

// Set the WAL directory (e.g. <data_dir>/pg_wal). When set, InitializeWal
// loads existing WAL from <dir>/wal.log and XLogWriteRaw appends to it.
// When empty (the default), WAL is purely in-memory (test mode).
void SetWalDirectory(const std::string& dir);

// Initialize the WAL subsystem. Resets the in-memory buffer. If a WAL
// directory has been set, loads existing WAL from <dir>/wal.log into the
// buffer (so crash recovery works across process restarts) and opens the
// file for appending. Must be called once at startup before any XLogInsert.
void InitializeWal();

// Reset the WAL to an empty state (for testing). Closes any open WAL file,
// clears the in-memory buffer, and truncates the on-disk file if a directory
// is configured.
void ResetWal();

// Shut down the WAL subsystem (close the WAL file if open). Called at server
// shutdown.
void ShutdownWal();

// Get the current insert position (the LSN where the next record will go).
XLogRecPtr GetXLogInsertRecPtr();

// Get the current flush position (the LSN up to which WAL is durable).
XLogRecPtr GetXLogWriteRecPtr();

// Write raw bytes into the WAL buffer at the current insert position.
// Returns the LSN of the start of the written data. Advances the insert
// pointer. Called by XLogInsert after assembling a record. If a WAL
// directory is configured, also appends to <dir>/wal.log.
XLogRecPtr XLogWriteRaw(const void* data, std::size_t len);

// Read raw bytes from the WAL buffer at a given LSN.
// Returns the number of bytes actually read (may be less than len if the
// LSN is near the end). Used by XLogReader.
std::size_t XLogReadRaw(XLogRecPtr lsn, void* buffer, std::size_t len);

// Get the total size of the WAL buffer (in bytes).
std::size_t GetWalBufferSize();

// Get a read-only pointer to the WAL buffer (for testing/diagnostics).
const std::vector<uint8_t>& GetWalBuffer();

// XLogFlush — flush WAL up to the given LSN. If a WAL directory is
// configured, fsyncs the WAL file so the records up to `upto` survive a
// crash. If no directory is configured (test mode), this is a no-op.
void XLogFlush(XLogRecPtr upto);

// ===========================================================================
// WAL Segment File Support (Step 2)
//
// PostgreSQL stores WAL in pg_wal/ as a series of segment files, each named
// "<TLI:8><LogId:8><SegNo:8>" (24 hex chars), e.g. "000000010000000000000001".
// The LogId is segno / 256 and the trailing field is segno % 256 (for the
// default 16MB segment size, 256 segments per XLogId).
//
// pgcpp keeps the legacy single-file `wal.log` path as default. The segment
// file API below is provided as a standalone utility; integration into
// XLogWriteRaw is gated by the PGCPP_USE_WAL_SEGMENTS compile macro.
// ===========================================================================

// TimeLineId — identifies a timeline in the WAL stream (1 = initial timeline).
using TimeLineId = uint32_t;

// Default timeline ID for a freshly initialized cluster.
constexpr TimeLineId kDefaultTimelineId = 1;

// Number of WAL segments per XLogId (the middle 8 hex digits of the filename).
// For 16MB segments: 0x100000000 / 16MB = 256.
constexpr uint32_t kXLogSegmentsPerXLogId = 0x100000000u / kWalSegmentSize;

// Format a segment file name: "%08X%08X%08X" (TLI, LogId, SegNo-in-Id).
// Example: XLogFileName(1, 1) -> "000000010000000000000001"
std::string XLogFileName(TimeLineId tli, XLogSegNo segno);

// Convert (segno, offset) to an LSN. segno is 0-based: segno 0 covers
// LSN [0, kWalSegmentSize). offset is the byte position within the segment.
XLogRecPtr XLogSegNoOffsetToRecPtr(XLogSegNo segno, uint32_t offset);

// Convert an LSN to its segment number (0-based).
XLogSegNo RecPtrToXLogSegNo(XLogRecPtr lsn);

// Convert an LSN to the byte offset within its segment.
uint32_t RecPtrToSegmentOffset(XLogRecPtr lsn);

// Initialize a new WAL segment file at `path`. Creates the file with
// kWalSegmentSize bytes of zeros (pre-allocated). Returns true on success,
// false on I/O error.
bool XLogFileInit(const std::string& path);

// Open a WAL segment file by (tli, segno) under directory `dir`.
// The file path is "<dir>/XLogFileName(tli, segno)". Returns the fd or -1
// on error. `flags` and `mode` are passed to open(2).
int XLogFileOpen(const std::string& dir, TimeLineId tli, XLogSegNo segno, int flags, int mode);

// Install (create if missing) a WAL segment file in `<dir>` with the standard
// segment name. If the file already exists, this is a no-op. Returns true
// if the file exists (created or pre-existing) on success.
bool InstallXLogFileSegment(const std::string& dir, TimeLineId tli, XLogSegNo segno);

// Copy a WAL segment file from `src` to `dst` (for archive/restore).
// Returns true on success.
bool XLogFileCopy(const std::string& dst, const std::string& src);

// WalSegmentWriter — writes a logical WAL byte stream into segment files,
// switching to the next segment when the current one is full.
//
// Usage:
//   WalSegmentWriter writer("/tmp/pg_wal", kDefaultTimelineId, seg_size);
//   writer.Write(lsn, data, len);   // auto-switches segments
//   writer.Flush();                 // fsync current segment
//   writer.Close();                 // close current fd
//
// The writer does NOT buffer writes — each Write() call issues write(2)
// directly to the current segment file at the correct offset. LSN to
// (segno, offset) mapping uses RecPtrToXLogSegNo / RecPtrToSegmentOffset.
class WalSegmentWriter {
public:
    // Construct a writer for directory `dir` and timeline `tli`.
    // `segment_size` defaults to kWalSegmentSize; tests may pass a smaller
    // value to force segment switching without writing 16MB.
    WalSegmentWriter(std::string dir, TimeLineId tli, uint32_t segment_size = kWalSegmentSize);
    ~WalSegmentWriter();

    WalSegmentWriter(const WalSegmentWriter&) = delete;
    WalSegmentWriter& operator=(const WalSegmentWriter&) = delete;

    // Write `len` bytes starting at LSN `lsn`. If the write spans a segment
    // boundary, automatically closes the current segment and opens the next
    // one. Returns the number of bytes written (== len on success).
    std::size_t Write(XLogRecPtr lsn, const void* data, std::size_t len);

    // fsync the currently open segment file (if any).
    void Flush();

    // Close the currently open segment file (if any). Called automatically
    // by the destructor.
    void Close();

    // Get the segment number of the currently open file (0 if none open).
    XLogSegNo GetCurrentSegNo() const { return current_segno_; }

private:
    bool EnsureSegmentOpen(XLogSegNo segno);

    std::string dir_;
    TimeLineId tli_;
    uint32_t segment_size_;
    int fd_ = -1;
    XLogSegNo current_segno_ = 0;
};

}  // namespace pgcpp::transaction
