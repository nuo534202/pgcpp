// xloginsert.cpp — WAL record assembly and insertion.
//
// Converted from PostgreSQL 15's src/backend/access/transam/xloginsert.c
// (simplified: no compression of backup blocks, no block references).
//
// XLogBeginInsert starts a record; XLogRegisterData accumulates payload;
// XLogRegisterBuffer registers a page (with optional FPW); XLogInsert
// finalizes the XLogRecord header and writes header + backup blocks + payload
// to the WAL buffer via XLogWriteRaw. xl_prev chains records back to the
// previous record's LSN.
//
// Record layout with FPW:
//   [XLogRecord header (24 bytes)]
//   [backup block 1: block_id(1) + page_len(4) + page_data]
//   [backup block 2: ...]
//   [main data]
#include "transaction/xloginsert.hpp"

#include <cstring>
#include <vector>

#include "transaction/crc32c.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::transaction {

namespace {

// Payload data registered for the record currently being assembled.
std::vector<uint8_t>& RegisteredData() {
    static std::vector<uint8_t> data;
    return data;
}

// Buffers (pages) registered for the current record.
std::vector<RegisteredBlock>& RegisteredBlocks() {
    static std::vector<RegisteredBlock> blocks;
    return blocks;
}

// Flag bits for the current record (PG's XLR flags).
uint8_t& CurrentFlags() {
    static uint8_t flags = 0;
    return flags;
}

// LSN of the previously-inserted record (for xl_prev). kInvalidXLogRecPtr
// means "no previous record" (first record in the WAL).
XLogRecPtr& PrevRecordLsn() {
    static XLogRecPtr lsn = kInvalidXLogRecPtr;
    return lsn;
}

}  // namespace

void XLogBeginInsert() {
    RegisteredData().clear();
    RegisteredBlocks().clear();
    CurrentFlags() = 0;
}

void XLogRegisterData(const void* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    auto& rd = RegisteredData();
    const auto* bytes = static_cast<const uint8_t*>(data);
    rd.insert(rd.end(), bytes, bytes + len);
}

void XLogRegisterBuffer(uint8_t block_id, const void* page_data, std::size_t page_len,
                        bool is_fpw) {
    if (page_data == nullptr || page_len == 0) {
        return;
    }
    auto& blocks = RegisteredBlocks();
    // If this block_id is already registered, replace it.
    for (auto& b : blocks) {
        if (b.block_id == block_id) {
            const auto* bytes = static_cast<const uint8_t*>(page_data);
            b.page_data.assign(bytes, bytes + page_len);
            b.is_fpw = is_fpw;
            return;
        }
    }
    RegisteredBlock block;
    block.block_id = block_id;
    const auto* bytes = static_cast<const uint8_t*>(page_data);
    block.page_data.assign(bytes, bytes + page_len);
    block.is_fpw = is_fpw;
    blocks.push_back(std::move(block));
}

void XLogSetRecordFlags(uint8_t flags) {
    CurrentFlags() |= flags;
}

XLogRecPtr XLogInsert(RmgrId rmid, uint8_t info) {
    auto& rd = RegisteredData();
    auto& blocks = RegisteredBlocks();

    // Determine if any registered buffer has FPW. If so, set the
    // kXlrBkpBlockImage flag in xl_info.
    bool has_fpw = false;
    std::size_t fpw_count = 0;
    for (const auto& b : blocks) {
        if (b.is_fpw) {
            has_fpw = true;
            fpw_count++;
        }
    }

    // Compute the total backup block payload size.
    // Layout: count_byte (1) + N × [block_id (1) + page_len (4) + page_data].
    // The leading count byte lets the reader parse unambiguously without
    // knowing main_data size in advance.
    std::size_t bkp_total = 0;
    if (has_fpw) {
        bkp_total += 1;  // count byte
        for (const auto& b : blocks) {
            if (b.is_fpw) {
                bkp_total += 1 + sizeof(uint32_t) + b.page_data.size();
            }
        }
    }

    // Build the record header. {} value-initializes padding bytes to zero
    // so the written bytes are deterministic (avoids sanitizer warnings).
    XLogRecord rec{};
    rec.xl_tot_len = static_cast<uint32_t>(kSizeofXlogRecord + bkp_total + rd.size());
    TransactionId xid = GetCurrentTransactionIdIfAny();
    rec.xl_xid = (xid != kInvalidTransactionId) ? xid : kBootstrapTransactionId;
    rec.xl_prev = PrevRecordLsn();
    rec.xl_info = info;
    if (has_fpw) {
        rec.xl_info |= kXlrBkpBlockImage;
    }
    rec.xl_rmid = rmid;

    // Compute CRC32C over: header (excluding xl_crc) + backup blocks + main data.
    Crc32C crc;
    constexpr std::size_t kHeaderCrcLen = offsetof(XLogRecord, xl_crc);
    crc.Update(&rec, kHeaderCrcLen);

    // Assemble backup block bytes for writing and CRC.
    // Layout: [count (1)] [block_id (1) + page_len (4) + page_data] × count.
    std::vector<uint8_t> bkp_bytes;
    if (has_fpw) {
        bkp_bytes.push_back(static_cast<uint8_t>(fpw_count));
        for (const auto& b : blocks) {
            if (!b.is_fpw)
                continue;
            bkp_bytes.push_back(b.block_id);
            uint32_t plen = static_cast<uint32_t>(b.page_data.size());
            bkp_bytes.insert(bkp_bytes.end(), reinterpret_cast<const uint8_t*>(&plen),
                             reinterpret_cast<const uint8_t*>(&plen) + sizeof(plen));
            bkp_bytes.insert(bkp_bytes.end(), b.page_data.begin(), b.page_data.end());
        }
        crc.Update(bkp_bytes.data(), bkp_bytes.size());
    }

    // Update CRC with main data.
    if (!rd.empty()) {
        crc.Update(rd.data(), rd.size());
    }
    rec.xl_crc = crc.Finalize();

    // Write the record: header, backup blocks, main data.
    XLogRecPtr start_lsn = XLogWriteRaw(&rec, sizeof(XLogRecord));
    if (has_fpw && !bkp_bytes.empty()) {
        XLogWriteRaw(bkp_bytes.data(), bkp_bytes.size());
    }
    if (!rd.empty()) {
        XLogWriteRaw(rd.data(), rd.size());
    }

    // Remember this record's LSN for the next record's xl_prev.
    PrevRecordLsn() = start_lsn;

    // Clear the accumulators (ready for the next XLogBeginInsert).
    rd.clear();
    blocks.clear();
    CurrentFlags() = 0;

    return start_lsn;
}

void XLogResetInsert() {
    RegisteredData().clear();
    RegisteredBlocks().clear();
    CurrentFlags() = 0;
}

void ResetXlogInsertState() {
    RegisteredData().clear();
    RegisteredBlocks().clear();
    CurrentFlags() = 0;
    PrevRecordLsn() = kInvalidXLogRecPtr;
}

const std::vector<uint8_t>& GetRegisteredData() {
    return RegisteredData();
}

const std::vector<RegisteredBlock>& GetRegisteredBlocks() {
    return RegisteredBlocks();
}

}  // namespace pgcpp::transaction
