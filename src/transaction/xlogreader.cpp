// xlogreader.cpp — WAL record reader for crash recovery and replication.
//
// Converted from PostgreSQL 15's src/backend/access/transam/xlogreader.c
// (simplified: no segment files, no page validation).
//
// XLogReadRecord reads the record at *start_lsn (or reader->next_lsn if
// start_lsn is nullptr): it reads kSizeofXlogRecord bytes for the header via
// XLogReadRaw, then reads xl_tot_len - kSizeofXlogRecord bytes for the
// payload. After reading, it verifies the CRC32C of the record (header fields
// excluding xl_crc + payload) and reports a crc_mismatch if verification
// fails. Returns false at end-of-WAL or on CRC error.
//
// If the record's xl_info has kXlrBkpBlockImage set, the payload begins with
// a 1-byte backup-block count followed by that many backup blocks (each:
// block_id(1) + page_len(4) + page_data). These are parsed into
// reader->backup_blocks; reader->main_data holds the remaining payload bytes.
#include "transaction/xlogreader.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

#include "transaction/crc32c.hpp"
#include "transaction/xlog.hpp"
#include "transaction/xloginsert.hpp"  // kXlrBkpBlockImage, BackupBlock

namespace pgcpp::transaction {

namespace {

// Verify the CRC32C of a WAL record.
// CRC covers: header bytes [0, offsetof(xl_crc)) + payload.
// Returns true if the CRC matches, false otherwise.
bool VerifyRecordCrc(const XLogRecord& header, const uint8_t* payload, std::size_t payload_len) {
    Crc32C crc;
    constexpr std::size_t kHeaderCrcLen = offsetof(XLogRecord, xl_crc);
    crc.Update(&header, kHeaderCrcLen);
    if (payload_len > 0) {
        crc.Update(payload, payload_len);
    }
    return crc.Finalize() == header.xl_crc;
}

// Parse backup blocks from the start of `payload` (length `payload_len`).
// Layout: [count (1)] [block_id (1) + page_len (4) + page_data] × count.
// On success, returns true and fills `out_blocks` and `out_main_offset`
// (byte offset where main_data begins). On malformed input, returns false.
bool ParseBackupBlocks(const uint8_t* payload, std::size_t payload_len,
                       std::vector<BackupBlock>& out_blocks, std::size_t& out_main_offset) {
    out_blocks.clear();
    out_main_offset = 0;
    if (payload_len < 1) {
        return false;
    }
    uint8_t count = payload[0];
    std::size_t off = 1;
    out_blocks.reserve(count);
    for (uint8_t i = 0; i < count; i++) {
        // Need at least block_id (1) + page_len (4).
        if (off + 1 + sizeof(uint32_t) > payload_len) {
            out_blocks.clear();
            return false;
        }
        BackupBlock bb;
        bb.block_id = payload[off];
        off += 1;
        uint32_t plen;
        std::memcpy(&plen, payload + off, sizeof(plen));
        off += sizeof(plen);
        if (off + plen > payload_len) {
            out_blocks.clear();
            return false;
        }
        bb.page_data.assign(payload + off, payload + off + plen);
        off += plen;
        out_blocks.push_back(std::move(bb));
    }
    out_main_offset = off;
    return true;
}

// Read header + payload, verify CRC, and split payload into backup_blocks
// (if FPW flag set) and main_data. Sets reader->record/current_lsn on success.
// Sets reader->crc_mismatch/bad_lsn on CRC or parse failure.
// Returns true on success, false on any failure (truncation, CRC, malformed).
// Does NOT set reader->end_of_wal — caller decides based on context.
bool ReadAndDecodeRecord(XLogReaderState* reader, XLogRecPtr lsn) {
    // Read the record header (kSizeofXlogRecord bytes).
    XLogRecord header{};
    std::size_t got = XLogReadRaw(lsn, &header, kSizeofXlogRecord);
    if (got < static_cast<std::size_t>(kSizeofXlogRecord) ||
        header.xl_tot_len < static_cast<uint32_t>(kSizeofXlogRecord)) {
        return false;
    }

    // Read the payload (xl_tot_len - header_size bytes).
    std::size_t data_len = static_cast<std::size_t>(header.xl_tot_len) - kSizeofXlogRecord;
    std::vector<uint8_t> payload;
    if (data_len > 0) {
        payload.resize(data_len);
        std::size_t got_data = XLogReadRaw(lsn + kSizeofXlogRecord, payload.data(), data_len);
        if (got_data < data_len) {
            return false;
        }
    }

    // Verify CRC32C over header + full payload (backup blocks + main data).
    if (!VerifyRecordCrc(header, payload.data(), data_len)) {
        reader->crc_mismatch = true;
        reader->bad_lsn = lsn;
        return false;
    }

    // Split payload into backup_blocks and main_data.
    reader->backup_blocks.clear();
    reader->main_data.clear();
    std::size_t main_offset = 0;
    if ((header.xl_info & kXlrBkpBlockImage) && data_len > 0) {
        if (!ParseBackupBlocks(payload.data(), data_len, reader->backup_blocks, main_offset)) {
            // Malformed backup block section — treat as corrupt.
            reader->crc_mismatch = true;
            reader->bad_lsn = lsn;
            return false;
        }
    }
    // main_data = payload bytes after backup blocks (or whole payload if no FPW).
    if (main_offset < data_len) {
        reader->main_data.assign(payload.data() + main_offset, payload.data() + data_len);
    }

    reader->current_lsn = lsn;
    reader->record = header;
    return true;
}

}  // namespace

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

    reader->crc_mismatch = false;
    reader->bad_lsn = kInvalidXLogRecPtr;

    XLogRecPtr lsn;
    if (start_lsn != nullptr && *start_lsn != kInvalidXLogRecPtr) {
        lsn = *start_lsn;
    } else {
        lsn = reader->next_lsn;
    }

    // Peek 0 bytes at this LSN to detect end-of-WAL before full decode.
    // We do a 0-byte header read: if XLogReadRaw returns 0, there's no data.
    XLogRecord probe{};
    std::size_t got = XLogReadRaw(lsn, &probe, kSizeofXlogRecord);
    if (got == 0) {
        reader->end_of_wal = true;
        return false;
    }

    if (!ReadAndDecodeRecord(reader, lsn)) {
        // Truncation, CRC error, or malformed backup blocks → end of WAL.
        reader->end_of_wal = true;
        return false;
    }

    reader->next_lsn = lsn + reader->record.xl_tot_len;
    if (start_lsn != nullptr) {
        *start_lsn = reader->next_lsn;
    }
    return true;
}

bool XLogReadRecordAt(XLogReaderState* reader, XLogRecPtr lsn) {
    if (reader == nullptr) {
        return false;
    }

    reader->crc_mismatch = false;
    reader->bad_lsn = kInvalidXLogRecPtr;

    // ReadAndDecodeRecord sets crc_mismatch/bad_lsn on failure.
    // Does not advance next_lsn (per the header: caller decides iteration).
    return ReadAndDecodeRecord(reader, lsn);
}

}  // namespace pgcpp::transaction
