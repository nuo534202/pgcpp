// acl.cpp — access control list implementation.
//
// Mirrors PostgreSQL's utils/adt/acl.c with a simplified in-memory AclData.

#include "pgcpp/types/acl.hpp"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

namespace pgcpp::types {

using pgcpp::error::LogLevel;
using pgcpp::memory::MemoryContext;
using pgcpp::memory::palloc;

namespace {

char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

// Decode a privilege letter into a privilege bit. Returns 0 on unknown.
uint32_t PrivCharToBit(char c) {
    switch (c) {
        case 'a':
            return kAclInsert;
        case 'r':
            return kAclSelect;
        case 'w':
            return kAclUpdate;
        case 'd':
            return kAclDelete;
        case 'D':
            return kAclTruncate;
        case 'x':
            return kAclReferences;
        case 't':
            return kAclTrigger;
        case 'X':
            return kAclExecute;
        case 'U':
            return kAclUsage;
        case 'C':
            return kAclCreate;
        case 'c':
            return kAclConnect;
        case 'T':
            return kAclTemp;
        default:
            return 0;
    }
}

char PrivBitToChar(uint32_t bit) {
    switch (bit) {
        case kAclInsert:
            return 'a';
        case kAclSelect:
            return 'r';
        case kAclUpdate:
            return 'w';
        case kAclDelete:
            return 'd';
        case kAclTruncate:
            return 'D';
        case kAclReferences:
            return 'x';
        case kAclTrigger:
            return 't';
        case kAclExecute:
            return 'X';
        case kAclUsage:
            return 'U';
        case kAclCreate:
            return 'C';
        case kAclConnect:
            return 'c';
        case kAclTemp:
            return 'T';
        default:
            return '?';
    }
}

// Parse a non-negative integer using strtoul. Returns false on overflow or
// invalid input.
bool ParseUInt(const std::string& s, uint32_t& out) {
    if (s.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if (errno == ERANGE || *end != '\0' || end == s.c_str()) {
        return false;
    }
    if (v > 0xffffffffu) {
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

}  // namespace

Datum MakeAclDatum(const AclData& acl) {
    auto* p = static_cast<AclData*>(palloc(sizeof(AclData)));
    new (p) AclData(acl);
    MemoryContext* ctx = pgcpp::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        ctx->RegisterDestructor(p, [](void* o) { static_cast<AclData*>(o)->~AclData(); });
    }
    return reinterpret_cast<Datum>(p);
}

Datum MakeAclItemDatum(uint32_t grantee, uint32_t grantor, uint32_t privileges) {
    AclData acl;
    acl.items.push_back({grantee, grantor, privileges});
    return MakeAclDatum(acl);
}

Datum acl_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type aclitem[]: NULL");
    }
    std::string_view s(str);
    std::size_t it = 0;
    while (it < s.size() && (s[it] == '{' || std::isspace(static_cast<unsigned char>(s[it])))) {
        ++it;
    }
    AclData acl{};
    while (it < s.size() && s[it] != '}') {
        // Format: grantee_oid=privs/grantor_oid
        AclItem item{};
        std::size_t start = it;
        while (it < s.size() && s[it] != '=' && s[it] != ',' && s[it] != '}') {
            ++it;
        }
        if (it >= s.size() || s[it] != '=') {
            // No '=' -> grantee is empty.
            it = start;
            item.grantee_oid = 0;
        } else {
            std::string grantee_str(s.substr(start, it - start));
            if (grantee_str.empty()) {
                item.grantee_oid = 0;
            } else {
                uint32_t oid = 0;
                if (ParseUInt(grantee_str, oid)) {
                    item.grantee_oid = oid;
                } else {
                    // For test simplicity: hash the name into an OID. PG would
                    // resolve via pg_authid.oid; we use a deterministic hash.
                    uint32_t hash = 0;
                    for (char c : grantee_str) {
                        hash = hash * 31 + static_cast<unsigned char>(c);
                    }
                    item.grantee_oid = 0x80000000u | hash;
                }
            }
            ++it;  // skip '='
        }
        // Privileges.
        item.privilege_bits = 0;
        while (it < s.size() && s[it] != '/' && s[it] != ',' && s[it] != '}') {
            char c = s[it];
            if (c == '*') {
                // With-grant-option marker — ignored for tests.
                ++it;
                continue;
            }
            uint32_t bit = PrivCharToBit(c);
            if (bit == 0) {
                ereport(LogLevel::kError, std::string("unknown ACL privilege letter: '") + c + "'");
            }
            item.privilege_bits |= bit;
            ++it;
        }
        if (it < s.size() && s[it] == '/') {
            ++it;
            std::size_t g_start = it;
            while (it < s.size() && s[it] != ',' && s[it] != '}') {
                ++it;
            }
            std::string grantor_str(s.substr(g_start, it - g_start));
            if (grantor_str.empty()) {
                item.grantor_oid = 0;
            } else {
                uint32_t oid = 0;
                if (ParseUInt(grantor_str, oid)) {
                    item.grantor_oid = oid;
                } else {
                    uint32_t hash = 0;
                    for (char c : grantor_str) {
                        hash = hash * 31 + static_cast<unsigned char>(c);
                    }
                    item.grantor_oid = 0x80000000u | hash;
                }
            }
        } else {
            item.grantor_oid = 0;
        }
        acl.items.push_back(item);
        // Skip optional ',' separator.
        while (it < s.size() && (s[it] == ',' || std::isspace(static_cast<unsigned char>(s[it])))) {
            ++it;
        }
    }
    return MakeAclDatum(acl);
}

char* acl_out(Datum value) {
    const auto* acl = DatumGetAcl(value);
    std::string out = "{";
    for (std::size_t i = 0; i < acl->items.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        const auto& item = acl->items[i];
        if (item.grantee_oid != 0) {
            out += std::to_string(item.grantee_oid);
        }
        out.push_back('=');
        for (uint32_t bit = 1; bit != 0; bit <<= 1) {
            if (item.privilege_bits & bit) {
                out.push_back(PrivBitToChar(bit));
            }
        }
        out.push_back('/');
        if (item.grantor_oid != 0) {
            out += std::to_string(item.grantor_oid);
        }
    }
    out.push_back('}');
    return PallocCString(out);
}

}  // namespace pgcpp::types
