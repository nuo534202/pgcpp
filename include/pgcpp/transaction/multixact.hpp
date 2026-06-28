// multixact.h — MultiXact ID management for shared row locks.
//
// Converted from PostgreSQL 15's src/include/access/multixact.h.
//
// A MultiXactId identifies a set of transactions that jointly hold a lock
// on a tuple (e.g., SELECT ... FOR SHARE creates a multixact). PG stores
// multixact members in pg_multixact/members and offsets in pg_multixact/offsets.
//
// MyToyDB keeps an in-memory vector of member-lists indexed by MultiXactId.
#pragma once

#include <cstdint>
#include <vector>

#include "pgcpp/transaction/transam.hpp"

namespace mytoydb::transaction {

// MultiXactMember — one transaction's lock membership in a multixact.
struct MultiXactMember {
    TransactionId xid = kInvalidTransactionId;
    uint8_t status = 0;  // matching PG's MultiXactStatus (lock mode)
};

// FirstMultiXactId — the first valid MultiXactId (PG: 1).
constexpr MultiXactId kFirstMultiXactId = 1;

// InvalidMultiXactId — sentinel for "no multixact".
constexpr MultiXactId kInvalidMultiXactId = 0;

// InitializeMultiXact — set up the multixact subsystem (clear the table).
void InitializeMultiXact();

// ResetMultiXact — clear all multixact data (for testing).
void ResetMultiXact();

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

}  // namespace mytoydb::transaction
