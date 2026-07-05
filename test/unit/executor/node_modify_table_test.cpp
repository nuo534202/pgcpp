// node_modify_table_test.cpp — Unit tests for the ModifyTable node.
//
// Tests INSERT, DELETE, and UPDATE operations through the ModifyTable executor
// node:
//   - INSERT via Result/Append/SeqScan child plans
//   - DELETE via SeqScan child (filtered and unfiltered)
//   - UPDATE via SeqScan child with projection of new values
//   - Combinations (insert-then-delete, insert-then-update)
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

class NodeModifyTableTest : public ::testing::Test {
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

        test_dir_ = "/tmp/pgcpp_modify_test_" + std::to_string(getpid());
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

    // Helper: build a Result plan producing one (a, b) int4 row.
    Result* MakeResultRowPlan(int32_t a, int32_t b) {
        auto* result_plan = makePallocNode<Result>();
        result_plan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(a), 1, "a"));
        result_plan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(b), 2, "b"));
        return result_plan;
    }

    // Helper: build a SeqScan plan that scans range-table entry `varno` and
    // projects (a, b) columns.
    SeqScan* MakeSeqScanPlan(int varno) {
        auto* scan = makePallocNode<SeqScan>();
        scan->scanrelid = varno;
        scan->targetlist.push_back(MakeTargetEntry(MakeVar(varno, 1, kInt4Oid), 1, "a"));
        scan->targetlist.push_back(MakeTargetEntry(MakeVar(varno, 2, kInt4Oid), 2, "b"));
        return scan;
    }

    // Helper: build a ModifyTable plan for the given operation on RT entry 1.
    ModifyTable* MakeModifyTablePlan(CmdType op, Plan* child) {
        auto* mt = makePallocNode<ModifyTable>();
        mt->operation = op;
        mt->resultRelid = 1;
        mt->lefttree = child;
        mt->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
        mt->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
        return mt;
    }

    // Helper: build a QueryDesc for a DML operation on RT index 1.
    QueryDesc* MakeDmlQueryDesc(CmdType op, Oid relid, Plan* plan) {
        auto* rte = MakeRTE(relid);
        auto* query = makePallocNode<Query>();
        query->command_type = op;
        query->result_relation = 1;
        query->rtable.push_back(rte);
        auto* qd = makePallocNode<QueryDesc>();
        qd->query = query;
        qd->plan = plan;
        return qd;
    }

    // Helper: execute a DML plan to completion (drains all output slots).
    void RunDml(QueryDesc* qd) {
        ExecutorStart(qd);
        TupleTableSlot* slot = nullptr;
        while ((slot = ExecutorRun(qd)) != nullptr) {
            // DML may return tuples for RETURNING; drain them all.
        }
        ExecutorFinish(qd);
        ExecutorEnd(qd);
    }

    // Helper: scan a relation and return all (a, b) rows. Caller must have
    // committed (CommitAndStartNew) before calling so inserted/deleted rows
    // are visible.
    std::vector<std::pair<int, int>> ScanAllRows(Oid relid) {
        Relation rel = RelationOpen(relid);
        HeapScanDesc scan = heap_beginscan(rel, nullptr);
        std::vector<std::pair<int, int>> rows;
        HeapTuple tup = nullptr;
        while ((tup = heap_getnext(scan)) != nullptr) {
            TupleTableSlot* slot = TupleTableSlot::Make(rel->rd_att);
            slot->StoreTuple(tup, false);
            int a = slot->tts_isnull[0] ? 0 : DatumGetInt32(slot->tts_values[0]);
            int b = slot->tts_isnull[1] ? 0 : DatumGetInt32(slot->tts_values[1]);
            rows.emplace_back(a, b);
        }
        heap_endscan(scan);
        RelationClose(rel);
        return rows;
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
// 1. InsertSingleRow — INSERT one row, verify via heap scan
// ===========================================================================

TEST_F(NodeModifyTableTest, InsertSingleRow) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    RelationClose(rel);

    // Child: Result producing one row (42, 99).
    auto* mt_plan = MakeModifyTablePlan(CmdType::kInsert, MakeResultRowPlan(42, 99));
    auto* qd = MakeDmlQueryDesc(CmdType::kInsert, relid, mt_plan);
    RunDml(qd);

    // Verify the row was inserted.
    CommitAndStartNew();
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].first, 42);
    EXPECT_EQ(rows[0].second, 99);
}

// ===========================================================================
// 2. InsertMultipleRows — INSERT 3 rows, verify count via scan
// ===========================================================================

TEST_F(NodeModifyTableTest, InsertMultipleRows) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    RelationClose(rel);

    // Child: Append of three Result children, each producing one row.
    auto* append_plan = makePallocNode<Append>();
    append_plan->append_plans.push_back(MakeResultRowPlan(1, 10));
    append_plan->append_plans.push_back(MakeResultRowPlan(2, 20));
    append_plan->append_plans.push_back(MakeResultRowPlan(3, 30));
    append_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    append_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* mt_plan = MakeModifyTablePlan(CmdType::kInsert, append_plan);
    auto* qd = MakeDmlQueryDesc(CmdType::kInsert, relid, mt_plan);
    RunDml(qd);

    // Verify 3 rows were inserted.
    CommitAndStartNew();
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u);
    std::set<int> a_values;
    for (const auto& r : rows) {
        a_values.insert(r.first);
    }
    EXPECT_EQ(a_values.size(), 3u);
    EXPECT_TRUE(a_values.count(1));
    EXPECT_TRUE(a_values.count(2));
    EXPECT_TRUE(a_values.count(3));
}

// ===========================================================================
// 3. InsertAndRead — INSERT then SELECT to verify data persists
// ===========================================================================

TEST_F(NodeModifyTableTest, InsertAndRead) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    RelationClose(rel);

    // INSERT one row (7, 77).
    auto* mt_plan = MakeModifyTablePlan(CmdType::kInsert, MakeResultRowPlan(7, 77));
    auto* qd = MakeDmlQueryDesc(CmdType::kInsert, relid, mt_plan);
    RunDml(qd);

    // Commit so the inserted row is visible to a new snapshot.
    CommitAndStartNew();

    // SELECT via SeqScan to verify persistence.
    auto* seqplan = MakeSeqScanPlan(1);
    auto* rte = MakeRTE(relid);
    auto* qd_sel = makePallocNode<QueryDesc>();
    qd_sel->query = MakeSelectQuery({rte});
    qd_sel->plan = seqplan;

    ExecutorStart(qd_sel);
    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd_sel)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }
    ExecutorFinish(qd_sel);
    ExecutorEnd(qd_sel);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 7);
    EXPECT_EQ(results[0].second, 77);
}

// ===========================================================================
// 4. InsertZeroRows — INSERT with empty source, verify table stays empty
// ===========================================================================

TEST_F(NodeModifyTableTest, InsertZeroRows) {
    Oid target_relid = kFirstNormalObjectId;
    Oid source_relid = kFirstNormalObjectId + 1;
    Relation target = CreateTestRelation(target_relid, "target", MakeIntIntSchema(target_relid));
    Relation source = CreateTestRelation(source_relid, "source", MakeIntIntSchema(source_relid));
    // Source is intentionally left empty.
    CommitAndStartNew();
    RelationClose(target);
    RelationClose(source);

    // Child: SeqScan over the empty source relation (range table entry 2).
    // Target table is range table entry 1.
    auto* seqplan = MakeSeqScanPlan(2);
    auto* mt_plan = makePallocNode<ModifyTable>();
    mt_plan->operation = CmdType::kInsert;
    mt_plan->resultRelid = 1;  // target is RT entry 1
    mt_plan->lefttree = seqplan;
    mt_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    mt_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte_target = MakeRTE(target_relid);
    auto* rte_source = MakeRTE(source_relid);
    auto* query = makePallocNode<Query>();
    query->command_type = CmdType::kInsert;
    query->result_relation = 1;
    query->rtable.push_back(rte_target);
    query->rtable.push_back(rte_source);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = mt_plan;
    RunDml(qd);

    // Verify the target table is still empty.
    CommitAndStartNew();
    auto rows = ScanAllRows(target_relid);
    EXPECT_EQ(rows.size(), 0u);
}

// ===========================================================================
// 5. DeleteAllRows — DELETE all rows (no qual), verify table is empty
// ===========================================================================

TEST_F(NodeModifyTableTest, DeleteAllRows) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 3, 30);
    CommitAndStartNew();
    RelationClose(rel);

    // Child: SeqScan over the target relation with no qual.
    auto* seqplan = MakeSeqScanPlan(1);
    auto* mt_plan = MakeModifyTablePlan(CmdType::kDelete, seqplan);
    auto* qd = MakeDmlQueryDesc(CmdType::kDelete, relid, mt_plan);
    RunDml(qd);

    // Verify the table is now empty.
    CommitAndStartNew();
    auto rows = ScanAllRows(relid);
    EXPECT_EQ(rows.size(), 0u);
}

// ===========================================================================
// 6. DeleteWithCondition — DELETE WHERE a = 42, verify only matching rows deleted
// ===========================================================================

TEST_F(NodeModifyTableTest, DeleteWithCondition) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 42, 420);
    InsertIntIntRow(rel, 42, 430);
    InsertIntIntRow(rel, 3, 30);
    CommitAndStartNew();
    RelationClose(rel);

    // Child: SeqScan with qual a = 42.
    auto* seqplan = MakeSeqScanPlan(1);
    seqplan->qual = MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(42));

    auto* mt_plan = MakeModifyTablePlan(CmdType::kDelete, seqplan);
    auto* qd = MakeDmlQueryDesc(CmdType::kDelete, relid, mt_plan);
    RunDml(qd);

    // Verify: only rows with a=1 and a=3 remain.
    CommitAndStartNew();
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 2u);
    std::set<int> a_values;
    for (const auto& r : rows) {
        a_values.insert(r.first);
    }
    EXPECT_TRUE(a_values.count(1));
    EXPECT_TRUE(a_values.count(3));
    EXPECT_FALSE(a_values.count(42));
}

// ===========================================================================
// 7. DeleteNoMatch — DELETE WHERE a = 999 (no match), verify no rows deleted
// ===========================================================================

TEST_F(NodeModifyTableTest, DeleteNoMatch) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 3, 30);
    CommitAndStartNew();
    RelationClose(rel);

    // Child: SeqScan with qual a = 999 (matches nothing).
    auto* seqplan = MakeSeqScanPlan(1);
    seqplan->qual = MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(999));

    auto* mt_plan = MakeModifyTablePlan(CmdType::kDelete, seqplan);
    auto* qd = MakeDmlQueryDesc(CmdType::kDelete, relid, mt_plan);
    RunDml(qd);

    // Verify: all 3 rows still present.
    CommitAndStartNew();
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u);
}

// ===========================================================================
// 8. UpdateAllRows — UPDATE SET b = 0 (no qual), verify all rows updated
// ===========================================================================

TEST_F(NodeModifyTableTest, UpdateAllRows) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 3, 30);
    CommitAndStartNew();
    RelationClose(rel);

    // Child: SeqScan projecting (a, 0) — i.e. SET b = 0.
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(0), 2, "b"));

    auto* mt_plan = MakeModifyTablePlan(CmdType::kUpdate, seqplan);
    auto* qd = MakeDmlQueryDesc(CmdType::kUpdate, relid, mt_plan);
    RunDml(qd);

    // Verify: all rows have b = 0.
    CommitAndStartNew();
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u);
    for (const auto& r : rows) {
        EXPECT_EQ(r.second, 0);
    }
}

// ===========================================================================
// 9. UpdateWithCondition — UPDATE SET b = 100 WHERE a = 1
// ===========================================================================

TEST_F(NodeModifyTableTest, UpdateWithCondition) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 3, 30);
    CommitAndStartNew();
    RelationClose(rel);

    // Child: SeqScan projecting (a, 100) with qual a = 1.
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(100), 2, "b"));
    seqplan->qual = MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(1));

    auto* mt_plan = MakeModifyTablePlan(CmdType::kUpdate, seqplan);
    auto* qd = MakeDmlQueryDesc(CmdType::kUpdate, relid, mt_plan);
    RunDml(qd);

    // Verify: row with a=1 has b=100; others unchanged.
    CommitAndStartNew();
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u);
    for (const auto& r : rows) {
        if (r.first == 1) {
            EXPECT_EQ(r.second, 100);
        } else if (r.first == 2) {
            EXPECT_EQ(r.second, 20);
        } else if (r.first == 3) {
            EXPECT_EQ(r.second, 30);
        }
    }
}

// ===========================================================================
// 10. UpdateNoMatch — UPDATE WHERE a = 999 (no match), verify no rows changed
// ===========================================================================

TEST_F(NodeModifyTableTest, UpdateNoMatch) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 3, 30);
    CommitAndStartNew();
    RelationClose(rel);

    // Child: SeqScan projecting (a, 999) with qual a = 999 (matches nothing).
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(999), 2, "b"));
    seqplan->qual = MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(999));

    auto* mt_plan = MakeModifyTablePlan(CmdType::kUpdate, seqplan);
    auto* qd = MakeDmlQueryDesc(CmdType::kUpdate, relid, mt_plan);
    RunDml(qd);

    // Verify: all rows unchanged.
    CommitAndStartNew();
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u);
    std::set<int> b_values;
    for (const auto& r : rows) {
        b_values.insert(r.second);
    }
    EXPECT_TRUE(b_values.count(10));
    EXPECT_TRUE(b_values.count(20));
    EXPECT_TRUE(b_values.count(30));
    EXPECT_FALSE(b_values.count(999));
}

// ===========================================================================
// 11. InsertThenDelete — INSERT 3 rows, DELETE all, verify empty
// ===========================================================================

TEST_F(NodeModifyTableTest, InsertThenDelete) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    RelationClose(rel);

    // Step 1: INSERT 3 rows via Append of Result children.
    auto* append_plan = makePallocNode<Append>();
    append_plan->append_plans.push_back(MakeResultRowPlan(1, 10));
    append_plan->append_plans.push_back(MakeResultRowPlan(2, 20));
    append_plan->append_plans.push_back(MakeResultRowPlan(3, 30));
    append_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    append_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* insert_plan = MakeModifyTablePlan(CmdType::kInsert, append_plan);
    auto* qd_ins = MakeDmlQueryDesc(CmdType::kInsert, relid, insert_plan);
    RunDml(qd_ins);

    // Commit so inserted rows are visible.
    CommitAndStartNew();

    // Step 2: DELETE all rows (no qual).
    auto* seqplan = MakeSeqScanPlan(1);
    auto* delete_plan = MakeModifyTablePlan(CmdType::kDelete, seqplan);
    auto* qd_del = MakeDmlQueryDesc(CmdType::kDelete, relid, delete_plan);
    RunDml(qd_del);

    // Verify: table is empty.
    CommitAndStartNew();
    auto rows = ScanAllRows(relid);
    EXPECT_EQ(rows.size(), 0u);
}

// ===========================================================================
// 12. InsertThenUpdate — INSERT rows, UPDATE, verify updated values
// ===========================================================================

TEST_F(NodeModifyTableTest, InsertThenUpdate) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target", MakeIntIntSchema(relid));
    RelationClose(rel);

    // Step 1: INSERT 3 rows via Append of Result children.
    auto* append_plan = makePallocNode<Append>();
    append_plan->append_plans.push_back(MakeResultRowPlan(1, 10));
    append_plan->append_plans.push_back(MakeResultRowPlan(2, 20));
    append_plan->append_plans.push_back(MakeResultRowPlan(3, 30));
    append_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    append_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* insert_plan = MakeModifyTablePlan(CmdType::kInsert, append_plan);
    auto* qd_ins = MakeDmlQueryDesc(CmdType::kInsert, relid, insert_plan);
    RunDml(qd_ins);

    // Commit so inserted rows are visible.
    CommitAndStartNew();

    // Step 2: UPDATE SET b = a * 10 for all rows (project a, a*10).
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    // b = a * 10 → use a + a + a ... (no int4 * int4 operator handy); use a
    // computed expression via repeated Plus. Simpler: SET b = 100 (constant).
    seqplan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(100), 2, "b"));

    auto* update_plan = MakeModifyTablePlan(CmdType::kUpdate, seqplan);
    auto* qd_upd = MakeDmlQueryDesc(CmdType::kUpdate, relid, update_plan);
    RunDml(qd_upd);

    // Verify: all rows have b = 100; a values unchanged.
    CommitAndStartNew();
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u);
    std::set<int> a_values;
    for (const auto& r : rows) {
        EXPECT_EQ(r.second, 100);
        a_values.insert(r.first);
    }
    EXPECT_EQ(a_values.size(), 3u);
    EXPECT_TRUE(a_values.count(1));
    EXPECT_TRUE(a_values.count(2));
    EXPECT_TRUE(a_values.count(3));
}

}  // namespace
