// fsstubs.cpp — Large-object file-descriptor stubs (be-fsstubs.c).
//
// Maintains a process-wide fd table (array of LargeObjectDesc*) indexed by
// small integers. Each lo_open returns the next free slot; lo_close frees
// the slot; lo_read/lo_write/lo_lseek/lo_tell/lo_truncate dispatch to the
// corresponding inv_api function on the descriptor stored in the slot.
#include "pgcpp/protocol/fsstubs.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>

#include "pgcpp/storage/large_object/inv_api.hpp"

namespace pgcpp::protocol {

namespace {

std::mutex& FdMutex() {
    static std::mutex m;
    return m;
}

// Fd table — slot i is non-null when fd i is open.
std::vector<storage::LargeObjectDesc*>& FdTable() {
    static std::vector<storage::LargeObjectDesc*> t(kMaxLargeObjects, nullptr);
    return t;
}

int AllocFd(storage::LargeObjectDesc* desc) {
    auto& t = FdTable();
    for (int i = 0; i < kMaxLargeObjects; ++i) {
        if (t[i] == nullptr) {
            t[i] = desc;
            return i;
        }
    }
    return kInvalidLargeObjectFd;
}

bool IsValidFd(int fd) {
    if (fd < 0 || fd >= kMaxLargeObjects)
        return false;
    return FdTable()[fd] != nullptr;
}

}  // namespace

storage::LargeObjectOid lo_create(int32_t lobjId) {
    return storage::inv_create(static_cast<storage::LargeObjectOid>(lobjId));
}

int lo_open(storage::LargeObjectOid lobjId, int mode) {
    storage::LargeObjectDesc* desc = storage::inv_open(lobjId, mode);
    if (desc == nullptr)
        return kInvalidLargeObjectFd;
    std::lock_guard<std::mutex> g(FdMutex());
    int fd = AllocFd(desc);
    if (fd == kInvalidLargeObjectFd) {
        storage::inv_close(desc);
    }
    return fd;
}

int lo_close(int fd) {
    std::lock_guard<std::mutex> g(FdMutex());
    if (!IsValidFd(fd))
        return -1;
    auto* desc = FdTable()[fd];
    FdTable()[fd] = nullptr;
    return storage::inv_close(desc);
}

int lo_read(int fd, char* buf, int len) {
    std::lock_guard<std::mutex> g(FdMutex());
    if (!IsValidFd(fd))
        return -1;
    auto* desc = FdTable()[fd];
    return storage::inv_read(desc, reinterpret_cast<uint8_t*>(buf), len);
}

int lo_write(int fd, const char* buf, int len) {
    std::lock_guard<std::mutex> g(FdMutex());
    if (!IsValidFd(fd))
        return -1;
    auto* desc = FdTable()[fd];
    return storage::inv_write(desc, reinterpret_cast<const uint8_t*>(buf), len);
}

int lo_lseek(int fd, int32_t offset, int32_t whence) {
    std::lock_guard<std::mutex> g(FdMutex());
    if (!IsValidFd(fd))
        return -1;
    auto* desc = FdTable()[fd];
    int64_t r = storage::inv_seek(desc, offset, whence);
    return r < 0 ? -1 : static_cast<int>(r);
}

int64_t lo_lseek64(int fd, int64_t offset, int32_t whence) {
    std::lock_guard<std::mutex> g(FdMutex());
    if (!IsValidFd(fd))
        return -1;
    auto* desc = FdTable()[fd];
    return storage::inv_seek(desc, offset, whence);
}

int32_t lo_tell(int fd) {
    std::lock_guard<std::mutex> g(FdMutex());
    if (!IsValidFd(fd))
        return -1;
    auto* desc = FdTable()[fd];
    int64_t r = storage::inv_tell(desc);
    return r < 0 ? -1 : static_cast<int32_t>(r);
}

int64_t lo_tell64(int fd) {
    std::lock_guard<std::mutex> g(FdMutex());
    if (!IsValidFd(fd))
        return -1;
    auto* desc = FdTable()[fd];
    return storage::inv_tell(desc);
}

int lo_truncate(int fd, int32_t len) {
    std::lock_guard<std::mutex> g(FdMutex());
    if (!IsValidFd(fd))
        return -1;
    auto* desc = FdTable()[fd];
    return storage::inv_truncate(desc, len);
}

int lo_truncate64(int fd, int64_t len) {
    std::lock_guard<std::mutex> g(FdMutex());
    if (!IsValidFd(fd))
        return -1;
    auto* desc = FdTable()[fd];
    return storage::inv_truncate(desc, len);
}

int lo_unlink(storage::LargeObjectOid lobjId) {
    // Close any open fds pointing at this LO.
    std::lock_guard<std::mutex> g(FdMutex());
    auto& t = FdTable();
    for (int i = 0; i < kMaxLargeObjects; ++i) {
        if (t[i] != nullptr && t[i]->oid == lobjId) {
            storage::inv_close(t[i]);
            t[i] = nullptr;
        }
    }
    return storage::inv_drop(lobjId) == 0 ? 1 : -1;
}

void ResetLargeObjectFds() {
    std::lock_guard<std::mutex> g(FdMutex());
    auto& t = FdTable();
    for (int i = 0; i < kMaxLargeObjects; ++i) {
        if (t[i] != nullptr) {
            storage::inv_close(t[i]);
            t[i] = nullptr;
        }
    }
}

int NumOpenLargeObjectFds() {
    std::lock_guard<std::mutex> g(FdMutex());
    int n = 0;
    for (int i = 0; i < kMaxLargeObjects; ++i) {
        if (FdTable()[i] != nullptr)
            ++n;
    }
    return n;
}

}  // namespace pgcpp::protocol
