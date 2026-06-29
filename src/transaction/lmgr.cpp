// lmgr.cpp — Relation-level lock convenience functions.
//
// Converted from PostgreSQL 15's src/backend/storage/lmgr/lmgr.cpp.
//
// Thin wrappers around the lock manager that build a LockTag from a
// relation OID and call LockAcquire/LockRelease.
#include "transaction/lmgr.hpp"

namespace pgcpp::transaction {

bool LockRelation(pgcpp::catalog::Oid relid, LockMode lockmode) {
    LockTag tag;
    tag.relid = relid;
    tag.locktag_type = kLockTagRelation;
    return LockAcquire(tag, lockmode, false);
}

bool UnlockRelation(pgcpp::catalog::Oid relid, LockMode lockmode) {
    LockTag tag;
    tag.relid = relid;
    tag.locktag_type = kLockTagRelation;
    return LockRelease(tag, lockmode);
}

void UnlockRelations(const std::vector<pgcpp::catalog::Oid>& relids, LockMode lockmode) {
    for (pgcpp::catalog::Oid relid : relids) {
        UnlockRelation(relid, lockmode);
    }
}

bool LockRelationIdForSession(pgcpp::catalog::Oid relid, LockMode lockmode) {
    LockTag tag;
    tag.relid = relid;
    tag.locktag_type = kLockTagRelation;
    return LockAcquire(tag, lockmode, true);
}

bool UnlockRelationIdForSession(pgcpp::catalog::Oid relid, LockMode lockmode) {
    LockTag tag;
    tag.relid = relid;
    tag.locktag_type = kLockTagRelation;
    return LockRelease(tag, lockmode);
}

}  // namespace pgcpp::transaction
