// xlogreader.h — WAL record reader for crash recovery and replication.
//
// Converted from PostgreSQL 15's src/include/access/xlogreader.h.
//
// XLogReaderState iterates over WAL records starting from a given LSN,
// decoding each XLogRecord header + payload. Used by crash recovery to
// replay records one at a time. If a record carries backup blocks (FPW),
// they are parsed into backup_blocks; main_data holds the remaining payload.
#pragma once

#include <cstdint>
#include <vector>

#include "transaction/transam.hpp"
#include "transaction/xlog.hpp"
#include "transaction/xloginsert.hpp"  // for BackupBlock, kXlrBkpBlockImage

namespace pgcpp::transaction {

// XLogReaderState — iterator over WAL records.
//
// Usage:
//   auto* reader = XLogReaderAlloc();
//   XLogRecPtr lsn = kFirstRecordLsn;
//   while (XLogReadRecord(reader, &lsn)) {
//       XLogRecord* rec = &reader->record;
//       // inspect rec->xl_rmid, rec->xl_info, reader->main_data, ...
//       // if (rec->xl_info & kXlrBkpBlockImage) { reader->backup_blocks; ... }
//   }
//   XLogReaderFree(reader);
struct XLogReaderState {
    XLogRecPtr current_lsn = kInvalidXLogRecPtr;  // LSN of current record
    XLogRecPtr next_lsn = kInvalidXLogRecPtr;     // LSN where to read next
    XLogRecord record;                            // decoded header
    std::vector<uint8_t> main_data;               // record payload (excl. backup blocks)
    std::vector<BackupBlock> backup_blocks;       // parsed FPW backup blocks

    bool end_of_wal = false;      // true when we've reached the end of WAL
    bool crc_mismatch = false;    // true if the last record failed CRC check
    XLogRecPtr bad_lsn = kInvalidXLogRecPtr;  // LSN of the corrupt record
};

// XLogReaderAlloc — allocate a new reader state (in the current memory context).
XLogReaderState* XLogReaderAlloc();

// XLogReaderFree — release a reader state.
void XLogReaderFree(XLogReaderState* reader);

// XLogReadRecord — read the record at `start_lsn` (or the next record after
// the previous one if start_lsn is nullptr). Returns true if a record was
// read, false at end-of-WAL. On success, updates reader->record (the header)
// and reader->main_data (the payload), and sets *start_lsn to the next record.
bool XLogReadRecord(XLogReaderState* reader, XLogRecPtr* start_lsn);

// XLogReadRecordAt — convenience: read the record at the given LSN.
// Returns true if a complete record exists there. Sets reader->record and
// reader->main_data. Does not advance next_lsn (caller decides iteration).
bool XLogReadRecordAt(XLogReaderState* reader, XLogRecPtr lsn);

}  // namespace pgcpp::transaction
