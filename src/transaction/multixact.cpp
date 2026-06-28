// multixact.cpp — MultiXact ID management for shared row locks.
//
// Converted from PostgreSQL 15's src/backend/access/transam/multixact.cpp.
//
// A MultiXactId identifies a set of transactions that jointly hold a lock
// on a tuple (e.g., SELECT ... FOR SHARE creates a multixact). In PostgreSQL
// members are stored in pg_multixact/members and offsets in pg_multixact/offsets.
// MyToyDB keeps an in-memory vector of member-lists indexed by MultiXactId.
#include "pgcpp/transaction/multixact.hpp"

#include <vector>

namespace mytoydb::transaction {

namespace {

// In-memory multixact members: index by (MultiXactId - kFirstMultiXactId).
// Uses function-local statics to avoid the global initialization order
// fiasco (Google C++ Style).
std::vector<std::vector<MultiXactMember>>& MultiXactMembers() {
    static std::vector<std::vector<MultiXactMember>> members;
    return members;
}

// The next MultiXactId to assign. Starts at kFirstMultiXactId.
MultiXactId& NextMultiXactId() {
    static MultiXactId next = kFirstMultiXactId;
    return next;
}

}  // namespace

void InitializeMultiXact() {
    MultiXactMembers().clear();
    NextMultiXactId() = kFirstMultiXactId;
}

void ResetMultiXact() {
    MultiXactMembers().clear();
    NextMultiXactId() = kFirstMultiXactId;
}

MultiXactId MultiXactIdCreate(const std::vector<MultiXactMember>& members) {
    MultiXactId multi = NextMultiXactId()++;
    MultiXactMembers().push_back(members);
    return multi;
}

MultiXactId MultiXactIdExpand(MultiXactId multi, TransactionId xid, uint8_t status) {
    if (!MultiXactIdIsValid(multi)) {
        return multi;
    }
    std::vector<MultiXactMember>& members = MultiXactMembers()[multi - kFirstMultiXactId];
    for (const MultiXactMember& m : members) {
        if (m.xid == xid) {
            return multi;
        }
    }
    MultiXactMember member;
    member.xid = xid;
    member.status = status;
    members.push_back(member);
    return multi;
}

std::vector<MultiXactMember> MultiXactIdGetMembers(MultiXactId multi) {
    if (!MultiXactIdIsValid(multi)) {
        return {};
    }
    return MultiXactMembers()[multi - kFirstMultiXactId];
}

bool MultiXactIdIsValid(MultiXactId multi) {
    return multi != kInvalidMultiXactId && multi < NextMultiXactId();
}

MultiXactId GetNextMultiXactId() {
    return NextMultiXactId();
}

}  // namespace mytoydb::transaction
