// fd.cpp — Virtual File Descriptor (VFD) cache.
//
// Converted from PostgreSQL 15's src/backend/storage/file/fd.c.
#include "mytoydb/storage/file/fd.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mytoydb::storage {

namespace {

// VfdEntry — one entry in the VFD pool.
struct VfdEntry {
    int fd = -1;                     // underlying OS fd (-1 = not open)
    std::string path;                // file path (saved for reopen)
    int flags = 0;                   // original open() flags
    int mode = 0644;                 // original open() mode
    int64_t file_size = 0;           // last known file size
    bool transient = false;          // true if opened via AllocateFile (auto-closed)
    void* transient_file = nullptr;  // FILE* for AllocateFile entries
};

// VfdPool — function-local static (matches the procarray.cpp pattern).
std::vector<VfdEntry>& VfdPool() {
    static std::vector<VfdEntry> pool;
    return pool;
}

// FindFreeSlot — returns the index of the first free slot, growing the pool
// if necessary. VFD indices are 1-based (slot 0 is reserved for kInvalidFile).
int FindFreeSlot() {
    auto& pool = VfdPool();
    for (std::size_t i = 1; i < pool.size(); ++i) {
        if (pool[i].fd == -1 && pool[i].transient_file == nullptr) {
            return static_cast<int>(i);
        }
    }
    pool.push_back(VfdEntry{});
    return static_cast<int>(pool.size() - 1);
}

// TranslateFlags — convert FileAccessFlags bitmask to POSIX open() flags.
int TranslateFlags(int flags) {
    int posix_flags = 0;
    if ((flags & kOReadWrite) == kOReadWrite) {
        posix_flags |= O_RDWR;
    } else {
        posix_flags |= O_RDONLY;
    }
    if (flags & kOCreate) {
        posix_flags |= O_CREAT;
    }
    if (flags & kOExclusive) {
        posix_flags |= O_EXCL;
    }
    if (flags & kOAppend) {
        posix_flags |= O_APPEND;
    }
    return posix_flags;
}

}  // namespace

void* AllocateFile(const char* name, const char* mode) {
    FILE* fp = std::fopen(name, mode);
    if (fp == nullptr) {
        return nullptr;
    }
    int slot = FindFreeSlot();
    auto& pool = VfdPool();
    pool[slot].fd = -1;  // AllocateFile uses FILE*, not raw fd
    pool[slot].path = name;
    pool[slot].transient = true;
    pool[slot].transient_file = fp;
    return fp;
}

int FreeFile(void* file) {
    FILE* fp = static_cast<FILE*>(file);
    auto& pool = VfdPool();
    for (std::size_t i = 1; i < pool.size(); ++i) {
        if (pool[i].transient_file == fp) {
            int rc = std::fclose(fp);
            pool[i] = VfdEntry{};
            return rc;
        }
    }
    // Not in the VFD pool: just close.
    return std::fclose(fp);
}

File PathNameOpenFile(const char* name, int flags, int mode) {
    int posix_flags = TranslateFlags(flags);
    int fd = ::open(name, posix_flags, mode);
    if (fd < 0) {
        return kInvalidFile;
    }
    int slot = FindFreeSlot();
    auto& pool = VfdPool();
    pool[slot].fd = fd;
    pool[slot].path = name;
    pool[slot].flags = flags;
    pool[slot].mode = mode;
    pool[slot].transient = false;
    // Stash the file size for FileSeek(SEEK_END) optimizations.
    struct stat st {};
    if (::fstat(fd, &st) == 0) {
        pool[slot].file_size = static_cast<int64_t>(st.st_size);
    }
    return slot;
}

int FileClose(File file) {
    if (file <= 0 || file >= static_cast<int>(VfdPool().size())) {
        return -1;
    }
    auto& entry = VfdPool()[file];
    int rc = 0;
    if (entry.fd >= 0) {
        rc = ::close(entry.fd);
    } else if (entry.transient_file != nullptr) {
        rc = std::fclose(static_cast<FILE*>(entry.transient_file));
    }
    entry = VfdEntry{};
    return rc;
}

int FileRead(File file, void* buffer, std::size_t nbytes, int64_t* offset) {
    if (file <= 0 || file >= static_cast<int>(VfdPool().size())) {
        return -1;
    }
    int fd = VfdPool()[file].fd;
    if (fd < 0) {
        return -1;
    }
    ssize_t n = ::pread(fd, buffer, nbytes, static_cast<off_t>(*offset));
    if (n < 0) {
        return -1;
    }
    *offset += n;
    return static_cast<int>(n);
}

int FileWrite(File file, const void* buffer, std::size_t nbytes, int64_t* offset) {
    if (file <= 0 || file >= static_cast<int>(VfdPool().size())) {
        return -1;
    }
    int fd = VfdPool()[file].fd;
    if (fd < 0) {
        return -1;
    }
    ssize_t n = ::pwrite(fd, buffer, nbytes, static_cast<off_t>(*offset));
    if (n < 0) {
        return -1;
    }
    *offset += n;
    if (*offset > VfdPool()[file].file_size) {
        VfdPool()[file].file_size = *offset;
    }
    return static_cast<int>(n);
}

int FileSync(File file) {
    if (file <= 0 || file >= static_cast<int>(VfdPool().size())) {
        return -1;
    }
    int fd = VfdPool()[file].fd;
    if (fd < 0) {
        return -1;
    }
    return ::fsync(fd);
}

int64_t FileSeek(File file, int64_t offset, int whence) {
    if (file <= 0 || file >= static_cast<int>(VfdPool().size())) {
        return -1;
    }
    int fd = VfdPool()[file].fd;
    if (fd < 0) {
        return -1;
    }
    return static_cast<int64_t>(::lseek(fd, static_cast<off_t>(offset), whence));
}

int FileTruncate(File file, int64_t length) {
    if (file <= 0 || file >= static_cast<int>(VfdPool().size())) {
        return -1;
    }
    int fd = VfdPool()[file].fd;
    if (fd < 0) {
        return -1;
    }
    int rc = ::ftruncate(fd, static_cast<off_t>(length));
    if (rc == 0) {
        VfdPool()[file].file_size = length;
    }
    return rc;
}

const char* FileName(File file) {
    if (file <= 0 || file >= static_cast<int>(VfdPool().size())) {
        return "";
    }
    return VfdPool()[file].path.c_str();
}

int FileFd(File file) {
    if (file <= 0 || file >= static_cast<int>(VfdPool().size())) {
        return -1;
    }
    return VfdPool()[file].fd;
}

void InitFileAccess() {
    (void)VfdPool();  // lazily construct
}

void CloseTransientFiles() {
    auto& pool = VfdPool();
    for (std::size_t i = 1; i < pool.size(); ++i) {
        if (pool[i].transient) {
            if (pool[i].transient_file != nullptr) {
                std::fclose(static_cast<FILE*>(pool[i].transient_file));
            }
            pool[i] = VfdEntry{};
        }
    }
}

void ResetVfdCache() {
    VfdPool().clear();
    VfdPool().push_back(VfdEntry{});  // reserve slot 0
}

int NumOpenFiles() {
    int count = 0;
    for (std::size_t i = 1; i < VfdPool().size(); ++i) {
        if (VfdPool()[i].fd >= 0 || VfdPool()[i].transient_file != nullptr) {
            ++count;
        }
    }
    return count;
}

}  // namespace mytoydb::storage
