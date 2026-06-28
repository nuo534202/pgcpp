// set_refs_test.cpp — Unit tests for plan-reference finalization (M10 15.3).
//
// Tests set_plan_references. For MyToyDB's single-table workload with
// rtoffset=0, the fixup is mostly a no-op: Var.varno is already correct.
// These tests verify that set_plan_references does not corrupt the plan tree.

#include "pgcpp/optimizer/plan/set_refs.hpp"

#include <gtest/gtest.h>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/optimizer/path.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/primnodes.hpp"
#include "pgcpp/types/datum.hpp"

using mytoydb::executor::Agg;
using mytoydb::executor::Plan;
using mytoydb::executor::PlanType;
using mytoydb::executor::Result;
using mytoydb::executor::SeqScan;
using mytoydb::executor::Sort;
using mytoydb::nodes::makePallocNode;
using mytoydb::optimizer::PlannerInfo;
using mytoydb::optimizer::set_plan_references;
using mytoydb::parser::CmdType;
using mytoydb::parser::Const;
using mytoydb::parser::FromExpr;
using mytoydb::parser::Node;
using mytoydb::parser::OpExpr;
using mytoydb::parser::Query;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::RangeTblRef;
using mytoydb::parser::RTEKind;
using mytoydb::parser::SortGroupClause;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::Var;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::kBoolOid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;

namespace {

constexpr mytoydb::catalog::Oid kInt4GtOp = 521;  // int4 > int4

class SetRefsTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = mytoydb::memory::AllocSetContext::Create("set_refs_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    Var* MakeVar(int varno, int varattno) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = kInt4Oid;
        return var;
    }

    Const* MakeInt4Const(int32_t value) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(value);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 4;
        return con;
    }

    OpExpr* MakeOpExpr(mytoydb::catalog::Oid opno, Node* left, Node* right) {
        auto* op = makePallocNode<OpExpr>();
        op->opno = opno;
        op->opresulttype = kBoolOid;
        op->args.push_back(left);
        op->args.push_back(right);
        return op;
    }

    TargetEntry* MakeTargetEntry(Node* expr, int resno, const std::string& resname = "",
                                 int ressortgroupref = 0) {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = resname;
        te->ressortgroupref = ressortgroupref;
        return te;
    }

    RangeTblEntry* MakeRTE(int relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = relid;
        return rte;
    }

    RangeTblRef* MakeRangeTblRef(int rtindex) {
        auto* ref = makePallocNode<RangeTblRef>();
        ref->rtindex = rtindex;
        return ref;
    }

    FromExpr* MakeFromExpr(int rtindex, Node* quals = nullptr) {
        auto* from = makePallocNode<FromExpr>();
        from->fromlist.push_back(MakeRangeTblRef(rtindex));
        from->quals = quals;
        return from;
    }

    Query* MakeSelectQuery() {
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kSelect;
        return query;
    }

    mytoydb::memory::AllocSetContext* context_ = nullptr;
};

// set_plan_references on a simple SeqScan does not corrupt the plan.
TEST_F(SetRefsTest, SeqScan_PreservesPlanStructure) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));
    root->parse = query;

    auto* scan = makePallocNode<SeqScan>();
    scan->scanrelid = 1;
    Var* var = MakeVar(1, 1);
    scan->targetlist.push_back(MakeTargetEntry(var, 1, "a"));

    set_plan_references(root, scan);

    // The plan structure should be unchanged.
    EXPECT_EQ(scan->type, PlanType::kSeqScan);
    EXPECT_EQ(scan->scanrelid, 1);
    ASSERT_EQ(scan->targetlist.size(), 1u);
    // The Var in the target list should still have varno=1 (rtoffset=0).
    auto* te = scan->targetlist[0];
    auto* result_var = static_cast<Var*>(te->expr);
    EXPECT_EQ(result_var->varno, 1);
    EXPECT_EQ(result_var->varattno, 1);
}

// set_plan_references on a SeqScan with a qual preserves the qual.
TEST_F(SetRefsTest, SeqScanWithQual_PreservesQual) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    Node* qual = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeInt4Const(5));
    query->jointree = MakeFromExpr(1, qual);
    root->parse = query;

    auto* scan = makePallocNode<SeqScan>();
    scan->scanrelid = 1;
    scan->qual = qual;

    set_plan_references(root, scan);

    EXPECT_NE(scan->qual, nullptr);
    EXPECT_EQ(scan->qual->GetTag(), mytoydb::nodes::NodeTag::kOpExpr);
}

// set_plan_references on an Agg plan preserves the child plan.
TEST_F(SetRefsTest, AggPlan_PreservesChildPlan) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->has_aggs = true;
    auto* aggref = makePallocNode<mytoydb::parser::Aggref>();
    aggref->aggfnoid = 2147;
    aggref->aggtype = kInt8Oid;
    aggref->aggstar = true;
    query->target_list.push_back(MakeTargetEntry(aggref, 1, "count"));
    root->parse = query;

    auto* scan = makePallocNode<SeqScan>();
    scan->scanrelid = 1;

    auto* agg = makePallocNode<Agg>();
    agg->aggstrategy = Agg::Strategy::kPlain;
    agg->lefttree = scan;
    agg->targetlist.push_back(MakeTargetEntry(aggref, 1, "count"));

    set_plan_references(root, agg);

    EXPECT_EQ(agg->type, PlanType::kAgg);
    EXPECT_NE(agg->lefttree, nullptr);
    EXPECT_EQ(agg->lefttree->type, PlanType::kSeqScan);
}

// set_plan_references on a Sort plan preserves the child plan.
TEST_F(SetRefsTest, SortPlan_PreservesChildPlan) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a", 1));
    root->parse = query;

    auto* scan = makePallocNode<SeqScan>();
    scan->scanrelid = 1;

    auto* sort = makePallocNode<Sort>();
    sort->lefttree = scan;
    sort->sortColIdx.push_back(1);
    sort->sortOperators.push_back(0);

    set_plan_references(root, sort);

    EXPECT_EQ(sort->type, PlanType::kSort);
    EXPECT_NE(sort->lefttree, nullptr);
    EXPECT_EQ(sort->lefttree->type, PlanType::kSeqScan);
    ASSERT_EQ(sort->sortColIdx.size(), 1u);
    EXPECT_EQ(sort->sortColIdx[0], 1);
}

// set_plan_references on nullptr is a no-op (does not crash).
TEST_F(SetRefsTest, NullPlan_NoOp) {
    auto* root = makePallocNode<PlannerInfo>();
    set_plan_references(root, nullptr);
    // No crash = pass.
}

// set_plan_references on a Result plan preserves the target list.
TEST_F(SetRefsTest, ResultPlan_PreservesTargetList) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->target_list.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "?column?"));
    root->parse = query;

    auto* result = makePallocNode<Result>();
    result->targetlist.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "?column?"));

    set_plan_references(root, result);

    EXPECT_EQ(result->type, PlanType::kResult);
    EXPECT_EQ(result->targetlist.size(), 1u);
}

}  // namespace
