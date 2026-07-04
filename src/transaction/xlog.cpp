// xlog.cpp — Write-Ahead Log (WAL) core buffer management.
//
// Converted from PostgreSQL 15's src/backend/access/transam/xlog.c (simplified).
//
// pgcpp keeps the WAL in an in-memory std::vector<uint8_t>; LSNs are byte
// offsets into it. The first kSizeofXlogRecord bytes are a reserved "page
// header area" so that LSN 0 (kInvalidXLogRecPtr) is never a valid record
// start; the first record begins at LSN kSizeofXlogRecord (24).
//
// When a WAL directory is configured via SetWalDirectory(), WAL records are
// also appended to <dir>/wal.log. The on-disk file contains only WAL records
// (NOT the 24-byte in-memory header), so the file size equals InsertPtr - 24.
// InitializeWal() loads the file back into the buffer so crash recovery works
// across process restarts. XLogFlush() fsyncs the file. When no directory is
// set (test mode), WAL is purely in-memory.
#include "transaction/xlog.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pgcpp::transaction {

namespace {

// The in-memory WAL stream. LSN X maps directly to buffer[X].
std::vector<uint8_t>& WalBuffer() {
    static std::vector<uint8_t> buffer;
    return buffer;
}

XLogRecPtr& InsertPtr() {
    static XLogRecPtr ptr = kSizeofXlogRecord;
    return ptr;
}

XLogRecPtr& WritePtr() {
    static XLogRecPtr ptr = kSizeofXlogRecord;
    return ptr;
}

std::string& WalDirectory() {
    static std::string dir;
    return dir;
}

int& WalFileFd() {
    static int fd = -1;
    return fd;
}

void CloseWalFile() {
    if (WalFileFd() >= 0) {
        close(WalFileFd());
        WalFileFd() = -1;
    }
}

bool WriteAllToFd(int fd, const uint8_t* data, std::size_t len) {
    std::size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        written += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

void SetWalDirectory(const std::string& dir) {
    WalDirectory() = dir;
}

void InitializeWal() {
    CloseWalFile();

    auto& buffer = WalBuffer();
    buffer.clear();
    buffer.resize(kSizeofXlogRecord, 0);  // reserve page-header area (LSN 0..23)
    InsertPtr() = kSizeofXlogRecord;
    WritePtr() = kSizeofXlogRecord;

    if (!WalDirectory().empty()) {
        std::string path = WalDirectory() + "/wal.log";
        // Load existing WAL records from disk (file contains records only,
        // no in-memory header). Append them after the 24-byte header.
        int rfd = open(path.c_str(), O_RDONLY);
        if (rfd >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(rfd, buf, sizeof(buf))) > 0) {
                buffer.insert(buffer.end(), buf, buf + n);
            }
            close(rfd);
            InsertPtr() = buffer.size();
            WritePtr() = InsertPtr();
        }
        // Open for appending (creates the file if it doesn't exist).
        WalFileFd() = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    }
}

void ResetWal() {
    CloseWalFile();

    auto& buffer = WalBuffer();
    buffer.clear();
    buffer.resize(kSizeofXlogRecord, 0);
    InsertPtr() = kSizeofXlogRecord;
    WritePtr() = kSizeofXlogRecord;

    if (!WalDirectory().empty()) {
        std::string path = WalDirectory() + "/wal.log";
        // Truncate the file (clean slate for tests).
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (fd >= 0)
            close(fd);
        // Reopen for appending.
        WalFileFd() = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    }
}

void ShutdownWal() {
    CloseWalFile();
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
        if (WalFileFd() >= 0) {
            WriteAllToFd(WalFileFd(), bytes, len);
        }
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
    if (WalFileFd() >= 0) {
        fsync(WalFileFd());
    }
    WritePtr() = InsertPtr();
}

}  // namespace pgcpp::transaction
