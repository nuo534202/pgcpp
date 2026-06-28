// xlog.cpp — Write-Ahead Log (WAL) core buffer management.
//
// Converted from PostgreSQL 15's src/backend/access/transam/xlog.c (simplified).
//
// pgcpp keeps the WAL in an in-memory std::vector<uint8_t>; LSNs are byte
// offsets into it. The first kSizeofXlogRecord bytes are a reserved "page
// header area" so that LSN 0 (kInvalidXLogRecPtr) is never a valid record
// start; the first record begins at LSN kSizeofXlogRecord (24). All inserted
// records are immediately durable (the write pointer tracks the insert
// pointer), so XLogFlush is a no-op.
#include "pgcpp/transaction/xlog.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

namespace pgcpp::transaction {

namespace {

// The in-memory WAL stream. LSN X maps directly to buffer[X].
std::vector<uint8_t>& WalBuffer() {
    static std::vector<uint8_t> buffer;
    return buffer;
}

// The next write position (LSN where the next record will be appended).
XLogRecPtr& InsertPtr() {
    static XLogRecPtr ptr = kSizeofXlogRecord;
    return ptr;
}

// The flush position. In pgcpp's in-memory model, this always equals the
// insert position (writes are immediately durable).
XLogRecPtr& WritePtr() {
    static XLogRecPtr ptr = kSizeofXlogRecord;
    return ptr;
}

}  // namespace

void InitializeWal() {
    auto& buffer = WalBuffer();
    buffer.clear();
    buffer.resize(kSizeofXlogRecord, 0);  // reserve page-header area (LSN 0..23)
    InsertPtr() = kSizeofXlogRecord;
    WritePtr() = kSizeofXlogRecord;
}

void ResetWal() {
    InitializeWal();
}

XLogRecPtr GetXLogInsertRecPtr() {
    return InsertPtr();
}

XLogRecPtr GetXLogWriteRecPtr() {
    return WritePtr();
}

XLogRecPtr XLogWriteRaw(const void* data, std::size_t len) {
    auto& buffer = WalBuffer();
    XLogRecPtr start_lsn = InsertPtr();
    if (len > 0) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        buffer.insert(buffer.end(), bytes, bytes + len);
        InsertPtr() += len;
        WritePtr() = InsertPtr();  // immediately durable
    }
    return start_lsn;
}

std::size_t XLogReadRaw(XLogRecPtr lsn, void* out, std::size_t len) {
    auto& buffer = WalBuffer();
    if (lsn >= buffer.size() || len == 0) {
        return 0;
    }
    std::size_t available = buffer.size() - lsn;
    std::size_t to_read = (len < available) ? len : available;
    std::memcpy(out, buffer.data() + lsn, to_read);
    return to_read;
}

std::size_t GetWalBufferSize() {
    return WalBuffer().size();
}

const std::vector<uint8_t>& GetWalBuffer() {
    return WalBuffer();
}

void XLogFlush(XLogRecPtr /*upto*/) {
    // No-op: the in-memory buffer is always durable. Keep write_ptr in sync.
    WritePtr() = InsertPtr();
}

}  // namespace pgcpp::transaction
