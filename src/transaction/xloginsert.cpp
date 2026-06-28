// xloginsert.cpp — WAL record assembly and insertion.
//
// Converted from PostgreSQL 15's src/backend/access/transam/xloginsert.c
// (simplified: no full-page images, no backup blocks).
//
// XLogBeginInsert starts a record; XLogRegisterData accumulates payload;
// XLogInsert finalizes the XLogRecord header (xl_tot_len, xl_xid, xl_prev,
// xl_info, xl_rmid) and writes header+payload to the WAL buffer via
// XLogWriteRaw. xl_prev chains records back to the previous record's LSN.
#include "pgcpp/transaction/xloginsert.hpp"

#include <vector>

#include "pgcpp/transaction/transam.hpp"
#include "pgcpp/transaction/xact.hpp"
#include "pgcpp/transaction/xlog.hpp"

namespace pgcpp::transaction {

namespace {

// Payload data registered for the record currently being assembled.
std::vector<uint8_t>& RegisteredData() {
    static std::vector<uint8_t> data;
    return data;
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

void XLogSetRecordFlags(uint8_t flags) {
    CurrentFlags() |= flags;
}

XLogRecPtr XLogInsert(RmgrId rmid, uint8_t info) {
    auto& rd = RegisteredData();

    // Build the record header. {} value-initializes padding bytes to zero
    // so the written bytes are deterministic (avoids sanitizer warnings).
    XLogRecord rec{};
    rec.xl_tot_len = static_cast<uint32_t>(kSizeofXlogRecord + rd.size());
    TransactionId xid = GetCurrentTransactionIdIfAny();
    rec.xl_xid = (xid != kInvalidTransactionId) ? xid : kBootstrapTransactionId;
    rec.xl_prev = PrevRecordLsn();
    rec.xl_info = info;
    rec.xl_rmid = rmid;
    rec.xl_crc = 0;  // simplified: CRC not verified

    // Write the header, then the payload, to the WAL buffer.
    XLogRecPtr start_lsn = XLogWriteRaw(&rec, sizeof(XLogRecord));
    if (!rd.empty()) {
        XLogWriteRaw(rd.data(), rd.size());
    }

    // Remember this record's LSN for the next record's xl_prev.
    PrevRecordLsn() = start_lsn;

    // Clear the accumulator (ready for the next XLogBeginInsert).
    rd.clear();
    CurrentFlags() = 0;

    return start_lsn;
}

void XLogResetInsert() {
    RegisteredData().clear();
    CurrentFlags() = 0;
}

void ResetXlogInsertState() {
    RegisteredData().clear();
    CurrentFlags() = 0;
    PrevRecordLsn() = kInvalidXLogRecPtr;
}

const std::vector<uint8_t>& GetRegisteredData() {
    return RegisteredData();
}

}  // namespace pgcpp::transaction
