// xloginsert.h — WAL record assembly and insertion API.
//
// Converted from PostgreSQL 15's src/include/access/xloginsert.h.
//
// PostgreSQL assembles each WAL record from multiple registered buffers and
// data chunks, then writes it atomically into the WAL buffer. pgcpp
// preserves the BeginInsert/RegisterData/Insert API. Full-page images (FPW)
// are supported via XLogRegisterBuffer with the is_fpw flag.
//
// Record layout with FPW:
//   [XLogRecord header (24 bytes)]
//   [bkp_count (1)] [backup block 1: block_id(1) + page_len(4) + page_data]
//                   [backup block 2: ...]
//   [main data]
// The kXlrBkpBlockImage bit in xl_info indicates backup blocks are present.
// The leading bkp_count byte lets the reader parse blocks unambiguously.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "transaction/transam.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::transaction {

// XLogRegisterFlags — flag bits for the current record (PG's XLR flags).
constexpr uint8_t kXlrRelMgrInfo = 0x01;        // RMGR-specific info present
constexpr uint8_t kXlrBkpBlockImage = 0x04;     // backup block images present

// RegisteredBlock — a buffer (page) registered for the current record.
// If is_fpw is true, the full page image is included in the WAL record
// (Full-Page Write). This happens when a page is modified for the first
// time after the last checkpoint.
struct RegisteredBlock {
    uint8_t block_id = 0;
    std::vector<uint8_t> page_data;
    bool is_fpw = false;
};

// BackupBlock — a parsed backup block image in a WAL record (read-side
// counterpart of RegisteredBlock). Used by recovery to restore full pages.
struct BackupBlock {
    uint8_t block_id = 0;
    std::vector<uint8_t> page_data;
};

// XLogBeginInsert — start assembling a new WAL record. Resets the
// registered-data accumulator. Only one record can be assembled at a time.
void XLogBeginInsert();

// XLogRegisterData — append `len` bytes of payload data to the current
// record. Called between XLogBeginInsert and XLogInsert (may be called
// multiple times). The data is copied internally.
void XLogRegisterData(const void* data, std::size_t len);

// XLogRegisterBuffer — register a buffer (page) for the current record.
// If `is_fpw` is true, the full page image (`page_data` of `page_len` bytes)
// is included in the WAL record as a backup block. `block_id` identifies
// the buffer within the record (0-based). May be called multiple times
// between XLogBeginInsert and XLogInsert for different block_ids.
void XLogRegisterBuffer(uint8_t block_id, const void* page_data,
                        std::size_t page_len, bool is_fpw);

// XLogSetRecordFlags — set additional flag bits for the current record.
void XLogSetRecordFlags(uint8_t flags);

// XLogInsert — finalize the current record, write it to the WAL buffer,
// and return the LSN of the record start. The record header is filled with
// the given rmid and info, plus the current transaction's XID. If any
// registered buffers have is_fpw=true, the kXlrBkpBlockImage flag is OR'd
// into xl_info and the backup block images are inserted between the header
// and the main data.
//
// After XLogInsert, the registered data and buffers are cleared.
XLogRecPtr XLogInsert(RmgrId rmid, uint8_t info);

// XLogResetInsert — discard any partially-assembled record (aborts the
// current XLogBeginInsert without writing).
void XLogResetInsert();

// ResetXlogInsertState — reset all XLogInsert internal state (registered
// data, buffers, flags, and the prev-record LSN). Called by tests between runs.
void ResetXlogInsertState();

// --- Internal: the assembled record buffer (for XLogReader/recovery) ---
//
// GetRegisteredData — returns the data registered for the most recent
// record (used by tests to verify round-trip).
const std::vector<uint8_t>& GetRegisteredData();

// GetRegisteredBlocks — returns the buffers registered for the most recent
// record (used by tests to verify FPW content).
const std::vector<RegisteredBlock>& GetRegisteredBlocks();

}  // namespace pgcpp::transaction
