// md.cpp — Magnetic disk storage manager.
//
// Converted from PostgreSQL 15's src/backend/storage/smgr/md.c.
//
// Implements the on-disk file layout for relations. Each relation fork is
// stored as one or more segment files, each holding RELSEG_SIZE blocks
// (1 GB by default). This segmentation avoids exceeding filesystem file
// size limits.
//
// File path format:
//   <base_dir>/base/<dboid>/<relfilenode>[_forkname].<segno>
//
// The main fork has no forkname suffix. Segment 0 has no segment suffix
// for backward compatibility with PostgreSQL's layout.
//
// All I/O is synchronous (MyToyDB is single-process). Errors are reported
// via ereport(ERROR), which longjmps to the nearest PG_CATCH handler.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "mytoydb/common/error/elog.h"
#include "mytoydb/storage/smgr.h"

namespace mytoydb::storage {

namespace {

// Build the full filesystem path for a segment file.
std::string BuildFullPath(const std::string& base_dir, const RelFileNodeBackend& rnode,
                          ForkNumber fork_num, int segno) {
    std::string path = base_dir;
    path += '/';
    path += relpathbackend(rnode, fork_num);

    // Append segment number suffix (PostgreSQL uses ".N" for N > 0).
    if (segno > 0) {
        path += '.';
        path += std::to_string(segno);
    }
    return path;
}

// Ensure the parent directory exists (creates it if needed).
void EnsureParentDir(const std::string& file_path) {
    // Find the last '/' and create the directory.
    std::size_t pos = file_path.find_last_of('/');
    if (pos == std::string::npos || pos == 0)
        return;

    std::string dir = file_path.substr(0, pos);

    // Create intermediate directories (mkdir -p).
    // Walk the path components and create each directory.
    std::size_t start = 0;
    while (start < dir.size()) {
        std::size_t next = dir.find('/', start + 1);
        if (next == std::string::npos)
            next = dir.size();
        std::string component = dir.substr(0, next);

        // Try to create; ignore EEXIST.
        if (mkdir(component.c_str(), 0755) != 0 && errno != EEXIST) {
            // If the directory already exists, that's fine.
            struct stat st;
            if (stat(component.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                ereport(mytoydb::error::LogLevel::kError,
                        "could not create directory " + component + ": " + std::strerror(errno));
            }
        }
        start = next;
    }
}

// Open a file for reading/writing, creating if requested.
int OpenFile(const std::string& path, bool create) {
    int flags = O_RDWR;
    if (create)
        flags |= O_CREAT;

    int fd = open(path.c_str(), flags, 0600);
    if (fd < 0) {
        ereport(mytoydb::error::LogLevel::kError,
                "could not open file " + path + ": " + std::strerror(errno));
    }
    return fd;
}

// Read exactly `count` bytes at `offset` from the file descriptor.
// Errors if the read is short or fails.
void FileReadBlock(int fd, off_t offset, char* buffer) {
    if (lseek(fd, offset, SEEK_SET) < 0) {
        ereport(mytoydb::error::LogLevel::kError,
                std::string("lseek failed: ") + std::strerror(errno));
    }
    ssize_t n = read(fd, buffer, kBlckSz);
    if (n < 0) {
        ereport(mytoydb::error::LogLevel::kError,
                std::string("read failed: ") + std::strerror(errno));
    }
    if (n < kBlckSz) {
        // Short read: this means the block doesn't exist yet.
        // PostgreSQL treats this as an error for mdread (block should exist).
        ereport(mytoydb::error::LogLevel::kError, "could not read block: unexpected end of file");
    }
}

// Write exactly BLCKSZ bytes at `offset` to the file descriptor.
void FileWriteBlock(int fd, off_t offset, const char* buffer) {
    if (lseek(fd, offset, SEEK_SET) < 0) {
        ereport(mytoydb::error::LogLevel::kError,
                std::string("lseek failed: ") + std::strerror(errno));
    }
    ssize_t n = write(fd, buffer, kBlckSz);
    if (n < 0) {
        ereport(mytoydb::error::LogLevel::kError,
                std::string("write failed: ") + std::strerror(errno));
    }
    if (n < kBlckSz) {
        ereport(mytoydb::error::LogLevel::kError, "could not write block: short write");
    }
}

}  // namespace

// --- SmgrRelationData methods (md.c implementation) ---

std::string SmgrRelationData::mdFilePath(ForkNumber fork_num, int segno) const {
    return BuildFullPath(GetStorageBaseDir(), smgr_rnode, fork_num, segno);
}

void SmgrRelationData::mdEnsureSegments(ForkNumber fork_num, int segno) {
    auto& fds = md_fd[static_cast<int>(fork_num)];
    if (static_cast<int>(fds.size()) <= segno) {
        fds.resize(segno + 1);
    }
}

int SmgrRelationData::mdGetFd(ForkNumber fork_num, int segno, bool create) {
    mdEnsureSegments(fork_num, segno);
    auto& fds = md_fd[static_cast<int>(fork_num)];

    if (segno < static_cast<int>(fds.size()) && fds[segno].fd >= 0) {
        return fds[segno].fd;
    }

    // Need to open the segment.
    std::string path = mdFilePath(fork_num, segno);
    int fd = OpenFile(path, create);

    fds[segno].fd = fd;
    fds[segno].segno = segno;
    return fd;
}

int SmgrRelationData::mdOpenSegment(ForkNumber fork_num, int segno, bool create) {
    return mdGetFd(fork_num, segno, create);
}

void SmgrRelationData::mdcreate(ForkNumber fork_num, bool /*is_redo*/) {
    // Build the error message inside a block scope so that `path` (a std::string)
    // is destructed BEFORE we call ereport(ERROR). ereport does longjmp, which
    // bypasses C++ destructors; without the block scope, `path` would leak.
    bool error = false;
    char errbuf[512];

    {
        std::string path = mdFilePath(fork_num, 0);
        EnsureParentDir(path);

        // Create the file (fail if it already exists, matching PostgreSQL).
        int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) {
                // PostgreSQL: if is_redo, silently ignore. Otherwise error.
                std::snprintf(errbuf, sizeof(errbuf), "relation file already exists: %s",
                              path.c_str());
            } else {
                std::snprintf(errbuf, sizeof(errbuf), "could not create file %s: %s", path.c_str(),
                              std::strerror(errno));
            }
            error = true;
        } else {
            close(fd);
        }
    }
    // path is now destructed — safe to ereport

    if (error) {
        ereport(mytoydb::error::LogLevel::kError, errbuf);
    }

    // Open it through the normal path so it's cached.
    mdEnsureSegments(fork_num, 0);
    md_fd[static_cast<int>(fork_num)][0].fd = -1;  // will be opened on first access
}

void SmgrRelationData::mdclose(ForkNumber fork_num) {
    auto& fds = md_fd[static_cast<int>(fork_num)];
    for (auto& entry : fds) {
        if (entry.fd >= 0) {
            close(entry.fd);
            entry.fd = -1;
        }
    }
    fds.clear();
}

void SmgrRelationData::mdread(ForkNumber fork_num, BlockNumber block_num, char* buffer) {
    // Compute segment number and offset within segment.
    int segno = static_cast<int>(block_num / kRelSegSize);
    BlockNumber block_in_seg = block_num % kRelSegSize;
    off_t offset = static_cast<off_t>(block_in_seg) * kBlckSz;

    int fd = mdGetFd(fork_num, segno, false);
    FileReadBlock(fd, offset, buffer);
}

void SmgrRelationData::mdwrite(ForkNumber fork_num, BlockNumber block_num, const char* buffer,
                               bool /*skip_fsync*/) {
    int segno = static_cast<int>(block_num / kRelSegSize);
    BlockNumber block_in_seg = block_num % kRelSegSize;
    off_t offset = static_cast<off_t>(block_in_seg) * kBlckSz;

    int fd = mdGetFd(fork_num, segno, false);
    FileWriteBlock(fd, offset, buffer);
}

void SmgrRelationData::mdextend(ForkNumber fork_num, BlockNumber block_num, const char* buffer,
                                bool skip_fsync) {
    // mdextend writes a new block at the end of the file.
    // The block must be exactly the next block after the current EOF.
    int segno = static_cast<int>(block_num / kRelSegSize);
    BlockNumber block_in_seg = block_num % kRelSegSize;
    off_t offset = static_cast<off_t>(block_in_seg) * kBlckSz;

    int fd = mdGetFd(fork_num, segno, true);
    FileWriteBlock(fd, offset, buffer);

    // If we just wrote to a new segment, the previous segment might need
    // to be extended to full RELSEG_SIZE. PostgreSQL handles this in
    // mdextend; for MyToyDB's testing scale, we skip this complexity.
    (void)skip_fsync;
}

BlockNumber SmgrRelationData::mdnblocks(ForkNumber fork_num) {
    // Count blocks across all segments.
    // For each open segment, stat the file to get its size.
    // For unopened segments, assume they don't exist (0 blocks).
    BlockNumber total = 0;

    auto& fds = md_fd[static_cast<int>(fork_num)];

    // First, check how many segments we know about.
    // If none are open, try opening segment 0 to see if the file exists.
    if (fds.empty()) {
        std::string path = mdFilePath(fork_num, 0);
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            return 0;  // file doesn't exist
        }
        // File exists but not open. Compute blocks from file size.
        total = static_cast<BlockNumber>(st.st_size / kBlckSz);
        return total;
    }

    // Sum blocks across all open segments.
    for (int segno = 0; segno < static_cast<int>(fds.size()); ++segno) {
        int fd = fds[segno].fd;
        if (fd < 0) {
            // Try to open it.
            std::string path = mdFilePath(fork_num, segno);
            struct stat st;
            if (stat(path.c_str(), &st) != 0) {
                break;  // segment doesn't exist, stop counting
            }
            total += static_cast<BlockNumber>(st.st_size / kBlckSz);
        } else {
            struct stat st;
            if (fstat(fd, &st) == 0) {
                total += static_cast<BlockNumber>(st.st_size / kBlckSz);
            }
        }
    }

    return total;
}

void SmgrRelationData::mdtruncate(ForkNumber fork_num, BlockNumber nblocks) {
    // Truncate the relation to exactly nblocks blocks.
    // This involves truncating the last segment and removing any
    // segments beyond it.
    int last_seg = static_cast<int>(nblocks / kRelSegSize);
    BlockNumber blocks_in_last = nblocks % kRelSegSize;
    off_t truncate_offset = static_cast<off_t>(blocks_in_last) * kBlckSz;

    auto& fds = md_fd[static_cast<int>(fork_num)];

    // Truncate the last segment we're keeping.
    if (blocks_in_last > 0) {
        int fd = mdGetFd(fork_num, last_seg, false);
        if (ftruncate(fd, truncate_offset) < 0) {
            ereport(mytoydb::error::LogLevel::kError,
                    std::string("could not truncate file: ") + std::strerror(errno));
        }
    }

    // Remove segments beyond last_seg.
    for (int segno = static_cast<int>(fds.size()) - 1; segno > last_seg; --segno) {
        std::string path = mdFilePath(fork_num, segno);
        if (fds[segno].fd >= 0) {
            close(fds[segno].fd);
            fds[segno].fd = -1;
        }
        unlink(path.c_str());  // ignore errors
    }

    // If blocks_in_last == 0, also remove the last_seg file.
    if (blocks_in_last == 0 && last_seg == 0) {
        std::string path = mdFilePath(fork_num, 0);
        if (!fds.empty() && fds[0].fd >= 0) {
            close(fds[0].fd);
            fds[0].fd = -1;
        }
        unlink(path.c_str());
    }
}

void SmgrRelationData::mdimmedsync(ForkNumber fork_num) {
    auto& fds = md_fd[static_cast<int>(fork_num)];
    for (auto& entry : fds) {
        if (entry.fd >= 0) {
            fsync(entry.fd);
        }
    }
}

}  // namespace mytoydb::storage
