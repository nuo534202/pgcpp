// ifaddr.cpp — Network interface address enumeration.
//
// Uses getifaddrs(3) on Linux to list interface (addr, netmask) pairs.
// Provides IsSameHost/IsSameNet matchers for the "samehost" and "samenet"
// keywords in pg_hba.conf, plus CheckIpMatch used by hba.c.
#include "pgcpp/protocol/ifaddr.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>

#include <cstring>
#include <string>
#include <vector>

namespace pgcpp::protocol {

bool ParseIPv4(const std::string& s, uint32_t& out) {
    struct in_addr addr;
    if (inet_pton(AF_INET, s.c_str(), &addr) != 1) {
        return false;
    }
    out = ntohl(addr.s_addr);
    return true;
}

std::string FormatIPv4(uint32_t v) {
    struct in_addr addr;
    addr.s_addr = htonl(v);
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return std::string(buf);
}

std::string MaskBitsToIPv4(int bits) {
    if (bits < 0)
        bits = 0;
    if (bits > 32)
        bits = 32;
    uint32_t mask = bits == 0 ? 0u : (0xffffffffu << (32 - bits));
    return FormatIPv4(mask);
}

std::vector<InterfaceAddress> ListInterfaceAddresses() {
    std::vector<InterfaceAddress> out;
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) {
        return out;
    }
    for (struct ifaddrs* p = ifap; p != nullptr; p = p->ifa_next) {
        if (p->ifa_addr == nullptr)
            continue;
        if (p->ifa_addr->sa_family != AF_INET)
            continue;  // IPv4 only
        auto* sa = reinterpret_cast<struct sockaddr_in*>(p->ifa_addr);
        char addr_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, addr_buf, sizeof(addr_buf));
        InterfaceAddress ia;
        ia.addr = addr_buf;
        ia.name = p->ifa_name;
        if (p->ifa_netmask != nullptr) {
            auto* nm = reinterpret_cast<struct sockaddr_in*>(p->ifa_netmask);
            char nm_buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &nm->sin_addr, nm_buf, sizeof(nm_buf));
            ia.netmask = nm_buf;
        }
        out.push_back(std::move(ia));
    }
    freeifaddrs(ifap);
    return out;
}

bool IsSameHost(const std::string& client_addr) {
    uint32_t c;
    if (!ParseIPv4(client_addr, c))
        return false;
    for (const auto& ia : ListInterfaceAddresses()) {
        uint32_t a;
        if (ParseIPv4(ia.addr, a) && a == c) {
            return true;
        }
    }
    return false;
}

bool IsSameNet(const std::string& client_addr) {
    uint32_t c;
    if (!ParseIPv4(client_addr, c))
        return false;
    for (const auto& ia : ListInterfaceAddresses()) {
        uint32_t a, m;
        if (!ParseIPv4(ia.addr, a))
            continue;
        if (!ParseIPv4(ia.netmask, m))
            continue;
        if ((c & m) == (a & m)) {
            return true;
        }
    }
    return false;
}

bool CheckIpMatch(const std::string& token, const std::string& client_addr) {
    if (token == "samehost")
        return IsSameHost(client_addr);
    if (token == "samenet")
        return IsSameNet(client_addr);
    // CIDR form: "addr/bits".
    size_t slash = token.find('/');
    if (slash == std::string::npos) {
        // Bare IP — treat as /32.
        uint32_t c, t;
        if (!ParseIPv4(client_addr, c))
            return false;
        if (!ParseIPv4(token, t))
            return false;
        return c == t;
    }
    std::string addr_part = token.substr(0, slash);
    int bits = 0;
    try {
        bits = std::stoi(token.substr(slash + 1));
    } catch (...) {
        return false;
    }
    uint32_t c, t;
    if (!ParseIPv4(client_addr, c))
        return false;
    if (!ParseIPv4(addr_part, t))
        return false;
    if (bits == 0)
        return true;
    if (bits > 32)
        return false;
    uint32_t mask = 0xffffffffu << (32 - bits);
    return (c & mask) == (t & mask);
}

}  // namespace pgcpp::protocol
