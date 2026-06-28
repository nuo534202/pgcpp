// network_types.cpp — inet/cidr/macaddr/macaddr8 implementations.
//
// Mirrors PostgreSQL's utils/adt/network.c, mac.c, mac8.c with a simplified
// in-memory NetworkAddr struct.

#include "mytoydb/types/network_types.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"

namespace mytoydb::types {

using mytoydb::error::LogLevel;
using mytoydb::memory::palloc;

namespace {

char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}
bool IsHex(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

// Parse a single IPv4 dotted-quad address. On success, fills `bytes` (4 bytes)
// and returns true; otherwise returns false. `it` is advanced past the
// address (excluding any netmask separator).
bool ParseIpv4(std::string_view s, std::size_t& it, uint8_t bytes[4]) {
    for (int i = 0; i < 4; ++i) {
        if (it >= s.size() || !IsDigit(s[it])) {
            return false;
        }
        int val = 0;
        int count = 0;
        while (it < s.size() && IsDigit(s[it]) && count < 3) {
            val = val * 10 + (s[it] - '0');
            ++it;
            ++count;
        }
        if (val > 255) {
            return false;
        }
        bytes[i] = static_cast<uint8_t>(val);
        if (i < 3) {
            if (it >= s.size() || s[it] != '.') {
                return false;
            }
            ++it;  // skip '.'
        }
    }
    return true;
}

// Parse a single IPv6 address into 16 bytes. Accepts the standard forms
// including "::" compression, but NOT IPv4-in-IPv6 mapping (we keep this
// deliberately minimal).
bool ParseIpv6(std::string_view s, std::size_t& it, uint8_t bytes[16]) {
    for (int i = 0; i < 16; ++i) {
        bytes[i] = 0;
    }
    std::size_t start = it;
    int groups[8] = {0};
    int n_groups = 0;
    int double_colon_pos = -1;
    if (it < s.size() && s[it] == ':' && it + 1 < s.size() && s[it + 1] == ':') {
        double_colon_pos = 0;
        it += 2;
    }
    while (n_groups < 8) {
        // Try to read 1-4 hex digits.
        int val = 0;
        int count = 0;
        while (it < s.size() && IsHex(s[it]) && count < 4) {
            val = val * 16 + HexVal(s[it]);
            ++it;
            ++count;
        }
        if (count == 0) {
            break;
        }
        groups[n_groups++] = val;
        // Check for "::"
        if (it < s.size() && s[it] == ':' && it + 1 < s.size() && s[it + 1] == ':') {
            if (double_colon_pos != -1) {
                return false;  // two '::'
            }
            double_colon_pos = n_groups;
            it += 2;
            continue;
        }
        // Expect a ':' separator if there are more groups.
        if (it < s.size() && s[it] == ':') {
            ++it;
        } else {
            break;
        }
    }
    if (n_groups == 0 && double_colon_pos == -1) {
        return false;
    }
    // Expand the '::' zero groups.
    if (double_colon_pos != -1) {
        int zero_count = 8 - n_groups;
        for (int i = n_groups - 1; i >= double_colon_pos; --i) {
            groups[i + zero_count] = groups[i];
        }
        for (int i = double_colon_pos; i < double_colon_pos + zero_count; ++i) {
            groups[i] = 0;
        }
    }
    if (double_colon_pos == -1 && n_groups != 8) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        bytes[i * 2] = static_cast<uint8_t>((groups[i] >> 8) & 0xff);
        bytes[i * 2 + 1] = static_cast<uint8_t>(groups[i] & 0xff);
    }
    (void)start;
    return true;
}

// Format the IPv4 portion of bytes[0..3] into a 16-byte buffer.
std::string FormatIpv4(const uint8_t bytes[4]) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    return buf;
}

// Format the IPv6 bytes (16 bytes) into compressed form.
std::string FormatIpv6(const uint8_t bytes[16]) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%x:%x:%x:%x:%x:%x:%x:%x", (bytes[0] << 8) | bytes[1],
                  (bytes[2] << 8) | bytes[3], (bytes[4] << 8) | bytes[5],
                  (bytes[6] << 8) | bytes[7], (bytes[8] << 8) | bytes[9],
                  (bytes[10] << 8) | bytes[11], (bytes[12] << 8) | bytes[13],
                  (bytes[14] << 8) | bytes[15]);
    return buf;
}

// Parse a string of the form "address[/netmask]" into a NetworkAddr.
// `require_network` true enforces host bits zero (cidr behaviour).
Datum ParseNetworkAddress(std::string_view s, bool require_network, const char* type_name) {
    NetworkAddr addr{};
    addr.netmask_bits = 0;
    std::size_t it = 0;
    bool ipv6 = (s.size() > 0 && s[0] == ':');
    if (ipv6) {
        addr.family = IpFamily::kIpv6;
        if (!ParseIpv6(s, it, addr.bytes)) {
            ereport(LogLevel::kError, std::string("invalid input syntax for type ") + type_name +
                                          ": \"" + std::string(s) + "\"");
        }
    } else {
        addr.family = IpFamily::kIpv4;
        if (!ParseIpv4(s, it, addr.bytes)) {
            ereport(LogLevel::kError, std::string("invalid input syntax for type ") + type_name +
                                          ": \"" + std::string(s) + "\"");
        }
    }
    int max_bits = (addr.family == IpFamily::kIpv4) ? 32 : 128;
    if (it < s.size() && s[it] == '/') {
        ++it;
        int val = 0;
        int count = 0;
        while (it < s.size() && IsDigit(s[it]) && count < 3) {
            val = val * 10 + (s[it] - '0');
            ++it;
            ++count;
        }
        if (count == 0 || val > max_bits) {
            ereport(LogLevel::kError, std::string("invalid netmask in ") + type_name + ": \"" +
                                          std::string(s) + "\"");
        }
        addr.netmask_bits = static_cast<uint8_t>(val);
    } else {
        addr.netmask_bits = static_cast<uint8_t>(max_bits);
    }
    if (it < s.size()) {
        ereport(LogLevel::kError,
                std::string("trailing garbage in ") + type_name + ": \"" + std::string(s) + "\"");
    }
    if (require_network) {
        // Verify host bits are zero.
        int total_bits = (addr.family == IpFamily::kIpv4) ? 32 : 128;
        for (int bit = addr.netmask_bits; bit < total_bits; ++bit) {
            int byte_idx = bit / 8;
            int bit_idx = 7 - (bit % 8);
            if (addr.bytes[byte_idx] & (1u << bit_idx)) {
                ereport(LogLevel::kError, std::string("invalid cidr value — host bits set: \"") +
                                              std::string(s) + "\"");
            }
        }
    }
    return MakeInetDatum(addr);
}

}  // namespace

Datum MakeInetDatum(const NetworkAddr& addr) {
    auto* p = static_cast<NetworkAddr*>(palloc(sizeof(NetworkAddr)));
    *p = addr;
    return reinterpret_cast<Datum>(p);
}

const NetworkAddr* DatumGetInet(Datum value) {
    return reinterpret_cast<const NetworkAddr*>(value);
}

Datum inet_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type inet: NULL");
    }
    return ParseNetworkAddress(str, false, "inet");
}

char* inet_out(Datum value) {
    const auto* addr = DatumGetInet(value);
    std::string s;
    if (addr->family == IpFamily::kIpv4) {
        s = FormatIpv4(addr->bytes);
        if (addr->netmask_bits != 32) {
            s += "/" + std::to_string(addr->netmask_bits);
        }
    } else {
        s = FormatIpv6(addr->bytes);
        if (addr->netmask_bits != 128) {
            s += "/" + std::to_string(addr->netmask_bits);
        }
    }
    return PallocCString(s);
}

int inet_cmp(Datum a, Datum b) {
    const auto* x = DatumGetInet(a);
    const auto* y = DatumGetInet(b);
    if (x->family != y->family) {
        return (static_cast<int>(x->family) < static_cast<int>(y->family)) ? -1 : 1;
    }
    int n = (x->family == IpFamily::kIpv4) ? 4 : 16;
    int cmp = std::memcmp(x->bytes, y->bytes, static_cast<std::size_t>(n));
    if (cmp != 0) {
        return (cmp < 0) ? -1 : 1;
    }
    if (x->netmask_bits != y->netmask_bits) {
        return (x->netmask_bits < y->netmask_bits) ? -1 : 1;
    }
    return 0;
}

Datum inet_eq(Datum a, Datum b) {
    return BoolGetDatum(inet_cmp(a, b) == 0);
}
Datum inet_lt(Datum a, Datum b) {
    return BoolGetDatum(inet_cmp(a, b) < 0);
}
Datum inet_le(Datum a, Datum b) {
    return BoolGetDatum(inet_cmp(a, b) <= 0);
}
Datum inet_gt(Datum a, Datum b) {
    return BoolGetDatum(inet_cmp(a, b) > 0);
}
Datum inet_ge(Datum a, Datum b) {
    return BoolGetDatum(inet_cmp(a, b) >= 0);
}

Datum cidr_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type cidr: NULL");
    }
    return ParseNetworkAddress(str, true, "cidr");
}

char* cidr_out(Datum value) {
    return inet_out(value);
}
int cidr_cmp(Datum a, Datum b) {
    return inet_cmp(a, b);
}
Datum cidr_eq(Datum a, Datum b) {
    return BoolGetDatum(cidr_cmp(a, b) == 0);
}
Datum cidr_lt(Datum a, Datum b) {
    return BoolGetDatum(cidr_cmp(a, b) < 0);
}

// --- macaddr / macaddr8 ---

namespace {

// Parse a MAC address of either ':' or '-' separated form into `bytes`.
// `expected` is the number of expected bytes (6 or 8).
bool ParseMac(std::string_view s, uint8_t bytes[], int expected) {
    std::size_t it = 0;
    char sep = 0;
    for (int i = 0; i < expected; ++i) {
        if (it + 2 > s.size() || !IsHex(s[it]) || !IsHex(s[it + 1])) {
            return false;
        }
        bytes[i] = static_cast<uint8_t>(HexVal(s[it]) * 16 + HexVal(s[it + 1]));
        it += 2;
        if (i < expected - 1) {
            if (it >= s.size()) {
                return false;
            }
            if (s[it] == ':' || s[it] == '-') {
                if (sep == 0) {
                    sep = s[it];
                } else if (sep != s[it]) {
                    return false;
                }
                ++it;
            } else {
                return false;
            }
        }
    }
    return it == s.size();
}

}  // namespace

Datum MakeMacAddrDatum(const MacAddr& mac) {
    auto* p = static_cast<MacAddr*>(palloc(sizeof(MacAddr)));
    *p = mac;
    return reinterpret_cast<Datum>(p);
}

const MacAddr* DatumGetMacAddr(Datum value) {
    return reinterpret_cast<const MacAddr*>(value);
}

Datum MakeMacAddr8Datum(const MacAddr8& mac) {
    auto* p = static_cast<MacAddr8*>(palloc(sizeof(MacAddr8)));
    *p = mac;
    return reinterpret_cast<Datum>(p);
}

const MacAddr8* DatumGetMacAddr8(Datum value) {
    return reinterpret_cast<const MacAddr8*>(value);
}

Datum macaddr_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type macaddr: NULL");
    }
    MacAddr mac{};
    if (!ParseMac(str, mac.bytes, 6)) {
        ereport(LogLevel::kError,
                "invalid input syntax for type macaddr: \"" + std::string(str) + "\"");
    }
    return MakeMacAddrDatum(mac);
}

char* macaddr_out(Datum value) {
    const auto* mac = DatumGetMacAddr(value);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac->bytes[0], mac->bytes[1],
                  mac->bytes[2], mac->bytes[3], mac->bytes[4], mac->bytes[5]);
    return PallocCString(buf);
}

int macaddr_cmp(Datum a, Datum b) {
    const auto* x = DatumGetMacAddr(a);
    const auto* y = DatumGetMacAddr(b);
    int cmp = std::memcmp(x->bytes, y->bytes, 6);
    return (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
}

Datum macaddr_eq(Datum a, Datum b) {
    return BoolGetDatum(macaddr_cmp(a, b) == 0);
}

Datum macaddr8_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type macaddr8: NULL");
    }
    MacAddr8 mac{};
    if (!ParseMac(str, mac.bytes, 8)) {
        ereport(LogLevel::kError,
                "invalid input syntax for type macaddr8: \"" + std::string(str) + "\"");
    }
    return MakeMacAddr8Datum(mac);
}

char* macaddr8_out(Datum value) {
    const auto* mac = DatumGetMacAddr8(value);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", mac->bytes[0],
                  mac->bytes[1], mac->bytes[2], mac->bytes[3], mac->bytes[4], mac->bytes[5],
                  mac->bytes[6], mac->bytes[7]);
    return PallocCString(buf);
}

int macaddr8_cmp(Datum a, Datum b) {
    const auto* x = DatumGetMacAddr8(a);
    const auto* y = DatumGetMacAddr8(b);
    int cmp = std::memcmp(x->bytes, y->bytes, 8);
    return (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
}

Datum macaddr8_eq(Datum a, Datum b) {
    return BoolGetDatum(macaddr8_cmp(a, b) == 0);
}

}  // namespace mytoydb::types
