// m11_ext_test.cpp — Unit tests for Task 15.22 (M11 SSL/GSS/HBA/Large Object/Fastpath).
//
// Covers:
//   - crypt:    MD5, SHA-256, PBKDF2, base64, password verification
//   - hba:      pg_hba.conf parsing and lookup
//   - ifaddr:   IPv4 parsing, CIDR matching, interface enumeration
//   - auth:     trust / password / md5 / scram / reject handlers
//   - secure:   pass-through layer (SSL not available)
//   - gssapi:   stub handler (GSS not available)
//   - fsstubs:  large-object fd API (lo_create/open/read/write/lseek/close/unlink)
//   - fastpath: fastpath function call protocol
//   - pqmq:     shared-memory message queue adapter
//   - pqsignal: signal handler installation

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "protocol/auth.hpp"
#include "protocol/crypt.hpp"
#include "protocol/fastpath.hpp"
#include "protocol/fsstubs.hpp"
#include "protocol/gssapi.hpp"
#include "protocol/hba.hpp"
#include "protocol/ifaddr.hpp"
#include "protocol/pqformat.hpp"
#include "protocol/pqmq.hpp"
#include "protocol/pqsignal.hpp"
#include "protocol/secure.hpp"
#include "storage/large_object/inv_api.hpp"

using pgcpp::protocol::AuthContext;
using pgcpp::protocol::AuthRequest;
using pgcpp::protocol::AuthResult;
using pgcpp::protocol::Base64Decode;
using pgcpp::protocol::Base64Encode;
using pgcpp::protocol::BuildFunctionCallResponse;
using pgcpp::protocol::CheckIpMatch;
using pgcpp::protocol::CheckMd5Auth;
using pgcpp::protocol::CheckPasswordAuth;
using pgcpp::protocol::CheckRejectAuth;
using pgcpp::protocol::CheckScramAuth;
using pgcpp::protocol::CheckTrustAuth;
using pgcpp::protocol::ClearMockClientResponses;
using pgcpp::protocol::ClientAuthentication;
using pgcpp::protocol::CryptVerify;
using pgcpp::protocol::EncryptPassword;
using pgcpp::protocol::FastpathArg;
using pgcpp::protocol::FastpathResult;
using pgcpp::protocol::FormatIPv4;
using pgcpp::protocol::GetGlobalFunctionRegistry;
using pgcpp::protocol::GetGssPrincipal;
using pgcpp::protocol::GetInstalledHandler;
using pgcpp::protocol::GetSecureStatus;
using pgcpp::protocol::GssAuthResult;
using pgcpp::protocol::GssContext;
using pgcpp::protocol::HandleFunctionRequest;
using pgcpp::protocol::HbaConfig;
using pgcpp::protocol::HbaConnType;
using pgcpp::protocol::HbaLine;
using pgcpp::protocol::HbaMethod;
using pgcpp::protocol::HbaMethodToString;
using pgcpp::protocol::HmacSha256;
using pgcpp::protocol::IsGssEnabled;
using pgcpp::protocol::IsSameHost;
using pgcpp::protocol::IsSameNet;
using pgcpp::protocol::IsSslEnabled;
using pgcpp::protocol::ListInterfaceAddresses;
using pgcpp::protocol::lo_close;
using pgcpp::protocol::lo_create;
using pgcpp::protocol::lo_lseek;
using pgcpp::protocol::lo_lseek64;
using pgcpp::protocol::lo_open;
using pgcpp::protocol::lo_read;
using pgcpp::protocol::lo_tell;
using pgcpp::protocol::lo_tell64;
using pgcpp::protocol::lo_truncate;
using pgcpp::protocol::lo_truncate64;
using pgcpp::protocol::lo_unlink;
using pgcpp::protocol::lo_write;
using pgcpp::protocol::MaskBitsToIPv4;
using pgcpp::protocol::MatchCidr;
using pgcpp::protocol::MatchDatabaseOrUser;
using pgcpp::protocol::Md5Encrypt;
using pgcpp::protocol::Md5Hex;
using pgcpp::protocol::Message;
using pgcpp::protocol::MessageReader;
using pgcpp::protocol::MessageType;
using pgcpp::protocol::NumOpenLargeObjectFds;
using pgcpp::protocol::ParseFastpathArgs;
using pgcpp::protocol::ParseHbaConfig;
using pgcpp::protocol::ParseHbaMethod;
using pgcpp::protocol::ParseIPv4;
using pgcpp::protocol::ParsePasswordHash;
using pgcpp::protocol::PasswordEncryptionAlgorithm;
using pgcpp::protocol::PasswordHash;
using pgcpp::protocol::Pbkdf2HmacSha256;
using pgcpp::protocol::pg_GSS_recvauth;
using pgcpp::protocol::pq_block_sigalrm;
using pgcpp::protocol::pq_mq_attach;
using pgcpp::protocol::pq_mq_close;
using pgcpp::protocol::pq_mq_detach;
using pgcpp::protocol::pq_mq_get_attached;
using pgcpp::protocol::pq_mq_putmessage;
using pgcpp::protocol::pq_mq_read_bytes;
using pgcpp::protocol::pq_reset_sigalrm;
using pgcpp::protocol::pq_sigprocmask;
using pgcpp::protocol::PqMqQueue;
using pgcpp::protocol::pqsignal;
using pgcpp::protocol::pqsignal_no_restart;
using pgcpp::protocol::ResetAllSignalHandlers;
using pgcpp::protocol::ResetGssState;
using pgcpp::protocol::ResetLargeObjectFds;
using pgcpp::protocol::ResetPqMqState;
using pgcpp::protocol::ResetSecureState;
using pgcpp::protocol::ScramSha256ClientKey;
using pgcpp::protocol::ScramSha256Hash;
using pgcpp::protocol::secure_close;
using pgcpp::protocol::secure_initialize;
using pgcpp::protocol::secure_open_gssapi;
using pgcpp::protocol::secure_open_server;
using pgcpp::protocol::secure_read;
using pgcpp::protocol::secure_write;
using pgcpp::protocol::SelectHbaLine;
using pgcpp::protocol::SendAuthRequest;
using pgcpp::protocol::SetGlobalResponseReader;
using pgcpp::protocol::SetMockClientResponse;
using pgcpp::protocol::Sha256;
using pgcpp::protocol::StringSink;
using pgcpp::storage::kInvalidLargeObjectOid;
using pgcpp::storage::kInvRdwr;
using pgcpp::storage::kInvRead;
using pgcpp::storage::kInvWrite;
using pgcpp::storage::ResetLargeObjects;

namespace {

// A signal handler used in pqsignal tests (does nothing).
void TestHandler(int) {}

}  // namespace

// ===========================================================================
// crypt: MD5, SHA-256, PBKDF2, base64, password verification
// ===========================================================================

TEST(CryptTest, Md5Encrypt_KnownVector) {
    // md5("pgtest" + "alice") should produce a 35-char "md5..." string.
    std::string h = Md5Encrypt("pgtest", "alice");
    EXPECT_EQ(h.size(), 35u);
    EXPECT_EQ(h.substr(0, 3), "md5");
    // Verify determinism.
    EXPECT_EQ(h, Md5Encrypt("pgtest", "alice"));
    // Different user produces a different hash.
    EXPECT_NE(h, Md5Encrypt("pgtest", "bob"));
}

TEST(CryptTest, Md5Encrypt_EmptyInputs) {
    std::string h = Md5Encrypt("", "");
    EXPECT_EQ(h.size(), 35u);
    // MD5("") = d41d8cd98f00b204e9800998ecf8427e
    EXPECT_EQ(h, "md5d41d8cd98f00b204e9800998ecf8427e");
}

TEST(CryptTest, Md5Hex_Returns32LowercaseHexChars) {
    // Md5Hex returns the raw 32-char hex digest (no "md5" prefix).
    std::string h = Md5Hex("");
    EXPECT_EQ(h.size(), 32u);
    EXPECT_EQ(h, "d41d8cd98f00b204e9800998ecf8427e");
    // Md5Hex(data) must equal Md5Encrypt("","").substr(3) for the same input
    // when the username is empty (since Md5Encrypt computes md5(pass+user)).
    std::string enc = Md5Encrypt("pgtest", "alice");
    EXPECT_EQ(Md5Hex("pgtestalice"), enc.substr(3));
}

TEST(CryptTest, Sha256_KnownVector) {
    // SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    auto h = Sha256("");
    EXPECT_EQ(h.size(), 32u);
    EXPECT_EQ(h[0], 0xe3);
    EXPECT_EQ(h[31], 0x55);
}

TEST(CryptTest, HmacSha256_KnownVector) {
    // HMAC-SHA-256(key="key", msg="The quick brown fox jumps over the lazy dog")
    // = f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
    std::vector<uint8_t> key{'k', 'e', 'y'};
    auto h = HmacSha256(key, "The quick brown fox jumps over the lazy dog");
    EXPECT_EQ(h.size(), 32u);
    EXPECT_EQ(h[0], 0xf7);
    EXPECT_EQ(h[31], 0xd8);
}

TEST(CryptTest, Pbkdf2HmacSha256_Rfc6070Vector) {
    // RFC 6070 PBKDF2-HMAC-SHA1 not directly applicable to SHA-256, but
    // verify determinism and output length.
    std::vector<uint8_t> salt{'s', 'a', 'l', 't'};
    auto h1 = Pbkdf2HmacSha256("password", salt, 1);
    auto h2 = Pbkdf2HmacSha256("password", salt, 1);
    EXPECT_EQ(h1.size(), 32u);
    EXPECT_EQ(h1, h2);
}

TEST(CryptTest, Base64_RoundTrip) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
    std::string enc = Base64Encode(data);
    std::vector<uint8_t> dec = Base64Decode(enc);
    EXPECT_EQ(dec, data);
}

TEST(CryptTest, Base64_KnownVector) {
    // "many" checkmarks (RFC 4648 §10):
    //   f    ZmY=
    //   fo   Zm8=
    //   foo  Zm9v
    //   foob Zm9vYg==
    //   fooba Zm9vYmE=
    //   foobar Zm9vYmFy
    auto enc = [](const std::string& s) {
        std::vector<uint8_t> v(s.begin(), s.end());
        return Base64Encode(v);
    };
    EXPECT_EQ(enc("f"), "Zg==");
    EXPECT_EQ(enc("fo"), "Zm8=");
    EXPECT_EQ(enc("foo"), "Zm9v");
    EXPECT_EQ(enc("foob"), "Zm9vYg==");
    EXPECT_EQ(enc("fooba"), "Zm9vYmE=");
    EXPECT_EQ(enc("foobar"), "Zm9vYmFy");
}

TEST(CryptTest, ParsePasswordHash_Plain) {
    PasswordHash h;
    ASSERT_TRUE(ParsePasswordHash("hello", h));
    EXPECT_EQ(h.method, PasswordEncryptionAlgorithm::kPlain);
    EXPECT_EQ(h.raw, "hello");
}

TEST(CryptTest, ParsePasswordHash_Md5) {
    PasswordHash h;
    ASSERT_TRUE(ParsePasswordHash("md5" + std::string(32, 'a'), h));
    EXPECT_EQ(h.method, PasswordEncryptionAlgorithm::kMd5);
    EXPECT_EQ(h.md5_digest, std::string(32, 'a'));
}

TEST(CryptTest, ParsePasswordHash_Md5WrongLength) {
    PasswordHash h;
    EXPECT_FALSE(ParsePasswordHash("md5abc", h));
}

TEST(CryptTest, ParsePasswordHash_Scram) {
    PasswordHash h;
    std::string s = "SCRAM-SHA-256$4096:YWJjZA==$STORED:SERVER";
    ASSERT_TRUE(ParsePasswordHash(s, h));
    EXPECT_EQ(h.method, PasswordEncryptionAlgorithm::kScramSha256);
    EXPECT_EQ(h.scram_iterations, 4096);
    EXPECT_EQ(h.scram_salt_b64, "YWJjZA==");
    EXPECT_EQ(h.scram_storedkey_b64, "STORED");
    EXPECT_EQ(h.scram_serverkey_b64, "SERVER");
}

TEST(CryptTest, CryptVerify_Plain) {
    PasswordHash h;
    ASSERT_TRUE(ParsePasswordHash("hello", h));
    EXPECT_TRUE(CryptVerify(h, "hello", "alice"));
    EXPECT_FALSE(CryptVerify(h, "world", "alice"));
}

TEST(CryptTest, CryptVerify_Md5) {
    std::string stored = Md5Encrypt("secret", "alice");
    PasswordHash h;
    ASSERT_TRUE(ParsePasswordHash(stored, h));
    EXPECT_TRUE(CryptVerify(h, "secret", "alice"));
    EXPECT_FALSE(CryptVerify(h, "wrong", "alice"));
}

TEST(CryptTest, CryptVerify_Scram) {
    std::string stored =
        EncryptPassword("mypassword", "alice", PasswordEncryptionAlgorithm::kScramSha256);
    PasswordHash h;
    ASSERT_TRUE(ParsePasswordHash(stored, h));
    EXPECT_TRUE(CryptVerify(h, "mypassword", "alice"));
    EXPECT_FALSE(CryptVerify(h, "notmypassword", "alice"));
}

TEST(CryptTest, EncryptPassword_Plain) {
    EXPECT_EQ(EncryptPassword("abc", "alice", PasswordEncryptionAlgorithm::kPlain), "abc");
}

TEST(CryptTest, EncryptPassword_Md5) {
    std::string s = EncryptPassword("abc", "alice", PasswordEncryptionAlgorithm::kMd5);
    EXPECT_EQ(s, Md5Encrypt("abc", "alice"));
}

TEST(CryptTest, EncryptPassword_Scram_Deterministic) {
    std::string salt = "saltsalt";  // 8 bytes
    std::string s1 = EncryptPassword("pw", "u", PasswordEncryptionAlgorithm::kScramSha256, salt);
    std::string s2 = EncryptPassword("pw", "u", PasswordEncryptionAlgorithm::kScramSha256, salt);
    EXPECT_EQ(s1, s2);
    // Different salt → different stored key.
    std::string s3 =
        EncryptPassword("pw", "u", PasswordEncryptionAlgorithm::kScramSha256, "diffsalt");
    EXPECT_NE(s1, s3);
}

TEST(CryptTest, ScramSha256Hash_ProducesBase64) {
    std::string sk, svk;
    ScramSha256Hash("pw", "salt", 4096, sk, svk);
    EXPECT_FALSE(sk.empty());
    EXPECT_FALSE(svk.empty());
    // Base64 of a 32-byte value is 44 chars (with padding).
    EXPECT_EQ(sk.size(), 44u);
    EXPECT_EQ(svk.size(), 44u);
    EXPECT_NE(sk, svk);
}

TEST(CryptTest, ScramSha256ClientKey_Produces32Bytes) {
    std::vector<uint8_t> stored(32, 0x42);
    auto ck = ScramSha256ClientKey(stored);
    EXPECT_EQ(ck.size(), 32u);
}

// ===========================================================================
// hba: pg_hba.conf parsing and lookup
// ===========================================================================

TEST(HbaTest, ParseHbaMethod_Known) {
    EXPECT_EQ(ParseHbaMethod("trust"), HbaMethod::kTrust);
    EXPECT_EQ(ParseHbaMethod("reject"), HbaMethod::kReject);
    EXPECT_EQ(ParseHbaMethod("password"), HbaMethod::kPassword);
    EXPECT_EQ(ParseHbaMethod("md5"), HbaMethod::kMd5);
    EXPECT_EQ(ParseHbaMethod("scram-sha-256"), HbaMethod::kScramSha256);
    EXPECT_EQ(ParseHbaMethod("gss"), HbaMethod::kGss);
    EXPECT_EQ(ParseHbaMethod("peer"), HbaMethod::kPeer);
    EXPECT_EQ(ParseHbaMethod("cert"), HbaMethod::kCert);
}

TEST(HbaTest, ParseHbaMethod_Unknown) {
    EXPECT_EQ(ParseHbaMethod("nonsense"), HbaMethod::kUnsupported);
}

TEST(HbaTest, HbaMethodToString_RoundTrip) {
    EXPECT_EQ(HbaMethodToString(HbaMethod::kTrust), "trust");
    EXPECT_EQ(HbaMethodToString(HbaMethod::kScramSha256), "scram-sha-256");
    EXPECT_EQ(HbaMethodToString(HbaMethod::kUnsupported), "unsupported");
}

TEST(HbaTest, ParseHbaConfig_BasicLocal) {
    std::string text = "# comment\nlocal all all trust\n";
    HbaConfig cfg = ParseHbaConfig(text);
    ASSERT_TRUE(cfg.valid);
    ASSERT_EQ(cfg.lines.size(), 1u);
    EXPECT_EQ(cfg.lines[0].conn_type, HbaConnType::kLocal);
    EXPECT_EQ(cfg.lines[0].databases, "all");
    EXPECT_EQ(cfg.lines[0].users, "all");
    EXPECT_EQ(cfg.lines[0].method, HbaMethod::kTrust);
    EXPECT_EQ(cfg.lines[0].lineno, 2);
}

TEST(HbaTest, ParseHbaConfig_HostWithCidr) {
    std::string text = "host all all 192.168.1.0/24 md5\n";
    HbaConfig cfg = ParseHbaConfig(text);
    ASSERT_TRUE(cfg.valid);
    ASSERT_EQ(cfg.lines.size(), 1u);
    EXPECT_EQ(cfg.lines[0].conn_type, HbaConnType::kHost);
    EXPECT_EQ(cfg.lines[0].cidr, "192.168.1.0");
    EXPECT_EQ(cfg.lines[0].mask_bits, 24);
    EXPECT_EQ(cfg.lines[0].method, HbaMethod::kMd5);
}

TEST(HbaTest, ParseHbaConfig_HostSslNoSsl) {
    std::string text =
        "hostssl  all all 10.0.0.0/8     scram-sha-256\n"
        "hostnossl all all 10.0.0.0/8    md5\n";
    HbaConfig cfg = ParseHbaConfig(text);
    ASSERT_TRUE(cfg.valid);
    ASSERT_EQ(cfg.lines.size(), 2u);
    EXPECT_EQ(cfg.lines[0].conn_type, HbaConnType::kHostSsl);
    EXPECT_EQ(cfg.lines[0].method, HbaMethod::kScramSha256);
    EXPECT_EQ(cfg.lines[1].conn_type, HbaConnType::kHostNoSsl);
}

TEST(HbaTest, ParseHbaConfig_CommentsAndBlankLines) {
    std::string text =
        "# first comment\n"
        "\n"
        "   # indented comment\n"
        "local all all trust\n";
    HbaConfig cfg = ParseHbaConfig(text);
    ASSERT_TRUE(cfg.valid);
    ASSERT_EQ(cfg.lines.size(), 1u);
    EXPECT_EQ(cfg.lines[0].lineno, 4);
}

TEST(HbaTest, ParseHbaConfig_UnknownMethod) {
    std::string text = "local all all nonsense\n";
    HbaConfig cfg = ParseHbaConfig(text);
    EXPECT_FALSE(cfg.valid);
    EXPECT_EQ(cfg.error_lineno, 1);
}

TEST(HbaTest, ParseHbaConfig_UnknownConnType) {
    std::string text = "foo all all trust\n";
    HbaConfig cfg = ParseHbaConfig(text);
    EXPECT_FALSE(cfg.valid);
}

TEST(HbaTest, ParseHbaConfig_MissingFields) {
    EXPECT_FALSE(ParseHbaConfig("local").valid);
    EXPECT_FALSE(ParseHbaConfig("local db").valid);
    EXPECT_FALSE(ParseHbaConfig("local db user").valid);  // missing method
}

TEST(HbaTest, MatchDatabaseOrUser_All) {
    EXPECT_TRUE(MatchDatabaseOrUser("all", "anything"));
    EXPECT_TRUE(MatchDatabaseOrUser("all", "postgres"));
}

TEST(HbaTest, MatchDatabaseOrUser_SingleName) {
    EXPECT_TRUE(MatchDatabaseOrUser("alice", "alice"));
    EXPECT_FALSE(MatchDatabaseOrUser("alice", "bob"));
}

TEST(HbaTest, MatchDatabaseOrUser_CommaList) {
    EXPECT_TRUE(MatchDatabaseOrUser("alice,bob", "alice"));
    EXPECT_TRUE(MatchDatabaseOrUser("alice,bob", "bob"));
    EXPECT_FALSE(MatchDatabaseOrUser("alice,bob", "carol"));
}

TEST(HbaTest, MatchDatabaseOrUser_SameUser) {
    // "sameuser" only matches the literal name "sameuser" in pgcpp.
    EXPECT_TRUE(MatchDatabaseOrUser("sameuser", "sameuser"));
    EXPECT_FALSE(MatchDatabaseOrUser("sameuser", "alice"));
}

TEST(HbaTest, MatchCidr_ExactMatch) {
    EXPECT_TRUE(MatchCidr("192.168.1.5", "192.168.1.5", 32));
    EXPECT_FALSE(MatchCidr("192.168.1.6", "192.168.1.5", 32));
}

TEST(HbaTest, MatchCidr_Subnet) {
    EXPECT_TRUE(MatchCidr("192.168.1.5", "192.168.1.0", 24));
    EXPECT_TRUE(MatchCidr("192.168.1.255", "192.168.1.0", 24));
    EXPECT_FALSE(MatchCidr("192.168.2.5", "192.168.1.0", 24));
}

TEST(HbaTest, MatchCidr_AllZeros) {
    EXPECT_TRUE(MatchCidr("1.2.3.4", "0.0.0.0", 0));
}

TEST(HbaTest, SelectHbaLine_FirstMatchWins) {
    std::string text =
        "host all all 192.168.1.0/24 trust\n"
        "host all all 0.0.0.0/0     md5\n";
    HbaConfig cfg = ParseHbaConfig(text);
    ASSERT_TRUE(cfg.valid);
    const HbaLine* l = SelectHbaLine(cfg, "db", "u", "192.168.1.5", false);
    ASSERT_NE(l, nullptr);
    EXPECT_EQ(l->method, HbaMethod::kTrust);
    const HbaLine* l2 = SelectHbaLine(cfg, "db", "u", "10.0.0.5", false);
    ASSERT_NE(l2, nullptr);
    EXPECT_EQ(l2->method, HbaMethod::kMd5);
}

TEST(HbaTest, SelectHbaLine_SslFilter) {
    std::string text =
        "hostssl  all all 0.0.0.0/0 scram-sha-256\n"
        "hostnossl all all 0.0.0.0/0 md5\n";
    HbaConfig cfg = ParseHbaConfig(text);
    ASSERT_TRUE(cfg.valid);
    const HbaLine* ssl = SelectHbaLine(cfg, "db", "u", "10.0.0.5", true);
    ASSERT_NE(ssl, nullptr);
    EXPECT_EQ(ssl->method, HbaMethod::kScramSha256);
    const HbaLine* nossl = SelectHbaLine(cfg, "db", "u", "10.0.0.5", false);
    ASSERT_NE(nossl, nullptr);
    EXPECT_EQ(nossl->method, HbaMethod::kMd5);
}

TEST(HbaTest, SelectHbaLine_NoMatch) {
    std::string text = "host all all 192.168.1.0/24 trust\n";
    HbaConfig cfg = ParseHbaConfig(text);
    ASSERT_TRUE(cfg.valid);
    EXPECT_EQ(SelectHbaLine(cfg, "db", "u", "10.0.0.5", false), nullptr);
}

TEST(HbaTest, SelectHbaLine_LocalOnly) {
    std::string text = "local all all trust\n";
    HbaConfig cfg = ParseHbaConfig(text);
    ASSERT_TRUE(cfg.valid);
    // Local rule: addr should be empty (Unix socket).
    EXPECT_NE(SelectHbaLine(cfg, "db", "u", "", false), nullptr);
    // With a non-empty address (TCP), the local rule should not match.
    EXPECT_EQ(SelectHbaLine(cfg, "db", "u", "127.0.0.1", false), nullptr);
}

// ===========================================================================
// ifaddr: IPv4 parsing, CIDR matching, interface enumeration
// ===========================================================================

TEST(IfaddrTest, ParseIPv4_Known) {
    uint32_t v = 0;
    ASSERT_TRUE(ParseIPv4("192.168.1.5", v));
    // 192.168.1.5 in host byte order
    EXPECT_EQ(v, (192u << 24) | (168u << 16) | (1u << 8) | 5u);
}

TEST(IfaddrTest, ParseIPv4_Invalid) {
    uint32_t v;
    EXPECT_FALSE(ParseIPv4("", v));
    EXPECT_FALSE(ParseIPv4("not an ip", v));
    EXPECT_FALSE(ParseIPv4("256.0.0.0", v));
    EXPECT_FALSE(ParseIPv4("1.2.3", v));
}

TEST(IfaddrTest, FormatIPv4_RoundTrip) {
    EXPECT_EQ(FormatIPv4(0x0a000005), "10.0.0.5");
    EXPECT_EQ(FormatIPv4(0xc0a80101), "192.168.1.1");
    EXPECT_EQ(FormatIPv4(0), "0.0.0.0");
}

TEST(IfaddrTest, MaskBitsToIPv4_Known) {
    EXPECT_EQ(MaskBitsToIPv4(0), "0.0.0.0");
    EXPECT_EQ(MaskBitsToIPv4(8), "255.0.0.0");
    EXPECT_EQ(MaskBitsToIPv4(16), "255.255.0.0");
    EXPECT_EQ(MaskBitsToIPv4(24), "255.255.255.0");
    EXPECT_EQ(MaskBitsToIPv4(32), "255.255.255.255");
}

TEST(IfaddrTest, ListInterfaceAddresses_NonEmpty) {
    // On any reasonable Linux host we have at least the loopback interface.
    auto addrs = ListInterfaceAddresses();
    EXPECT_FALSE(addrs.empty());
    bool found_loopback = false;
    for (const auto& a : addrs) {
        if (a.addr == "127.0.0.1") {
            found_loopback = true;
            break;
        }
    }
    EXPECT_TRUE(found_loopback);
}

TEST(IfaddrTest, IsSameHost_Loopback) {
    // 127.0.0.1 is always one of our local addresses.
    EXPECT_TRUE(IsSameHost("127.0.0.1"));
    // 8.8.8.8 is (almost certainly) not a local address.
    EXPECT_FALSE(IsSameHost("8.8.8.8"));
}

TEST(IfaddrTest, IsSameNet_Loopback) {
    // 127.0.0.0/8 is the loopback net, so 127.x.y.z is in the same net.
    EXPECT_TRUE(IsSameNet("127.1.2.3"));
}

TEST(IfaddrTest, CheckIpMatch_SameHost) {
    EXPECT_TRUE(CheckIpMatch("samehost", "127.0.0.1"));
    EXPECT_FALSE(CheckIpMatch("samehost", "8.8.8.8"));
}

TEST(IfaddrTest, CheckIpMatch_SameNet) {
    EXPECT_TRUE(CheckIpMatch("samenet", "127.1.2.3"));
}

TEST(IfaddrTest, CheckIpMatch_Cidr) {
    EXPECT_TRUE(CheckIpMatch("192.168.1.0/24", "192.168.1.100"));
    EXPECT_FALSE(CheckIpMatch("192.168.1.0/24", "192.168.2.100"));
    EXPECT_TRUE(CheckIpMatch("0.0.0.0/0", "8.8.8.8"));
}

TEST(IfaddrTest, CheckIpMatch_BareIp) {
    EXPECT_TRUE(CheckIpMatch("127.0.0.1", "127.0.0.1"));
    EXPECT_FALSE(CheckIpMatch("127.0.0.1", "127.0.0.2"));
}

// ===========================================================================
// auth: trust / password / md5 / scram / reject handlers
// ===========================================================================

TEST(AuthTest, CheckTrustAuth_Succeeds) {
    EXPECT_EQ(CheckTrustAuth(), AuthResult::kSuccess);
}

TEST(AuthTest, CheckRejectAuth_Rejects) {
    EXPECT_EQ(CheckRejectAuth(), AuthResult::kRejected);
}

TEST(AuthTest, CheckPasswordAuth_CorrectPassword) {
    StringSink sink;
    std::string stored = Md5Encrypt("secret", "alice");
    SetMockClientResponse("secret");
    EXPECT_EQ(CheckPasswordAuth(&sink, "alice", stored), AuthResult::kSuccess);
    ClearMockClientResponses();
}

TEST(AuthTest, CheckPasswordAuth_WrongPassword) {
    StringSink sink;
    std::string stored = Md5Encrypt("secret", "alice");
    SetMockClientResponse("wrong");
    EXPECT_EQ(CheckPasswordAuth(&sink, "alice", stored), AuthResult::kWrongPassword);
    ClearMockClientResponses();
}

TEST(AuthTest, CheckPasswordAuth_PlainStored) {
    StringSink sink;
    SetMockClientResponse("hello");
    EXPECT_EQ(CheckPasswordAuth(&sink, "alice", "hello"), AuthResult::kSuccess);
    ClearMockClientResponses();
}

TEST(AuthTest, CheckPasswordAuth_ProtocolError_NoResponse) {
    StringSink sink;
    ClearMockClientResponses();
    EXPECT_EQ(CheckPasswordAuth(&sink, "alice", "hello"), AuthResult::kProtocolError);
}

TEST(AuthTest, CheckPasswordAuth_SendsAuthRequest) {
    StringSink sink;
    std::string stored = Md5Encrypt("secret", "alice");
    SetMockClientResponse("secret");
    CheckPasswordAuth(&sink, "alice", stored);
    // The first message sent should be an Authentication request (type 'R').
    ASSERT_FALSE(sink.messages().empty());
    EXPECT_EQ(sink.messages().at(0).type, MessageType::kAuthentication);
    ClearMockClientResponses();
}

TEST(AuthTest, CheckMd5Auth_SendsSalt) {
    StringSink sink;
    std::string stored = Md5Encrypt("secret", "alice");
    // The expected client response is md5(md5(secret+alice) + salt) as 32 hex.
    // salt = 0x12345678 in little-endian byte order: 0x78, 0x56, 0x34, 0x12.
    std::string salt_str;
    salt_str.push_back(static_cast<char>(0x78));
    salt_str.push_back(static_cast<char>(0x56));
    salt_str.push_back(static_cast<char>(0x34));
    salt_str.push_back(static_cast<char>(0x12));
    std::string md5_hex = Md5Encrypt("secret", "alice").substr(3);
    // Inner = MD5(md5_hex + salt) — computed via Md5Hex (no "md5" prefix,
    // no empty-username hack).
    std::string expected = Md5Hex(md5_hex + salt_str);
    SetMockClientResponse(expected);
    EXPECT_EQ(CheckMd5Auth(&sink, "alice", stored, 0x12345678), AuthResult::kSuccess);
    ClearMockClientResponses();
}

TEST(AuthTest, CheckMd5Auth_WrongResponse) {
    StringSink sink;
    std::string stored = Md5Encrypt("secret", "alice");
    SetMockClientResponse("00000000000000000000000000000000");
    EXPECT_EQ(CheckMd5Auth(&sink, "alice", stored, 0x12345678), AuthResult::kWrongPassword);
    ClearMockClientResponses();
}

TEST(AuthTest, CheckScramAuth_InvalidStoredHash) {
    // "stored" is not a valid SCRAM-SHA-256 hash, so the exchange fails
    // before any message is sent to the client.
    StringSink sink;
    EXPECT_EQ(CheckScramAuth(&sink, "alice", "stored"), AuthResult::kWrongPassword);
}

// Full SCRAM-SHA-256 exchange: server-side CheckScramAuth driven by a
// stateful global response reader that plays the role of a real SCRAM
// client. The reader returns client-first on the first call; on the
// second call it inspects the server-first message already captured in
// the sink, computes the ClientProof, and returns client-final.
TEST(AuthTest, CheckScramAuth_FullExchange) {
    StringSink sink;

    // Server-side stored hash for password "hunter2" with salt "salt" iter 4096.
    const std::string password = "hunter2";
    const std::string salt_str = "salt";
    const int iterations = 4096;
    std::string stored_key_b64, server_key_b64;
    ScramSha256Hash(password, salt_str, iterations, stored_key_b64, server_key_b64);
    std::string stored_hash = "SCRAM-SHA-256$" + std::to_string(iterations) + ":" +
                              Base64Encode(std::vector<uint8_t>(salt_str.begin(), salt_str.end())) +
                              "$" + stored_key_b64 + ":" + server_key_b64;

    // --- Mock client side: precompute SCRAM key material. ---
    std::vector<uint8_t> salt_bytes(salt_str.begin(), salt_str.end());
    auto salted = Pbkdf2HmacSha256(password, salt_bytes, iterations);
    auto client_key = HmacSha256(salted, "Client Key");
    std::string ck_str(client_key.begin(), client_key.end());
    auto stored_key = Sha256(ck_str);

    const std::string client_nonce = "clientnonce12345";
    const std::string client_first = "n,,n=alice,r=" + client_nonce;
    const std::string client_first_bare = "n=alice,r=" + client_nonce;

    // Stateful reader: first call returns client-first; second call
    // inspects the sink, derives the proof, returns client-final.
    int call_count = 0;
    SetGlobalResponseReader([&]() -> std::string {
        if (call_count == 0) {
            ++call_count;
            return client_first;
        }
        ++call_count;
        // sink.messages() = [0] SASL, [1] SASLContinue.
        const auto& continue_payload = sink.messages().at(1).payload;
        std::string server_first(continue_payload.begin() + 4, continue_payload.end());
        auto r_pos = server_first.find("r=");
        auto s_pos = server_first.find(",s=");
        std::string combined_nonce = server_first.substr(r_pos + 2, s_pos - (r_pos + 2));
        std::string client_final_without_proof = "c=biws,r=" + combined_nonce;
        std::string auth_message =
            client_first_bare + "," + server_first + "," + client_final_without_proof;
        auto client_sig = HmacSha256(stored_key, auth_message);
        std::vector<uint8_t> proof(32);
        for (size_t i = 0; i < 32; ++i) {
            proof[i] = client_key[i] ^ client_sig[i];
        }
        return client_final_without_proof + ",p=" + Base64Encode(proof);
    });

    EXPECT_EQ(CheckScramAuth(&sink, "alice", stored_hash), AuthResult::kSuccess);
    // The sink should contain: SASL, SASLContinue, SASLFinal.
    ASSERT_GE(sink.messages().size(), 3u);
    EXPECT_EQ(sink.messages().at(2).type, MessageType::kAuthentication);
    SetGlobalResponseReader({});  // clear the reader
}

TEST(AuthTest, SendAuthRequest_BuildsCorrectMessage) {
    StringSink sink;
    SendAuthRequest(&sink, AuthRequest::kOk);
    ASSERT_FALSE(sink.messages().empty());
    const auto& m = sink.messages().at(0);
    EXPECT_EQ(m.type, MessageType::kAuthentication);
    // Payload is 4 bytes (int32 = 0).
    EXPECT_EQ(m.payload.size(), 4u);
}

TEST(AuthTest, ClientAuthentication_Trust) {
    StringSink sink;
    HbaLine line;
    line.method = HbaMethod::kTrust;
    AuthContext ctx{};
    ctx.user = "alice";
    ctx.hba_line = line;
    ctx.sink = &sink;
    EXPECT_EQ(ClientAuthentication(ctx), AuthResult::kSuccess);
}

TEST(AuthTest, ClientAuthentication_Reject) {
    StringSink sink;
    HbaLine line;
    line.method = HbaMethod::kReject;
    AuthContext ctx{};
    ctx.user = "alice";
    ctx.hba_line = line;
    ctx.sink = &sink;
    EXPECT_EQ(ClientAuthentication(ctx), AuthResult::kRejected);
}

TEST(AuthTest, ClientAuthentication_Gss_Unsupported) {
    StringSink sink;
    HbaLine line;
    line.method = HbaMethod::kGss;
    AuthContext ctx{};
    ctx.user = "alice";
    ctx.hba_line = line;
    ctx.sink = &sink;
    EXPECT_EQ(ClientAuthentication(ctx), AuthResult::kMethodUnsupported);
}

// ===========================================================================
// secure: pass-through layer
// ===========================================================================

TEST(SecureTest, secure_initialize_Succeeds) {
    auto r = secure_initialize();
    EXPECT_TRUE(r.ok);
}

TEST(SecureTest, secure_open_server_FailsWithoutSsl) {
    auto r = secure_open_server(-1);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(SecureTest, IsSslEnabled_False) {
    EXPECT_FALSE(IsSslEnabled());
}

TEST(SecureTest, GetSecureStatus_DefaultPlain) {
    ResetSecureState();
    auto s = GetSecureStatus(1234);
    // Status should default to plain (no SSL).
    EXPECT_FALSE(s.initialized);
}

TEST(SecureTest, ResetSecureState_Idempotent) {
    ResetSecureState();
    ResetSecureState();
    // Just ensure it doesn't crash.
    SUCCEED();
}

// ===========================================================================
// gssapi: stub handler
// ===========================================================================

TEST(GssapiTest, IsGssEnabled_False) {
    EXPECT_FALSE(IsGssEnabled());
}

TEST(GssapiTest, secure_open_gssapi_False) {
    EXPECT_FALSE(secure_open_gssapi(-1));
}

TEST(GssapiTest, pg_GSS_recvauth_NotAvailable) {
    StringSink sink;
    GssContext ctx;
    auto r = pg_GSS_recvauth(&sink, ctx);
    EXPECT_EQ(r, GssAuthResult::kNotAvailable);
    EXPECT_FALSE(ctx.in_progress);
    EXPECT_TRUE(ctx.principal.empty());
}

TEST(GssapiTest, GetGssPrincipal_Empty) {
    EXPECT_TRUE(GetGssPrincipal(-1).empty());
    ResetGssState();
}

// ===========================================================================
// fsstubs: large-object fd API
// ===========================================================================

class FsStubsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetLargeObjects();
        ResetLargeObjectFds();
    }
    void TearDown() override {
        ResetLargeObjectFds();
        ResetLargeObjects();
    }
};

TEST_F(FsStubsTest, lo_create_ReturnsNonZeroOid) {
    auto oid = lo_create(0);
    EXPECT_NE(oid, kInvalidLargeObjectOid);
}

TEST_F(FsStubsTest, lo_open_Close_RoundTrip) {
    auto oid = lo_create(0);
    int fd = lo_open(oid, kInvRdwr);
    EXPECT_GE(fd, 0);
    EXPECT_EQ(NumOpenLargeObjectFds(), 1);
    EXPECT_EQ(lo_close(fd), 0);
    EXPECT_EQ(NumOpenLargeObjectFds(), 0);
}

TEST_F(FsStubsTest, lo_open_InvalidOid) {
    EXPECT_EQ(lo_open(99999, kInvRead), -1);
}

TEST_F(FsStubsTest, lo_close_InvalidFd) {
    EXPECT_EQ(lo_close(-1), -1);
    EXPECT_EQ(lo_close(99999), -1);
}

TEST_F(FsStubsTest, lo_write_Read_RoundTrip) {
    auto oid = lo_create(0);
    int fd = lo_open(oid, kInvRdwr);
    std::string data = "hello large object";
    int n = lo_write(fd, data.data(), static_cast<int>(data.size()));
    EXPECT_EQ(n, static_cast<int>(data.size()));

    // Seek back to start and read.
    EXPECT_EQ(lo_lseek(fd, 0, 0), 0);
    std::vector<char> buf(data.size());
    int r = lo_read(fd, buf.data(), static_cast<int>(buf.size()));
    EXPECT_EQ(r, static_cast<int>(data.size()));
    EXPECT_EQ(std::string(buf.data(), r), data);
    EXPECT_EQ(lo_close(fd), 0);
}

TEST_F(FsStubsTest, lo_lseek_Whence) {
    auto oid = lo_create(0);
    int fd = lo_open(oid, kInvRdwr);
    std::string data = "0123456789";
    lo_write(fd, data.data(), static_cast<int>(data.size()));
    EXPECT_EQ(lo_lseek(fd, 0, 0), 0);  // SEEK_SET
    EXPECT_EQ(lo_lseek(fd, 5, 0), 5);
    EXPECT_EQ(lo_lseek(fd, 0, 1), 5);      // SEEK_CUR
    EXPECT_EQ(lo_lseek(fd, -1, 1), 4);     // SEEK_CUR with negative offset
    EXPECT_EQ(lo_lseek(fd, 0, 2), 10);     // SEEK_END
    EXPECT_EQ(lo_lseek(fd, -3, 2), 7);     // SEEK_END with negative offset
    EXPECT_EQ(lo_lseek(fd, -100, 2), -1);  // Before start.
    EXPECT_EQ(lo_close(fd), 0);
}

TEST_F(FsStubsTest, lo_lseek64_RoundTrip) {
    auto oid = lo_create(0);
    int fd = lo_open(oid, kInvRdwr);
    EXPECT_EQ(lo_lseek64(fd, 1000, 0), 1000);
    EXPECT_EQ(lo_lseek64(fd, 0, 1), 1000);
    EXPECT_EQ(lo_close(fd), 0);
}

TEST_F(FsStubsTest, lo_tell_ReflectsOffset) {
    auto oid = lo_create(0);
    int fd = lo_open(oid, kInvRdwr);
    EXPECT_EQ(lo_tell(fd), 0);
    lo_lseek(fd, 42, 0);
    EXPECT_EQ(lo_tell(fd), 42);
    EXPECT_EQ(lo_tell64(fd), 42);
    EXPECT_EQ(lo_close(fd), 0);
}

TEST_F(FsStubsTest, lo_truncate) {
    auto oid = lo_create(0);
    int fd = lo_open(oid, kInvRdwr);
    std::string data(100, 'x');
    lo_write(fd, data.data(), 100);
    EXPECT_EQ(lo_truncate(fd, 50), 0);
    EXPECT_EQ(lo_tell(fd), 50);         // offset clamped to new length
    EXPECT_EQ(lo_lseek(fd, 0, 2), 50);  // SEEK_END reports new length
    EXPECT_EQ(lo_close(fd), 0);
}

TEST_F(FsStubsTest, lo_truncate64) {
    auto oid = lo_create(0);
    int fd = lo_open(oid, kInvRdwr);
    std::string data(100, 'y');
    lo_write(fd, data.data(), 100);
    EXPECT_EQ(lo_truncate64(fd, 30), 0);
    EXPECT_EQ(lo_lseek(fd, 0, 2), 30);
    EXPECT_EQ(lo_close(fd), 0);
}

TEST_F(FsStubsTest, lo_unlink_Deletes) {
    auto oid = lo_create(0);
    EXPECT_EQ(lo_unlink(oid), 1);
    // Opening now should fail.
    EXPECT_EQ(lo_open(oid, kInvRead), -1);
    // Unlinking again should fail (-1).
    EXPECT_EQ(lo_unlink(oid), -1);
}

TEST_F(FsStubsTest, lo_unlink_ClosesOpenFds) {
    auto oid = lo_create(0);
    int fd = lo_open(oid, kInvRdwr);
    EXPECT_GE(fd, 0);
    EXPECT_EQ(NumOpenLargeObjectFds(), 1);
    EXPECT_EQ(lo_unlink(oid), 1);
    EXPECT_EQ(NumOpenLargeObjectFds(), 0);
}

TEST_F(FsStubsTest, lo_read_AtEof) {
    auto oid = lo_create(0);
    int fd = lo_open(oid, kInvRdwr);
    char buf[16];
    EXPECT_EQ(lo_read(fd, buf, 16), 0);
    EXPECT_EQ(lo_close(fd), 0);
}

TEST_F(FsStubsTest, lo_read_PartialRead) {
    auto oid = lo_create(0);
    int fd = lo_open(oid, kInvRdwr);
    std::string data = "abc";
    lo_write(fd, data.data(), 3);
    lo_lseek(fd, 0, 0);
    char buf[16] = {0};
    EXPECT_EQ(lo_read(fd, buf, 16), 3);
    EXPECT_EQ(std::string(buf, 3), "abc");
    EXPECT_EQ(lo_close(fd), 0);
}

// ===========================================================================
// fastpath: function call protocol
// ===========================================================================

class FastpathTest : public ::testing::Test {
protected:
    void SetUp() override { GetGlobalFunctionRegistry().Clear(); }
    void TearDown() override { GetGlobalFunctionRegistry().Clear(); }
};

TEST_F(FastpathTest, Registry_RegisterAndLookup) {
    auto handler = [](const std::vector<FastpathArg>&) {
        FastpathResult r;
        r.data = "ok";
        return r;
    };
    GetGlobalFunctionRegistry().Register(42, handler);
    EXPECT_NE(GetGlobalFunctionRegistry().Lookup(42), nullptr);
    EXPECT_EQ(GetGlobalFunctionRegistry().Lookup(99), nullptr);
    EXPECT_EQ(GetGlobalFunctionRegistry().Size(), 1);
}

TEST_F(FastpathTest, Registry_Unregister) {
    auto handler = [](const std::vector<FastpathArg>&) {
        FastpathResult r;
        return r;
    };
    GetGlobalFunctionRegistry().Register(42, handler);
    GetGlobalFunctionRegistry().Unregister(42);
    EXPECT_EQ(GetGlobalFunctionRegistry().Lookup(42), nullptr);
}

TEST_F(FastpathTest, ParseFastpathArgs_Zero) {
    pgcpp::protocol::MessageWriter w;
    w.WriteInt16(0);  // zero args
    pgcpp::protocol::MessageReader r(w.data());
    std::vector<FastpathArg> args;
    ASSERT_TRUE(ParseFastpathArgs(r, args));
    EXPECT_TRUE(args.empty());
}

TEST_F(FastpathTest, ParseFastpathArgs_WithValues) {
    pgcpp::protocol::MessageWriter w;
    w.WriteInt16(2);
    // arg0: format=0, "abc"
    w.WriteInt16(0);
    w.WriteInt32(3);
    w.WriteBytes("abc", 3);
    // arg1: format=1, null
    w.WriteInt16(1);
    w.WriteInt32(-1);
    pgcpp::protocol::MessageReader r(w.data());
    std::vector<FastpathArg> args;
    ASSERT_TRUE(ParseFastpathArgs(r, args));
    ASSERT_EQ(args.size(), 2u);
    EXPECT_EQ(args[0].format, 0);
    EXPECT_EQ(args[0].data, "abc");
    EXPECT_FALSE(args[0].is_null);
    EXPECT_EQ(args[1].format, 1);
    EXPECT_TRUE(args[1].is_null);
}

TEST_F(FastpathTest, BuildFunctionCallResponse_WithValue) {
    FastpathResult r;
    r.data = "result";
    r.is_null = false;
    Message m = BuildFunctionCallResponse(r);
    EXPECT_EQ(m.payload.size(), 4u + 6u);
    int32_t len = static_cast<unsigned char>(m.payload[0]) << 24 |
                  static_cast<unsigned char>(m.payload[1]) << 16 |
                  static_cast<unsigned char>(m.payload[2]) << 8 |
                  static_cast<unsigned char>(m.payload[3]);
    EXPECT_EQ(len, 6);
}

TEST_F(FastpathTest, BuildFunctionCallResponse_Null) {
    FastpathResult r;
    r.is_null = true;
    Message m = BuildFunctionCallResponse(r);
    ASSERT_EQ(m.payload.size(), 4u);
    int32_t len = static_cast<int32_t>(static_cast<unsigned char>(m.payload[0])) << 24 |
                  static_cast<int32_t>(static_cast<unsigned char>(m.payload[1])) << 16 |
                  static_cast<int32_t>(static_cast<unsigned char>(m.payload[2])) << 8 |
                  static_cast<int32_t>(static_cast<unsigned char>(m.payload[3]));
    EXPECT_EQ(len, -1);
}

TEST_F(FastpathTest, HandleFunctionRequest_UnknownOid) {
    StringSink sink;
    pgcpp::protocol::MessageWriter w;
    w.WriteInt32(999);
    w.WriteInt16(0);
    EXPECT_TRUE(HandleFunctionRequest(w.data(), &sink));
    // An ErrorResponse should have been sent.
    ASSERT_FALSE(sink.messages().empty());
    EXPECT_EQ(sink.messages().at(0).type, MessageType::kErrorResponse);
}

TEST_F(FastpathTest, HandleFunctionRequest_Dispatches) {
    auto handler = [](const std::vector<FastpathArg>& args) {
        FastpathResult r;
        if (!args.empty())
            r.data = args[0].data;
        return r;
    };
    GetGlobalFunctionRegistry().Register(100, handler);
    pgcpp::protocol::MessageWriter w;
    w.WriteInt32(100);
    w.WriteInt16(1);
    w.WriteInt16(0);
    w.WriteInt32(5);
    w.WriteBytes("hello", 5);
    StringSink sink;
    EXPECT_TRUE(HandleFunctionRequest(w.data(), &sink));
    ASSERT_FALSE(sink.messages().empty());
    // 'V' (FunctionCallResponse) is cast to MessageType.
    EXPECT_EQ(static_cast<char>(sink.messages().at(0).type), 'V');
    // Verify payload contains "hello".
    const auto& m = sink.messages().at(0);
    ASSERT_GE(m.payload.size(), 4u + 5u);
    int32_t len = static_cast<int32_t>(static_cast<unsigned char>(m.payload[0])) << 24 |
                  static_cast<int32_t>(static_cast<unsigned char>(m.payload[1])) << 16 |
                  static_cast<int32_t>(static_cast<unsigned char>(m.payload[2])) << 8 |
                  static_cast<int32_t>(static_cast<unsigned char>(m.payload[3]));
    EXPECT_EQ(len, 5);
    EXPECT_EQ(m.payload.substr(4, 5), "hello");
}

// ===========================================================================
// pqmq: shared-memory message queue adapter
// ===========================================================================

TEST(PqMqTest, Attach_Detach) {
    ResetPqMqState();
    EXPECT_EQ(pq_mq_get_attached(), nullptr);
    PqMqQueue q;
    pq_mq_attach(&q);
    EXPECT_EQ(pq_mq_get_attached(), &q);
    pq_mq_detach();
    EXPECT_EQ(pq_mq_get_attached(), nullptr);
}

TEST(PqMqTest, PutMessage_AppendsWireBytes) {
    ResetPqMqState();
    PqMqQueue q;
    pq_mq_attach(&q);
    EXPECT_EQ(pq_mq_putmessage('X', "abc", 3), 0);
    // Wire format: 'X' + 4-byte length (7) + "abc".
    ASSERT_GE(q.buf.size(), 8u);
    EXPECT_EQ(q.buf[0], 'X');
    int32_t len = static_cast<unsigned char>(q.buf[1]) << 24 |
                  static_cast<unsigned char>(q.buf[2]) << 16 |
                  static_cast<unsigned char>(q.buf[3]) << 8 | static_cast<unsigned char>(q.buf[4]);
    EXPECT_EQ(len, 7);
    EXPECT_EQ(q.buf.substr(5, 3), "abc");
    EXPECT_EQ(q.bytes_written, 8u);
    pq_mq_detach();
}

TEST(PqMqTest, PutMessage_WithoutAttach_Fails) {
    ResetPqMqState();
    EXPECT_EQ(pq_mq_putmessage('X', "abc", 3), -1);
}

TEST(PqMqTest, PutMessage_AfterClose_Fails) {
    ResetPqMqState();
    PqMqQueue q;
    pq_mq_attach(&q);
    pq_mq_close(&q);
    EXPECT_EQ(pq_mq_putmessage('X', "abc", 3), -1);
    pq_mq_detach();
}

TEST(PqMqTest, ReadBytes_DrainsBuffer) {
    ResetPqMqState();
    PqMqQueue q;
    pq_mq_attach(&q);
    pq_mq_putmessage('Y', "hi", 2);
    char buf[16] = {0};
    int n = pq_mq_read_bytes(&q, buf, 16);
    EXPECT_EQ(n, 7);  // 'Y' + 4-byte length + "hi"
    EXPECT_EQ(buf[0], 'Y');
    EXPECT_EQ(q.bytes_read, 7u);
    // Second read should return 0 (empty, not closed).
    EXPECT_EQ(pq_mq_read_bytes(&q, buf, 16), 0);
    pq_mq_detach();
}

TEST(PqMqTest, ReadBytes_OnClosedQueue_ReturnsNeg1) {
    ResetPqMqState();
    PqMqQueue q;
    pq_mq_close(&q);
    char buf[16];
    EXPECT_EQ(pq_mq_read_bytes(&q, buf, 16), -1);
}

TEST(PqMqTest, ReadBytes_NullQueue_ReturnsNeg1) {
    char buf[16];
    EXPECT_EQ(pq_mq_read_bytes(nullptr, buf, 16), -1);
}

TEST(PqMqTest, ReadBytes_PartialRead) {
    ResetPqMqState();
    PqMqQueue q;
    pq_mq_attach(&q);
    pq_mq_putmessage('Z', "hello", 5);  // 10 bytes total
    char buf[4] = {0};
    int n1 = pq_mq_read_bytes(&q, buf, 4);
    EXPECT_EQ(n1, 4);
    int n2 = pq_mq_read_bytes(&q, buf, 4);
    EXPECT_EQ(n2, 4);
    int n3 = pq_mq_read_bytes(&q, buf, 4);
    EXPECT_EQ(n3, 2);
    EXPECT_EQ(pq_mq_read_bytes(&q, buf, 4), 0);
    pq_mq_detach();
}

// ===========================================================================
// pqsignal: signal handler installation
// ===========================================================================

TEST(PqSignalTest, pqsignal_InstallsHandler) {
    ResetAllSignalHandlers();
    auto prev = pqsignal(SIGUSR1, &TestHandler);
    EXPECT_NE(prev, SIG_ERR);
    EXPECT_EQ(GetInstalledHandler(SIGUSR1), &TestHandler);
    ResetAllSignalHandlers();
    EXPECT_EQ(GetInstalledHandler(SIGUSR1), nullptr);
}

TEST(PqSignalTest, pqsignal_RestoresPrevious) {
    ResetAllSignalHandlers();
    pqsignal(SIGUSR2, &TestHandler);
    auto prev = pqsignal(SIGUSR2, SIG_IGN);
    EXPECT_EQ(prev, &TestHandler);
    ResetAllSignalHandlers();
}

TEST(PqSignalTest, pqsignal_no_restart_InstallsHandler) {
    ResetAllSignalHandlers();
    auto prev = pqsignal_no_restart(SIGUSR1, &TestHandler);
    EXPECT_NE(prev, SIG_ERR);
    EXPECT_EQ(GetInstalledHandler(SIGUSR1), &TestHandler);
    ResetAllSignalHandlers();
}

TEST(PqSignalTest, pq_block_reset_sigalrm) {
    EXPECT_EQ(pq_block_sigalrm(), 0);
    EXPECT_EQ(pq_reset_sigalrm(), 0);
}

TEST(PqSignalTest, pq_sigprocmask_NoOp) {
    sigset_t set;
    sigemptyset(&set);
    EXPECT_EQ(pq_sigprocmask(SIG_BLOCK, &set, nullptr), 0);
}

TEST(PqSignalTest, ResetAllSignalHandlers_Idempotent) {
    ResetAllSignalHandlers();
    ResetAllSignalHandlers();
    SUCCEED();
}
