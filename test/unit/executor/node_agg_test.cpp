// node_agg_test.cpp — Unit tests for the Agg plan node (M9).
//
// Exercises the aggregate executor (COUNT/SUM/AVG/MIN/MAX) with and without
// GROUP BY, HAVING, multiple aggregates per query, and empty-input edge
// cases. Each test builds a SeqScan leaf, stacks an Agg plan on top, and
// drives the ExecutorStart/Run/Finish/End lifecycle.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <map>
#include <string>
#include <tuple>
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
using pgcpp::catalog::GetCatalog;
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
using pgcpp::parser::Aggref;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::JoinType;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RTEKind;
using pgcpp::parser::SortGroupClause;
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
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::Float8GetDatum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;

namespace {

using pgcpp::nodes::makePallocNode;

// Operator OIDs (from bootstrap_catalog.cpp).
constexpr Oid kInt4EqOp = 96;     // int4 = int4
constexpr Oid kInt4LtOp = 97;     // int4 < int4
constexpr Oid kInt4GtOp = 521;    // int4 > int4
constexpr Oid kInt4PlusOp = 551;  // int4 + int4

// Aggregate function OIDs (from bootstrap_catalog.cpp).
constexpr Oid kCountInt4Oid = 2147;
constexpr Oid kSumInt4Oid = 2108;
constexpr Oid kAvgInt4Oid = 2107;
constexpr Oid kMinInt4Oid = 2131;
constexpr Oid kMaxInt4Oid = 2116;

class NodeAggTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("node_agg_test_context");
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

        test_dir_ = "/tmp/pgcpp_node_agg_test_" + std::to_string(getpid());
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

    // Insert a row with a NULL b value (used to verify COUNT(*) counts all
    // rows regardless of NULLs in non-aggregated columns).
    void InsertIntNullRow(Relation rel, int32_t a) {
        TupleDesc tupdesc = rel->rd_att;
        Datum values[2] = {Int32GetDatum(a), 0};
        bool isnull[2] = {false, true};
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

    Const* MakeInt4Const(int32_t value) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(value);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 4;
        return con;
    }

    Const* MakeInt8Const(int64_t value) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt8Oid;
        con->constvalue = pgcpp::types::Int64GetDatum(value);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 8;
        return con;
    }

    TargetEntry* MakeTargetEntry(Node* expr, int resno, const std::string& resname = "") {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = resname;
        return te;
    }

    OpExpr* MakeOpExpr(Oid opno, Oid resulttype, Node* left, Node* right) {
        auto* op = makePallocNode<OpExpr>();
        op->opno = opno;
        op->opresulttype = resulttype;
        op->args.push_back(left);
        op->args.push_back(right);
        return op;
    }

    Aggref* MakeAggref(Oid aggfnoid, Oid aggtype, bool aggstar = false) {
        auto* agg = makePallocNode<Aggref>();
        agg->aggfnoid = aggfnoid;
        agg->aggtype = aggtype;
        agg->aggstar = aggstar;
        return agg;
    }

    // Build a SortGroupClause for a DISTINCT marker on the first targetlist
    // entry. The executor currently ignores aggdistinct, but the field is
    // populated so the test exercises the documented plan shape.
    SortGroupClause* MakeSortGroupClause(int sortgroupref) {
        auto* sgc = makePallocNode<SortGroupClause>();
        sgc->tle_sort_group_ref = sortgroupref;
        sgc->eqop = kInt4EqOp;
        sgc->sortop = kInt4LtOp;
        sgc->nulls_first = false;
        sgc->hashable = true;
        return sgc;
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
// 1. EmptyGroupByReturnsOneRow
//    SELECT COUNT(*) FROM empty_table — plain agg always returns exactly one
//    row even when no input tuples exist; COUNT(*) is 0.
// ===========================================================================

TEST_F(NodeAggTest, EmptyGroupByReturnsOneRow) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->targetlist.push_back(MakeTargetEntry(count_agg, 1, "count"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), 0);
    // Second call returns nullptr — only one row from plain agg.
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 2. GroupByMultipleKeys
//    GROUP BY a, b on rows with duplicate (a,b) pairs — verify the number of
//    distinct groups.
// ===========================================================================

TEST_F(NodeAggTest, GroupByMultipleKeys) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // 4 distinct (a,b) pairs, with duplicates.
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 1, 10);  // dup of previous
    InsertIntIntRow(rel, 1, 20);
    InsertIntIntRow(rel, 2, 10);
    InsertIntIntRow(rel, 2, 20);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;
    aggplan->groupColIdx = {1, 2};  // GROUP BY a, b

    // Output: a, b, COUNT(*)
    aggplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    aggplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->targetlist.push_back(MakeTargetEntry(count_agg, 3, "count"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    std::vector<std::tuple<int, int, int64_t>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]), DatumGetInt32(slot->tts_values[1]),
                             DatumGetInt64(slot->tts_values[2]));
    }
    // 4 distinct (a,b) pairs.
    EXPECT_EQ(results.size(), 4u);
    // The (1,10) pair appears twice — find it and verify COUNT=2.
    bool found_dup = false;
    for (const auto& r : results) {
        if (std::get<0>(r) == 1 && std::get<1>(r) == 10) {
            EXPECT_EQ(std::get<2>(r), 2);
            found_dup = true;
        } else {
            // All other groups have count 1.
            EXPECT_EQ(std::get<2>(r), 1);
        }
    }
    EXPECT_TRUE(found_dup);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 3. MultipleAggregates
//    SELECT COUNT(*), SUM(a), AVG(a), MIN(a), MAX(a) FROM t1 — verify all
//    five aggregates compute correctly in a single Agg node.
// ===========================================================================

TEST_F(NodeAggTest, MultipleAggregates) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a = 1..5 → COUNT=5, SUM=15, AVG=3.0, MIN=1, MAX=5
    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->targetlist.push_back(MakeTargetEntry(count_agg, 1, "count"));

    Aggref* sum_agg = MakeAggref(kSumInt4Oid, kInt8Oid, false);
    sum_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(sum_agg, 2, "sum"));

    Aggref* avg_agg = MakeAggref(kAvgInt4Oid, kFloat8Oid, false);
    avg_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(avg_agg, 3, "avg"));

    Aggref* min_agg = MakeAggref(kMinInt4Oid, kInt4Oid, false);
    min_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(min_agg, 4, "min"));

    Aggref* max_agg = MakeAggref(kMaxInt4Oid, kInt4Oid, false);
    max_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(max_agg, 5, "max"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), 5);            // COUNT
    EXPECT_EQ(DatumGetInt64(slot->tts_values[1]), 15);           // SUM
    EXPECT_DOUBLE_EQ(DatumGetFloat8(slot->tts_values[2]), 3.0);  // AVG
    EXPECT_EQ(DatumGetInt32(slot->tts_values[3]), 1);            // MIN
    EXPECT_EQ(DatumGetInt32(slot->tts_values[4]), 5);            // MAX
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 4. CountDistinctBasic
//    COUNT(DISTINCT a) on rows with duplicate values — verifies that the
//    aggdistinct marker is honored. The current Agg executor ignores
//    aggdistinct; this test documents the expected DISTINCT semantics and
//    will fail until the executor is extended to honor it.
// ===========================================================================

TEST_F(NodeAggTest, CountDistinctBasic) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a values: 1, 2, 2, 3, 3, 3 → 3 distinct values
    InsertIntIntRow(rel, 1, 100);
    InsertIntIntRow(rel, 2, 200);
    InsertIntIntRow(rel, 2, 201);
    InsertIntIntRow(rel, 3, 300);
    InsertIntIntRow(rel, 3, 301);
    InsertIntIntRow(rel, 3, 302);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    // COUNT(DISTINCT a): aggfnoid=count, aggtype=int8, arg=Var(a), distinct
    // marker set via aggdistinct (list of SortGroupClause).
    Aggref* count_distinct = MakeAggref(kCountInt4Oid, kInt8Oid, false);
    count_distinct->args.push_back(MakeVar(1, 1, kInt4Oid));
    count_distinct->aggdistinct.push_back(MakeSortGroupClause(1));
    aggplan->targetlist.push_back(MakeTargetEntry(count_distinct, 1, "count_distinct"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->tts_isnull[0]);
    // 3 distinct a values (1, 2, 3) out of 6 rows.
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), 3);
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 5. SumBasic
//    SELECT SUM(a) FROM t1 — verify SUM of int4 returns int8 sum.
// ===========================================================================

TEST_F(NodeAggTest, SumBasic) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a = 1..5 → SUM = 15
    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    Aggref* sum_agg = MakeAggref(kSumInt4Oid, kInt8Oid, false);
    sum_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(sum_agg, 1, "sum"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), 15);
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 6. AvgBasic
//    SELECT AVG(a) FROM t1 — verify AVG of int4 returns float8 average.
// ===========================================================================

TEST_F(NodeAggTest, AvgBasic) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a = 2, 4, 6, 8 → AVG = 5.0
    InsertIntIntRow(rel, 2, 200);
    InsertIntIntRow(rel, 4, 400);
    InsertIntIntRow(rel, 6, 600);
    InsertIntIntRow(rel, 8, 800);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    Aggref* avg_agg = MakeAggref(kAvgInt4Oid, kFloat8Oid, false);
    avg_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(avg_agg, 1, "avg"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(slot->tts_values[0]), 5.0);
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 7. MinMaxBasic
//    SELECT MIN(a), MAX(a) FROM t1 — verify MIN/MAX return the extremes.
// ===========================================================================

TEST_F(NodeAggTest, MinMaxBasic) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a = 5, 1, 3, 4, 2 → MIN=1, MAX=5
    InsertIntIntRow(rel, 5, 50);
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 3, 30);
    InsertIntIntRow(rel, 4, 40);
    InsertIntIntRow(rel, 2, 20);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    Aggref* min_agg = MakeAggref(kMinInt4Oid, kInt4Oid, false);
    min_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(min_agg, 1, "min"));

    Aggref* max_agg = MakeAggref(kMaxInt4Oid, kInt4Oid, false);
    max_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(max_agg, 2, "max"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_FALSE(slot->tts_isnull[1]);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 1);  // MIN
    EXPECT_EQ(DatumGetInt32(slot->tts_values[1]), 5);  // MAX
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 8. GroupBySingleKey
//    GROUP BY a with COUNT(*) and SUM(b) — verify per-group aggregates.
// ===========================================================================

TEST_F(NodeAggTest, GroupBySingleKey) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a=1: b=10, 20 → COUNT=2, SUM(b)=30
    // a=2: b=100 → COUNT=1, SUM(b)=100
    // a=3: b=1000, 2000, 3000 → COUNT=3, SUM(b)=6000
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 1, 20);
    InsertIntIntRow(rel, 2, 100);
    InsertIntIntRow(rel, 3, 1000);
    InsertIntIntRow(rel, 3, 2000);
    InsertIntIntRow(rel, 3, 3000);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;
    aggplan->groupColIdx = {1};  // GROUP BY a

    aggplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));

    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->targetlist.push_back(MakeTargetEntry(count_agg, 2, "count"));

    Aggref* sum_b = MakeAggref(kSumInt4Oid, kInt8Oid, false);
    sum_b->args.push_back(MakeVar(1, 2, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(sum_b, 3, "sum_b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    std::map<int, std::pair<int64_t, int64_t>> by_group;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        int a = DatumGetInt32(slot->tts_values[0]);
        int64_t cnt = DatumGetInt64(slot->tts_values[1]);
        int64_t sum = DatumGetInt64(slot->tts_values[2]);
        by_group[a] = {cnt, sum};
    }
    EXPECT_EQ(by_group.size(), 3u);
    EXPECT_EQ(by_group[1].first, 2);
    EXPECT_EQ(by_group[1].second, 30);
    EXPECT_EQ(by_group[2].first, 1);
    EXPECT_EQ(by_group[2].second, 100);
    EXPECT_EQ(by_group[3].first, 3);
    EXPECT_EQ(by_group[3].second, 6000);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 9. CountStarIncludesAll
//    COUNT(*) counts every row, including rows with NULLs in non-aggregated
//    columns. Contrasts with COUNT(b) which skips NULL b.
// ===========================================================================

TEST_F(NodeAggTest, CountStarIncludesAll) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // 3 rows, one with NULL b.
    InsertIntIntRow(rel, 1, 10);
    InsertIntNullRow(rel, 2);  // b is NULL
    InsertIntIntRow(rel, 3, 30);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    // COUNT(*) — counts all rows.
    Aggref* count_star = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->targetlist.push_back(MakeTargetEntry(count_star, 1, "count_star"));

    // COUNT(b) — skips rows where b is NULL.
    Aggref* count_b = MakeAggref(kCountInt4Oid, kInt8Oid, false);
    count_b->args.push_back(MakeVar(1, 2, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(count_b, 2, "count_b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), 3);  // COUNT(*) = 3
    EXPECT_EQ(DatumGetInt64(slot->tts_values[1]), 2);  // COUNT(b) = 2 (skips NULL)
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 10. EmptyTableAgg
//     SELECT COUNT(*), SUM(a), AVG(a), MIN(a), MAX(a) FROM empty_table —
//     plain agg always returns one row; COUNT=0, others NULL.
// ===========================================================================

TEST_F(NodeAggTest, EmptyTableAgg) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->targetlist.push_back(MakeTargetEntry(count_agg, 1, "count"));

    Aggref* sum_agg = MakeAggref(kSumInt4Oid, kInt8Oid, false);
    sum_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(sum_agg, 2, "sum"));

    Aggref* avg_agg = MakeAggref(kAvgInt4Oid, kFloat8Oid, false);
    avg_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(avg_agg, 3, "avg"));

    Aggref* min_agg = MakeAggref(kMinInt4Oid, kInt4Oid, false);
    min_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(min_agg, 4, "min"));

    Aggref* max_agg = MakeAggref(kMaxInt4Oid, kInt4Oid, false);
    max_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(max_agg, 5, "max"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    // COUNT(*) on empty table returns 0 (never NULL).
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), 0);
    // SUM, AVG, MIN, MAX all return NULL on empty input.
    EXPECT_TRUE(slot->tts_isnull[1]);  // SUM
    EXPECT_TRUE(slot->tts_isnull[2]);  // AVG
    EXPECT_TRUE(slot->tts_isnull[3]);  // MIN
    EXPECT_TRUE(slot->tts_isnull[4]);  // MAX
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 11. GroupByWithHaving
//     GROUP BY a HAVING COUNT(*) > 1 — only groups with more than one row
//     are output.
// ===========================================================================

TEST_F(NodeAggTest, GroupByWithHaving) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a=1: 2 rows → passes HAVING
    // a=2: 1 row → filtered out
    // a=3: 3 rows → passes HAVING
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 1, 11);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 3, 30);
    InsertIntIntRow(rel, 3, 31);
    InsertIntIntRow(rel, 3, 32);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;
    aggplan->groupColIdx = {1};  // GROUP BY a

    aggplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));

    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->targetlist.push_back(MakeTargetEntry(count_agg, 2, "count"));

    // HAVING COUNT(*) > 1
    Aggref* having_count = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->having_qual = MakeOpExpr(kInt4GtOp, kBoolOid, having_count, MakeInt8Const(1));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    std::map<int, int64_t> by_group;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        int a = DatumGetInt32(slot->tts_values[0]);
        int64_t cnt = DatumGetInt64(slot->tts_values[1]);
        by_group[a] = cnt;
    }
    // Only groups with count > 1 should appear (a=1 and a=3, not a=2).
    EXPECT_EQ(by_group.size(), 2u);
    EXPECT_EQ(by_group[1], 2);
    EXPECT_EQ(by_group[3], 3);
    EXPECT_EQ(by_group.count(2), 0u);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 12. MultipleGroupBys
//     GROUP BY a, b with COUNT(*) and SUM(b) — verify aggregates per
//     (a,b) group.
// ===========================================================================

TEST_F(NodeAggTest, MultipleGroupBys) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // (1, 10) x2, (1, 20) x1, (2, 10) x3
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 1, 20);
    InsertIntIntRow(rel, 2, 10);
    InsertIntIntRow(rel, 2, 10);
    InsertIntIntRow(rel, 2, 10);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;
    aggplan->groupColIdx = {1, 2};  // GROUP BY a, b

    aggplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    aggplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->targetlist.push_back(MakeTargetEntry(count_agg, 3, "count"));

    Aggref* sum_b = MakeAggref(kSumInt4Oid, kInt8Oid, false);
    sum_b->args.push_back(MakeVar(1, 2, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(sum_b, 4, "sum_b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    // Key: (a, b) → (count, sum_b).
    std::map<std::pair<int, int>, std::pair<int64_t, int64_t>> by_group;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        int a = DatumGetInt32(slot->tts_values[0]);
        int b = DatumGetInt32(slot->tts_values[1]);
        int64_t cnt = DatumGetInt64(slot->tts_values[2]);
        int64_t sum = DatumGetInt64(slot->tts_values[3]);
        by_group[{a, b}] = {cnt, sum};
    }
    EXPECT_EQ(by_group.size(), 3u);
    // Pull each group's aggregates into locals before comparing — the brace
    // initializer `{1, 10}` would otherwise confuse the EXPECT_EQ macro.
    auto g_1_10 = by_group.at({1, 10});
    auto g_1_20 = by_group.at({1, 20});
    auto g_2_10 = by_group.at({2, 10});
    EXPECT_EQ(g_1_10.first, 2);
    EXPECT_EQ(g_1_10.second, 20);
    EXPECT_EQ(g_1_20.first, 1);
    EXPECT_EQ(g_1_20.second, 20);
    EXPECT_EQ(g_2_10.first, 3);
    EXPECT_EQ(g_2_10.second, 30);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 13. CountStarNoGroup
//     SELECT COUNT(*) FROM t1 (without GROUP BY) — plain aggregation that
//     returns a single row with the total row count.
// ===========================================================================

TEST_F(NodeAggTest, CountStarNoGroup) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    for (int i = 1; i <= 7; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->targetlist.push_back(MakeTargetEntry(count_agg, 1, "count"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), 7);
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 14. SumPositive
//     SELECT SUM(a) FROM t1 with positive integers only.
// ===========================================================================

TEST_F(NodeAggTest, SumPositive) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a = 10, 20, 30, 40 → SUM = 100
    InsertIntIntRow(rel, 10, 1);
    InsertIntIntRow(rel, 20, 1);
    InsertIntIntRow(rel, 30, 1);
    InsertIntIntRow(rel, 40, 1);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    Aggref* sum_agg = MakeAggref(kSumInt4Oid, kInt8Oid, false);
    sum_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(sum_agg, 1, "sum"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), 100);
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// 15. SumNegative
//     SELECT SUM(a) FROM t1 including negative values — verifies SUM handles
//     signed arithmetic correctly.
// ===========================================================================

TEST_F(NodeAggTest, SumNegative) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // a = -10, 5, -3, 8, -20 → SUM = -20
    InsertIntIntRow(rel, -10, 1);
    InsertIntIntRow(rel, 5, 1);
    InsertIntIntRow(rel, -3, 1);
    InsertIntIntRow(rel, 8, 1);
    InsertIntIntRow(rel, -20, 1);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = MakeSeqScanAB();
    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    Aggref* sum_agg = MakeAggref(kSumInt4Oid, kInt8Oid, false);
    sum_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(sum_agg, 1, "sum"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = aggplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), -20);
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

}  // namespace
