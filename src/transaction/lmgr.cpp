// lmgr.cpp — Relation-level lock convenience functions.
//
// Converted from PostgreSQL 15's src/backend/storage/lmgr/lmgr.cpp.
//
// Thin wrappers around the lock manager that build a LockTag from a
// relation OID and call LockAcquire/LockRelease.
#include "mytoydb/transaction/lmgr.h"

namespace mytoydb::transaction {

bool LockRelation(mytoydb::catalog::Oid relid, LockMode lockmode) {
    LockTag tag;
    tag.relid = relid;
    tag.locktag_type = kLockTagRelation;
    return LockAcquire(tag, lockmode, false);
}

bool UnlockRelation(mytoydb::catalog::Oid relid, LockMode lockmode) {
    LockTag tag;
    tag.relid = relid;
    tag.locktag_type = kLockTagRelation;
    return LockRelease(tag, lockmode);
}

void UnlockRelations(const std::vector<mytoydb::catalog::Oid>& relids,
                     LockMode lockmode) {
    for (mytoydb::catalog::Oid relid : relids) {
        UnlockRelation(relid, lockmode);
    }
}

bool LockRelationIdForSession(mytoydb::catalog::Oid relid, LockMode lockmode) {
    LockTag tag;
    tag.relid = relid;
    tag.locktag_type = kLockTagRelation;
    return LockAcquire(tag, lockmode, true);
}

bool UnlockRelationIdForSession(mytoydb::catalog::Oid relid, LockMode lockmode) {
    LockTag tag;
    tag.relid = relid;
    tag.locktag_type = kLockTagRelation;
    return LockRelease(tag, lockmode);
}

}  // namespace mytoydb::transaction
