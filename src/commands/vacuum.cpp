// vacuum.cpp — VACUUM command implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/vacuum.c.
// pgcpp's MVCC implementation marks dead tuples in-place during DML
// (heap_delete sets t_xmax; heap_update sets t_xmax + inserts a new
// version). VACUUM reclaims physical space by:
//   1. Scanning every page of the target relation.
//   2. Marking line pointers of surely-dead tuples as LP_DEAD.
//   3. Compacting the page via PageRepairFragmentation (moves live items
//      to the end, reclaims the gap).
//
// VACUUM FREEZE additionally freezes old committed tuples:
//   4. For each live tuple whose t_xmin is committed and older than the
//      freeze limit, replace t_xmin with FrozenTransactionId and set the
//      HEAP_XMIN_FROZEN hint. This prevents XID wraparound: frozen tuples
//      no longer require commit-log lookups.
//   5. After a full scan, advance pg_class.relfrozenxid to the freeze
//      limit, recording that all tuples older than this have been frozen.
//
// A tuple is "surely dead" when its deleting transaction (t_xmax) is
// committed and older than the oldest XID still running (OldestXmin) —
// i.e., no snapshot can ever see it again.
//
// A tuple is "freezable" when its inserting transaction (t_xmin) is
// committed and older than the freeze limit. The freeze limit is:
//   - VACUUM FREEZE: OldestXmin (freeze everything committed & not running)
//   - normal VACUUM: OldestXmin - vacuum_freeze_min_age (only very old XIDs)
#include "commands/vacuum.hpp"

#include <string>

#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/namespace.hpp"
#include "catalog/pg_class.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"
#include "storage/bufmgr.hpp"
#include "storage/bufpage.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/procarray.hpp"
#include "transaction/transam.hpp"
#include "transaction/visibility.hpp"

namespace pgcpp::commands {

using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationGetNumberOfBlocks;
using pgcpp::access::RelationGetSmgr;
using pgcpp::access::RelationOpen;
using pgcpp::catalog::CatalogTupleUpdate;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RangeVarGetRelid;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::RangeVar;
using pgcpp::parser::VacuumStmt;
using pgcpp::storage::BlockNumber;
using pgcpp::storage::Buffer;
using pgcpp::storage::BufferGetPage;
using pgcpp::storage::ForkNumber;
using pgcpp::storage::ItemIdIsNormal;
using pgcpp::storage::kLPDead;
using pgcpp::storage::MarkBufferDirty;
using pgcpp::storage::OffsetNumber;
using pgcpp::storage::Page;
using pgcpp::storage::PageGetItem;
using pgcpp::storage::PageGetItemId;
using pgcpp::storage::PageGetMaxOffsetNumber;
using pgcpp::storage::PageRepairFragmentation;
using pgcpp::storage::ReadBuffer;
using pgcpp::storage::ReadBufferMode;
using pgcpp::storage::ReleaseBuffer;
using pgcpp::transaction::GetNextTransactionId;
using pgcpp::transaction::GetOldestXmin;
using pgcpp::transaction::HeapTupleHeaderData;
using pgcpp::transaction::HeapTupleHeaderGetXmin;
using pgcpp::transaction::HeapTupleHeaderGetXminStatus;
using pgcpp::transaction::HeapTupleHeaderSetXmin;
using pgcpp::transaction::HeapTupleIsSurelyDead;
using pgcpp::transaction::kFrozenTransactionId;
using pgcpp::transaction::kHeapXminCommitted;
using pgcpp::transaction::kHeapXminFrozen;
using pgcpp::transaction::kHeapXminInvalid;
using pgcpp::transaction::SnapshotData;
using pgcpp::transaction::TransactionId;
using pgcpp::transaction::TransactionIdDidCommit;
using pgcpp::transaction::TransactionIdIsNormal;
using pgcpp::transaction::TransactionIdPrecedes;
using pgcpp::transaction::XactStatus;

// Freeze a tuple's t_xmin if it is committed and older than freeze_limit.
// Returns true if the tuple was frozen (xmin was replaced).
static bool FreezeTupleIfNeeded(HeapTupleHeaderData* header, TransactionId freeze_limit) {
    if (header == nullptr) {
        return false;
    }

    // Already frozen — nothing to do.
    if (HeapTupleHeaderGetXminStatus(header) == XactStatus::kFrozen) {
        return false;
    }

    TransactionId xmin = HeapTupleHeaderGetXmin(header);

    // Special XIDs (bootstrap) are already treated as committed/frozen.
    if (!TransactionIdIsNormal(xmin)) {
        return false;
    }

    // Not old enough to freeze.
    if (!TransactionIdPrecedes(xmin, freeze_limit)) {
        return false;
    }

    // Only freeze committed inserters. Aborted inserters mean the tuple is
    // dead and will be reclaimed by the dead-tuple pass; in-progress XIDs
    // are never frozen.
    if (!TransactionIdDidCommit(xmin)) {
        return false;
    }

    // Freeze: replace t_xmin with FrozenTransactionId and set hint flags.
    HeapTupleHeaderSetXmin(header, kFrozenTransactionId);
    header->t_infomask |= kHeapXminFrozen | kHeapXminCommitted;
    header->t_infomask &= ~kHeapXminInvalid;
    return true;
}

TransactionId VacuumGetFreezeLimit(TransactionId oldest_xmin, bool aggressive) {
    // VACUUM FREEZE: freeze everything older than OldestXmin.
    if (aggressive) {
        return oldest_xmin;
    }

    // Normal VACUUM: freeze only XIDs older than (OldestXmin - min_age).
    // Guard against underflow for small oldest_xmin values (e.g., in tests
    // where oldest_xmin may be near FirstNormalTransactionId). If the
    // subtraction would underflow below FirstNormalTransactionId, fall back
    // to oldest_xmin — this freezes tuples committed by previous transactions
    // (xmin < oldest_xmin), matching PostgreSQL's conservative behavior.
    if (oldest_xmin < static_cast<TransactionId>(kVacuumFreezeMinAge) +
                          pgcpp::transaction::kFirstNormalTransactionId) {
        return oldest_xmin;  // conservative: same as aggressive at low XIDs
    }
    return oldest_xmin - static_cast<TransactionId>(kVacuumFreezeMinAge);
}

bool RelationNeedsVacuumForWraparound(Oid relid) {
    Relation rel = RelationOpen(relid);
    if (rel == nullptr || rel->rd_rel == nullptr) {
        return false;
    }

    TransactionId relfrozenxid = rel->rd_rel->relfrozenxid;
    RelationClose(rel);

    // A relation with no frozen XID recorded (0 or invalid) is not at risk.
    if (!TransactionIdIsNormal(relfrozenxid)) {
        return false;
    }

    // Age = (nextXid - relfrozenxid) using modular subtraction (XIDs wrap
    // at 2^32). nextXid here is the next XID to be assigned (= last + 1).
    TransactionId next_xid = GetNextTransactionId() + 1;
    uint32_t age = next_xid - relfrozenxid;
    return age > static_cast<uint32_t>(kVacuumFreezeMaxAge);
}

std::string ExecVacuum(VacuumStmt* stmt, VacuumStats* stats) {
    if (stats != nullptr) {
        *stats = VacuumStats{};
    }

    if (stmt == nullptr || stmt->rels.empty()) {
        // No relation specified — pgcpp does not implement VACUUM ALL.
        return stmt != nullptr && !stmt->is_vacuumcmd ? "ANALYZE" : "VACUUM";
    }

    // Resolve the first target relation to an OID. pgcpp's VACUUM supports
    // a single relation per call (PG loops over all; we keep it simple).
    auto* rangevar = dynamic_cast<RangeVar*>(stmt->rels.front());
    if (rangevar == nullptr) {
        return stmt->is_vacuumcmd ? "VACUUM" : "ANALYZE";
    }
    Oid relid = RangeVarGetRelid(rangevar, /*failOK=*/true);
    if (relid == pgcpp::catalog::kInvalidOid) {
        return stmt->is_vacuumcmd ? "VACUUM" : "ANALYZE";
    }

    Relation rel = RelationOpen(relid);
    if (rel == nullptr) {
        return stmt->is_vacuumcmd ? "VACUUM" : "ANALYZE";
    }

    // OldestXmin: no tuple deleted by a transaction older than this can still
    // be visible to any running backend. Tuples with committed t_xmax <
    // OldestXmin are surely dead.
    TransactionId oldest_xmin = GetOldestXmin();

    // Build a minimal SnapshotData for HeapTupleIsSurelyDead. The function
    // only consults xmin/xmax of the snapshot (not xip), so a minimal snapshot
    // with xmin=oldest_xmin and xmax=nextXid+1 suffices.
    SnapshotData snap;
    snap.xmin = oldest_xmin;
    snap.xmax = GetNextTransactionId() + 1;

    // Compute the freeze limit for this VACUUM run.
    TransactionId freeze_limit = VacuumGetFreezeLimit(oldest_xmin, stmt->freeze);

    rel->rd_smgr = RelationGetSmgr(rel);
    BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

    for (BlockNumber blk = 0; blk < nblocks; ++blk) {
        if (stats != nullptr) {
            ++stats->pages_scanned;
        }

        Buffer buf = ReadBuffer(rel->rd_smgr, ForkNumber::kMain, blk, ReadBufferMode::kNormal);
        Page page = BufferGetPage(buf);

        // Mark dead line pointers as LP_DEAD, and freeze eligible tuples.
        OffsetNumber max_off = PageGetMaxOffsetNumber(page);
        bool has_dead = false;
        bool page_modified = false;
        for (OffsetNumber off = 1; off <= max_off; ++off) {
            auto* item_id = PageGetItemId(page, off);
            if (!ItemIdIsNormal(item_id))
                continue;
            auto* header = reinterpret_cast<HeapTupleHeaderData*>(PageGetItem(page, item_id));

            // Dead-tuple pass: reclaim tuples no longer visible to anyone.
            if (HeapTupleIsSurelyDead(header, snap)) {
                item_id->li_flags = kLPDead;
                has_dead = true;
                if (stats != nullptr) {
                    ++stats->tuples_dead_reclaimed;
                }
                continue;
            }

            // Live tuple — count it and attempt to freeze.
            if (stats != nullptr) {
                ++stats->tuples_live;
            }

            if (FreezeTupleIfNeeded(header, freeze_limit)) {
                page_modified = true;
                if (stats != nullptr) {
                    ++stats->tuples_frozen;
                }
            }
        }

        if (has_dead) {
            // Compact the page: moves live items to the end, resets dead
            // line pointers to unused, and packs the line pointer array.
            PageRepairFragmentation(page);
            MarkBufferDirty(buf);
        } else if (page_modified) {
            // Only hint/xmin changes (freeze) — still need to mark dirty.
            MarkBufferDirty(buf);
        }
        ReleaseBuffer(buf);
    }

    // Advance relfrozenxid in pg_class if we froze tuples (or if the freeze
    // limit is newer than the current relfrozenxid). This is safe because
    // we scanned every page: any tuple with xmin < freeze_limit is now
    // either frozen or reclaimed (dead).
    if (rel->rd_rel != nullptr) {
        TransactionId current_frozen = rel->rd_rel->relfrozenxid;
        // Only advance if freeze_limit is newer (follows) the current value.
        if (TransactionIdPrecedes(current_frozen, freeze_limit) ||
            !TransactionIdIsNormal(current_frozen)) {
            // Build an updated pg_class row (copy the existing one, bump
            // relfrozenxid) and persist via CatalogTupleUpdate.
            auto* updated = makePallocNode<FormData_pg_class>(*rel->rd_rel);
            updated->relfrozenxid = freeze_limit;
            CatalogTupleUpdate(relid, updated);
            if (stats != nullptr) {
                stats->relfrozenxid_advanced = true;
            }
        }
    }

    RelationClose(rel);
    return stmt->is_vacuumcmd ? "VACUUM" : "ANALYZE";
}

}  // namespace pgcpp::commands
