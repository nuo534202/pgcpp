// xlogreader.cpp — WAL record reader for crash recovery and replication.
//
// Converted from PostgreSQL 15's src/backend/access/transam/xlogreader.c
// (simplified: no segment files, no page validation, no CRC verification).
//
// XLogReadRecord reads the record at *start_lsn (or reader->next_lsn if
// start_lsn is nullptr): it reads kSizeofXlogRecord bytes for the header via
// XLogReadRaw, then reads xl_tot_len - kSizeofXlogRecord bytes for the
// payload. Returns false at end-of-WAL.
#include "transaction/xlogreader.hpp"

#include <cstddef>
#include <vector>

#include "transaction/xlog.hpp"

namespace pgcpp::transaction {

XLogReaderState* XLogReaderAlloc() {
    return new XLogReaderState();
}

void XLogReaderFree(XLogReaderState* reader) {
    delete reader;
}

bool XLogReadRecord(XLogReaderState* reader, XLogRecPtr* start_lsn) {
    if (reader == nullptr) {
        return false;
    }

    XLogRecPtr lsn;
    if (start_lsn != nullptr && *start_lsn != kInvalidXLogRecPtr) {
        lsn = *start_lsn;
    } else {
        lsn = reader->next_lsn;
    }

    // Read the record header (kSizeofXlogRecord bytes).
    XLogRecord header{};
    std::size_t got = XLogReadRaw(lsn, &header, kSizeofXlogRecord);
    if (got == 0) {
        // No data at this LSN — end of WAL.
        reader->end_of_wal = true;
        return false;
    }
    if (got < static_cast<std::size_t>(kSizeofXlogRecord) ||
        header.xl_tot_len < static_cast<uint32_t>(kSizeofXlogRecord)) {
        // Truncated header or corrupt length — end of WAL.
        reader->end_of_wal = true;
        return false;
    }

    // Read the payload (xl_tot_len - header_size bytes).
    std::size_t data_len = static_cast<std::size_t>(header.xl_tot_len) - kSizeofXlogRecord;
    reader->main_data.clear();
    if (data_len > 0) {
        reader->main_data.resize(data_len);
        std::size_t got_data =
            XLogReadRaw(lsn + kSizeofXlogRecord, reader->main_data.data(), data_len);
        if (got_data < data_len) {
            // Truncated payload — end of WAL.
            reader->end_of_wal = true;
            return false;
        }
    }

    reader->current_lsn = lsn;
    reader->record = header;
    reader->next_lsn = lsn + header.xl_tot_len;
    if (start_lsn != nullptr) {
        *start_lsn = reader->next_lsn;
    }
    return true;
}

bool XLogReadRecordAt(XLogReaderState* reader, XLogRecPtr lsn) {
    if (reader == nullptr) {
        return false;
    }

    // Read the header.
    XLogRecord header{};
    std::size_t got = XLogReadRaw(lsn, &header, kSizeofXlogRecord);
    if (got < static_cast<std::size_t>(kSizeofXlogRecord) ||
        header.xl_tot_len < static_cast<uint32_t>(kSizeofXlogRecord)) {
        return false;
    }

    // Read the payload.
    std::size_t data_len = static_cast<std::size_t>(header.xl_tot_len) - kSizeofXlogRecord;
    reader->main_data.clear();
    if (data_len > 0) {
        reader->main_data.resize(data_len);
        std::size_t got_data =
            XLogReadRaw(lsn + kSizeofXlogRecord, reader->main_data.data(), data_len);
        if (got_data < data_len) {
            return false;
        }
    }

    reader->current_lsn = lsn;
    reader->record = header;
    // Does not advance next_lsn (per the header: caller decides iteration).
    return true;
}

}  // namespace pgcpp::transaction
