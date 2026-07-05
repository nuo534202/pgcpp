// multixact.h — MultiXact ID management for shared row locks.
//
// Converted from PostgreSQL 15's src/include/access/multixact.h.
//
// A MultiXactId identifies a set of transactions that jointly hold a lock
// on a tuple (e.g., SELECT ... FOR SHARE creates a multixact). PG stores
// multixact members in pg_multixact/members and offsets in pg_multixact/offsets.
//
// pgcpp uses two SLRUs to mirror PG's on-disk layout:
//   - offsets SLRU: one MultiXactOffset (4 bytes) per MultiXactId, pointing
//     to the start of the member list in the members SLRU.
//   - members SLRU: packed MultiXactMember entries (4 bytes XID + 1 byte
//     status, 8-byte stride for alignment).
// Both SLRUs are optionally persisted to <data_dir>/pg_multixact/{offsets,members}/.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "transaction/slru.hpp"
#include "transaction/transam.hpp"

namespace pgcpp::transaction {

// MultiXactMember — one transaction's lock membership in a multixact.
struct MultiXactMember {
    TransactionId xid = kInvalidTransactionId;
    uint8_t status = 0;  // matching PG's MultiXactStatus (lock mode)
};

// FirstMultiXactId — the first valid MultiXactId (PG: 1).
constexpr MultiXactId kFirstMultiXactId = 1;

// InvalidMultiXactId — sentinel for "no multixact".
constexpr MultiXactId kInvalidMultiXactId = 0;

// MultiXactOffsetsPerPage — 8 KB / 4 bytes = 2048 entries per page.
constexpr int kMultiXactOffsetsPerPage = kSlruPageSize / static_cast<int>(sizeof(MultiXactOffset));

// MultiXactMemberStride — bytes per member entry in the members SLRU.
// PG packs (xid, status) into 4 bytes (xid 32 bits + status 4 bits) but
// pgcpp uses an 8-byte stride (xid 4 bytes + status 1 byte + 3 pad) for
// simplicity and alignment safety.
constexpr int kMultiXactMemberStride = 8;

// MultiXactMembersPerPage — 8 KB / 8 bytes = 1024 entries per page.
constexpr int kMultiXactMembersPerPage = kSlruPageSize / kMultiXactMemberStride;

// InitializeMultiXact — set up the multixact subsystem.
// Call with empty dirs for in-memory operation (tests), or with
// <data_dir>/pg_multixact/{offsets,members} for persistence.
void InitializeMultiXact(const std::string& offsets_dir = "", const std::string& members_dir = "");

// ResetMultiXact — clear all multixact data and SLRU caches (for testing).
void ResetMultiXact();

// ShutdownMultiXact — flush dirty pages to disk.
void ShutdownMultiXact();

// FlushMultiXact — flush dirty pages to disk (called by checkpointer).
void FlushMultiXact();

// MultiXactIdCreate — create a new MultiXactId with the given members.
// Returns the new ID (1-based). The members are copied internally.
MultiXactId MultiXactIdCreate(const std::vector<MultiXactMember>& members);

// MultiXactIdExpand — add `xid` to an existing multixact. If `multi` already
// contains `xid`, returns `multi` unchanged. Otherwise creates a new
// MultiXactId (or reuses an existing one with the same membership).
MultiXactId MultiXactIdExpand(MultiXactId multi, TransactionId xid, uint8_t status);

// MultiXactIdGetMembers — look up the members of a MultiXactId.
// Returns an empty vector if the ID is invalid or unknown.
std::vector<MultiXactMember> MultiXactIdGetMembers(MultiXactId multi);

// MultiXactIdIsValid — true if the ID is a valid (allocated) multixact.
bool MultiXactIdIsValid(MultiXactId multi);

// GetNextMultiXactId — the next MultiXactId that will be allocated.
MultiXactId GetNextMultiXactId();

}  // namespace pgcpp::transaction
