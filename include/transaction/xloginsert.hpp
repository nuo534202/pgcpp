// xloginsert.h — WAL record assembly and insertion API.
//
// Converted from PostgreSQL 15's src/include/access/xloginsert.h.
//
// PostgreSQL assembles each WAL record from multiple registered buffers and
// data chunks, then writes it atomically into the WAL buffer. pgcpp
// preserves the BeginInsert/RegisterData/Insert API but simplifies the
// internal assembly (no full-page images, no backup blocks).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "transaction/transam.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::transaction {

// XLogRegisterFlags — flag bits for the current record (PG's XLR flags).
constexpr uint8_t kXlrRelMgrInfo = 0x01;  // RMGR-specific info present

// XLogBeginInsert — start assembling a new WAL record. Resets the
// registered-data accumulator. Only one record can be assembled at a time.
void XLogBeginInsert();

// XLogRegisterData — append `len` bytes of payload data to the current
// record. Called between XLogBeginInsert and XLogInsert (may be called
// multiple times). The data is copied internally.
void XLogRegisterData(const void* data, std::size_t len);

// XLogSetRecordFlags — set additional flag bits for the current record.
void XLogSetRecordFlags(uint8_t flags);

// XLogInsert — finalize the current record, write it to the WAL buffer,
// and return the LSN of the record start. The record header is filled with
// the given rmid and info, plus the current transaction's XID.
//
// After XLogInsert, the registered data is cleared (no need to call Reset).
XLogRecPtr XLogInsert(RmgrId rmid, uint8_t info);

// XLogResetInsert — discard any partially-assembled record (aborts the
// current XLogBeginInsert without writing).
void XLogResetInsert();

// ResetXlogInsertState — reset all XLogInsert internal state (registered
// data, flags, and the prev-record LSN). Called by tests between runs.
void ResetXlogInsertState();

// --- Internal: the assembled record buffer (for XLogReader/recovery) ---
//
// GetRegisteredData — returns the data registered for the most recent
// record (used by tests to verify round-trip). Returns nullptr and 0 if
// no record is being assembled.
const std::vector<uint8_t>& GetRegisteredData();

}  // namespace pgcpp::transaction
