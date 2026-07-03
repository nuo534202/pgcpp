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
// A tuple is "surely dead" when its deleting transaction (t_xmax) is
// committed and older than the oldest XID still running (OldestXmin) —
// i.e., no snapshot can ever see it again.
#include "commands/vacuum.hpp"

#include <string>

#include "access/rel.hpp"
#include "catalog/namespace.hpp"
#include "catalog/pg_class.hpp"
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
using pgcpp::catalog::Oid;
using pgcpp::catalog::RangeVarGetRelid;
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
using pgcpp::transaction::HeapTupleIsSurelyDead;
using pgcpp::transaction::SnapshotData;
using pgcpp::transaction::TransactionId;

std::string ExecVacuum(VacuumStmt* stmt) {
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

    rel->rd_smgr = RelationGetSmgr(rel);
    BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

    for (BlockNumber blk = 0; blk < nblocks; ++blk) {
        Buffer buf = ReadBuffer(rel->rd_smgr, ForkNumber::kMain, blk,
                                ReadBufferMode::kNormal);
        Page page = BufferGetPage(buf);

        // Mark dead line pointers as LP_DEAD.
        OffsetNumber max_off = PageGetMaxOffsetNumber(page);
        bool has_dead = false;
        for (OffsetNumber off = 1; off <= max_off; ++off) {
            auto* item_id = PageGetItemId(page, off);
            if (!ItemIdIsNormal(item_id))
                continue;
            auto* header = reinterpret_cast<HeapTupleHeaderData*>(
                PageGetItem(page, item_id));
            if (HeapTupleIsSurelyDead(header, snap)) {
                item_id->li_flags = kLPDead;
                has_dead = true;
            }
        }

        if (has_dead) {
            // Compact the page: moves live items to the end, resets dead
            // line pointers to unused, and packs the line pointer array.
            PageRepairFragmentation(page);
            MarkBufferDirty(buf);
        }
        ReleaseBuffer(buf);
    }

    RelationClose(rel);
    return stmt->is_vacuumcmd ? "VACUUM" : "ANALYZE";
}

}  // namespace pgcpp::commands
