// node_sort_limit_test.cpp — Unit tests for the Sort and Limit plan nodes (M9).
//
// Exercises the Sort executor (ascending/descending, multi-key, duplicate
// keys, already-sorted and reverse-order inputs, empty input) and the Limit
// executor (LIMIT 0/1/N, OFFSET, LIMIT larger than input). Each test builds
// a SeqScan leaf, stacks a Sort and/or Limit plan on top, and drives the
// ExecutorStart/Run/Finish/End lifecycle.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_operator.hpp"
#include "catalog/pg_proc.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_main.hpp"
#include "executor/exec_utils.hpp"
#include "executor/node_exec.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

using pgcpp::access::heap_form_tuple;
using pgcpp::access::heap_freetuple;
using pgcpp::access::heap_insert;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::kFirstNormalObjectId;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::executor::Agg;
using pgcpp::executor::CreateExprContext;
using pgcpp::executor::EState;
using pgcpp::executor::ExecEndNode;
using pgcpp::executor::ExecInitNode;
using pgcpp::executor::ExecutorEnd;
using pgcpp::executor::ExecutorFinish;
using pgcpp::executor::ExecutorRun;
using pgcpp::executor::ExecutorStart;
using pgcpp::executor::ExprContext;
using pgcpp::executor::Limit;
using pgcpp::executor::MakeTupleTableSlot;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanState;
using pgcpp::executor::QueryDesc;
using pgcpp::executor::ResetExprContext;
using pgcpp::executor::Result;
using pgcpp::executor::SeqScan;
using pgcpp::executor::Sort;
using pgcpp::executor::TupleTableSlot;
using pgcpp::memory::AllocSetContext;
using pgcpp::memory::palloc;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RTEKind;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;

namespace {

using pgcpp::nodes::makePallocNode;

// Operator OIDs (from bootstrap_catalog.cpp).
constexpr Oid kInt4EqOp = 96;  // int4 = int4
constexpr Oid kInt4LtOp = 97;  // int4 < int4

class NodeSortLimitTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("node_sort_limit_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        pgcpp::transaction::InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_node_sort_limit_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();
    }

    void TearDown() override {
        EndTransactionBlock();
        ResetRelcache();
        ShutdownBufferPool();
        smgrcloseall();
        RunShell("rm -rf " + test_dir_);

        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        ResetTransactionState();
        InitializeTransactionSystem();
        pgcpp::transaction::InitializeSnapshotManager();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Commit and start a new transaction so inserted tuples are visible.
    void CommitAndStartNew() {
        EndTransactionBlock();
        pgcpp::transaction::InitializeSnapshotManager();
        BeginTransactionBlock();
    }

    FormData_pg_class* MakeClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kRelation;
        row->relpersistence = RelPersistence::kPermanent;
        return row;
    }

    // Create a relation with the given OID and schema.
    Relation CreateTestRelation(Oid relid, const std::string& name,
                                const std::vector<FormData_pg_attribute>& attrs) {
        auto* class_row = MakeClassRow(name, relid);
        catalog_->InsertClass(class_row);
        for (const auto& attr : attrs) {
            auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
            catalog_->InsertAttribute(attr_row);
        }
        RelationCreateStorage(relid, false);
        return RelationOpen(relid);
    }

    // Build a simple 2-column int4 schema (a, b).
    std::vector<FormData_pg_attribute> MakeIntIntSchema(Oid relid) {
        FormData_pg_attribute a1;
        a1.attrelid = relid;
        a1.attname = "a";
        a1.attnum = 1;
        a1.atttypid = kInt4Oid;
        a1.attlen = 4;
        a1.attbyval = true;
        a1.attalign = AttAlign::kInt;
        a1.attstorage = AttStorage::kPlain;

        FormData_pg_attribute a2;
        a2.attrelid = relid;
        a2.attname = "b";
        a2.attnum = 2;
        a2.atttypid = kInt4Oid;
        a2.attlen = 4;
        a2.attbyval = true;
        a2.attalign = AttAlign::kInt;
        a2.attstorage = AttStorage::kPlain;

        return {a1, a2};
    }

    // Insert a row (int4, int4) into a relation.
    void InsertIntIntRow(Relation rel, int32_t a, int32_t b) {
        TupleDesc tupdesc = rel->rd_att;
        Datum values[2] = {Int32GetDatum(a), Int32GetDatum(b)};
        bool isnull[2] = {false, false};
        HeapTuple tup = heap_form_tuple(tupdesc, values, isnull);
        heap_insert(rel, tup);
        heap_freetuple(tup);
    }

    RangeTblEntry* MakeRTE(Oid relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = static_cast<int>(relid);
        return rte;
    }

    Var* MakeVar(int varno, int varattno, Oid vartype) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = vartype;
        return var;
    }

    TargetEntry* MakeTargetEntry(Node* expr, int resno, const std::string& resname = "") {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = resname;
        return te;
    }

    Query* MakeSelectQuery(std::vector<RangeTblEntry*> rtable) {
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kSelect;
        for (auto* rte : rtable) {
            query->rtable.push_back(rte);
        }
        return query;
    }

    // Build a SeqScan leaf projecting (a, b) from range-table entry 1.
    SeqScan* MakeSeqScanAB() {
        auto* seqplan = makePallocNode<SeqScan>();
        seqplan->scanrelid = 1;
        seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
        seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
        return seqplan;
    }

    // Build a Sort node on top of a child plan. `col_idx`, `reverse`, and
    // `nulls_first` must be parallel arrays (one entry per sort key).
    Sort* MakeSortOn(Plan* child, std::vector<int> col_idx, std::vector<bool> reverse,
                     std::vector<bool> nulls_first) {
        auto* sortplan = makePallocNode<Sort>();
        sortplan->lefttree = child;
        sortplan->sortColIdx = std::move(col_idx);
        sortplan->sortOperators.assign(sortplan->sortColIdx.size(), kInt4LtOp);
        sortplan->reverse = std::move(reverse);
        sortplan->nullsFirst = std::move(nulls_first);
        sortplan->limit = -1;  // no Top-N limit
        sortplan->offset = 0;
        // Sort passes through the child's columns unchanged.
        sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
        sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
        return sortplan;
    }

    // Build a Limit node on top of a child plan, projecting (a, b).
    Limit* MakeLimitOn(Plan* child, int64_t limit_count, int64_t offset_count) {
        auto* limitplan = makePallocNode<Limit>();
        limitplan->lefttree = child;
        limitplan->limit_count = limit_count;
        limitplan->offset_count = offset_count;
        limitplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
        limitplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
        return limitplan;
    }

    // Run a plan tree and collect the (a, b) values from each output row.
    std::vector<std::pair<int, int>> RunCollectAB(QueryDesc* qd) {
        std::vector<std::pair<int, int>> rows;
        TupleTableSlot* slot = nullptr;
        while ((slot = ExecutorRun(qd)) != nullptr) {
            rows.emplace_back(DatumGetInt32(slot->tts_values[0]),
                              DatumGetInt32(slot->tts_values[1]));
        }
        return rows;
    }

    // Build a QueryDesc for a SELECT over the given range-table entry.
    QueryDesc* MakeQueryDesc(Oid relid, Plan* plan) {
        auto* rte = MakeRTE(relid);
        auto* qd = makePallocNode<QueryDesc>();
        qd->query = MakeSelectQuery({rte});
        qd->plan = plan;
        return qd;
    }

    static void RunShell(const std::string& cmd) {
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// ===========================================================================
// 1. SortEmptyInput
//    Sort on an empty table — the executor should produce no output rows.
// ===========================================================================

TEST_F(NodeSortLimitTest, SortEmptyInput) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    CommitAndStartNew();
    RelationClose(rel);

    auto* sortplan = MakeSortOn(MakeSeqScanAB(), {1}, {false}, {false});
    QueryDesc* qd = MakeQueryDesc(relid, sortplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    EXPECT_TRUE(rows.empty());
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 2. SortSingleRow
//    Sort a single-row input — should return exactly that row.
// ===========================================================================

TEST_F(NodeSortLimitTest, SortSingleRow) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 42, 420);
    CommitAndStartNew();
    RelationClose(rel);

    auto* sortplan = MakeSortOn(MakeSeqScanAB(), {1}, {false}, {false});
    QueryDesc* qd = MakeQueryDesc(relid, sortplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].first, 42);
    EXPECT_EQ(rows[0].second, 420);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 3. SortMultipleRows
//    Sort 5 rows in random order by ascending key — verify the output is
//    sorted by `a` ascending.
// ===========================================================================

TEST_F(NodeSortLimitTest, SortMultipleRows) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a in random order: 3, 1, 5, 2, 4 → expect 1, 2, 3, 4, 5
    InsertIntIntRow(rel, 3, 30);
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 5, 50);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 4, 40);
    CommitAndStartNew();
    RelationClose(rel);

    auto* sortplan = MakeSortOn(MakeSeqScanAB(), {1}, {false}, {false});
    QueryDesc* qd = MakeQueryDesc(relid, sortplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0].first, 1);
    EXPECT_EQ(rows[1].first, 2);
    EXPECT_EQ(rows[2].first, 3);
    EXPECT_EQ(rows[3].first, 4);
    EXPECT_EQ(rows[4].first, 5);
    // Verify b follows a (since we sorted only on a, b should still be a*10).
    EXPECT_EQ(rows[0].second, 10);
    EXPECT_EQ(rows[4].second, 50);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 4. SortAlreadySorted
//    Input already in ascending order — sort should be a no-op.
// ===========================================================================

TEST_F(NodeSortLimitTest, SortAlreadySorted) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* sortplan = MakeSortOn(MakeSeqScanAB(), {1}, {false}, {false});
    QueryDesc* qd = MakeQueryDesc(relid, sortplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    ASSERT_EQ(rows.size(), 5u);
    for (size_t i = 0; i < rows.size(); i++) {
        EXPECT_EQ(rows[i].first, static_cast<int>(i + 1));
        EXPECT_EQ(rows[i].second, static_cast<int>(i + 1) * 10);
    }
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 5. SortReverseOrder
//    Input in descending order — sort should reverse it to ascending.
// ===========================================================================

TEST_F(NodeSortLimitTest, SortReverseOrder) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a = 5, 4, 3, 2, 1 → expect 1, 2, 3, 4, 5
    for (int i = 5; i >= 1; i--) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* sortplan = MakeSortOn(MakeSeqScanAB(), {1}, {false}, {false});
    QueryDesc* qd = MakeQueryDesc(relid, sortplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0].first, 1);
    EXPECT_EQ(rows[1].first, 2);
    EXPECT_EQ(rows[2].first, 3);
    EXPECT_EQ(rows[3].first, 4);
    EXPECT_EQ(rows[4].first, 5);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 6. SortDuplicateKeys
//    Multiple rows with the same key value — sort is stable enough that all
//    duplicate-key rows appear consecutively; verify count and key identity.
// ===========================================================================

TEST_F(NodeSortLimitTest, SortDuplicateKeys) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a = 2, 1, 2, 3, 1, 2 → expect 1, 1, 2, 2, 2, 3
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 2, 21);
    InsertIntIntRow(rel, 3, 30);
    InsertIntIntRow(rel, 1, 11);
    InsertIntIntRow(rel, 2, 22);
    CommitAndStartNew();
    RelationClose(rel);

    auto* sortplan = MakeSortOn(MakeSeqScanAB(), {1}, {false}, {false});
    QueryDesc* qd = MakeQueryDesc(relid, sortplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    ASSERT_EQ(rows.size(), 6u);
    // Verify ascending order on `a`.
    EXPECT_EQ(rows[0].first, 1);
    EXPECT_EQ(rows[1].first, 1);
    EXPECT_EQ(rows[2].first, 2);
    EXPECT_EQ(rows[3].first, 2);
    EXPECT_EQ(rows[4].first, 2);
    EXPECT_EQ(rows[5].first, 3);
    // Verify all rows with a=2 are accounted for (b values 20, 21, 22).
    std::vector<int> b_for_a2;
    for (const auto& r : rows) {
        if (r.first == 2) {
            b_for_a2.push_back(r.second);
        }
    }
    ASSERT_EQ(b_for_a2.size(), 3u);
    // The 3 b-values for a=2 should be 20, 21, 22 (in some order).
    std::sort(b_for_a2.begin(), b_for_a2.end());
    EXPECT_EQ(b_for_a2[0], 20);
    EXPECT_EQ(b_for_a2[1], 21);
    EXPECT_EQ(b_for_a2[2], 22);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 7. LimitZero
//    LIMIT 0 returns no rows.
// ===========================================================================

TEST_F(NodeSortLimitTest, LimitZero) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* limitplan = MakeLimitOn(MakeSeqScanAB(), 0, 0);
    QueryDesc* qd = MakeQueryDesc(relid, limitplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    EXPECT_TRUE(rows.empty());
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 8. LimitOne
//    LIMIT 1 returns exactly the first row.
// ===========================================================================

TEST_F(NodeSortLimitTest, LimitOne) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* limitplan = MakeLimitOn(MakeSeqScanAB(), 1, 0);
    QueryDesc* qd = MakeQueryDesc(relid, limitplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].first, 1);
    EXPECT_EQ(rows[0].second, 10);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 9. LimitLargerThanInput
//    LIMIT > row count returns all rows.
// ===========================================================================

TEST_F(NodeSortLimitTest, LimitLargerThanInput) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    for (int i = 1; i <= 3; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    // LIMIT 100 on a 3-row table → all 3 rows.
    auto* limitplan = MakeLimitOn(MakeSeqScanAB(), 100, 0);
    QueryDesc* qd = MakeQueryDesc(relid, limitplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].first, 1);
    EXPECT_EQ(rows[1].first, 2);
    EXPECT_EQ(rows[2].first, 3);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 10. LimitWithOffset
//     LIMIT 2 OFFSET 1 — skip the first row, return the next two.
// ===========================================================================

TEST_F(NodeSortLimitTest, LimitWithOffset) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    // LIMIT 2 OFFSET 1 on rows 1..5 → rows 2, 3.
    auto* limitplan = MakeLimitOn(MakeSeqScanAB(), 2, 1);
    QueryDesc* qd = MakeQueryDesc(relid, limitplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].first, 2);
    EXPECT_EQ(rows[0].second, 20);
    EXPECT_EQ(rows[1].first, 3);
    EXPECT_EQ(rows[1].second, 30);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 11. SortDescending
//     ORDER BY a DESC — verify the Sort node reverses the comparison via
//     the per-key `reverse` flag.
// ===========================================================================

TEST_F(NodeSortLimitTest, SortDescending) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a = 1, 3, 2, 5, 4 → expect DESC: 5, 4, 3, 2, 1
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 3, 30);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 5, 50);
    InsertIntIntRow(rel, 4, 40);
    CommitAndStartNew();
    RelationClose(rel);

    auto* sortplan = MakeSortOn(MakeSeqScanAB(), {1}, {true}, {false});
    QueryDesc* qd = MakeQueryDesc(relid, sortplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0].first, 5);
    EXPECT_EQ(rows[1].first, 4);
    EXPECT_EQ(rows[2].first, 3);
    EXPECT_EQ(rows[3].first, 2);
    EXPECT_EQ(rows[4].first, 1);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 12. SortMultipleKeys
//     ORDER BY a ASC, b DESC — verify the Sort node honors per-key direction
//     on a multi-key sort.
// ===========================================================================

TEST_F(NodeSortLimitTest, SortMultipleKeys) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // (a, b):
    //   (1, 30), (1, 10), (1, 20) → expect (1, 30), (1, 20), (1, 10)
    //   (2, 50), (2, 40)          → expect (2, 50), (2, 40)
    InsertIntIntRow(rel, 1, 30);
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 1, 20);
    InsertIntIntRow(rel, 2, 50);
    InsertIntIntRow(rel, 2, 40);
    CommitAndStartNew();
    RelationClose(rel);

    // ORDER BY a ASC, b DESC.
    auto* sortplan = MakeSortOn(MakeSeqScanAB(), {1, 2}, {false, true}, {false, false});
    QueryDesc* qd = MakeQueryDesc(relid, sortplan);

    ExecutorStart(qd);
    auto rows = RunCollectAB(qd);
    ASSERT_EQ(rows.size(), 5u);
    // a=1 group: b descending → 30, 20, 10.
    EXPECT_EQ(rows[0].first, 1);
    EXPECT_EQ(rows[0].second, 30);
    EXPECT_EQ(rows[1].first, 1);
    EXPECT_EQ(rows[1].second, 20);
    EXPECT_EQ(rows[2].first, 1);
    EXPECT_EQ(rows[2].second, 10);
    // a=2 group: b descending → 50, 40.
    EXPECT_EQ(rows[3].first, 2);
    EXPECT_EQ(rows[3].second, 50);
    EXPECT_EQ(rows[4].first, 2);
    EXPECT_EQ(rows[4].second, 40);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

}  // namespace
