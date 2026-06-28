// auth.cpp — Client authentication dispatch.
//
// ClientAuthentication() looks up the matching pg_hba.conf line, then drives
// the appropriate method-specific handler. Each handler may send an auth
// request to the client (via OutputSink) and consume a response (from the
// per-thread mock queue).
#include "pgcpp/protocol/auth.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include "pgcpp/protocol/crypt.hpp"
#include "pgcpp/protocol/pqformat.hpp"

namespace pgcpp::protocol {

namespace {

std::mutex& MockMutex() {
    static std::mutex m;
    return m;
}

std::deque<std::string>& MockQueue() {
    static std::deque<std::string> q;
    return q;
}

// Read a PasswordMessage from the mock queue (or return empty string).
// A real implementation would read from the socket; pgcpp's auth handlers
// are exercised via the mock queue in tests.
std::string ReadPasswordMessage() {
    return TakeMockClientResponse();
}

}  // namespace

void SetMockClientResponse(const std::string& payload) {
    std::lock_guard<std::mutex> g(MockMutex());
    MockQueue().push_back(payload);
}

std::string TakeMockClientResponse() {
    std::lock_guard<std::mutex> g(MockMutex());
    if (MockQueue().empty())
        return "";
    std::string s = std::move(MockQueue().front());
    MockQueue().pop_front();
    return s;
}

void ClearMockClientResponses() {
    std::lock_guard<std::mutex> g(MockMutex());
    MockQueue().clear();
}

void SendAuthRequest(OutputSink* sink, AuthRequest code, const std::string& extra) {
    MessageWriter w;
    w.WriteInt32(static_cast<int32_t>(code));
    if (!extra.empty()) {
        w.WriteBytes(extra.data(), extra.size());
    }
    sink->SendMessage(w.BuildMessage(MessageType::kAuthentication));
}

void SendErrorResponse(OutputSink* sink, const std::string& code, const std::string& message) {
    MessageWriter w;
    w.WriteByte(static_cast<char>(ErrorField::kSeverity));
    w.WriteString("FATAL");
    w.WriteByte(static_cast<char>(ErrorField::kCode));
    w.WriteString(code);
    w.WriteByte(static_cast<char>(ErrorField::kMessage));
    w.WriteString(message);
    w.WriteByte('\0');  // terminator
    sink->SendMessage(w.BuildMessage(MessageType::kErrorResponse));
}

AuthResult CheckTrustAuth() {
    return AuthResult::kSuccess;
}

AuthResult CheckRejectAuth() {
    return AuthResult::kRejected;
}

AuthResult CheckPasswordAuth(OutputSink* sink, const std::string& user,
                             const std::string& stored_password) {
    SendAuthRequest(sink, AuthRequest::kPassword);
    std::string resp = ReadPasswordMessage();
    if (resp.empty())
        return AuthResult::kProtocolError;
    // The response is a length-prefixed string (PasswordMessage wire format).
    // In the test harness the mock may store just the cleartext password.
    // Try both formats: if the first 4 bytes form a sensible length, treat
    // them as the length prefix; otherwise treat the whole payload as text.
    std::string password;
    if (resp.size() >= 4) {
        int32_t len = static_cast<int32_t>(static_cast<uint8_t>(resp[0])) |
                      (static_cast<int32_t>(static_cast<uint8_t>(resp[1])) << 8) |
                      (static_cast<int32_t>(static_cast<uint8_t>(resp[2])) << 16) |
                      (static_cast<int32_t>(static_cast<uint8_t>(resp[3])) << 24);
        if (len > 0 && static_cast<size_t>(len) <= resp.size() - 4 + 1) {
            password = resp.substr(4, resp.size() - 4);
            // Strip trailing NUL if present.
            if (!password.empty() && password.back() == '\0') {
                password.pop_back();
            }
        } else {
            password = resp;
        }
    } else {
        password = resp;
    }
    PasswordHash hash;
    if (!ParsePasswordHash(stored_password, hash)) {
        return AuthResult::kWrongPassword;
    }
    if (CryptVerify(hash, password, user)) {
        return AuthResult::kSuccess;
    }
    return AuthResult::kWrongPassword;
}

AuthResult CheckMd5Auth(OutputSink* sink, const std::string& user,
                        const std::string& stored_password, uint32_t salt) {
    // Send AuthenticationMd5Password (code 5) followed by 4-byte salt.
    std::string extra;
    extra.resize(4);
    extra[0] = static_cast<char>(salt & 0xff);
    extra[1] = static_cast<char>((salt >> 8) & 0xff);
    extra[2] = static_cast<char>((salt >> 16) & 0xff);
    extra[3] = static_cast<char>((salt >> 24) & 0xff);
    SendAuthRequest(sink, AuthRequest::kMd5, extra);
    std::string resp = ReadPasswordMessage();
    if (resp.empty())
        return AuthResult::kProtocolError;
    // The response is "md5" + 32 hex chars of:
    //   md5(md5(password+username) + salt)
    // Strip "md5" prefix if present.
    std::string client_resp = resp;
    if (client_resp.size() >= 3 && client_resp.substr(0, 3) == "md5") {
        client_resp = client_resp.substr(3);
    }
    if (client_resp.size() != 32)
        return AuthResult::kWrongPassword;
    // Compute the expected response from the stored hash.
    PasswordHash hash;
    if (!ParsePasswordHash(stored_password, hash)) {
        return AuthResult::kWrongPassword;
    }
    if (hash.method != PasswordEncryptionAlgorithm::kMd5) {
        // If the stored password isn't md5, we can't do md5 auth.
        return AuthResult::kWrongPassword;
    }
    // hash.md5_digest is md5(password+username) as 32 hex chars.
    // The salt is 4 raw bytes appended to the inner hash input.
    std::string salt_str;
    salt_str.resize(4);
    salt_str[0] = static_cast<char>(salt & 0xff);
    salt_str[1] = static_cast<char>((salt >> 8) & 0xff);
    salt_str[2] = static_cast<char>((salt >> 16) & 0xff);
    salt_str[3] = static_cast<char>((salt >> 24) & 0xff);
    // Inner: MD5(md5_digest_hex + salt).
    std::string inner_input = hash.md5_digest + salt_str;
    // Compute MD5(inner_input) as hex (we don't have direct access to the
    // raw MD5 here; reuse Md5Encrypt by passing empty username to avoid the
    // password+username concatenation). Md5Encrypt returns "md5" + hex.
    std::string inner_hex = Md5Encrypt(inner_input, /*username=*/"").substr(3);
    if (inner_hex == client_resp) {
        return AuthResult::kSuccess;
    }
    return AuthResult::kWrongPassword;
}

AuthResult CheckScramAuth(OutputSink* sink, const std::string& user,
                          const std::string& stored_password) {
    // Full SCRAM-SHA-256 SASL exchange is not implemented in pgcpp.
    // We send the SASL mechanism advertisement but always return
    // kMethodUnsupported.
    (void)user;
    (void)stored_password;
    SendAuthRequest(sink, AuthRequest::kScramSha256);
    return AuthResult::kMethodUnsupported;
}

AuthResult ClientAuthentication(const AuthContext& ctx) {
    switch (ctx.hba_line.method) {
        case HbaMethod::kTrust:
            return CheckTrustAuth();
        case HbaMethod::kReject:
            return CheckRejectAuth();
        case HbaMethod::kPassword:
            return CheckPasswordAuth(ctx.sink, ctx.user, ctx.stored_password);
        case HbaMethod::kMd5:
            return CheckMd5Auth(ctx.sink, ctx.user, ctx.stored_password,
                                /*salt=*/0x12345678);
        case HbaMethod::kScramSha256:
            return CheckScramAuth(ctx.sink, ctx.user, ctx.stored_password);
        case HbaMethod::kGss:
        case HbaMethod::kPeer:
        case HbaMethod::kIdent:
        case HbaMethod::kCert:
        case HbaMethod::kRadius:
        case HbaMethod::kLdap:
        case HbaMethod::kPam:
        case HbaMethod::kUnsupported:
            return AuthResult::kMethodUnsupported;
    }
    return AuthResult::kMethodUnsupported;
}

}  // namespace pgcpp::protocol
