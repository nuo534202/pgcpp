// ssi.cpp — Serializable Snapshot Isolation (SSI) conflict detection.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/predicate.c
// (specifically the rw-conflict tracking and dangerous-structure detection).
//
// pgcpp is single-process: there is no real concurrency. However, the
// conflict-detection machinery is preserved and is testable by directly
// registering multiple "concurrent" serializable transactions, simulating
// reads via predicate locks, and simulating writes via
// CheckForSerializableConflict.
#include "transaction/ssi.hpp"

#include <cstdio>
#include <vector>

#include "common/error/elog.hpp"
#include "storage/block.hpp"
#include "storage/ipc/predicate.hpp"

namespace pgcpp::transaction {

namespace {

std::vector<SERIALIZABLEXact>& Xacts() {
    static std::vector<SERIALIZABLEXact> v;
    return v;
}

// Find the SSI state record for `xid`, or nullptr if not registered.
SERIALIZABLEXact* FindXact(TransactionId xid) {
    for (auto& sx : Xacts()) {
        if (sx.xid == xid) {
            return &sx;
        }
    }
    return nullptr;
}

// Add a directed rw-conflict edge reader → writer to both endpoints'
// conflict lists (idempotent: duplicate edges are silently dropped).
void AddRWConflict(TransactionId reader, TransactionId writer) {
    if (reader == writer) {
        return;  // a transaction never conflicts with itself
    }
    SERIALIZABLEXact* r = FindXact(reader);
    SERIALIZABLEXact* w = FindXact(writer);
    if (r == nullptr || w == nullptr) {
        return;
    }
    // De-duplicate by (reader, writer).
    for (const auto& c : r->out_conflicts) {
        if (c.writer == writer) {
            return;
        }
    }
    r->out_conflicts.push_back({reader, writer});
    w->in_conflicts.push_back({reader, writer});
}

// MarkDangerous — flag the three participants of a dangerous structure
// so that OnConflict_CheckForSerializationFailure can abort them at the
// next check.
void MarkDangerous(TransactionId t1, TransactionId t2, TransactionId t3) {
    SERIALIZABLEXact* sx1 = FindXact(t1);
    SERIALIZABLEXact* sx2 = FindXact(t2);
    SERIALIZABLEXact* sx3 = FindXact(t3);
    if (sx1 != nullptr) {
        sx1->dangerous = true;
    }
    if (sx2 != nullptr) {
        sx2->dangerous = true;
    }
    if (sx3 != nullptr) {
        sx3->dangerous = true;
    }
}

}  // namespace

void RegisterSerializableTransaction(TransactionId xid, bool read_only) {
    if (FindXact(xid) != nullptr) {
        return;  // idempotent
    }
    SERIALIZABLEXact sx;
    sx.xid = xid;
    sx.read_only = read_only;
    Xacts().push_back(std::move(sx));
}

void ReleaseSerializableTransaction(TransactionId xid, bool committed) {
    SERIALIZABLEXact* sx = FindXact(xid);
    if (sx == nullptr) {
        return;
    }
    if (committed && !sx->finished) {
        // PreCommit_CheckForSerializationFailure: a committing transaction
        // may complete a dangerous structure (Case A: this tx is T2 in
        // the middle, with T1 already committed and T3 still active; or
        // Case C: this tx is T1, with T2 and T3 already committed).
        CheckForDangerousStructure(xid);
        OnConflict_CheckForSerializationFailure(xid);  // throws if dangerous
    }
    sx->finished = true;
    sx->committed = committed;
    // PostgreSQL keeps a committed transaction's predicate locks around
    // (transferred to the "old" predicate lock list) until no active
    // transaction's snapshot could conflict with them. pgcpp is single-
    // process, so we keep them until ResetSSIState for correctness of
    // the conflict-detection algorithm. Aborted transactions discard
    // their locks (their reads didn't happen).
    if (!committed) {
        pgcpp::storage::PredicateLockRelease(xid);
    }
}

void CheckForSerializableConflict(TransactionId writer_xid,
                                  const pgcpp::storage::RelFileNode& rnode, uint32_t block_num,
                                  uint16_t offset_num) {
    SERIALIZABLEXact* writer = FindXact(writer_xid);
    if (writer == nullptr || writer->finished || writer->read_only) {
        return;  // not serializable, already done, or read-only (no writes)
    }

    // Inspect every predicate lock covering the write target. Each one
    // held by a different serializable transaction becomes a new
    // reader → writer conflict edge.
    auto locks = pgcpp::storage::GetPredicateLocks();
    for (const auto& lock : locks) {
        if (lock.xid == writer_xid) {
            continue;  // own lock, not a conflict
        }
        SERIALIZABLEXact* reader = FindXact(lock.xid);
        if (reader == nullptr) {
            continue;
        }
        // A committed reader's predicate lock still conflicts (its read
        // happened before its commit, which precedes this write). An
        // aborted reader's locks were already released, so we won't see
        // them here — but defend against the case anyway.
        if (reader->finished && !reader->committed) {
            continue;  // aborted: reads didn't happen
        }
        // PostgreSQL's MatchesTag check: relation covers all, page covers
        // all tuples on page, tuple is exact. Re-implement inline to keep
        // this module independent of predicate.cpp's static helpers.
        bool covered = false;
        if (lock.tag.rnode == rnode) {
            if (lock.tag.block_num == pgcpp::storage::kInvalidBlockNumber) {
                covered = true;  // relation-level lock
            } else if (lock.tag.block_num == block_num && lock.tag.offset_num == 0) {
                covered = true;  // page-level lock
            } else if (lock.tag.block_num == block_num && lock.tag.offset_num == offset_num) {
                covered = true;  // tuple-level lock
            }
        }
        if (!covered) {
            continue;
        }
        AddRWConflict(lock.xid, writer_xid);
        CheckForDangerousStructure(writer_xid);
        OnConflict_CheckForSerializationFailure(writer_xid);
    }
}

bool CheckForDangerousStructure(TransactionId xid) {
    SERIALIZABLEXact* sx = FindXact(xid);
    if (sx == nullptr) {
        return false;
    }

    // Dangerous structure detection (PostgreSQL's PreCommit_CheckFor-
    // SerializationFailure / CheckForDangerousStructures). The new edge
    // either made `xid` a writer (someone → xid) or a reader (xid →
    // someone). We look for the canonical T1 → T2 → T3 antichain.
    //
    // Case A: xid is T2 (middle). T1 → xid (in_conflict) and xid → T3
    // (out_conflict), where T1 finished before xid's write and xid
    // finished before T3's write. For pgcpp we approximate "finished
    // before" by checking that T1 has finished.
    for (const auto& in : sx->in_conflicts) {
        SERIALIZABLEXact* t1 = FindXact(in.reader);
        if (t1 == nullptr) {
            continue;
        }
        for (const auto& out : sx->out_conflicts) {
            SERIALIZABLEXact* t3 = FindXact(out.writer);
            if (t3 == nullptr) {
                continue;
            }
            // T1 must precede T2 (xid) in commit order: T1 finished.
            if (!t1->finished) {
                continue;
            }
            // T2 (xid) must precede T3: T2 finished, or T3 is still
            // active and we are about to commit T2 (caller handles).
            if (!sx->finished && !t3->finished) {
                // Both active — still potentially dangerous; flag it.
            }
            MarkDangerous(t1->xid, xid, t3->xid);
            return true;
        }
    }

    // Case B: xid is T3 (right). T2 → xid (in_conflict) and T1 → T2
    // (T2's in_conflict), where T1 finished and T2 finished.
    for (const auto& in : sx->in_conflicts) {
        SERIALIZABLEXact* t2 = FindXact(in.reader);
        if (t2 == nullptr || !t2->finished) {
            continue;
        }
        for (const auto& t2in : t2->in_conflicts) {
            SERIALIZABLEXact* t1 = FindXact(t2in.reader);
            if (t1 == nullptr || !t1->finished) {
                continue;
            }
            MarkDangerous(t1->xid, t2->xid, xid);
            return true;
        }
    }

    // Case C: xid is T1 (left). xid → T2 (out_conflict) and T2 → T3
    // (T2's out_conflict), where T2 finished and we (T1) finished.
    if (!sx->finished) {
        return false;  // T1 must have finished first
    }
    for (const auto& out : sx->out_conflicts) {
        SERIALIZABLEXact* t2 = FindXact(out.writer);
        if (t2 == nullptr || !t2->finished) {
            continue;
        }
        for (const auto& t2out : t2->out_conflicts) {
            SERIALIZABLEXact* t3 = FindXact(t2out.writer);
            if (t3 == nullptr) {
                continue;
            }
            MarkDangerous(xid, t2->xid, t3->xid);
            return true;
        }
    }

    return false;
}

void OnConflict_CheckForSerializationFailure(TransactionId xid) {
    SERIALIZABLEXact* sx = FindXact(xid);
    if (sx == nullptr || !sx->dangerous) {
        return;
    }
    // PostgreSQL: ERRCODE_T_R_SERIALIZATION_FAILURE (40001).
    // pgcpp's ereport doesn't carry SQLSTATE yet; the message carries
    // enough context for tests and clients to identify the failure.
    char errbuf[256];
    std::snprintf(errbuf, sizeof(errbuf),
                  "could not serialize access due to read/write dependencies "
                  "among transactions (transaction XID %u)",
                  xid);
    ereport(pgcpp::error::LogLevel::kError, errbuf);
}

SERIALIZABLEXact* GetSerializableXact(TransactionId xid) {
    return FindXact(xid);
}

int NumSerializableXacts() {
    return static_cast<int>(Xacts().size());
}

void ResetSSIState() {
    Xacts().clear();
    pgcpp::storage::PredicateLockReleaseAll();
}

}  // namespace pgcpp::transaction
