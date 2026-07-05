// auth.cpp — Client authentication dispatch.
//
// ClientAuthentication() looks up the matching pg_hba.conf line, then drives
// the appropriate method-specific handler. Each handler may send an auth
// request to the client (via OutputSink) and consume a response (either from
// the global ResponseReader, installed by BackendMain in production, or from
// the per-process mock queue in tests).
#include "protocol/auth.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "protocol/crypt.hpp"
#include "protocol/pqformat.hpp"

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

// Process-wide response reader. When set (by BackendMain), ReadPasswordMessage
// delegates to it; when empty, it falls back to the mock queue (test mode).
ResponseReader& GlobalReader() {
    static ResponseReader r;
    return r;
}

// Read one client response (PasswordMessage or SASLResponse body).
// Uses the global response reader if installed (production); otherwise
// drains the mock queue (tests).
std::string ReadPasswordMessage() {
    ResponseReader& r = GlobalReader();
    if (r)
        return r();
    return TakeMockClientResponse();
}

// Generate a 4-byte random salt for md5 auth.
uint32_t RandomSalt() {
    std::random_device rd;
    uint32_t s = 0;
    for (int i = 0; i < 4; ++i) {
        s = (s << 8) | (rd() & 0xff);
    }
    return s;
}

// Generate a printable nonce for SCRAM (random hex string).
std::string RandomNonce() {
    std::random_device rd;
    std::array<uint8_t, 16> buf{};
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<uint8_t>(rd() & 0xff);
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(buf.size() * 2);
    for (auto b : buf) {
        out.push_back(kHex[(b >> 4) & 0xf]);
        out.push_back(kHex[b & 0xf]);
    }
    return out;
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

void SetGlobalResponseReader(ResponseReader reader) {
    GlobalReader() = std::move(reader);
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

AuthResult CheckMd5Auth(OutputSink* sink, [[maybe_unused]] const std::string& user,
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
    // Inner: MD5(md5_digest_hex + salt) — computed directly via Md5Hex
    // (replaces the previous Md5Encrypt(inner, "") hack).
    std::string inner_hex = Md5Hex(hash.md5_digest + salt_str);
    if (inner_hex == client_resp) {
        return AuthResult::kSuccess;
    }
    return AuthResult::kWrongPassword;
}

AuthResult CheckScramAuth(OutputSink* sink, const std::string& user,
                          const std::string& stored_password) {
    // SCRAM-SHA-256 SASL exchange per RFC 5802.
    //
    // Wire layout (pgcpp mock-queue form, no length prefixes):
    //   1. Server sends AuthenticationSASL with the mechanism list
    //      "SCRAM-SHA-256\0" (NUL-terminated).
    //   2. Client sends SASLInitialResponse: mechanism + client-first-body.
    //      client-first-body = "n,,n=<user>,r=<client-nonce>".
    //   3. Server sends AuthenticationSASLContinue with server-first:
    //      "r=<client+server-nonce>,s=<salt-b64>,i=<iter>".
    //   4. Client sends SASLResponse with client-final:
    //      "c=biws,r=<nonce>,p=<proof-b64>".
    //   5. Server verifies proof, sends AuthenticationSASLFinal with
    //      "v=<server-signature-b64>", then AuthenticationOk.
    (void)user;

    // Parse the stored password — must be a SCRAM-SHA-256 hash.
    PasswordHash hash;
    if (!ParsePasswordHash(stored_password, hash) ||
        hash.method != PasswordEncryptionAlgorithm::kScramSha256) {
        return AuthResult::kWrongPassword;
    }

    // Step 1: advertise the mechanism list (single mechanism).
    SendAuthRequest(sink, AuthRequest::kScramSha256, std::string("SCRAM-SHA-256", 13) + "\0");

    // Step 2: read the client-first message.
    std::string client_first_full = ReadPasswordMessage();
    if (client_first_full.empty())
        return AuthResult::kProtocolError;

    // The body we read is: "n,,n=<user>,r=<client-nonce>".
    // Strip the channel-binding prefix "n,," if present.
    std::string client_first_bare = client_first_full;
    if (client_first_bare.size() >= 3 && client_first_bare.substr(0, 3) == "n,,") {
        client_first_bare = client_first_bare.substr(3);
    } else {
        // Some clients may send gs2-header-less messages; tolerate by
        // treating the whole thing as the bare message.
    }

    // Extract client nonce (r=...).
    std::string client_nonce;
    {
        auto pos = client_first_bare.find("r=");
        if (pos == std::string::npos)
            return AuthResult::kProtocolError;
        auto end = client_first_bare.find(',', pos);
        client_nonce = (end == std::string::npos)
                           ? client_first_bare.substr(pos + 2)
                           : client_first_bare.substr(pos + 2, end - pos - 2);
        if (client_nonce.empty())
            return AuthResult::kProtocolError;
    }

    // Generate the server-first message.
    std::string server_nonce = RandomNonce();
    std::string combined_nonce = client_nonce + server_nonce;
    std::string server_first = "r=" + combined_nonce + ",s=" + hash.scram_salt_b64 +
                               ",i=" + std::to_string(hash.scram_iterations);
    SendAuthRequest(sink, AuthRequest::kSaslContinue, server_first);

    // Step 4: read the client-final message.
    std::string client_final = ReadPasswordMessage();
    if (client_final.empty())
        return AuthResult::kProtocolError;

    // Expected layout: "c=biws,r=<nonce>,p=<proof-b64>".
    // Extract channel-binding, nonce, and proof.
    std::string cb, nonce_recv, proof_b64;
    {
        std::istringstream iss(client_final);
        std::string token;
        bool got_cb = false, got_nonce = false, got_proof = false;
        while (std::getline(iss, token, ',')) {
            if (token.substr(0, 2) == "c=") {
                cb = token.substr(2);
                got_cb = true;
            } else if (token.substr(0, 2) == "r=") {
                nonce_recv = token.substr(2);
                got_nonce = true;
            } else if (token.substr(0, 2) == "p=") {
                proof_b64 = token.substr(2);
                got_proof = true;
            }
        }
        if (!got_cb || !got_nonce || !got_proof)
            return AuthResult::kProtocolError;
    }

    // Verify the nonce matches.
    if (nonce_recv != combined_nonce)
        return AuthResult::kWrongPassword;

    // Reconstruct the AuthMessage:
    //   client-first-bare + "," + server-first + "," + client-final-without-proof
    std::string client_final_without_proof = "c=" + cb + ",r=" + nonce_recv;
    std::string auth_message =
        client_first_bare + "," + server_first + "," + client_final_without_proof;

    // Recover the ClientKey from the ClientProof:
    //   ClientSignature = HMAC(StoredKey, AuthMessage)
    //   ClientProof XOR ClientSignature = ClientKey
    //   StoredKey' = SHA256(ClientKey); must equal StoredKey.
    auto stored_key = Base64Decode(hash.scram_storedkey_b64);
    if (stored_key.size() != 32)
        return AuthResult::kWrongPassword;

    auto sig = HmacSha256(stored_key, auth_message);
    auto proof = Base64Decode(proof_b64);
    if (proof.size() != 32)
        return AuthResult::kWrongPassword;

    // ClientKey = ClientProof XOR ClientSignature.
    std::vector<uint8_t> client_key(32);
    for (size_t i = 0; i < 32; ++i) {
        client_key[i] = proof[i] ^ sig[i];
    }
    // StoredKey' = SHA256(ClientKey).
    std::string ck_str(client_key.begin(), client_key.end());
    auto stored_key_prime = Sha256(ck_str);
    if (stored_key_prime != stored_key) {
        return AuthResult::kWrongPassword;
    }

    // Compute the ServerSignature and send the server-final message.
    auto server_key = Base64Decode(hash.scram_serverkey_b64);
    if (server_key.size() != 32)
        return AuthResult::kWrongPassword;
    auto server_sig = HmacSha256(server_key, auth_message);
    std::string server_final = "v=" + Base64Encode(server_sig);
    SendAuthRequest(sink, AuthRequest::kSaslFinal, server_final);

    return AuthResult::kSuccess;
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
                                /*salt=*/RandomSalt());
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
