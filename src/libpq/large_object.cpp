// large_object.cpp — Large object (lo_*) API implementation (P3-11).
//
// Mirrors PostgreSQL libpq's large-object client API. In a real libpq,
// the lo_* calls are forwarded to the server as function-call ('F')
// messages over the wire. pgcpp instead uses an in-process fd table
// backed by pg_largeobject catalog rows because the client library
// lives in the same binary as the server (M13 tooling model).
//
// The implementation here is intentionally minimal: it provides the
// API surface so that client code written against libpq compiles and
// runs against pgcpp. The semantics are: lo_create allocates a new
// (random) OID; lo_open/lo_close manage an in-process fd; lo_read /
// lo_write operate on an in-memory buffer; lo_seek / lo_tell / lo_truncate
// adjust the buffer pointer / size.
//
// This mirrors PostgreSQL's src/backend/libpq/be-fsstubs.c which keeps
// a per-process fd table (currently 256 entries) for large objects.
// Real on-disk storage is out of scope for the P3-11 milestone.
#include "libpq/large_object.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace pgcpp::libpq {

namespace {

// LargeObjectEntry — per-fd state for an open large object.
struct LargeObjectEntry {
    bool in_use = false;
    uint32_t oid = 0;
    int mode = 0;
    std::string data;  // in-memory content
    std::size_t pos = 0;
    bool writable = false;
};

// Maximum number of concurrent large-object fds (matches PG's MAX_LOBJ_FDS).
constexpr int kMaxLobjFds = 256;

// Global fd table — guarded by a mutex to be safe under sanitizers
// (the table is logically per-process).
std::vector<LargeObjectEntry>& LoTable() {
    static std::vector<LargeObjectEntry> table(kMaxLobjFds);
    return table;
}

std::mutex& LoMutex() {
    static std::mutex m;
    return m;
}

// Find an unused fd slot. Returns -1 if the table is full.
int AllocFd() {
    auto& table = LoTable();
    for (int i = 0; i < kMaxLobjFds; ++i) {
        if (!table[i].in_use) {
            return i;
        }
    }
    return -1;
}

// In-memory store for created-but-not-yet-opened large objects.
// Keyed by OID; value is the content blob.
struct LoStore {
    std::vector<std::pair<uint32_t, std::string>> entries;
};

LoStore& GetLoStore() {
    static LoStore s;
    return s;
}

// Allocate a new OID that does not collide with existing ones. Uses a
// simple monotonic counter seeded from a constant for predictability
// under tests.
uint32_t AllocOid() {
    static uint32_t next_oid = 0x10000;
    while (true) {
        uint32_t candidate = next_oid++;
        bool found = false;
        for (const auto& [oid, _] : GetLoStore().entries) {
            if (oid == candidate) {
                found = true;
                break;
            }
        }
        if (!found) {
            return candidate;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

uint32_t LoCreate(PgConn& /*conn*/, uint32_t lobjId) {
    std::lock_guard<std::mutex> guard(LoMutex());
    if (lobjId == 0) {
        lobjId = AllocOid();
    }
    // Ensure the store has an entry (empty content).
    for (auto& [oid, _] : GetLoStore().entries) {
        if (oid == lobjId) {
            return lobjId;  // already exists
        }
    }
    GetLoStore().entries.emplace_back(lobjId, std::string{});
    return lobjId;
}

uint32_t LoImport(PgConn& conn, const std::string& filename) {
    return LoImportWithOid(conn, filename, 0);
}

uint32_t LoImportWithOid(PgConn& conn, const std::string& filename, uint32_t lobjId) {
    // Read the file into memory.
    FILE* fp = std::fopen(filename.c_str(), "rb");
    if (fp == nullptr)
        return 0;
    std::string content;
    char buf[4096];
    while (true) {
        std::size_t n = std::fread(buf, 1, sizeof(buf), fp);
        if (n == 0)
            break;
        content.append(buf, n);
    }
    std::fclose(fp);

    uint32_t oid = LoCreate(conn, lobjId);
    if (oid == 0)
        return 0;

    std::lock_guard<std::mutex> guard(LoMutex());
    for (auto& [oid2, data] : GetLoStore().entries) {
        if (oid2 == oid) {
            data = std::move(content);
            return oid;
        }
    }
    return 0;
}

int LoExport(PgConn& /*conn*/, uint32_t lobjId, const std::string& filename) {
    std::lock_guard<std::mutex> guard(LoMutex());
    for (const auto& [oid, data] : GetLoStore().entries) {
        if (oid == lobjId) {
            FILE* fp = std::fopen(filename.c_str(), "wb");
            if (fp == nullptr)
                return -1;
            std::size_t written = std::fwrite(data.data(), 1, data.size(), fp);
            std::fclose(fp);
            return (written == data.size()) ? 1 : -1;
        }
    }
    return -1;
}

int LoOpen(PgConn& /*conn*/, uint32_t lobjId, int mode) {
    std::lock_guard<std::mutex> guard(LoMutex());
    int fd = AllocFd();
    if (fd < 0)
        return -1;
    auto& entry = LoTable()[fd];
    entry.in_use = true;
    entry.oid = lobjId;
    entry.mode = mode;
    entry.pos = 0;
    entry.writable = (mode & LoMode::kWrite) != 0;
    // Copy content from the store to the fd.
    entry.data.clear();
    for (const auto& [oid, data] : GetLoStore().entries) {
        if (oid == lobjId) {
            entry.data = data;
            break;
        }
    }
    return fd;
}

int LoClose(PgConn& /*conn*/, int fd) {
    if (fd < 0 || fd >= kMaxLobjFds)
        return -1;
    std::lock_guard<std::mutex> guard(LoMutex());
    auto& entry = LoTable()[fd];
    if (!entry.in_use)
        return -1;
    if (entry.writable) {
        // Persist the (possibly modified) content back to the store.
        for (auto& [oid, data] : GetLoStore().entries) {
            if (oid == entry.oid) {
                data = entry.data;
                break;
            }
        }
    }
    entry.in_use = false;
    entry.oid = 0;
    entry.data.clear();
    entry.pos = 0;
    return 0;
}

int LoRead(PgConn& /*conn*/, int fd, char* buf, int len) {
    if (fd < 0 || fd >= kMaxLobjFds || buf == nullptr || len < 0)
        return -1;
    std::lock_guard<std::mutex> guard(LoMutex());
    auto& entry = LoTable()[fd];
    if (!entry.in_use)
        return -1;
    int avail = static_cast<int>(entry.data.size() - entry.pos);
    if (avail <= 0)
        return 0;
    int n = (len < avail) ? len : avail;
    std::memcpy(buf, entry.data.data() + entry.pos, static_cast<std::size_t>(n));
    entry.pos += static_cast<std::size_t>(n);
    return n;
}

int LoWrite(PgConn& /*conn*/, int fd, const char* buf, int len) {
    if (fd < 0 || fd >= kMaxLobjFds || buf == nullptr || len < 0)
        return -1;
    std::lock_guard<std::mutex> guard(LoMutex());
    auto& entry = LoTable()[fd];
    if (!entry.in_use || !entry.writable)
        return -1;
    if (entry.pos + static_cast<std::size_t>(len) > entry.data.size()) {
        entry.data.resize(entry.pos + static_cast<std::size_t>(len));
    }
    std::memcpy(entry.data.data() + entry.pos, buf, static_cast<std::size_t>(len));
    entry.pos += static_cast<std::size_t>(len);
    return len;
}

int LoSeek(PgConn& /*conn*/, int fd, int offset, int whence) {
    if (fd < 0 || fd >= kMaxLobjFds)
        return -1;
    std::lock_guard<std::mutex> guard(LoMutex());
    auto& entry = LoTable()[fd];
    if (!entry.in_use)
        return -1;
    std::size_t new_pos = entry.pos;
    if (whence == 0) {  // SEEK_SET
        new_pos = static_cast<std::size_t>(offset);
    } else if (whence == 1) {  // SEEK_CUR
        new_pos = entry.pos + static_cast<std::size_t>(offset);
    } else if (whence == 2) {  // SEEK_END
        new_pos = entry.data.size() + static_cast<std::size_t>(offset);
    } else {
        return -1;
    }
    entry.pos = new_pos;
    return 0;
}

int LoSeek64(PgConn& conn, int fd, int64_t offset, int whence) {
    // pgcpp's in-memory store is limited to size_t; just delegate to LoSeek.
    return LoSeek(conn, fd, static_cast<int>(offset), whence);
}

int LoTell(PgConn& /*conn*/, int fd) {
    if (fd < 0 || fd >= kMaxLobjFds)
        return -1;
    std::lock_guard<std::mutex> guard(LoMutex());
    auto& entry = LoTable()[fd];
    if (!entry.in_use)
        return -1;
    return static_cast<int>(entry.pos);
}

int64_t LoTell64(PgConn& conn, int fd) {
    return static_cast<int64_t>(LoTell(conn, fd));
}

int LoTruncate(PgConn& /*conn*/, int fd, int len) {
    if (fd < 0 || fd >= kMaxLobjFds || len < 0)
        return -1;
    std::lock_guard<std::mutex> guard(LoMutex());
    auto& entry = LoTable()[fd];
    if (!entry.in_use || !entry.writable)
        return -1;
    if (static_cast<std::size_t>(len) > entry.data.size()) {
        entry.data.resize(static_cast<std::size_t>(len));
    }
    entry.data.resize(static_cast<std::size_t>(len));
    if (entry.pos > entry.data.size())
        entry.pos = entry.data.size();
    return 0;
}

int LoTruncate64(PgConn& conn, int fd, int64_t len) {
    return LoTruncate(conn, fd, static_cast<int>(len));
}

int LoUnlink(PgConn& /*conn*/, uint32_t lobjId) {
    std::lock_guard<std::mutex> guard(LoMutex());
    auto& entries = GetLoStore().entries;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->first == lobjId) {
            entries.erase(it);
            // Close any open fds referring to this OID.
            for (int i = 0; i < kMaxLobjFds; ++i) {
                auto& e = LoTable()[i];
                if (e.in_use && e.oid == lobjId) {
                    e.in_use = false;
                    e.oid = 0;
                    e.data.clear();
                    e.pos = 0;
                }
            }
            return 1;
        }
    }
    return -1;
}

}  // namespace pgcpp::libpq
