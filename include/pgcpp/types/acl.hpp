#pragma once

#include <cstdint>
#include <vector>

#include "pgcpp/types/datum.hpp"

namespace mytoydb::types {

// ---------------------------------------------------------------------------
// Access Control List (PostgreSQL utils/adt/acl.c).
//
// An aclitem stores a (grantee_oid, grantor_oid, privilege_bits) tuple.
// An ACL is a varlena-like sequence of aclitems. For MyToyDB we expose a
// simplified in-memory AclData: a vector of AclItem with a total length.
//
// Privilege bits and their character codes follow PostgreSQL's acl.h
// canonical order (so acl_out emits the canonical "arwdDxtXUcT" sequence
// when all privileges are present):
//   INSERT=1<<0 ('a'), SELECT=1<<1 ('r'), UPDATE=1<<2 ('w'),
//   DELETE=1<<3 ('d'), TRUNCATE=1<<4 ('D'), REFERENCES=1<<5 ('x'),
//   TRIGGER=1<<6 ('t'), EXECUTE=1<<7 ('X'), USAGE=1<<8 ('U'),
//   CREATE=1<<9 ('C'), CONNECT=1<<10 ('c'), TEMPORARY=1<<11 ('T').
// ---------------------------------------------------------------------------

constexpr uint32_t kAclInsert = 1u;
constexpr uint32_t kAclSelect = 1u << 1;
constexpr uint32_t kAclUpdate = 1u << 2;
constexpr uint32_t kAclDelete = 1u << 3;
constexpr uint32_t kAclTruncate = 1u << 4;
constexpr uint32_t kAclReferences = 1u << 5;
constexpr uint32_t kAclTrigger = 1u << 6;
constexpr uint32_t kAclExecute = 1u << 7;
constexpr uint32_t kAclUsage = 1u << 8;
constexpr uint32_t kAclCreate = 1u << 9;
constexpr uint32_t kAclConnect = 1u << 10;
constexpr uint32_t kAclTemp = 1u << 11;

struct AclItem {
    uint32_t grantee_oid;
    uint32_t grantor_oid;
    uint32_t privilege_bits;
};

struct AclData {
    std::vector<AclItem> items;
};

// Parse ACL literal of the form "{user=arwdRxt/grantor,...}".
Datum acl_in(const char* str);
char* acl_out(Datum value);

// Helpers.
Datum MakeAclDatum(const AclData& acl);
inline AclData* DatumGetAcl(Datum x) {
    return reinterpret_cast<AclData*>(x);
}

// Convenience: build a single-item ACL from raw fields.
Datum MakeAclItemDatum(uint32_t grantee, uint32_t grantor, uint32_t privileges);

}  // namespace mytoydb::types
