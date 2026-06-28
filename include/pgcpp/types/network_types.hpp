#pragma once

#include <cstdint>

#include "pgcpp/types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// Network address family — inet, cidr, macaddr, macaddr8.
//
// inet (OID 86)  : a host or network address (with optional netmask bits).
// cidr (OID 650) : a *network* address (host bits must be zero).
// macaddr (OID 829)   : 6-byte EUI-48 MAC address.
// macaddr8 (OID 774)  : 8-byte EUI-64 MAC address.
//
// Storage: a palloc'd NetworkAddr struct (Datum is a pointer). inet and cidr
// share the same struct; the cidr_* input variants additionally enforce that
// the bits after the netmask are zero.
// ---------------------------------------------------------------------------

enum class IpFamily : uint8_t {
    kIpv4 = 4,  // AF_INET
    kIpv6 = 6,  // AF_INET6
};

struct NetworkAddr {
    IpFamily family;
    uint8_t netmask_bits;  // 0..32 for IPv4, 0..128 for IPv6
    uint8_t bytes[16];     // 4 bytes used for IPv4, 16 for IPv6
};

// inet input/output (accepts both "192.168.1.1/24" and "/24" forms).
Datum inet_in(const char* str);
char* inet_out(Datum value);
int inet_cmp(Datum a, Datum b);
Datum inet_eq(Datum a, Datum b);
Datum inet_lt(Datum a, Datum b);
Datum inet_le(Datum a, Datum b);
Datum inet_gt(Datum a, Datum b);
Datum inet_ge(Datum a, Datum b);

// cidr — like inet but host bits must be zero.
Datum cidr_in(const char* str);
char* cidr_out(Datum value);
int cidr_cmp(Datum a, Datum b);
Datum cidr_eq(Datum a, Datum b);
Datum cidr_lt(Datum a, Datum b);

// macaddr — 6-byte EUI-48 ("08:00:2b:01:02:03" or "08-00-2b-01-02-03").
struct MacAddr {
    uint8_t bytes[6];
};

Datum macaddr_in(const char* str);
char* macaddr_out(Datum value);
int macaddr_cmp(Datum a, Datum b);
Datum macaddr_eq(Datum a, Datum b);

// macaddr8 — 8-byte EUI-64 ("08:00:2b:01:02:03:04:05").
struct MacAddr8 {
    uint8_t bytes[8];
};

Datum macaddr8_in(const char* str);
char* macaddr8_out(Datum value);
int macaddr8_cmp(Datum a, Datum b);
Datum macaddr8_eq(Datum a, Datum b);

// Helper: allocate a NetworkAddr datum and copy the contents in.
Datum MakeInetDatum(const NetworkAddr& addr);
const NetworkAddr* DatumGetInet(Datum value);

// Helper: allocate a macaddr datum.
Datum MakeMacAddrDatum(const MacAddr& mac);
const MacAddr* DatumGetMacAddr(Datum value);
Datum MakeMacAddr8Datum(const MacAddr8& mac);
const MacAddr8* DatumGetMacAddr8(Datum value);

}  // namespace pgcpp::types
