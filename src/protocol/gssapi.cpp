// gssapi.cpp — GSSAPI authentication (degraded mode for pgcpp).
//
// pgcpp does not link libgssapi_krb5, so the GSSAPI auth handler is a stub
// that always reports "GSS not available". The API is preserved so that
// auth.c can dispatch to it for the "gss" pg_hba method.
#include "pgcpp/protocol/gssapi.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace pgcpp::protocol {

namespace {

std::mutex& StateMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<int, GssContext>& StateMap() {
    static std::unordered_map<int, GssContext> m;
    return m;
}

}  // namespace

GssAuthResult pg_GSS_recvauth(OutputSink* /*sink*/, GssContext& ctx) {
    ctx.in_progress = false;
    ctx.principal.clear();
    return GssAuthResult::kNotAvailable;
}

bool secure_open_gssapi(int /*fd*/) {
    return false;
}

bool IsGssEnabled() {
    return false;
}

std::string GetGssPrincipal(int fd) {
    std::lock_guard<std::mutex> g(StateMutex());
    auto it = StateMap().find(fd);
    if (it == StateMap().end())
        return "";
    return it->second.principal;
}

void ResetGssState() {
    std::lock_guard<std::mutex> g(StateMutex());
    StateMap().clear();
}

}  // namespace pgcpp::protocol
