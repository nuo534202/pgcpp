// secure.cpp — Secure transport layer (SSL/TLS pass-through for pgcpp).
//
// pgcpp does not link OpenSSL, so the secure layer is a pass-through that
// calls plain read(2)/write(2). The API is preserved so callers can use the
// same call sites as PG.
#include "protocol/secure.hpp"

#include <unistd.h>

#include <mutex>
#include <string>
#include <unordered_map>

namespace pgcpp::protocol {

namespace {

std::mutex& StateMutex() {
    static std::mutex m;
    return m;
}

// Per-fd secure status (none of pgcpp's tests actually uses SSL, so this
// map is mostly empty; it exists to keep the API shape faithful).
std::unordered_map<int, SecureStatus>& StateMap() {
    static std::unordered_map<int, SecureStatus> m;
    return m;
}

SecureStatus& GetOrInsert(int fd) {
    auto& m = StateMap();
    auto it = m.find(fd);
    if (it == m.end()) {
        SecureStatus s;
        s.layer = SecureLayer::kPlain;
        s.initialized = false;
        it = m.emplace(fd, s).first;
    }
    return it->second;
}

}  // namespace

SecureInitResult secure_initialize() {
    SecureInitResult r;
    r.ok = true;
    return r;
}

SecureInitResult secure_open_server(int fd) {
    SecureInitResult r;
    r.ok = false;
    r.error = "SSL is not compiled into this server";
    std::lock_guard<std::mutex> g(StateMutex());
    auto& s = GetOrInsert(fd);
    s.layer = SecureLayer::kPlain;
    s.initialized = true;
    return r;
}

void secure_close(int fd) {
    std::lock_guard<std::mutex> g(StateMutex());
    StateMap().erase(fd);
}

long secure_read(int fd, void* buf, size_t len) {
    long n = ::read(fd, buf, len);
    return n;
}

long secure_write(int fd, const void* buf, size_t len) {
    long n = ::write(fd, buf, len);
    return n;
}

bool IsSslEnabled() {
    return false;
}

SecureStatus GetSecureStatus(int fd) {
    std::lock_guard<std::mutex> g(StateMutex());
    return GetOrInsert(fd);
}

void ResetSecureState() {
    std::lock_guard<std::mutex> g(StateMutex());
    StateMap().clear();
}

}  // namespace pgcpp::protocol
