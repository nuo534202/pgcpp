// node_bitmap_indexscan.cpp — BitmapIndexScan node implementation.
//
// Builds a TID bitmap from a B-tree index scan. The index quals are
// interpreted exactly as in IndexScan (single equality/range qual on the
// first key column). All matching TIDs are gathered into a vector via
// btgetbitmap and stored in `tids` for the parent BitmapHeapScan to fetch.
//
// ExecProcNode returns nullptr on the first call (after building the
// bitmap) — this node produces no tuples itself.
#include "executor/node_bitmap_indexscan.hpp"

#include "access/heapam.hpp"
#include "access/nbtpage.hpp"
#include "access/nbtree.hpp"
#include "access/rel.hpp"
#include "catalog/pg_operator.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

namespace pgcpp::executor {

using pgcpp::access::BTKeyKind;
using pgcpp::access::BTScanKeyData;
using pgcpp::access::BTStrategy;
using pgcpp::access::Relation;
using pgcpp::access::RelationOpen;
using pgcpp::catalog::GetCatalog;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::Const;
using pgcpp::parser::OpExpr;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::Var;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::VARSIZE_DATA;

namespace {

BTKeyKind KeyKindFromType(pgcpp::catalog::Oid typid) {
    if (typid == kInt4Oid)
        return BTKeyKind::kInt32;
    if (typid == kInt8Oid)
        return BTKeyKind::kInt64;
    if (typid == kTextOid)
        return BTKeyKind::kText;
    return BTKeyKind::kInt32;
}

}  // namespace

void BitmapIndexScanState::ExecInit() {
    auto* idxplan = static_cast<BitmapIndexScan*>(plan);

    int rtindex = idxplan->scanrelid - 1;
    if (rtindex < 0 || rtindex >= static_cast<int>(state->es_range_table.size())) {
        ereport(pgcpp::error::LogLevel::kError, "BitmapIndexScan: invalid scanrelid");
    }
    RangeTblEntry* rte = state->es_range_table[rtindex];

    biss_relation = RelationOpen(static_cast<pgcpp::catalog::Oid>(rte->relid));
    if (biss_relation == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "BitmapIndexScan: relation not found");
    }
    state->es_open_relations.push_back(biss_relation);

    biss_index = RelationOpen(idxplan->indexid);
    if (biss_index == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "BitmapIndexScan: index not found");
    }
    state->es_open_relations.push_back(biss_index);

    // Build the scan key from the index quals (same logic as IndexScan).
    BTScanKeyData scan_key;
    scan_key.strategy = BTStrategy::kAll;
    BTKeyKind key_kind = BTKeyKind::kInt32;

    if (!idxplan->indexqual.empty()) {
        pgcpp::parser::Node* qual = idxplan->indexqual[0];
        if (qual != nullptr && qual->GetTag() == NodeTag::kOpExpr) {
            auto* op = static_cast<OpExpr*>(qual);
            if (op->args.size() == 2) {
                Var* var = nullptr;
                Const* con = nullptr;
                for (pgcpp::parser::Node* arg : op->args) {
                    if (arg == nullptr)
                        continue;
                    if (arg->GetTag() == NodeTag::kVar) {
                        var = static_cast<Var*>(arg);
                    } else if (arg->GetTag() == NodeTag::kConst) {
                        con = static_cast<Const*>(arg);
                    }
                }

                if (var != nullptr && con != nullptr) {
                    const auto* oprow = GetCatalog()->GetOperatorByOid(op->opno);
                    if (oprow != nullptr) {
                        const std::string& opname = oprow->oprname;
                        key_kind = KeyKindFromType(var->vartype);
                        scan_key.kind = key_kind;
                        scan_key.key = &con->constvalue;
                        scan_key.key_len =
                            static_cast<uint16_t>((key_kind == BTKeyKind::kText)
                                                      ? VARSIZE_DATA(DatumGetTextP(con->constvalue))
                                                      : 0);

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

    biss_scanDesc = pgcpp::access::btbeginscan(biss_index, key_kind, &scan_key);

    // A result slot is still required by the executor framework even though
    // this node emits no tuples; build it from the target list.
    auto* result_desc = BuildTupleDescFromTargetList(idxplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();

    bitmap_built = false;
    tids.clear();
}

TupleTableSlot* BitmapIndexScanState::ExecProcNode() {
    if (bitmap_built) {
        return nullptr;  // single-shot: bitmap already produced
    }

    // Drain the entire index scan into the TID vector.
    tids.clear();
    pgcpp::access::btgetbitmap(biss_scanDesc, &tids);
    bitmap_built = true;
    return nullptr;
}

void BitmapIndexScanState::ExecEnd() {
    if (biss_scanDesc != nullptr) {
        pgcpp::access::btendscan(biss_scanDesc);
        biss_scanDesc = nullptr;
    }
    biss_relation = nullptr;
    biss_index = nullptr;
    tids.clear();
    bitmap_built = false;

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void BitmapIndexScanState::ExecReScan() {
    if (biss_scanDesc != nullptr) {
        pgcpp::access::btrescan(biss_scanDesc, nullptr);
    }
    tids.clear();
    bitmap_built = false;
}

}  // namespace pgcpp::executor
