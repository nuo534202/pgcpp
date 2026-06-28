// ifaddr.h — Network interface address enumeration (ifaddr.c).
//
// Converted from PostgreSQL 15's src/backend/libpq/ifaddr.c.
//
// PG's pg_hba.conf supports matching a client IP against the addresses of
// the server's own network interfaces (using the "samehost" and "samenet"
// keywords in the ADDRESS column). To do this, the server must enumerate
// its interfaces and their netmasks.
//
// pgcpp uses getifaddrs(3) (Linux) to enumerate interfaces. The API is:
//   - ListInterfaceAddresses: return all (addr, netmask) pairs.
//   - IsSameHost: true if `client_addr` matches one of the server's
//     interface addresses.
//   - IsSameNet: true if `client_addr` is in the same subnet as one of the
//     server's interfaces.
//   - CheckIpMatch: a combined matcher used by hba.c.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::protocol {

// InterfaceAddress — one (address, netmask) pair from getifaddrs().
struct InterfaceAddress {
    std::string addr;     // IPv4 dotted-quad (e.g. "192.168.1.5")
    std::string netmask;  // IPv4 dotted-quad (e.g. "255.255.255.0")
    std::string name;     // interface name (e.g. "eth0")
};

// ListInterfaceAddresses — enumerate the local network interfaces.
// Returns an empty vector if getifaddrs() fails.
std::vector<InterfaceAddress> ListInterfaceAddresses();

// IsSameHost — true if `client_addr` exactly matches one of the server's
// interface addresses (PG's "samehost" rule).
bool IsSameHost(const std::string& client_addr);

// IsSameNet — true if `client_addr` is in the same subnet as one of the
// server's interfaces (PG's "samenet" rule). Uses the interface's netmask.
bool IsSameNet(const std::string& client_addr);

// CheckIpMatch — combined matcher used by hba.c.
// Returns true if:
//   - `token` is "samehost" and IsSameHost(client_addr), or
//   - `token` is "samenet"  and IsSameNet(client_addr), or
//   - `token` is a CIDR and client_addr falls within it.
// Returns false otherwise (including malformed tokens).
bool CheckIpMatch(const std::string& token, const std::string& client_addr);

// --- IP parsing helpers (also used by hba.c) ---

// ParseIPv4 — parse a dotted-quad string into a uint32_t (host byte order).
// Returns false on parse error.
bool ParseIPv4(const std::string& s, uint32_t& out);

// FormatIPv4 — format a uint32_t (host byte order) as a dotted-quad.
std::string FormatIPv4(uint32_t v);

// MaskBitsToIPv4 — convert a prefix length (0-32) to a netmask in dotted-quad.
std::string MaskBitsToIPv4(int bits);

}  // namespace pgcpp::protocol
