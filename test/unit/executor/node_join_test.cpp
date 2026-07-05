// node_join_test.cpp — Unit tests for join nodes (NestLoop, HashJoin).
//
// Tests NestLoop and HashJoin execution with various data scenarios:
//   - Empty left/right relations
//   - Cross product (no join qual)
//   - Equi-join on int4 columns
//   - LEFT JOIN with NULL-padded unmatched rows
//   - Duplicate keys and multiple matches
//   - Joins on non-first attributes
//
// The fixture is identical to executor_test.cpp's (renamed), setting up the
// full stack: error subsystem, memory context, catalog + syscache,
// transaction system, buffer pool, storage directory, and relcache.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <set>
#include <string>
#include <tuple>
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
#include "types/datetime.hpp"
#include "types/datum.hpp"

using pgcpp::access::CreateTupleDesc;
using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_form_tuple;
using pgcpp::access::heap_freetuple;
using pgcpp::access::heap_getnext;
using pgcpp::access::heap_insert;
using pgcpp::access::HeapScanDesc;
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
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::executor::Agg;
using pgcpp::executor::Append;
using pgcpp::executor::BuildTupleDescFromTargetList;
using pgcpp::executor::CreateExprContext;
using pgcpp::executor::CteScan;
using pgcpp::executor::EState;
using pgcpp::executor::ExecEndNode;
using pgcpp::executor::ExecEvalExpr;
using pgcpp::executor::ExecInitNode;
using pgcpp::executor::ExecProject;
using pgcpp::executor::ExecQual;
using pgcpp::executor::ExecutorEnd;
using pgcpp::executor::ExecutorFinish;
using pgcpp::executor::ExecutorRun;
using pgcpp::executor::ExecutorStart;
using pgcpp::executor::ExprContext;
using pgcpp::executor::HashJoin;
using pgcpp::executor::Limit;
using pgcpp::executor::MakeTupleTableSlot;
using pgcpp::executor::Material;
using pgcpp::executor::MergeJoin;
using pgcpp::executor::ModifyTable;
using pgcpp::executor::NestLoop;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanState;
using pgcpp::executor::PlanType;
using pgcpp::executor::QueryDesc;
using pgcpp::executor::ResetExprContext;
using pgcpp::executor::Result;
using pgcpp::executor::SeqScan;
using pgcpp::executor::Sort;
using pgcpp::executor::SubqueryScan;
using pgcpp::executor::TupleTableSlot;
using pgcpp::executor::Unique;
using pgcpp::executor::WindowAgg;
using pgcpp::memory::AllocSetContext;
using pgcpp::memory::palloc;
using pgcpp::memory::pfree;
using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::AArrayExpr;
using pgcpp::parser::Aggref;
using pgcpp::parser::CaseExpr;
using pgcpp::parser::CaseWhen;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::JoinType;
using pgcpp::parser::kInnerVar;
using pgcpp::parser::kOuterVar;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RTEKind;
using pgcpp::parser::ScalarArrayOpExpr;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::AllocateNextTransactionId;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::MakeSnapshot;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::SnapshotData;
using pgcpp::transaction::TransactionIdCommit;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::Int64GetDatum;
using pgcpp::types::kBoolOid;
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

class NodeJoinTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("executor_test_context");
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

        test_dir_ = "/tmp/pgcpp_join_test_" + std::to_string(getpid());
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

    // Helper: commit and start a new transaction so inserted tuples are visible.
    void CommitAndStartNew() {
        EndTransactionBlock();
        pgcpp::transaction::InitializeSnapshotManager();
        BeginTransactionBlock();
    }

    // Helper: build a pg_class row and insert it into the catalog.
    FormData_pg_class* MakeClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kRelation;
        row->relpersistence = RelPersistence::kPermanent;
        return row;
    }

    // Helper: build a pg_attribute row.
    FormData_pg_attribute* MakeAttrRow(Oid relid, const std::string& name, int16_t attnum,
                                       Oid typid, int16_t attlen, bool attbyval,
                                       AttAlign attalign) {
        auto* row = makePallocNode<FormData_pg_attribute>();
        row->attrelid = relid;
        row->attname = name;
        row->attnum = attnum;
        row->atttypid = typid;
        row->attlen = attlen;
        row->attbyval = attbyval;
        row->attalign = attalign;
        row->attstorage = AttStorage::kPlain;
        return row;
    }

    // Helper: create a relation with the given OID and schema.
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

    // Helper: build a simple 2-column int4 schema (a, b).
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

    // Helper: insert a row (int4, int4) into a relation.
    void InsertIntIntRow(Relation rel, int32_t a, int32_t b) {
        TupleDesc tupdesc = rel->rd_att;
        Datum values[2] = {Int32GetDatum(a), Int32GetDatum(b)};
        bool isnull[2] = {false, false};
        HeapTuple tup = heap_form_tuple(tupdesc, values, isnull);
        heap_insert(rel, tup);
        heap_freetuple(tup);
    }

    // Helper: create a RangeTblEntry for a relation.
    RangeTblEntry* MakeRTE(Oid relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = static_cast<int>(relid);
        return rte;
    }

    // Helper: create a Var node.
    Var* MakeVar(int varno, int varattno, Oid vartype) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = vartype;
        return var;
    }

    // Helper: create a Const node for int4.
    Const* MakeInt4Const(int32_t value) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(value);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 4;
        return con;
    }

    // Helper: create a TargetEntry.
    TargetEntry* MakeTargetEntry(Node* expr, int resno, const std::string& resname = "") {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = resname;
        return te;
    }

    // Helper: create an OpExpr (e.g., a = b).
    OpExpr* MakeOpExpr(Oid opno, Oid resulttype, Node* left, Node* right) {
        auto* op = makePallocNode<OpExpr>();
        op->opno = opno;
        op->opresulttype = resulttype;
        op->args.push_back(left);
        op->args.push_back(right);
        return op;
    }

    // Helper: create a Query for SELECT.
    Query* MakeSelectQuery(std::vector<RangeTblEntry*> rtable) {
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kSelect;
        for (auto* rte : rtable) {
            query->rtable.push_back(rte);
        }
        return query;
    }

    // Helper: build a left SeqScan plan for a 2-column relation (varno).
    SeqScan* MakeSeqScanPlan(int varno) {
        auto* scan = makePallocNode<SeqScan>();
        scan->scanrelid = varno;
        scan->targetlist.push_back(MakeTargetEntry(MakeVar(varno, 1, kInt4Oid), 1, "a"));
        scan->targetlist.push_back(MakeTargetEntry(MakeVar(varno, 2, kInt4Oid), 2, "b"));
        return scan;
    }

    // Helper: build a NestLoop inner/left join plan with equi-join qual on
    // column `attno` of both sides. Output: outer.a, outer.b, inner.b.
    NestLoop* MakeNestLoopPlan(JoinType jt, SeqScan* left, SeqScan* right, int attno) {
        auto* nl = makePallocNode<NestLoop>();
        nl->jointype = jt;
        nl->lefttree = left;
        nl->righttree = right;
        nl->qual = MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(kOuterVar, attno, kInt4Oid),
                              MakeVar(kInnerVar, attno, kInt4Oid));
        nl->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 1, kInt4Oid), 1, "a"));
        nl->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 2, kInt4Oid), 2, "b1"));
        nl->targetlist.push_back(MakeTargetEntry(MakeVar(kInnerVar, 2, kInt4Oid), 3, "b2"));
        return nl;
    }

    // Helper: build a HashJoin inner/left join plan with equi-join hashclause
    // on column `attno` of both sides. Output: outer.a, outer.b, inner.b.
    HashJoin* MakeHashJoinPlan(JoinType jt, SeqScan* left, SeqScan* right, int attno) {
        auto* right_hash = makePallocNode<pgcpp::executor::Hash>();
        right_hash->lefttree = right;
        right_hash->targetlist.push_back(MakeTargetEntry(MakeVar(2, 1, kInt4Oid), 1, "a"));
        right_hash->targetlist.push_back(MakeTargetEntry(MakeVar(2, 2, kInt4Oid), 2, "b"));

        auto* hj = makePallocNode<HashJoin>();
        hj->jointype = jt;
        hj->lefttree = left;
        hj->righttree = right_hash;
        hj->hashclauses.push_back(MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(1, attno, kInt4Oid),
                                             MakeVar(2, attno, kInt4Oid)));
        hj->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 1, kInt4Oid), 1, "a"));
        hj->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 2, kInt4Oid), 2, "b1"));
        hj->targetlist.push_back(MakeTargetEntry(MakeVar(kInnerVar, 2, kInt4Oid), 3, "b2"));
        return hj;
    }

    // Helper: run a join plan and collect (a, b1, b2) result rows.
    std::vector<std::tuple<int, int, int>> RunJoin(QueryDesc* qd) {
        ExecutorStart(qd);
        std::vector<std::tuple<int, int, int>> results;
        TupleTableSlot* slot = nullptr;
        while ((slot = ExecutorRun(qd)) != nullptr) {
            int a = slot->tts_isnull[0] ? -1 : DatumGetInt32(slot->tts_values[0]);
            int b1 = slot->tts_isnull[1] ? -1 : DatumGetInt32(slot->tts_values[1]);
            int b2 = slot->tts_isnull[2] ? -1 : DatumGetInt32(slot->tts_values[2]);
            results.emplace_back(a, b1, b2);
        }
        ExecutorFinish(qd);
        ExecutorEnd(qd);
        return results;
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
// 1. NestLoopEmptyLeft — empty left relation, NestLoop returns no rows
// ===========================================================================

TEST_F(NodeJoinTest, NestLoopEmptyLeft) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    // t1 left empty; t2 has 2 rows.
    InsertIntIntRow(rel2, 2, 200);
    InsertIntIntRow(rel2, 3, 300);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* nlplan = MakeNestLoopPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = nlplan;

    auto results = RunJoin(qd);
    EXPECT_EQ(results.size(), 0u);
}

// ===========================================================================
// 2. NestLoopEmptyRight — empty right relation, NestLoop returns no rows
// ===========================================================================

TEST_F(NodeJoinTest, NestLoopEmptyRight) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 2, 20);
    // t2 right empty.
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* nlplan = MakeNestLoopPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = nlplan;

    auto results = RunJoin(qd);
    EXPECT_EQ(results.size(), 0u);
}

// ===========================================================================
// 3. NestLoopCrossProduct — no join qual, Cartesian product (2x3=6 rows)
// ===========================================================================

TEST_F(NodeJoinTest, NestLoopCrossProduct) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 2, 20);
    InsertIntIntRow(rel2, 100, 1);
    InsertIntIntRow(rel2, 200, 2);
    InsertIntIntRow(rel2, 300, 3);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    // NestLoop with no qual (qual stays nullptr) → Cartesian product.
    auto* nlplan = makePallocNode<NestLoop>();
    nlplan->jointype = JoinType::kInner;
    nlplan->lefttree = MakeSeqScanPlan(1);
    nlplan->righttree = MakeSeqScanPlan(2);
    nlplan->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 1, kInt4Oid), 1, "a"));
    nlplan->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 2, kInt4Oid), 2, "b1"));
    nlplan->targetlist.push_back(MakeTargetEntry(MakeVar(kInnerVar, 2, kInt4Oid), 3, "b2"));

    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = nlplan;

    auto results = RunJoin(qd);
    EXPECT_EQ(results.size(), 6u);  // 2 * 3
}

// ===========================================================================
// 4. NestLoopEquiJoin — equi-join on int4 column, verify matched rows
// ===========================================================================

TEST_F(NodeJoinTest, NestLoopEquiJoin) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 2, 20);
    InsertIntIntRow(rel1, 3, 30);
    InsertIntIntRow(rel2, 2, 200);
    InsertIntIntRow(rel2, 3, 300);
    InsertIntIntRow(rel2, 4, 400);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* nlplan = MakeNestLoopPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = nlplan;

    auto results = RunJoin(qd);
    ASSERT_EQ(results.size(), 2u);
    bool found_2 = false, found_3 = false;
    for (const auto& r : results) {
        if (std::get<0>(r) == 2) {
            EXPECT_EQ(std::get<1>(r), 20);
            EXPECT_EQ(std::get<2>(r), 200);
            found_2 = true;
        }
        if (std::get<0>(r) == 3) {
            EXPECT_EQ(std::get<1>(r), 30);
            EXPECT_EQ(std::get<2>(r), 300);
            found_3 = true;
        }
    }
    EXPECT_TRUE(found_2);
    EXPECT_TRUE(found_3);
}

// ===========================================================================
// 5. NestLoopLeftJoin — LEFT JOIN, unmatched left rows have NULL right columns
// ===========================================================================

TEST_F(NodeJoinTest, NestLoopLeftJoin) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 2, 20);
    InsertIntIntRow(rel1, 3, 30);
    // Only key 2 matches on the right.
    InsertIntIntRow(rel2, 2, 200);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* nlplan = MakeNestLoopPlan(JoinType::kLeft, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = nlplan;

    ExecutorStart(qd);
    std::vector<std::tuple<int, int, bool>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        int a = DatumGetInt32(slot->tts_values[0]);
        int b1 = DatumGetInt32(slot->tts_values[1]);
        bool b2_null = slot->tts_isnull[2];
        int b2 = b2_null ? 0 : DatumGetInt32(slot->tts_values[2]);
        results.emplace_back(a, b1, b2_null);
        (void)b2;
    }
    ExecutorFinish(qd);
    ExecutorEnd(qd);

    ASSERT_EQ(results.size(), 3u);
    // Collect by left key.
    bool found_1 = false, found_2 = false, found_3 = false;
    for (const auto& r : results) {
        int a = std::get<0>(r);
        int b1 = std::get<1>(r);
        bool b2_null = std::get<2>(r);
        if (a == 1) {
            EXPECT_EQ(b1, 10);
            EXPECT_TRUE(b2_null);  // unmatched → NULL
            found_1 = true;
        }
        if (a == 2) {
            EXPECT_EQ(b1, 20);
            EXPECT_FALSE(b2_null);
            found_2 = true;
        }
        if (a == 3) {
            EXPECT_EQ(b1, 30);
            EXPECT_TRUE(b2_null);  // unmatched → NULL
            found_3 = true;
        }
    }
    EXPECT_TRUE(found_1);
    EXPECT_TRUE(found_2);
    EXPECT_TRUE(found_3);
}

// ===========================================================================
// 6. HashJoinEmptyLeft — empty left, no output
// ===========================================================================

TEST_F(NodeJoinTest, HashJoinEmptyLeft) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    // t1 empty; t2 has rows.
    InsertIntIntRow(rel2, 2, 200);
    InsertIntIntRow(rel2, 3, 300);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* hjplan = MakeHashJoinPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = hjplan;

    auto results = RunJoin(qd);
    EXPECT_EQ(results.size(), 0u);
}

// ===========================================================================
// 7. HashJoinEmptyRight — empty right, no output
// ===========================================================================

TEST_F(NodeJoinTest, HashJoinEmptyRight) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 2, 20);
    // t2 empty.
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* hjplan = MakeHashJoinPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = hjplan;

    auto results = RunJoin(qd);
    EXPECT_EQ(results.size(), 0u);
}

// ===========================================================================
// 8. HashJoinEquiJoin — basic hash equi-join
// ===========================================================================

TEST_F(NodeJoinTest, HashJoinEquiJoin) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 2, 20);
    InsertIntIntRow(rel1, 3, 30);
    InsertIntIntRow(rel2, 2, 200);
    InsertIntIntRow(rel2, 3, 300);
    InsertIntIntRow(rel2, 4, 400);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* hjplan = MakeHashJoinPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = hjplan;

    auto results = RunJoin(qd);
    ASSERT_EQ(results.size(), 2u);
    bool found_2 = false, found_3 = false;
    for (const auto& r : results) {
        if (std::get<0>(r) == 2) {
            EXPECT_EQ(std::get<1>(r), 20);
            EXPECT_EQ(std::get<2>(r), 200);
            found_2 = true;
        }
        if (std::get<0>(r) == 3) {
            EXPECT_EQ(std::get<1>(r), 30);
            EXPECT_EQ(std::get<2>(r), 300);
            found_3 = true;
        }
    }
    EXPECT_TRUE(found_2);
    EXPECT_TRUE(found_3);
}

// ===========================================================================
// 9. HashJoinNoMatch — no matching keys, empty output
// ===========================================================================

TEST_F(NodeJoinTest, HashJoinNoMatch) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 2, 20);
    InsertIntIntRow(rel2, 3, 300);
    InsertIntIntRow(rel2, 4, 400);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* hjplan = MakeHashJoinPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = hjplan;

    auto results = RunJoin(qd);
    EXPECT_EQ(results.size(), 0u);
}

// ===========================================================================
// 10. HashJoinDuplicateKeys — duplicate keys in both relations
// ===========================================================================

TEST_F(NodeJoinTest, HashJoinDuplicateKeys) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    // t1: two rows with key=1, one with key=2.
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 1, 11);
    InsertIntIntRow(rel1, 2, 20);
    // t2: two rows with key=1, one with key=2.
    InsertIntIntRow(rel2, 1, 100);
    InsertIntIntRow(rel2, 1, 101);
    InsertIntIntRow(rel2, 2, 200);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* hjplan = MakeHashJoinPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = hjplan;

    auto results = RunJoin(qd);
    // (1,10)×(1,100),(1,101) + (1,11)×(1,100),(1,101) + (2,20)×(2,200) = 5.
    EXPECT_EQ(results.size(), 5u);
    // Count how many have key=1 vs key=2.
    int count_key1 = 0, count_key2 = 0;
    for (const auto& r : results) {
        if (std::get<0>(r) == 1)
            count_key1++;
        if (std::get<0>(r) == 2)
            count_key2++;
    }
    EXPECT_EQ(count_key1, 4);
    EXPECT_EQ(count_key2, 1);
}

// ===========================================================================
// 11. HashJoinLeftJoin — LEFT JOIN with hash
// ===========================================================================

TEST_F(NodeJoinTest, HashJoinLeftJoin) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 2, 20);
    InsertIntIntRow(rel1, 3, 30);
    // Only key 2 matches on the right.
    InsertIntIntRow(rel2, 2, 200);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* hjplan = MakeHashJoinPlan(JoinType::kLeft, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = hjplan;

    ExecutorStart(qd);
    std::vector<std::tuple<int, int, bool>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        int a = DatumGetInt32(slot->tts_values[0]);
        int b1 = DatumGetInt32(slot->tts_values[1]);
        bool b2_null = slot->tts_isnull[2];
        results.emplace_back(a, b1, b2_null);
    }
    ExecutorFinish(qd);
    ExecutorEnd(qd);

    // LEFT JOIN should produce 3 rows: (1,10,NULL), (2,20,200), (3,30,NULL).
    ASSERT_EQ(results.size(), 3u);
    bool found_1 = false, found_2 = false, found_3 = false;
    for (const auto& r : results) {
        int a = std::get<0>(r);
        bool b2_null = std::get<2>(r);
        if (a == 1) {
            EXPECT_EQ(std::get<1>(r), 10);
            EXPECT_TRUE(b2_null);
            found_1 = true;
        }
        if (a == 2) {
            EXPECT_EQ(std::get<1>(r), 20);
            EXPECT_FALSE(b2_null);
            found_2 = true;
        }
        if (a == 3) {
            EXPECT_EQ(std::get<1>(r), 30);
            EXPECT_TRUE(b2_null);
            found_3 = true;
        }
    }
    EXPECT_TRUE(found_1);
    EXPECT_TRUE(found_2);
    EXPECT_TRUE(found_3);
}

// ===========================================================================
// 12. JoinThreeColumn — join on 2nd column (non-first attribute)
// ===========================================================================

TEST_F(NodeJoinTest, JoinThreeColumn) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    // Join on column b (attno=2).
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 2, 20);
    InsertIntIntRow(rel1, 3, 30);
    InsertIntIntRow(rel2, 100, 10);
    InsertIntIntRow(rel2, 200, 20);
    InsertIntIntRow(rel2, 300, 40);  // no match
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    // NestLoop on column 2 (b) of both sides. Build a custom plan that
    // projects (outer.a, outer.b, inner.a) — MakeNestLoopPlan always
    // projects inner.b as the third column, which is the join column
    // here and therefore redundant.
    auto* nl = makePallocNode<NestLoop>();
    nl->jointype = JoinType::kInner;
    nl->lefttree = MakeSeqScanPlan(1);
    nl->righttree = MakeSeqScanPlan(2);
    nl->qual = MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(kOuterVar, 2, kInt4Oid),
                          MakeVar(kInnerVar, 2, kInt4Oid));
    nl->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 1, kInt4Oid), 1, "a"));
    nl->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 2, kInt4Oid), 2, "b1"));
    nl->targetlist.push_back(MakeTargetEntry(MakeVar(kInnerVar, 1, kInt4Oid), 3, "a2"));
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = nl;

    auto results = RunJoin(qd);
    ASSERT_EQ(results.size(), 2u);
    // (1,10) matches (100,10) → output (1,10,100)
    // (2,20) matches (200,20) → output (2,20,200)
    bool found_1 = false, found_2 = false;
    for (const auto& r : results) {
        if (std::get<0>(r) == 1) {
            EXPECT_EQ(std::get<1>(r), 10);
            EXPECT_EQ(std::get<2>(r), 100);
            found_1 = true;
        }
        if (std::get<0>(r) == 2) {
            EXPECT_EQ(std::get<1>(r), 20);
            EXPECT_EQ(std::get<2>(r), 200);
            found_2 = true;
        }
    }
    EXPECT_TRUE(found_1);
    EXPECT_TRUE(found_2);
}

// ===========================================================================
// 13. NestLoopMultipleMatches — one left row matches multiple right rows
// ===========================================================================

TEST_F(NodeJoinTest, NestLoopMultipleMatches) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    InsertIntIntRow(rel1, 1, 10);
    // Three right rows with same key=1.
    InsertIntIntRow(rel2, 1, 100);
    InsertIntIntRow(rel2, 1, 200);
    InsertIntIntRow(rel2, 1, 300);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* nlplan = MakeNestLoopPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = nlplan;

    auto results = RunJoin(qd);
    ASSERT_EQ(results.size(), 3u);
    // All rows should have a=1, b1=10; b2 should be 100, 200, 300 in some order.
    std::set<int> b2_values;
    for (const auto& r : results) {
        EXPECT_EQ(std::get<0>(r), 1);
        EXPECT_EQ(std::get<1>(r), 10);
        b2_values.insert(std::get<2>(r));
    }
    EXPECT_EQ(b2_values.size(), 3u);
    EXPECT_TRUE(b2_values.count(100));
    EXPECT_TRUE(b2_values.count(200));
    EXPECT_TRUE(b2_values.count(300));
}

// ===========================================================================
// 14. HashJoinMultipleMatches — one left row matches multiple right rows
// ===========================================================================

TEST_F(NodeJoinTest, HashJoinMultipleMatches) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel2, 1, 100);
    InsertIntIntRow(rel2, 1, 200);
    InsertIntIntRow(rel2, 1, 300);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* hjplan = MakeHashJoinPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = hjplan;

    auto results = RunJoin(qd);
    ASSERT_EQ(results.size(), 3u);
    std::set<int> b2_values;
    for (const auto& r : results) {
        EXPECT_EQ(std::get<0>(r), 1);
        EXPECT_EQ(std::get<1>(r), 10);
        b2_values.insert(std::get<2>(r));
    }
    EXPECT_EQ(b2_values.size(), 3u);
    EXPECT_TRUE(b2_values.count(100));
    EXPECT_TRUE(b2_values.count(200));
    EXPECT_TRUE(b2_values.count(300));
}

// ===========================================================================
// 15. NestLoopBothEmpty — both relations empty, no output
// ===========================================================================

TEST_F(NodeJoinTest, NestLoopBothEmpty) {
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    // Both empty.
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    auto* nlplan = MakeNestLoopPlan(JoinType::kInner, MakeSeqScanPlan(1), MakeSeqScanPlan(2), 1);
    auto* query = MakeSelectQuery({MakeRTE(relid1), MakeRTE(relid2)});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = nlplan;

    auto results = RunJoin(qd);
    EXPECT_EQ(results.size(), 0u);
}

}  // namespace
