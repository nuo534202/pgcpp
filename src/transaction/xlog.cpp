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
#include <cstdio>
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

// ===========================================================================
// WAL Segment File Support (Step 2)
// ===========================================================================

std::string XLogFileName(TimeLineId tli, XLogSegNo segno) {
    char buf[32];
    uint32_t logid = static_cast<uint32_t>(segno / kXLogSegmentsPerXLogId);
    uint32_t seg = static_cast<uint32_t>(segno % kXLogSegmentsPerXLogId);
    std::snprintf(buf, sizeof(buf), "%08X%08X%08X", tli, logid, seg);
    return std::string(buf, 24);
}

XLogRecPtr XLogSegNoOffsetToRecPtr(XLogSegNo segno, uint32_t offset) {
    return static_cast<XLogRecPtr>(segno) * kWalSegmentSize + offset;
}

XLogSegNo RecPtrToXLogSegNo(XLogRecPtr lsn) {
    return static_cast<XLogSegNo>(lsn / kWalSegmentSize);
}

uint32_t RecPtrToSegmentOffset(XLogRecPtr lsn) {
    return static_cast<uint32_t>(lsn % kWalSegmentSize);
}

bool XLogFileInit(const std::string& path) {
    // Create the file (O_EXCL so we don't clobber an existing one). If it
    // already exists, treat as success (caller can re-open with the desired
    // flags).
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        if (errno == EEXIST) {
            return true;  // already exists
        }
        return false;
    }
    // Pre-allocate the segment size with zeros (matches PostgreSQL behavior
    // of pre-allocating segment files for performance).
    if (ftruncate(fd, kWalSegmentSize) != 0) {
        close(fd);
        return false;
    }
    if (fsync(fd) != 0) {
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

int XLogFileOpen(const std::string& dir, TimeLineId tli, XLogSegNo segno, int flags, int mode) {
    std::string path = dir + "/" + XLogFileName(tli, segno);
    return open(path.c_str(), flags, mode);
}

bool InstallXLogFileSegment(const std::string& dir, TimeLineId tli, XLogSegNo segno) {
    std::string path = dir + "/" + XLogFileName(tli, segno);
    // Fast path: if the file already exists, nothing to do.
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return true;
    }
    return XLogFileInit(path);
}

bool XLogFileCopy(const std::string& dst, const std::string& src) {
    int sfd = open(src.c_str(), O_RDONLY);
    if (sfd < 0) {
        return false;
    }
    int dfd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (dfd < 0) {
        close(sfd);
        return false;
    }
    char buf[8192];
    ssize_t n;
    bool ok = true;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dfd, buf + off, n - off);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok)
            break;
    }
    if (n < 0)
        ok = false;
    fsync(dfd);
    close(dfd);
    close(sfd);
    return ok;
}

// --- WalSegmentWriter ---

WalSegmentWriter::WalSegmentWriter(std::string dir, TimeLineId tli, uint32_t segment_size)
    : dir_(std::move(dir)), tli_(tli), segment_size_(segment_size) {}

WalSegmentWriter::~WalSegmentWriter() {
    Close();
}

bool WalSegmentWriter::EnsureSegmentOpen(XLogSegNo segno) {
    if (fd_ >= 0 && current_segno_ == segno) {
        return true;
    }
    // Close the previous segment if one is open.
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    // Install (create if missing) the segment file, then open it for writing.
    if (!InstallXLogFileSegment(dir_, tli_, segno)) {
        return false;
    }
    std::string path = dir_ + "/" + XLogFileName(tli_, segno);
    fd_ = open(path.c_str(), O_WRONLY, S_IRUSR | S_IWUSR);
    if (fd_ < 0) {
        return false;
    }
    current_segno_ = segno;
    return true;
}

std::size_t WalSegmentWriter::Write(XLogRecPtr lsn, const void* data, std::size_t len) {
    if (len == 0)
        return 0;
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::size_t written = 0;
    while (written < len) {
        // Map the current LSN to (segno, offset) using the writer's segment
        // size (which may differ from kWalSegmentSize in tests).
        XLogRecPtr cur = lsn + written;
        XLogSegNo segno = static_cast<XLogSegNo>(cur / segment_size_);
        uint32_t off = static_cast<uint32_t>(cur % segment_size_);
        std::size_t space_in_seg = segment_size_ - off;
        std::size_t chunk = len - written;
        if (chunk > space_in_seg) {
            chunk = space_in_seg;
        }
        if (!EnsureSegmentOpen(segno)) {
            return written;
        }
        // Seek to the offset within the segment and write the chunk.
        if (lseek(fd_, static_cast<off_t>(off), SEEK_SET) < 0) {
            return written;
        }
        std::size_t chunk_written = 0;
        while (chunk_written < chunk) {
            ssize_t n = write(fd_, bytes + written + chunk_written, chunk - chunk_written);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return written + chunk_written;
            }
            if (n == 0)
                return written + chunk_written;
            chunk_written += static_cast<std::size_t>(n);
        }
        written += chunk;
    }
    return written;
}

void WalSegmentWriter::Flush() {
    if (fd_ >= 0) {
        fsync(fd_);
    }
}

void WalSegmentWriter::Close() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

}  // namespace pgcpp::transaction
