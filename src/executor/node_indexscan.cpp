// node_indexscan.cpp — Index scan node implementation.
//
// Converted from PostgreSQL 15's src/backend/executor/nodeIndexscan.c.
//
// The IndexScan node uses a B-tree index to find matching TIDs, then
// fetches the corresponding heap tuples. It supports point lookups
// (equality) and range scans.
#include "pgcpp/executor/node_indexscan.hpp"

#include <new>

#include "pgcpp/access/heapam.hpp"
#include "pgcpp/access/nbtpage.hpp"
#include "pgcpp/access/nbtree.hpp"
#include "pgcpp/access/rel.hpp"
#include "pgcpp/catalog/pg_operator.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/executor/estate.hpp"
#include "pgcpp/executor/exec_expr.hpp"
#include "pgcpp/executor/exec_utils.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/executor/tupletable.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/primnodes.hpp"
#include "pgcpp/transaction/heap_tuple.hpp"
#include "pgcpp/types/datum.hpp"

namespace mytoydb::executor {

using mytoydb::access::BTKeyKind;
using mytoydb::access::BTScanDesc;
using mytoydb::access::BTScanKeyData;
using mytoydb::access::BTStrategy;
using mytoydb::access::Relation;
using mytoydb::access::RelationOpen;
using mytoydb::access::TupleDesc;
using mytoydb::memory::palloc;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::Const;
using mytoydb::parser::OpExpr;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::Var;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::DatumGetTextP;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::VARSIZE_DATA;

namespace {

// Determine the B-tree key kind from a type OID.
BTKeyKind KeyKindFromType(mytoydb::catalog::Oid typid) {
    if (typid == kInt4Oid)
        return BTKeyKind::kInt32;
    if (typid == kInt8Oid)
        return BTKeyKind::kInt64;
    if (typid == kTextOid)
        return BTKeyKind::kText;
    return BTKeyKind::kInt32;  // default
}

}  // namespace

void IndexScanState::ExecInit() {
    auto* idxplan = static_cast<IndexScan*>(plan);

    // Look up the range table entry.
    int rtindex = idxplan->scanrelid - 1;
    if (rtindex < 0 || rtindex >= static_cast<int>(state->es_range_table.size())) {
        ereport(mytoydb::error::LogLevel::kError, "IndexScan: invalid scanrelid");
    }
    RangeTblEntry* rte = state->es_range_table[rtindex];

    // Open the heap relation.
    iss_relation = RelationOpen(static_cast<mytoydb::catalog::Oid>(rte->relid));
    if (iss_relation == nullptr) {
        ereport(mytoydb::error::LogLevel::kError, "IndexScan: relation not found");
    }
    state->es_open_relations.push_back(iss_relation);

    // Open the index relation.
    iss_index = RelationOpen(idxplan->indexid);
    if (iss_index == nullptr) {
        ereport(mytoydb::error::LogLevel::kError, "IndexScan: index not found");
    }
    state->es_open_relations.push_back(iss_index);

    // Create the scan tuple slot.
    iss_ScanTupleSlot = TupleTableSlot::Make(iss_relation->rd_att);
    state->es_tupleTable.push_back(iss_ScanTupleSlot);

    // Build the scan key from the index quals.
    // We support a single equality or range qual on the first key column.
    BTScanKeyData scan_key;
    scan_key.strategy = BTStrategy::kAll;
    BTKeyKind key_kind = BTKeyKind::kInt32;

    if (!idxplan->indexqual.empty()) {
        // Examine the first index qual to determine the scan key.
        mytoydb::parser::Node* qual = idxplan->indexqual[0];
        if (qual != nullptr && qual->GetTag() == NodeTag::kOpExpr) {
            auto* op = static_cast<OpExpr*>(qual);
            if (op->args.size() == 2) {
                // One arg should be a Var (the indexed column), the other
                // a Const (the comparison value).
                Var* var = nullptr;
                Const* con = nullptr;
                for (mytoydb::parser::Node* arg : op->args) {
                    if (arg == nullptr)
                        continue;
                    if (arg->GetTag() == NodeTag::kVar) {
                        var = static_cast<Var*>(arg);
                    } else if (arg->GetTag() == NodeTag::kConst) {
                        con = static_cast<Const*>(arg);
                    }
                }

                if (var != nullptr && con != nullptr) {
                    // Look up the operator name to determine the strategy.
                    const auto* oprow = mytoydb::catalog::GetCatalog()->GetOperatorByOid(op->opno);
                    if (oprow != nullptr) {
                        const std::string& opname = oprow->oprname;
                        key_kind = KeyKindFromType(var->vartype);
                        scan_key.kind = key_kind;
                        scan_key.key = &con->constvalue;
                        scan_key.key_len = (key_kind == BTKeyKind::kText)
                                               ? VARSIZE_DATA(DatumGetTextP(con->constvalue))
                                               : 0;

                        if (opname == "=") {
                            scan_key.strategy = BTStrategy::kEqual;
                        } else if (opname == "<") {
                            scan_key.strategy = BTStrategy::kLess;
                        } else if (opname == "<=") {
                            scan_key.strategy = BTStrategy::kLessEqual;
                        } else if (opname == ">") {
                            scan_key.strategy = BTStrategy::kGreater;
                        } else if (opname == ">=") {
                            scan_key.strategy = BTStrategy::kGreaterEqual;
                        }
                    }
                }
            }
        }
    }

    // Start the B-tree scan.
    iss_scanDesc = mytoydb::access::btbeginscan(iss_index, key_kind, &scan_key);

    // Create the result slot from the target list.
    auto* result_desc = BuildTupleDescFromTargetList(idxplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    // Create the expression context.
    ps_ExprContext = CreateExprContext();
    ps_ExprContext->ecxt_scantuple = iss_ScanTupleSlot;
}

TupleTableSlot* IndexScanState::ExecProcNode() {
    for (;;) {
        // Get the next matching TID from the index.
        if (!mytoydb::access::btgettuple(iss_scanDesc)) {
            return nullptr;  // scan exhausted
        }

        // Fetch the heap tuple at the current TID.
        mytoydb::transaction::ItemPointerData tid = iss_scanDesc->curr_tid;
        // Use a heap scan to fetch the tuple by TID.
        // For simplicity, we do a point scan: begin a scan, position to
        // the TID, and fetch. A full implementation would use heap_fetch.
        // Here we use a simplified approach: scan the heap and find the
        // matching TID.
        mytoydb::access::HeapScanDesc hscan =
            mytoydb::access::heap_beginscan(iss_relation, state->es_snapshot);
        mytoydb::transaction::HeapTuple tuple = nullptr;
        while ((tuple = mytoydb::access::heap_getnext(hscan)) != nullptr) {
            if (tuple->t_self == tid)
                break;
        }
        mytoydb::access::heap_endscan(hscan);

        if (tuple == nullptr) {
            continue;  // TID not found (tuple may have been deleted)
        }

        // Store the tuple in the scan slot.
        iss_ScanTupleSlot->StoreTuple(tuple, false);

        // Evaluate the qual (residual filter).
        ResetExprContext(ps_ExprContext);
        if (!ExecQual(plan->qual, ps_ExprContext)) {
            continue;
        }

        // Project the target list.
        ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
        return ps_ResultTupleSlot;
    }
}

void IndexScanState::ExecEnd() {
    if (iss_scanDesc != nullptr) {
        mytoydb::access::btendscan(iss_scanDesc);
        iss_scanDesc = nullptr;
    }
    iss_relation = nullptr;
    iss_index = nullptr;

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void IndexScanState::ExecReScan() {
    if (iss_scanDesc != nullptr) {
        mytoydb::access::btrescan(iss_scanDesc, nullptr);
    }
}

}  // namespace mytoydb::executor
