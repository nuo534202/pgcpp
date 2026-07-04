// crypt.cpp — Password hashing and verification.
//
// Self-contained MD5, SHA-256, HMAC-SHA-256, and PBKDF2-SHA-256
// implementations (no OpenSSL dependency) so that the unit tests can run
// without external libraries.
//
// Format reference (PG 15):
//   * md5:        "md5" + 32 lowercase hex chars of MD5(password + username)
//   * scram:      "SCRAM-SHA-256$<iter>:<salt-b64>$<stored-key-b64>:<server-key-b64>"
//   * plain:      the cleartext password
#include "protocol/crypt.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace pgcpp::protocol {

namespace {

// ---------------------------------------------------------------------------
// MD5 (RFC 1321) — minimal implementation.
// ---------------------------------------------------------------------------

struct Md5Ctx {
    uint32_t state[4];
    uint64_t count;  // number of *bits* processed
    uint8_t buffer[64];
};

constexpr uint32_t kMd5T[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

constexpr int kMd5S[64] = {
    7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
    14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
    4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21,
};

inline uint32_t RotL32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

void Md5Init(Md5Ctx& c) {
    c.state[0] = 0x67452301;
    c.state[1] = 0xefcdab89;
    c.state[2] = 0x98badcfe;
    c.state[3] = 0x10325476;
    c.count = 0;
}

void Md5Transform(Md5Ctx& c, const uint8_t block[64]) {
    uint32_t a = c.state[0], b = c.state[1], cc = c.state[2], d = c.state[3];
    uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
        M[i] = static_cast<uint32_t>(block[i * 4]) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    }
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & cc) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & cc);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ cc ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = cc ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t temp = d;
        d = cc;
        cc = b;
        b = b + RotL32(a + f + kMd5T[i] + M[g], kMd5S[i]);
        a = temp;
    }
    c.state[0] += a;
    c.state[1] += b;
    c.state[2] += cc;
    c.state[3] += d;
}

void Md5Update(Md5Ctx& c, const uint8_t* data, size_t len) {
    size_t buf_used = (c.count / 8) % 64;
    c.count += static_cast<uint64_t>(len) * 8;
    if (buf_used > 0) {
        size_t need = 64 - buf_used;
        if (len < need) {
            std::memcpy(c.buffer + buf_used, data, len);
            return;
        }
        std::memcpy(c.buffer + buf_used, data, need);
        Md5Transform(c, c.buffer);
        data += need;
        len -= need;
    }
    while (len >= 64) {
        Md5Transform(c, data);
        data += 64;
        len -= 64;
    }
    if (len > 0) {
        std::memcpy(c.buffer, data, len);
    }
}

std::array<uint8_t, 16> Md5Final(Md5Ctx& c) {
    uint64_t bits = c.count;
    uint8_t pad[64];
    pad[0] = 0x80;
    for (size_t i = 1; i < 64; ++i)
        pad[i] = 0;
    size_t buf_used = (bits / 8) % 64;
    size_t pad_len = (buf_used < 56) ? (56 - buf_used) : (120 - buf_used);
    Md5Update(c, pad, pad_len);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) {
        len_bytes[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff);
    }
    Md5Update(c, len_bytes, 8);
    std::array<uint8_t, 16> out{};
    for (int i = 0; i < 4; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>(c.state[i] & 0xff);
        out[i * 4 + 1] = static_cast<uint8_t>((c.state[i] >> 8) & 0xff);
        out[i * 4 + 2] = static_cast<uint8_t>((c.state[i] >> 16) & 0xff);
        out[i * 4 + 3] = static_cast<uint8_t>((c.state[i] >> 24) & 0xff);
    }
    return out;
}

std::array<uint8_t, 16> Md5(const uint8_t* data, size_t len) {
    Md5Ctx c;
    Md5Init(c);
    Md5Update(c, data, len);
    return Md5Final(c);
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4) — minimal implementation.
// ---------------------------------------------------------------------------

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t RotR32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
};

void Sha256Init(Sha256Ctx& c) {
    c.state[0] = 0x6a09e667;
    c.state[1] = 0xbb67ae85;
    c.state[2] = 0x3c6ef372;
    c.state[3] = 0xa54ff53a;
    c.state[4] = 0x510e527f;
    c.state[5] = 0x9b05688c;
    c.state[6] = 0x1f83d9ab;
    c.state[7] = 0x5be0cd19;
    c.count = 0;
}

void Sha256Transform(Sha256Ctx& c, const uint8_t block[64]) {
    uint32_t W[64];
    for (int i = 0; i < 16; ++i) {
        W[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = RotR32(W[i - 15], 7) ^ RotR32(W[i - 15], 18) ^ (W[i - 15] >> 3);
        uint32_t s1 = RotR32(W[i - 2], 17) ^ RotR32(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + s0 + W[i - 7] + s1;
    }
    uint32_t a = c.state[0], b = c.state[1], cc = c.state[2], d = c.state[3];
    uint32_t e = c.state[4], f = c.state[5], g = c.state[6], h = c.state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = RotR32(e, 6) ^ RotR32(e, 11) ^ RotR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + kSha256K[i] + W[i];
        uint32_t S0 = RotR32(a, 2) ^ RotR32(a, 13) ^ RotR32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c.state[0] += a;
    c.state[1] += b;
    c.state[2] += cc;
    c.state[3] += d;
    c.state[4] += e;
    c.state[5] += f;
    c.state[6] += g;
    c.state[7] += h;
}

void Sha256Update(Sha256Ctx& c, const uint8_t* data, size_t len) {
    size_t buf_used = (c.count / 8) % 64;
    c.count += static_cast<uint64_t>(len) * 8;
    if (buf_used > 0) {
        size_t need = 64 - buf_used;
        if (len < need) {
            std::memcpy(c.buffer + buf_used, data, len);
            return;
        }
        std::memcpy(c.buffer + buf_used, data, need);
        Sha256Transform(c, c.buffer);
        data += need;
        len -= need;
    }
    while (len >= 64) {
        Sha256Transform(c, data);
        data += 64;
        len -= 64;
    }
    if (len > 0) {
        std::memcpy(c.buffer, data, len);
    }
}

std::array<uint8_t, 32> Sha256Final(Sha256Ctx& c) {
    uint64_t bits = c.count;
    uint8_t pad[64];
    pad[0] = 0x80;
    for (size_t i = 1; i < 64; ++i)
        pad[i] = 0;
    size_t buf_used = (bits / 8) % 64;
    size_t pad_len = (buf_used < 56) ? (56 - buf_used) : (120 - buf_used);
    Sha256Update(c, pad, pad_len);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) {
        len_bytes[i] = static_cast<uint8_t>((bits >> (56 - 8 * i)) & 0xff);
    }
    Sha256Update(c, len_bytes, 8);
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((c.state[i] >> 24) & 0xff);
        out[i * 4 + 1] = static_cast<uint8_t>((c.state[i] >> 16) & 0xff);
        out[i * 4 + 2] = static_cast<uint8_t>((c.state[i] >> 8) & 0xff);
        out[i * 4 + 3] = static_cast<uint8_t>(c.state[i] & 0xff);
    }
    return out;
}

std::array<uint8_t, 32> Sha256Raw(const uint8_t* data, size_t len) {
    Sha256Ctx c;
    Sha256Init(c);
    Sha256Update(c, data, len);
    return Sha256Final(c);
}

std::array<uint8_t, 32> HmacSha256Raw(const uint8_t* key, size_t key_len, const uint8_t* msg,
                                      size_t msg_len) {
    uint8_t k[64] = {0};
    if (key_len > 64) {
        auto h = Sha256Raw(key, key_len);
        std::memcpy(k, h.data(), 32);
    } else {
        std::memcpy(k, key, key_len);
    }
    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha256Ctx c;
    Sha256Init(c);
    Sha256Update(c, ipad, 64);
    Sha256Update(c, msg, msg_len);
    auto inner = Sha256Final(c);
    Sha256Init(c);
    Sha256Update(c, opad, 64);
    Sha256Update(c, inner.data(), 32);
    return Sha256Final(c);
}

std::array<uint8_t, 32> Pbkdf2HmacSha256Raw(const std::string& password, const uint8_t* salt,
                                            size_t salt_len, int iterations) {
    std::array<uint8_t, 32> out{};
    std::array<uint8_t, 32> u;
    std::array<uint8_t, 32> t;
    // PBKDF2 block #1 (32 bytes == dkLen, so only one block needed).
    std::vector<uint8_t> salt_block(salt_len + 4);
    std::memcpy(salt_block.data(), salt, salt_len);
    salt_block[salt_len + 0] = 0;
    salt_block[salt_len + 1] = 0;
    salt_block[salt_len + 2] = 0;
    salt_block[salt_len + 3] = 1;
    u = HmacSha256Raw(reinterpret_cast<const uint8_t*>(password.data()), password.size(),
                      salt_block.data(), salt_block.size());
    t = u;
    for (int i = 1; i < iterations; ++i) {
        u = HmacSha256Raw(reinterpret_cast<const uint8_t*>(password.data()), password.size(),
                          u.data(), u.size());
        for (size_t j = 0; j < 32; ++j) {
            t[j] ^= u[j];
        }
    }
    out = t;
    return out;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr char kHexChars[] = "0123456789abcdef";

std::string ToHex(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHexChars[(data[i] >> 4) & 0xf]);
        out.push_back(kHexChars[data[i] & 0xf]);
    }
    return out;
}

// Base64 alphabet (RFC 4648).
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int B64Index(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

// HMAC-SHA-256 for SCRAM ClientKey (used by ScramSha256ClientKey).
std::array<uint8_t, 32> HmacSha256Key(const std::vector<uint8_t>& key, const uint8_t* msg,
                                      size_t msg_len) {
    return HmacSha256Raw(key.data(), key.size(), msg, msg_len);
}

// SaltedPassword = PBKDF2(password, salt, iterations).
std::array<uint8_t, 32> ScramSaltedPassword(const std::string& password,
                                            const std::vector<uint8_t>& salt, int iterations) {
    return Pbkdf2HmacSha256Raw(password, salt.data(), salt.size(), iterations);
}

// Per RFC 5802:
//   ClientKey  = HMAC(SaltedPassword, "Client Key")
//   StoredKey  = H(ClientKey)
//   ServerKey  = HMAC(SaltedPassword, "Server Key")
constexpr const char* kClientKeyStr = "Client Key";
constexpr const char* kServerKeyStr = "Server Key";

std::array<uint8_t, 32> ScramClientKey(const std::array<uint8_t, 32>& salted) {
    return HmacSha256Raw(salted.data(), 32, reinterpret_cast<const uint8_t*>(kClientKeyStr),
                         std::strlen(kClientKeyStr));
}

std::array<uint8_t, 32> ScramServerKey(const std::array<uint8_t, 32>& salted) {
    return HmacSha256Raw(salted.data(), 32, reinterpret_cast<const uint8_t*>(kServerKeyStr),
                         std::strlen(kServerKeyStr));
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string Base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) | static_cast<uint32_t>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 0x3f]);
        out.push_back(kB64[(v >> 12) & 0x3f]);
        out.push_back(kB64[(v >> 6) & 0x3f]);
        out.push_back(kB64[v & 0x3f]);
        i += 3;
    }
    if (i + 1 == data.size()) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kB64[(v >> 18) & 0x3f]);
        out.push_back(kB64[(v >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == data.size()) {
        uint32_t v =
            (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kB64[(v >> 18) & 0x3f]);
        out.push_back(kB64[(v >> 12) & 0x3f]);
        out.push_back(kB64[(v >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

std::vector<uint8_t> Base64Decode(const std::string& s) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> buf;
    buf.reserve(s.size());
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ')
            continue;
        int v = B64Index(c);
        if (v < 0)
            return {};
        buf.push_back(static_cast<uint8_t>(v));
    }
    size_t i = 0;
    while (i + 4 <= buf.size()) {
        uint32_t v = (static_cast<uint32_t>(buf[i]) << 18) |
                     (static_cast<uint32_t>(buf[i + 1]) << 12) |
                     (static_cast<uint32_t>(buf[i + 2]) << 6) | static_cast<uint32_t>(buf[i + 3]);
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>(v & 0xff));
        i += 4;
    }
    if (i + 2 == buf.size()) {
        uint32_t v =
            (static_cast<uint32_t>(buf[i]) << 18) | (static_cast<uint32_t>(buf[i + 1]) << 12);
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    } else if (i + 3 == buf.size()) {
        uint32_t v = (static_cast<uint32_t>(buf[i]) << 18) |
                     (static_cast<uint32_t>(buf[i + 1]) << 12) |
                     (static_cast<uint32_t>(buf[i + 2]) << 6);
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    }
    return out;
}

std::string Md5Encrypt(const std::string& passwd, const std::string& username) {
    std::string input = passwd + username;
    auto digest = Md5(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    return "md5" + ToHex(digest.data(), 16);
}

std::string Md5Hex(const std::string& data) {
    auto digest = Md5(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return ToHex(digest.data(), 16);
}

std::vector<uint8_t> Sha256(const std::string& data) {
    auto digest = Sha256Raw(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return std::vector<uint8_t>(digest.begin(), digest.end());
}

std::vector<uint8_t> HmacSha256(const std::vector<uint8_t>& key, const std::string& msg) {
    auto digest = HmacSha256Raw(key.data(), key.size(),
                                reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    return std::vector<uint8_t>(digest.begin(), digest.end());
}

std::vector<uint8_t> Pbkdf2HmacSha256(const std::string& password, const std::vector<uint8_t>& salt,
                                      int iterations) {
    auto digest = Pbkdf2HmacSha256Raw(password, salt.data(), salt.size(), iterations);
    return std::vector<uint8_t>(digest.begin(), digest.end());
}

void ScramSha256Hash(const std::string& password, const std::string& salt, int iterations,
                     std::string& stored_key_b64, std::string& server_key_b64) {
    std::vector<uint8_t> salt_bytes(salt.begin(), salt.end());
    auto salted = ScramSaltedPassword(password, salt_bytes, iterations);
    auto client_key = ScramClientKey(salted);
    auto stored_key = Sha256Raw(client_key.data(), 32);
    auto server_key = ScramServerKey(salted);
    std::vector<uint8_t> sk(stored_key.begin(), stored_key.end());
    std::vector<uint8_t> svk(server_key.begin(), server_key.end());
    stored_key_b64 = Base64Encode(sk);
    server_key_b64 = Base64Encode(svk);
}

std::vector<uint8_t> ScramSha256ClientKey(const std::vector<uint8_t>& stored_key) {
    // The ClientKey cannot be derived from StoredKey alone (it's the other
    // direction). This function is provided for callers that already hold
    // the SaltedPassword-derived ClientKey; here we return the HMAC of the
    // given key over "Client Key" (used in tests to verify the HMAC shape).
    auto h = HmacSha256Key(stored_key, reinterpret_cast<const uint8_t*>(kClientKeyStr),
                           std::strlen(kClientKeyStr));
    return std::vector<uint8_t>(h.begin(), h.end());
}

bool ParsePasswordHash(const std::string& shadow_pass, PasswordHash& out) {
    out = PasswordHash{};
    out.raw = shadow_pass;
    if (shadow_pass.empty()) {
        out.method = PasswordEncryptionAlgorithm::kPlain;
        return true;
    }
    if (shadow_pass.size() >= 3 && shadow_pass.substr(0, 3) == "md5") {
        // md5 + 32 hex
        if (shadow_pass.size() != 35)
            return false;
        out.method = PasswordEncryptionAlgorithm::kMd5;
        out.md5_digest = shadow_pass.substr(3);
        for (char c : out.md5_digest) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        return true;
    }
    if (shadow_pass.substr(0, 14) == "SCRAM-SHA-256$") {
        // SCRAM-SHA-256$<iter>:<salt>$<storedkey>:<serverkey>
        std::string rest = shadow_pass.substr(14);
        size_t dollar = rest.find('$');
        if (dollar == std::string::npos)
            return false;
        std::string iter_salt = rest.substr(0, dollar);
        std::string keys = rest.substr(dollar + 1);
        size_t colon1 = iter_salt.find(':');
        if (colon1 == std::string::npos)
            return false;
        std::string iter_str = iter_salt.substr(0, colon1);
        std::string salt_b64 = iter_salt.substr(colon1 + 1);
        size_t colon2 = keys.find(':');
        if (colon2 == std::string::npos)
            return false;
        std::string stored_b64 = keys.substr(0, colon2);
        std::string server_b64 = keys.substr(colon2 + 1);
        try {
            out.scram_iterations = std::stoi(iter_str);
        } catch (...) {
            return false;
        }
        if (out.scram_iterations <= 0)
            return false;
        out.scram_salt_b64 = salt_b64;
        out.scram_storedkey_b64 = stored_b64;
        out.scram_serverkey_b64 = server_b64;
        out.method = PasswordEncryptionAlgorithm::kScramSha256;
        return true;
    }
    // Plain text (no recognised prefix).
    out.method = PasswordEncryptionAlgorithm::kPlain;
    return true;
}

bool CryptVerify(const PasswordHash& hash, const std::string& password,
                 const std::string& username) {
    switch (hash.method) {
        case PasswordEncryptionAlgorithm::kPlain:
            return password == hash.raw;
        case PasswordEncryptionAlgorithm::kMd5: {
            std::string computed = Md5Encrypt(password, username);
            // Strip "md5" prefix for comparison.
            return computed.substr(3) == hash.md5_digest;
        }
        case PasswordEncryptionAlgorithm::kScramSha256: {
            std::vector<uint8_t> salt = Base64Decode(hash.scram_salt_b64);
            if (salt.empty() && !hash.scram_salt_b64.empty())
                return false;
            auto salted = ScramSaltedPassword(password, salt, hash.scram_iterations);
            auto client_key = ScramClientKey(salted);
            auto stored_key = Sha256Raw(client_key.data(), 32);
            std::vector<uint8_t> sk(stored_key.begin(), stored_key.end());
            std::string computed_b64 = Base64Encode(sk);
            return computed_b64 == hash.scram_storedkey_b64;
        }
    }
    return false;
}

std::string EncryptPassword(const std::string& password, const std::string& username,
                            PasswordEncryptionAlgorithm method, const std::string& salt) {
    switch (method) {
        case PasswordEncryptionAlgorithm::kPlain:
            return password;
        case PasswordEncryptionAlgorithm::kMd5:
            return Md5Encrypt(password, username);
        case PasswordEncryptionAlgorithm::kScramSha256: {
            std::string salt_str = salt;
            if (salt_str.empty()) {
                // Default salt for pgcpp: 16 bytes of "0x01..0x10" (deterministic).
                salt_str.resize(16);
                for (int i = 0; i < 16; ++i) {
                    salt_str[i] = static_cast<char>(i + 1);
                }
            }
            const int kDefaultIter = 4096;  // PG 15 default.
            std::string stored_b64, server_b64;
            ScramSha256Hash(password, salt_str, kDefaultIter, stored_b64, server_b64);
            std::ostringstream oss;
            oss << "SCRAM-SHA-256$" << kDefaultIter << ":"
                << Base64Encode(std::vector<uint8_t>(salt_str.begin(), salt_str.end())) << "$"
                << stored_b64 << ":" << server_b64;
            return oss.str();
        }
    }
    return password;
}

}  // namespace pgcpp::protocol
