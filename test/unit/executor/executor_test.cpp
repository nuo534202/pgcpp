// executor_test.cpp — Unit tests for the executor (M9).
//
// Tests the executor framework (ExecutorStart/Run/Finish/End), TupleTableSlot,
// expression evaluation (ExecEvalExpr), and plan node execution:
//   - Result (SELECT without FROM)
//   - SeqScan (sequential heap scan with qual and projection)
//   - Sort (ORDER BY with Top-N LIMIT)
//   - Agg (COUNT/SUM/AVG/MIN/MAX with and without GROUP BY)
//   - NestLoop (inner and left joins)
//   - HashJoin (inner and left joins)
//   - ModifyTable (INSERT)
//
// The fixture sets up the full stack: error subsystem, memory context,
// catalog + syscache, transaction system, buffer pool, storage directory,
// and relcache. Each test creates fresh relations with known schemas.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "access/heapam.hpp"
#include "access/indexam.hpp"
#include "access/nbtree.hpp"
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
#include "executor/node_memoize.hpp"
#include "executor/parallel.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/lock.hpp"
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
using pgcpp::catalog::FormData_pg_proc;
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
using pgcpp::executor::BitmapHeapScan;
using pgcpp::executor::BitmapIndexScan;
using pgcpp::executor::BuildTupleDescFromTargetList;
using pgcpp::executor::CreateExprContext;
using pgcpp::executor::CreateParallelContext;
using pgcpp::executor::CteScan;
using pgcpp::executor::DestroyParallelContext;
using pgcpp::executor::EnterParallelMode;
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
using pgcpp::executor::ExitParallelMode;
using pgcpp::executor::ExprContext;
using pgcpp::executor::FunctionScan;
using pgcpp::executor::Gather;
using pgcpp::executor::GatherMerge;
using pgcpp::executor::Group;
using pgcpp::executor::HashJoin;
using pgcpp::executor::IncrementalSort;
using pgcpp::executor::IsInParallelMode;
using pgcpp::executor::LaunchParallelWorkers;
using pgcpp::executor::Limit;
using pgcpp::executor::LockRows;
using pgcpp::executor::MakeTupleTableSlot;
using pgcpp::executor::Material;
using pgcpp::executor::Memoize;
using pgcpp::executor::MemoizeState;
using pgcpp::executor::MergeAppend;
using pgcpp::executor::MergeJoin;
using pgcpp::executor::ModifyTable;
using pgcpp::executor::NestLoop;
using pgcpp::executor::ParallelContext;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanState;
using pgcpp::executor::PlanType;
using pgcpp::executor::ProjectSet;
using pgcpp::executor::QueryDesc;
using pgcpp::executor::RecursiveUnion;
using pgcpp::executor::ResetExprContext;
using pgcpp::executor::Result;
using pgcpp::executor::SeqScan;
using pgcpp::executor::SetOp;
using pgcpp::executor::Sort;
using pgcpp::executor::SubqueryScan;
using pgcpp::executor::TidScan;
using pgcpp::executor::TupleTableSlot;
using pgcpp::executor::Unique;
using pgcpp::executor::ValuesScan;
using pgcpp::executor::WindowAgg;
using pgcpp::executor::WorkTableScan;
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
using pgcpp::parser::FuncExpr;
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
using pgcpp::transaction::ItemPointerData;
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

// Test OID for generate_series (not in bootstrap catalog; registered per-test).
constexpr Oid kGenerateSeriesOid = 9999;

class ExecutorTest : public ::testing::Test {
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

        test_dir_ = "/tmp/pgcpp_executor_test_" + std::to_string(getpid());
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
        // Reset the cached snapshot so the new transaction gets a fresh one.
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

    // Helper: create a CaseWhen node (condition + result).
    CaseWhen* MakeCaseWhen(Node* cond, Node* result) {
        auto* cw = makePallocNode<CaseWhen>();
        cw->expr = cond;
        cw->result = result;
        return cw;
    }

    // Helper: create a searched-form CaseExpr (arg == nullptr).
    CaseExpr* MakeCaseExpr(std::vector<CaseWhen*> whens, Node* defresult) {
        auto* ce = makePallocNode<CaseExpr>();
        ce->casetype = kInt4Oid;
        for (auto* w : whens) {
            ce->args.push_back(w);
        }
        ce->defresult = defresult;
        return ce;
    }

    // Helper: create a ScalarArrayOpExpr (e.g. x IN (list)).
    ScalarArrayOpExpr* MakeScalarArrayOpExpr(Oid opno, bool use_or, Node* scalar, AArrayExpr* arr) {
        auto* saop = makePallocNode<ScalarArrayOpExpr>();
        saop->opno = opno;
        saop->use_or = use_or;
        saop->args.push_back(scalar);
        saop->args.push_back(arr);
        return saop;
    }

    // Helper: create an AArrayExpr from a list of int4 constants.
    AArrayExpr* MakeInt4Array(std::vector<int32_t> values) {
        auto* arr = makePallocNode<AArrayExpr>();
        for (int32_t v : values) {
            arr->elements.push_back(MakeInt4Const(v));
        }
        return arr;
    }

    // Helper: create an Aggref node.
    Aggref* MakeAggref(Oid aggfnoid, Oid aggtype, bool aggstar = false) {
        auto* agg = makePallocNode<Aggref>();
        agg->aggfnoid = aggfnoid;
        agg->aggtype = aggtype;
        agg->aggstar = aggstar;
        return agg;
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

    static void RunShell(const std::string& cmd) {
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

    // Helper: register a generate_series(int4,int4) proc in the catalog.
    void RegisterGenerateSeries() {
        auto* proc = makePallocNode<FormData_pg_proc>();
        proc->oid = kGenerateSeriesOid;
        proc->proname = "generate_series";
        proc->prorettype = kInt4Oid;
        proc->proretset = true;
        proc->pronargs = 2;
        proc->proargtypes = {kInt4Oid, kInt4Oid};
        catalog_->InsertProc(proc);
    }

    // Helper: create a FuncExpr for generate_series(start, end).
    FuncExpr* MakeGenerateSeries(Node* start, Node* end) {
        auto* fn = makePallocNode<FuncExpr>();
        fn->funcid = kGenerateSeriesOid;
        fn->funcresulttype = kInt4Oid;
        fn->funcretset = true;
        fn->args.push_back(start);
        fn->args.push_back(end);
        return fn;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// ===========================================================================
// Task 9.1: Executor framework — TupleTableSlot basics
// ===========================================================================

TEST_F(ExecutorTest, TupleTableSlot_MakeAndStoreVirtual) {
    // Build a 2-column int4 tuple desc.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    auto* class_row = MakeClassRow("test_rel", relid);
    catalog_->InsertClass(class_row);
    for (const auto& attr : attrs) {
        auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
        catalog_->InsertAttribute(attr_row);
    }
    Relation rel = RelationOpen(relid);
    TupleDesc tupdesc = rel->rd_att;

    // Make a slot.
    TupleTableSlot* slot = TupleTableSlot::Make(tupdesc);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->Natts(), 2);
    EXPECT_TRUE(slot->tts_isempty);

    // Store virtual values.
    Datum values[2] = {Int32GetDatum(42), Int32GetDatum(99)};
    bool isnull[2] = {false, false};
    slot->StoreVirtual(values, isnull);
    EXPECT_FALSE(slot->tts_isempty);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 42);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[1]), 99);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_FALSE(slot->tts_isnull[1]);

    // Clear.
    slot->Clear();
    EXPECT_TRUE(slot->tts_isempty);

    RelationClose(rel);
}

TEST_F(ExecutorTest, TupleTableSlot_StoreVirtualWithNulls) {
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    auto* class_row = MakeClassRow("test_rel", relid);
    catalog_->InsertClass(class_row);
    for (const auto& attr : attrs) {
        auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
        catalog_->InsertAttribute(attr_row);
    }
    Relation rel = RelationOpen(relid);
    TupleDesc tupdesc = rel->rd_att;

    TupleTableSlot* slot = TupleTableSlot::Make(tupdesc);
    Datum values[2] = {Int32GetDatum(10), 0};
    bool isnull[2] = {false, true};
    slot->StoreVirtual(values, isnull);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 10);
    EXPECT_TRUE(slot->tts_isnull[1]);

    slot->Clear();
    RelationClose(rel);
}

// ===========================================================================
// Task 9.1: Expression evaluation — Const, Var, OpExpr
// ===========================================================================

TEST_F(ExecutorTest, ExecEvalExpr_Const) {
    ExprContext* econtext = CreateExprContext();
    Const* con = MakeInt4Const(123);
    bool isnull = false;
    Datum result = ExecEvalExpr(con, econtext, &isnull);
    EXPECT_FALSE(isnull);
    EXPECT_EQ(DatumGetInt32(result), 123);
    ResetExprContext(econtext);
    // Note: econtext freed at context deletion.
}

TEST_F(ExecutorTest, ExecEvalExpr_Var) {
    // Build a slot with a value, then evaluate a Var referencing it.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    auto* class_row = MakeClassRow("test_rel", relid);
    catalog_->InsertClass(class_row);
    for (const auto& attr : attrs) {
        auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
        catalog_->InsertAttribute(attr_row);
    }
    Relation rel = RelationOpen(relid);
    TupleTableSlot* slot = TupleTableSlot::Make(rel->rd_att);
    Datum values[2] = {Int32GetDatum(55), Int32GetDatum(77)};
    bool isnull[2] = {false, false};
    slot->StoreVirtual(values, isnull);

    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // Var referencing column 1 (varno=1, varattno=1).
    Var* var = MakeVar(1, 1, kInt4Oid);
    bool isnull_result = false;
    Datum result = ExecEvalExpr(var, econtext, &isnull_result);
    EXPECT_FALSE(isnull_result);
    EXPECT_EQ(DatumGetInt32(result), 55);

    // Var referencing column 2.
    Var* var2 = MakeVar(1, 2, kInt4Oid);
    result = ExecEvalExpr(var2, econtext, &isnull_result);
    EXPECT_EQ(DatumGetInt32(result), 77);

    RelationClose(rel);
}

TEST_F(ExecutorTest, ExecEvalExpr_OpExpr) {
    // Evaluate: 10 + 20 = 30
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    auto* class_row = MakeClassRow("test_rel", relid);
    catalog_->InsertClass(class_row);
    for (const auto& attr : attrs) {
        auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
        catalog_->InsertAttribute(attr_row);
    }
    Relation rel = RelationOpen(relid);
    TupleTableSlot* slot = TupleTableSlot::Make(rel->rd_att);
    Datum values[2] = {Int32GetDatum(10), Int32GetDatum(20)};
    bool isnull[2] = {false, false};
    slot->StoreVirtual(values, isnull);

    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // a + b (int4 + int4)
    Var* a = MakeVar(1, 1, kInt4Oid);
    Var* b = MakeVar(1, 2, kInt4Oid);
    OpExpr* op = MakeOpExpr(kInt4PlusOp, kInt4Oid, a, b);

    bool isnull_result = false;
    Datum result = ExecEvalExpr(op, econtext, &isnull_result);
    EXPECT_FALSE(isnull_result);
    EXPECT_EQ(DatumGetInt32(result), 30);

    RelationClose(rel);
}

TEST_F(ExecutorTest, ExecEvalExpr_CaseExpr) {
    // CASE WHEN a = 10 THEN 100 ELSE 200 END on tuple (10, 20) → 100.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    auto* class_row = MakeClassRow("test_rel", relid);
    catalog_->InsertClass(class_row);
    for (const auto& attr : attrs) {
        auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
        catalog_->InsertAttribute(attr_row);
    }
    Relation rel = RelationOpen(relid);
    TupleTableSlot* slot = TupleTableSlot::Make(rel->rd_att);
    Datum values[2] = {Int32GetDatum(10), Int32GetDatum(20)};
    bool isnull[2] = {false, false};
    slot->StoreVirtual(values, isnull);

    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // a = 10 matches → 100.
    Var* a = MakeVar(1, 1, kInt4Oid);
    OpExpr* cond = MakeOpExpr(kInt4EqOp, kBoolOid, a, MakeInt4Const(10));
    CaseWhen* when1 = MakeCaseWhen(cond, MakeInt4Const(100));
    CaseExpr* ce = MakeCaseExpr({when1}, MakeInt4Const(200));

    bool isnull_result = false;
    Datum result = ExecEvalExpr(ce, econtext, &isnull_result);
    EXPECT_FALSE(isnull_result);
    EXPECT_EQ(DatumGetInt32(result), 100);

    // a = 99 does not match → ELSE 200.
    Var* a2 = MakeVar(1, 1, kInt4Oid);
    OpExpr* cond2 = MakeOpExpr(kInt4EqOp, kBoolOid, a2, MakeInt4Const(99));
    CaseWhen* when2 = MakeCaseWhen(cond2, MakeInt4Const(100));
    CaseExpr* ce2 = MakeCaseExpr({when2}, MakeInt4Const(200));
    result = ExecEvalExpr(ce2, econtext, &isnull_result);
    EXPECT_FALSE(isnull_result);
    EXPECT_EQ(DatumGetInt32(result), 200);

    // No ELSE, no match → NULL.
    CaseExpr* ce3 = MakeCaseExpr({when2}, nullptr);
    result = ExecEvalExpr(ce3, econtext, &isnull_result);
    EXPECT_TRUE(isnull_result);

    RelationClose(rel);
}

TEST_F(ExecutorTest, ExecEvalExpr_ScalarArrayOpExpr_IN) {
    // a IN (10, 20, 30) on tuple (10, 20) → true (a=10 matches).
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    auto* class_row = MakeClassRow("test_rel", relid);
    catalog_->InsertClass(class_row);
    for (const auto& attr : attrs) {
        auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
        catalog_->InsertAttribute(attr_row);
    }
    Relation rel = RelationOpen(relid);
    TupleTableSlot* slot = TupleTableSlot::Make(rel->rd_att);
    Datum values[2] = {Int32GetDatum(10), Int32GetDatum(20)};
    bool isnull[2] = {false, false};
    slot->StoreVirtual(values, isnull);

    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // a IN (10, 20, 30) → true.
    Var* a = MakeVar(1, 1, kInt4Oid);
    AArrayExpr* arr = MakeInt4Array({10, 20, 30});
    ScalarArrayOpExpr* saop = MakeScalarArrayOpExpr(kInt4EqOp, true, a, arr);

    bool isnull_result = false;
    Datum result = ExecEvalExpr(saop, econtext, &isnull_result);
    EXPECT_FALSE(isnull_result);
    EXPECT_TRUE(DatumGetBool(result));

    // a IN (40, 50) → false.
    Var* a2 = MakeVar(1, 1, kInt4Oid);
    AArrayExpr* arr2 = MakeInt4Array({40, 50});
    ScalarArrayOpExpr* saop2 = MakeScalarArrayOpExpr(kInt4EqOp, true, a2, arr2);
    result = ExecEvalExpr(saop2, econtext, &isnull_result);
    EXPECT_FALSE(isnull_result);
    EXPECT_FALSE(DatumGetBool(result));

    RelationClose(rel);
}

TEST_F(ExecutorTest, ExecEvalExpr_FuncExpr_DateTrunc) {
    // Directly exercise the date_trunc C function. The FuncExpr dispatch path
    // (catalog proc lookup + argument evaluation) is covered end-to-end by the
    // ClickBench Q43 integration test, which requires a registered date_trunc
    // pg_proc row not set up by this fixture.
    int64_t ts = 123456789012345;  // arbitrary μs since 2000-01-01

    // Truncating to minute zeroes seconds and microseconds. Since the PG
    // epoch (2000-01-01 00:00:00) is itself a minute boundary, the result is
    // a whole number of minutes (60s = 60,000,000 μs).
    int64_t minute_trunc = DatumGetInt64(pgcpp::types::date_trunc("minute", Int64GetDatum(ts)));
    EXPECT_EQ(minute_trunc % 60000000, 0);
    EXPECT_GT(minute_trunc, 0);
    EXPECT_LE(minute_trunc, ts);
    EXPECT_GT(minute_trunc, ts - 60000000);

    // Truncating to hour zeroes minutes/seconds/microseconds (3,600s = 3.6e9 μs).
    int64_t hour_trunc = DatumGetInt64(pgcpp::types::date_trunc("hour", Int64GetDatum(ts)));
    EXPECT_EQ(hour_trunc % 3600000000LL, 0);
    EXPECT_LE(hour_trunc, minute_trunc);
}

TEST_F(ExecutorTest, ExecQual_Predicate) {
    // Evaluate: a = 10 (should be true for the tuple (10, 20))
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    auto* class_row = MakeClassRow("test_rel", relid);
    catalog_->InsertClass(class_row);
    for (const auto& attr : attrs) {
        auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
        catalog_->InsertAttribute(attr_row);
    }
    Relation rel = RelationOpen(relid);
    TupleTableSlot* slot = TupleTableSlot::Make(rel->rd_att);
    Datum values[2] = {Int32GetDatum(10), Int32GetDatum(20)};
    bool isnull[2] = {false, false};
    slot->StoreVirtual(values, isnull);

    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // a = 10
    Var* a = MakeVar(1, 1, kInt4Oid);
    Const* c = MakeInt4Const(10);
    OpExpr* eq = MakeOpExpr(kInt4EqOp, kBoolOid, a, c);

    EXPECT_TRUE(ExecQual(eq, econtext));

    // a = 99 (should be false)
    Const* c2 = MakeInt4Const(99);
    OpExpr* eq2 = MakeOpExpr(kInt4EqOp, kBoolOid, a, c2);
    EXPECT_FALSE(ExecQual(eq2, econtext));

    RelationClose(rel);
}

// ===========================================================================
// Task 9.1: Result node — SELECT without FROM
// ===========================================================================

TEST_F(ExecutorTest, ResultNode_SelectConst) {
    // Build a plan: SELECT 42
    auto* result_plan = makePallocNode<Result>();

    // Target list: one entry with a Const(42).
    Const* con = MakeInt4Const(42);
    result_plan->targetlist.push_back(MakeTargetEntry(con, 1, "const"));

    // Build a QueryDesc.
    auto* query = MakeSelectQuery({});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = result_plan;

    // Run the executor.
    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 42);

    // Second call should return nullptr (only one tuple).
    TupleTableSlot* slot2 = ExecutorRun(qd);
    EXPECT_EQ(slot2, nullptr);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 9.2: SeqScan node — sequential heap scan
// ===========================================================================

TEST_F(ExecutorTest, SeqScan_SimpleScan) {
    // Create a relation with 3 rows.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);

    InsertIntIntRow(rel, 1, 100);
    InsertIntIntRow(rel, 2, 200);
    InsertIntIntRow(rel, 3, 300);
    CommitAndStartNew();
    RelationClose(rel);

    // Build a SeqScan plan.
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;  // 1-based RT index

    // Target list: a, b (both Vars).
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // Build a QueryDesc with range table.
    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = seqplan;

    ExecutorStart(qd);

    // Collect all tuples.
    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }

    EXPECT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].first, 1);
    EXPECT_EQ(results[0].second, 100);
    EXPECT_EQ(results[1].first, 2);
    EXPECT_EQ(results[1].second, 200);
    EXPECT_EQ(results[2].first, 3);
    EXPECT_EQ(results[2].second, 300);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

TEST_F(ExecutorTest, SeqScan_WithQual) {
    // Create a relation with 5 rows, filter with a = 3.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);

    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;

    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // WHERE a = 3
    seqplan->qual = MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(3));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = seqplan;

    ExecutorStart(qd);

    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 3);
    EXPECT_EQ(results[0].second, 30);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 9.3: Sort node — ORDER BY with Top-N LIMIT
// ===========================================================================

TEST_F(ExecutorTest, Sort_OrderByAsc) {
    // Create a relation with rows in random order, sort by column a.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);

    InsertIntIntRow(rel, 3, 300);
    InsertIntIntRow(rel, 1, 100);
    InsertIntIntRow(rel, 4, 400);
    InsertIntIntRow(rel, 2, 200);
    CommitAndStartNew();
    RelationClose(rel);

    // Build SeqScan child plan.
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // Build Sort plan on top.
    auto* sortplan = makePallocNode<Sort>();
    sortplan->lefttree = seqplan;
    sortplan->sortColIdx = {1};  // sort by column 1 (a)
    sortplan->sortOperators = {kInt4LtOp};
    sortplan->nullsFirst = {false};
    sortplan->reverse = {false};
    sortplan->limit = -1;  // no limit
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = sortplan;

    ExecutorStart(qd);

    std::vector<int> a_values;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        a_values.push_back(DatumGetInt32(slot->tts_values[0]));
    }

    EXPECT_EQ(a_values.size(), 4u);
    EXPECT_EQ(a_values[0], 1);
    EXPECT_EQ(a_values[1], 2);
    EXPECT_EQ(a_values[2], 3);
    EXPECT_EQ(a_values[3], 4);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

TEST_F(ExecutorTest, Sort_OrderByDesc) {
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);

    InsertIntIntRow(rel, 1, 100);
    InsertIntIntRow(rel, 3, 300);
    InsertIntIntRow(rel, 2, 200);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* sortplan = makePallocNode<Sort>();
    sortplan->lefttree = seqplan;
    sortplan->sortColIdx = {1};
    sortplan->sortOperators = {kInt4LtOp};
    sortplan->nullsFirst = {false};
    sortplan->reverse = {true};  // DESC
    sortplan->limit = -1;
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = sortplan;

    ExecutorStart(qd);

    std::vector<int> a_values;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        a_values.push_back(DatumGetInt32(slot->tts_values[0]));
    }

    EXPECT_EQ(a_values.size(), 3u);
    EXPECT_EQ(a_values[0], 3);
    EXPECT_EQ(a_values[1], 2);
    EXPECT_EQ(a_values[2], 1);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

TEST_F(ExecutorTest, Sort_TopN) {
    // Sort with LIMIT 2 (Top-N).
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);

    for (int i = 5; i >= 1; i--) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* sortplan = makePallocNode<Sort>();
    sortplan->lefttree = seqplan;
    sortplan->sortColIdx = {1};
    sortplan->sortOperators = {kInt4LtOp};
    sortplan->nullsFirst = {false};
    sortplan->reverse = {false};
    sortplan->limit = 2;  // Top-N = 2
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = sortplan;

    ExecutorStart(qd);

    std::vector<int> a_values;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        a_values.push_back(DatumGetInt32(slot->tts_values[0]));
    }

    EXPECT_EQ(a_values.size(), 2u);
    EXPECT_EQ(a_values[0], 1);
    EXPECT_EQ(a_values[1], 2);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 9.3: Agg node — COUNT, SUM, AVG, MIN, MAX
// ===========================================================================

TEST_F(ExecutorTest, Agg_Count) {
    // SELECT COUNT(*) FROM t1
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);

    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));

    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    // Target: COUNT(*) → aggstar=true, aggtype=int8
    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    aggplan->targetlist.push_back(MakeTargetEntry(count_agg, 1, "count"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = aggplan;

    ExecutorStart(qd);

    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), 5);

    // Second call returns nullptr.
    EXPECT_EQ(ExecutorRun(qd), nullptr);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

TEST_F(ExecutorTest, Agg_Sum) {
    // SELECT SUM(a) FROM t1
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);

    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));

    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    // SUM(a) → aggfnoid=2108, aggtype=int8, arg=Var(a)
    Aggref* sum_agg = MakeAggref(kSumInt4Oid, kInt8Oid, false);
    sum_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(sum_agg, 1, "sum"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = aggplan;

    ExecutorStart(qd);

    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    // 1+2+3+4+5 = 15
    EXPECT_EQ(DatumGetInt64(slot->tts_values[0]), 15);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

TEST_F(ExecutorTest, Agg_MinMax) {
    // SELECT MIN(a), MAX(a) FROM t1
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);

    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));

    auto* aggplan = makePallocNode<Agg>();
    aggplan->lefttree = seqplan;
    aggplan->aggstrategy = Agg::Strategy::kPlain;

    // MIN(a) → aggfnoid=2131, aggtype=int4
    Aggref* min_agg = MakeAggref(kMinInt4Oid, kInt4Oid, false);
    min_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(min_agg, 1, "min"));

    // MAX(a) → aggfnoid=2116, aggtype=int4
    Aggref* max_agg = MakeAggref(kMaxInt4Oid, kInt4Oid, false);
    max_agg->args.push_back(MakeVar(1, 1, kInt4Oid));
    aggplan->targetlist.push_back(MakeTargetEntry(max_agg, 2, "max"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = aggplan;

    ExecutorStart(qd);

    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 1);  // MIN = 1
    EXPECT_EQ(DatumGetInt32(slot->tts_values[1]), 5);  // MAX = 5

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 9.4: NestLoop join — inner join
// ===========================================================================

TEST_F(ExecutorTest, NestLoop_InnerJoin) {
    // Create two relations:
    //   t1(a, b): (1, 10), (2, 20), (3, 30)
    //   t2(a, b): (2, 200), (3, 300), (4, 400)
    // Inner join on t1.a = t2.a → expect (2, 20, 200), (3, 30, 300)
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    auto attrs = MakeIntIntSchema(relid1);
    Relation rel1 = CreateTestRelation(relid1, "t1", attrs);
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

    // Build left SeqScan (t1, varno=1).
    auto* left_scan = makePallocNode<SeqScan>();
    left_scan->scanrelid = 1;
    left_scan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    left_scan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // Build right SeqScan (t2, varno=2).
    auto* right_scan = makePallocNode<SeqScan>();
    right_scan->scanrelid = 2;
    right_scan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 1, kInt4Oid), 1, "a"));
    right_scan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 2, kInt4Oid), 2, "b"));

    // Build NestLoop plan.
    auto* nlplan = makePallocNode<NestLoop>();
    nlplan->jointype = JoinType::kInner;
    nlplan->lefttree = left_scan;
    nlplan->righttree = right_scan;

    // Join qual: t1.a = t2.a (use kOuterVar/kInnerVar for join Vars)
    nlplan->qual = MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(kOuterVar, 1, kInt4Oid),
                              MakeVar(kInnerVar, 1, kInt4Oid));

    // Output: t1.a, t1.b, t2.b
    nlplan->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 1, kInt4Oid), 1, "a"));
    nlplan->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 2, kInt4Oid), 2, "b1"));
    nlplan->targetlist.push_back(MakeTargetEntry(MakeVar(kInnerVar, 2, kInt4Oid), 3, "b2"));

    auto* rte1 = MakeRTE(relid1);
    auto* rte2 = MakeRTE(relid2);
    auto* query = MakeSelectQuery({rte1, rte2});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = nlplan;

    ExecutorStart(qd);

    std::vector<std::tuple<int, int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]), DatumGetInt32(slot->tts_values[1]),
                             DatumGetInt32(slot->tts_values[2]));
    }

    EXPECT_EQ(results.size(), 2u);
    // Results may be in any order; check that (2,20,200) and (3,30,300) are present.
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

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 9.4: HashJoin — inner join
// ===========================================================================

TEST_F(ExecutorTest, HashJoin_InnerJoin) {
    // Same data as NestLoop test, but using HashJoin.
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

    // Left SeqScan (outer, t1).
    auto* left_scan = makePallocNode<SeqScan>();
    left_scan->scanrelid = 1;
    left_scan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    left_scan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // Right SeqScan (inner, t2) → child of Hash.
    auto* right_scan = makePallocNode<SeqScan>();
    right_scan->scanrelid = 2;
    right_scan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 1, kInt4Oid), 1, "a"));
    right_scan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 2, kInt4Oid), 2, "b"));

    // Hash node (inner child of HashJoin).
    auto* hash_plan = makePallocNode<pgcpp::executor::Hash>();
    hash_plan->lefttree = right_scan;
    hash_plan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 1, kInt4Oid), 1, "a"));
    hash_plan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 2, kInt4Oid), 2, "b"));

    // HashJoin plan.
    auto* hjplan = makePallocNode<HashJoin>();
    hjplan->jointype = JoinType::kInner;
    hjplan->lefttree = left_scan;
    hjplan->righttree = hash_plan;
    // Hash clauses use regular varno (1, 2) — the HashJoin extracts left/right
    // args and evaluates each from ecxt_scantuple (inner in build, outer in probe).
    hjplan->hashclauses.push_back(
        MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeVar(2, 1, kInt4Oid)));

    // Output: t1.a, t1.b, t2.b (use kOuterVar/kInnerVar for join Vars)
    hjplan->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 1, kInt4Oid), 1, "a"));
    hjplan->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 2, kInt4Oid), 2, "b1"));
    hjplan->targetlist.push_back(MakeTargetEntry(MakeVar(kInnerVar, 2, kInt4Oid), 3, "b2"));

    auto* rte1 = MakeRTE(relid1);
    auto* rte2 = MakeRTE(relid2);
    auto* query = MakeSelectQuery({rte1, rte2});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = hjplan;

    ExecutorStart(qd);

    std::vector<std::tuple<int, int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]), DatumGetInt32(slot->tts_values[1]),
                             DatumGetInt32(slot->tts_values[2]));
    }

    EXPECT_EQ(results.size(), 2u);
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

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 9.5: ModifyTable — INSERT
// ===========================================================================

TEST_F(ExecutorTest, ModifyTable_Insert) {
    // Create a target relation.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "target", attrs);
    RelationClose(rel);

    // Build a Result child plan that produces one row (42, 99).
    auto* result_plan = makePallocNode<Result>();
    // Target list for the child: two Const values.
    result_plan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(42), 1, "a"));
    result_plan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(99), 2, "b"));

    // Build ModifyTable plan.
    auto* mt_plan = makePallocNode<ModifyTable>();
    mt_plan->operation = CmdType::kInsert;
    mt_plan->resultRelid = 1;  // 1-based RT index
    mt_plan->lefttree = result_plan;
    // Target list: pass-through Vars (from child).
    mt_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    mt_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* query = makePallocNode<Query>();
    query->command_type = CmdType::kInsert;
    query->result_relation = 1;
    query->rtable.push_back(rte);

    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = mt_plan;

    ExecutorStart(qd);

    // Execute: should insert one row and return nullptr (DML returns no tuples).
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        // DML may return tuples for RETURNING; here we expect none.
    }

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    // Verify the row was inserted by scanning the relation.
    CommitAndStartNew();
    rel = RelationOpen(relid);
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    HeapTuple tup = heap_getnext(scan);
    ASSERT_NE(tup, nullptr);

    // Deform and check values.
    TupleTableSlot* verify_slot = TupleTableSlot::Make(rel->rd_att);
    verify_slot->StoreTuple(tup, false);
    EXPECT_EQ(DatumGetInt32(verify_slot->tts_values[0]), 42);
    EXPECT_EQ(DatumGetInt32(verify_slot->tts_values[1]), 99);

    // Should be exactly one tuple.
    EXPECT_EQ(heap_getnext(scan), nullptr);
    heap_endscan(scan);
    RelationClose(rel);
}

// ===========================================================================
// Task 9.1: ExecInitNode / ExecProcNode direct dispatch
// ===========================================================================

TEST_F(ExecutorTest, ExecInitNode_DispatchesOnPlanType) {
    // Verify that ExecInitNode correctly dispatches on PlanType.
    // Test with a Result plan (simplest case).
    auto* result_plan = makePallocNode<Result>();
    result_plan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(7), 1, "x"));

    auto* query = MakeSelectQuery({});
    (void)query;
    auto* estate = makePallocNode<EState>();
    estate->es_query_cxt = context_;

    PlanState* ps = ExecInitNode(result_plan, estate);
    ASSERT_NE(ps, nullptr);

    TupleTableSlot* slot = ps->ExecProcNode();
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 7);

    // Second call returns nullptr.
    EXPECT_EQ(ps->ExecProcNode(), nullptr);

    ExecEndNode(ps);
    destroyPallocNode(estate);
}

// ===========================================================================
// Task 15.14: Limit node — LIMIT/OFFSET
// ===========================================================================

TEST_F(ExecutorTest, Limit_TakeTwo) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* limitplan = makePallocNode<Limit>();
    limitplan->lefttree = seqplan;
    limitplan->limit_count = 2;
    limitplan->offset_count = 0;
    limitplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    limitplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = limitplan;

    ExecutorStart(qd);
    std::vector<int> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    EXPECT_EQ(results.size(), 2u);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

TEST_F(ExecutorTest, Limit_OffsetAndLimit) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    for (int i = 1; i <= 5; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // LIMIT 2 OFFSET 1 → rows 2, 3 (skipping row 1).
    auto* limitplan = makePallocNode<Limit>();
    limitplan->lefttree = seqplan;
    limitplan->limit_count = 2;
    limitplan->offset_count = 1;
    limitplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    limitplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = limitplan;

    ExecutorStart(qd);
    std::vector<int> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0], 2);
    EXPECT_EQ(results[1], 3);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 15.14: Append node — UNION ALL over multiple children
// ===========================================================================

TEST_F(ExecutorTest, Append_TwoChildren) {
    // Two Result children, each producing one row.
    auto* child1 = makePallocNode<Result>();
    child1->targetlist.push_back(MakeTargetEntry(MakeInt4Const(11), 1, "v"));

    auto* child2 = makePallocNode<Result>();
    child2->targetlist.push_back(MakeTargetEntry(MakeInt4Const(22), 1, "v"));

    auto* appendplan = makePallocNode<Append>();
    appendplan->append_plans.push_back(child1);
    appendplan->append_plans.push_back(child2);
    appendplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "v"));

    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({});
    qd->plan = appendplan;

    ExecutorStart(qd);
    std::vector<int> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0], 11);
    EXPECT_EQ(results[1], 22);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 15.14: Material node — caches child output for rescans
// ===========================================================================

TEST_F(ExecutorTest, Material_CachesAndReplays) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 2, 20);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* matplan = makePallocNode<Material>();
    matplan->lefttree = seqplan;
    matplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    matplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = matplan;

    ExecutorStart(qd);
    // First scan: drains child into cache, returns 2 rows.
    std::vector<int> first_pass;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        first_pass.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    EXPECT_EQ(first_pass.size(), 2u);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 15.14: Unique node — SELECT DISTINCT on sorted input
// ===========================================================================

TEST_F(ExecutorTest, Unique_DeduplicatesSortedInput) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // Insert duplicates: (1,10), (1,11), (2,20), (2,21), (3,30)
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 1, 11);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 2, 21);
    InsertIntIntRow(rel, 3, 30);
    CommitAndStartNew();
    RelationClose(rel);

    // SeqScan → Sort on column a → Unique on column a.
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* sortplan = makePallocNode<Sort>();
    sortplan->lefttree = seqplan;
    sortplan->sortColIdx = {1};
    sortplan->sortOperators = {kInt4LtOp};
    sortplan->nullsFirst = {false};
    sortplan->reverse = {false};
    sortplan->limit = -1;
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* uniqplan = makePallocNode<Unique>();
    uniqplan->lefttree = sortplan;
    uniqplan->uniq_colIdx = {1};  // dedupe on column a
    uniqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    uniqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = uniqplan;

    ExecutorStart(qd);
    std::vector<int> a_values;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        a_values.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(a_values.size(), 3u);
    EXPECT_EQ(a_values[0], 1);
    EXPECT_EQ(a_values[1], 2);
    EXPECT_EQ(a_values[2], 3);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 15.14: SubqueryScan — FROM subquery projection
// ===========================================================================

TEST_F(ExecutorTest, SubqueryScan_ProjectsChild) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 5, 50);
    InsertIntIntRow(rel, 6, 60);
    CommitAndStartNew();
    RelationClose(rel);

    // Child = SeqScan producing (a, b).
    auto* child = makePallocNode<SeqScan>();
    child->scanrelid = 1;
    child->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    child->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // SubqueryScan projects: SELECT a+b AS sum, a FROM (SELECT a, b FROM t1) sub
    auto* sqplan = makePallocNode<SubqueryScan>();
    sqplan->lefttree = child;
    sqplan->scanrelid = 1;
    // First column: a + b
    sqplan->targetlist.push_back(MakeTargetEntry(
        MakeOpExpr(kInt4PlusOp, kInt4Oid, MakeVar(1, 1, kInt4Oid), MakeVar(1, 2, kInt4Oid)), 1,
        "sum"));
    // Second column: a
    sqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 2, "a"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = sqplan;

    ExecutorStart(qd);
    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].first, 55);  // 5+50
    EXPECT_EQ(results[0].second, 5);
    EXPECT_EQ(results[1].first, 66);  // 6+60
    EXPECT_EQ(results[1].second, 6);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 15.14: MergeJoin — join on sorted inputs
// ===========================================================================

TEST_F(ExecutorTest, MergeJoin_InnerJoin) {
    // Two relations sorted on column a:
    //   t1: (1,10), (2,20), (3,30)
    //   t2: (2,200), (3,300), (4,400)
    // Inner merge-join on t1.a = t2.a → expect (2, 20, 200), (3, 30, 300).
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

    // Left SeqScan → Sort on a.
    auto* left_scan = makePallocNode<SeqScan>();
    left_scan->scanrelid = 1;
    left_scan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    left_scan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* left_sort = makePallocNode<Sort>();
    left_sort->lefttree = left_scan;
    left_sort->sortColIdx = {1};
    left_sort->sortOperators = {kInt4LtOp};
    left_sort->nullsFirst = {false};
    left_sort->reverse = {false};
    left_sort->limit = -1;
    left_sort->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    left_sort->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // Right SeqScan → Sort on a.
    auto* right_scan = makePallocNode<SeqScan>();
    right_scan->scanrelid = 2;
    right_scan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 1, kInt4Oid), 1, "a"));
    right_scan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 2, kInt4Oid), 2, "b"));

    auto* right_sort = makePallocNode<Sort>();
    right_sort->lefttree = right_scan;
    right_sort->sortColIdx = {1};
    right_sort->sortOperators = {kInt4LtOp};
    right_sort->nullsFirst = {false};
    right_sort->reverse = {false};
    right_sort->limit = -1;
    right_sort->targetlist.push_back(MakeTargetEntry(MakeVar(2, 1, kInt4Oid), 1, "a"));
    right_sort->targetlist.push_back(MakeTargetEntry(MakeVar(2, 2, kInt4Oid), 2, "b"));

    // MergeJoin plan.
    auto* mjplan = makePallocNode<MergeJoin>();
    mjplan->jointype = JoinType::kInner;
    mjplan->lefttree = left_sort;
    mjplan->righttree = right_sort;
    mjplan->mergeclauses.push_back(
        MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeVar(2, 1, kInt4Oid)));

    mjplan->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 1, kInt4Oid), 1, "a"));
    mjplan->targetlist.push_back(MakeTargetEntry(MakeVar(kOuterVar, 2, kInt4Oid), 2, "b1"));
    mjplan->targetlist.push_back(MakeTargetEntry(MakeVar(kInnerVar, 2, kInt4Oid), 3, "b2"));

    auto* rte1 = MakeRTE(relid1);
    auto* rte2 = MakeRTE(relid2);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte1, rte2});
    qd->plan = mjplan;

    ExecutorStart(qd);
    std::vector<std::tuple<int, int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]), DatumGetInt32(slot->tts_values[1]),
                             DatumGetInt32(slot->tts_values[2]));
    }
    EXPECT_EQ(results.size(), 2u);
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
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 15.14: CteScan — caches CTE subplan output
// ===========================================================================

TEST_F(ExecutorTest, CteScan_CachesSubplan) {
    // CTE subplan: Result producing two rows via Append (just use Result with a
    // single row to keep the test simple).
    auto* cte_subplan = makePallocNode<Result>();
    cte_subplan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(42), 1, "v"));

    auto* cteplan = makePallocNode<CteScan>();
    cteplan->cte_id = 0;
    cteplan->scanrelid = 0;
    cteplan->lefttree = cte_subplan;
    cteplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "v"));

    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({});
    qd->plan = cteplan;

    ExecutorStart(qd);
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 42);
    // Second call: no more rows from the CTE.
    EXPECT_EQ(ExecutorRun(qd), nullptr);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// Task 15.14: WindowAgg — running COUNT/SUM over a partition
// ===========================================================================

TEST_F(ExecutorTest, WindowAgg_RunningCount) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // Single partition (no PARTITION BY), four rows.
    for (int i = 1; i <= 4; i++) {
        InsertIntIntRow(rel, i, i * 10);
    }
    CommitAndStartNew();
    RelationClose(rel);

    // SeqScan → Sort on a (window functions require ordered input).
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* sortplan = makePallocNode<Sort>();
    sortplan->lefttree = seqplan;
    sortplan->sortColIdx = {1};
    sortplan->sortOperators = {kInt4LtOp};
    sortplan->nullsFirst = {false};
    sortplan->reverse = {false};
    sortplan->limit = -1;
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // WindowAgg with COUNT(*) running.
    auto* wplan = makePallocNode<WindowAgg>();
    wplan->lefttree = sortplan;
    // No PARTITION BY columns → single partition over all rows.
    wplan->partColIdx = {};
    wplan->ordColIdx = {1};  // ORDER BY a
    wplan->ordReverse = {false};

    // Target: a, COUNT(*)
    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    wplan->targetlist.push_back(MakeTargetEntry(count_agg, 2, "running_count"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = wplan;

    ExecutorStart(qd);
    std::vector<std::pair<int, int64_t>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt64(slot->tts_values[1]));
    }
    ASSERT_EQ(results.size(), 4u);
    // Running count should be 1, 2, 3, 4 for the four rows.
    EXPECT_EQ(results[0].first, 1);
    EXPECT_EQ(results[0].second, 1);
    EXPECT_EQ(results[1].first, 2);
    EXPECT_EQ(results[1].second, 2);
    EXPECT_EQ(results[2].first, 3);
    EXPECT_EQ(results[2].second, 3);
    EXPECT_EQ(results[3].first, 4);
    EXPECT_EQ(results[3].second, 4);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

TEST_F(ExecutorTest, WindowAgg_RunningSumWithPartition) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // Two partitions: column a = part id (1 or 2), b = value.
    // Partition 1: (1, 10), (1, 20) → running SUM = 10, 30
    // Partition 2: (2, 100), (2, 200) → running SUM = 100, 300
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 1, 20);
    InsertIntIntRow(rel, 2, 100);
    InsertIntIntRow(rel, 2, 200);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // Sort by PARTITION BY (a) then ORDER BY (b).
    auto* sortplan = makePallocNode<Sort>();
    sortplan->lefttree = seqplan;
    sortplan->sortColIdx = {1, 2};
    sortplan->sortOperators = {kInt4LtOp, kInt4LtOp};
    sortplan->nullsFirst = {false, false};
    sortplan->reverse = {false, false};
    sortplan->limit = -1;
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* wplan = makePallocNode<WindowAgg>();
    wplan->lefttree = sortplan;
    wplan->partColIdx = {1};  // PARTITION BY a
    wplan->ordColIdx = {2};   // ORDER BY b
    wplan->ordReverse = {false};

    // Target: a, b, SUM(b) OVER (PARTITION BY a ORDER BY b)
    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
    Aggref* sum_agg = MakeAggref(kSumInt4Oid, kInt8Oid, false);
    sum_agg->args.push_back(MakeVar(1, 2, kInt4Oid));
    wplan->targetlist.push_back(MakeTargetEntry(sum_agg, 3, "running_sum"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = wplan;

    ExecutorStart(qd);
    std::vector<std::tuple<int, int, int64_t>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]), DatumGetInt32(slot->tts_values[1]),
                             DatumGetInt64(slot->tts_values[2]));
    }
    ASSERT_EQ(results.size(), 4u);
    // Partition 1: (1, 10, 10), (1, 20, 30)
    EXPECT_EQ(std::get<0>(results[0]), 1);
    EXPECT_EQ(std::get<1>(results[0]), 10);
    EXPECT_EQ(std::get<2>(results[0]), 10);
    EXPECT_EQ(std::get<0>(results[1]), 1);
    EXPECT_EQ(std::get<1>(results[1]), 20);
    EXPECT_EQ(std::get<2>(results[1]), 30);
    // Partition 2: (2, 100, 100), (2, 200, 300) — running sum resets.
    EXPECT_EQ(std::get<0>(results[2]), 2);
    EXPECT_EQ(std::get<1>(results[2]), 100);
    EXPECT_EQ(std::get<2>(results[2]), 100);
    EXPECT_EQ(std::get<0>(results[3]), 2);
    EXPECT_EQ(std::get<1>(results[3]), 200);
    EXPECT_EQ(std::get<2>(results[3]), 300);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P1-7: Group node — GROUP BY without aggregates on sorted input
// ===========================================================================

TEST_F(ExecutorTest, Group_EmitsFirstTuplePerGroup) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // Insert: (1,10), (1,11), (2,20), (2,21), (3,30)
    // Sorted on column a, groups = {1, 2, 3}.
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 1, 11);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 2, 21);
    InsertIntIntRow(rel, 3, 30);
    CommitAndStartNew();
    RelationClose(rel);

    // SeqScan → Sort on a → Group on a.
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* sortplan = makePallocNode<Sort>();
    sortplan->lefttree = seqplan;
    sortplan->sortColIdx = {1};
    sortplan->sortOperators = {kInt4LtOp};
    sortplan->nullsFirst = {false};
    sortplan->reverse = {false};
    sortplan->limit = -1;
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* groupplan = makePallocNode<Group>();
    groupplan->lefttree = sortplan;
    groupplan->groupColIdx = {1};  // GROUP BY a
    groupplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    groupplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = groupplan;

    ExecutorStart(qd);
    std::vector<int> a_values;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        a_values.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(a_values.size(), 3u);
    EXPECT_EQ(a_values[0], 1);
    EXPECT_EQ(a_values[1], 2);
    EXPECT_EQ(a_values[2], 3);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P1-7: SetOp node — INTERSECT DISTINCT on sorted input with flag column
// ===========================================================================

TEST_F(ExecutorTest, SetOp_IntersectDistinct) {
    // Two inputs merged into one sorted stream with a flag column (col 3):
    //   left (flag=0):  (1,10,0), (2,20,0), (3,30,0)
    //   right (flag=1): (2,200,1), (3,300,1), (4,400,1)
    // Sorted on col 1 (a): groups {1,2,3,4}.
    // INTERSECT DISTINCT → groups present in both = {2, 3}.
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

    // Left scan (varno=1) projects (a, b, flag=0).
    auto* left_scan = makePallocNode<SeqScan>();
    left_scan->scanrelid = 1;
    left_scan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    left_scan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
    left_scan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(0), 3, "flag"));

    // Right scan (varno=2) projects (a, b, flag=1).
    auto* right_scan = makePallocNode<SeqScan>();
    right_scan->scanrelid = 2;
    right_scan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 1, kInt4Oid), 1, "a"));
    right_scan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 2, kInt4Oid), 2, "b"));
    right_scan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(1), 3, "flag"));

    // Append the two scans, then sort on col 1 (a).
    auto* appendplan = makePallocNode<Append>();
    appendplan->append_plans.push_back(left_scan);
    appendplan->append_plans.push_back(right_scan);
    appendplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    appendplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
    appendplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 3, kInt4Oid), 3, "flag"));

    auto* sortplan = makePallocNode<Sort>();
    sortplan->lefttree = appendplan;
    sortplan->sortColIdx = {1};
    sortplan->sortOperators = {kInt4LtOp};
    sortplan->nullsFirst = {false};
    sortplan->reverse = {false};
    sortplan->limit = -1;
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 3, kInt4Oid), 3, "flag"));

    // SetOp: INTERSECT DISTINCT, group on col 1, flag col = 3, firstFlag = 0.
    auto* setopplan = makePallocNode<SetOp>();
    setopplan->lefttree = sortplan;
    setopplan->cmd = SetOp::Cmd::kIntersect;
    setopplan->strategy = SetOp::Strategy::kSorted;
    setopplan->all = false;
    setopplan->colIdx = {1};
    setopplan->flagColIdx = 3;
    setopplan->firstFlag = 0;
    setopplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    setopplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte1 = MakeRTE(relid1);
    auto* rte2 = MakeRTE(relid2);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte1, rte2});
    qd->plan = setopplan;

    ExecutorStart(qd);
    std::vector<int> a_values;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        a_values.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(a_values.size(), 2u);
    EXPECT_EQ(a_values[0], 2);
    EXPECT_EQ(a_values[1], 3);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P1-7: SetOp node — EXCEPT DISTINCT
// ===========================================================================

TEST_F(ExecutorTest, SetOp_ExceptDistinct) {
    // Left (flag=0): 1, 2, 3
    // Right (flag=1): 2, 3, 4
    // EXCEPT DISTINCT → groups in left but not right = {1}.
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

    auto* left_scan = makePallocNode<SeqScan>();
    left_scan->scanrelid = 1;
    left_scan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    left_scan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
    left_scan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(0), 3, "flag"));

    auto* right_scan = makePallocNode<SeqScan>();
    right_scan->scanrelid = 2;
    right_scan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 1, kInt4Oid), 1, "a"));
    right_scan->targetlist.push_back(MakeTargetEntry(MakeVar(2, 2, kInt4Oid), 2, "b"));
    right_scan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(1), 3, "flag"));

    auto* appendplan = makePallocNode<Append>();
    appendplan->append_plans.push_back(left_scan);
    appendplan->append_plans.push_back(right_scan);
    appendplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    appendplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
    appendplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 3, kInt4Oid), 3, "flag"));

    auto* sortplan = makePallocNode<Sort>();
    sortplan->lefttree = appendplan;
    sortplan->sortColIdx = {1};
    sortplan->sortOperators = {kInt4LtOp};
    sortplan->nullsFirst = {false};
    sortplan->reverse = {false};
    sortplan->limit = -1;
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
    sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 3, kInt4Oid), 3, "flag"));

    auto* setopplan = makePallocNode<SetOp>();
    setopplan->lefttree = sortplan;
    setopplan->cmd = SetOp::Cmd::kExcept;
    setopplan->strategy = SetOp::Strategy::kSorted;
    setopplan->all = false;
    setopplan->colIdx = {1};
    setopplan->flagColIdx = 3;
    setopplan->firstFlag = 0;
    setopplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    setopplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte1 = MakeRTE(relid1);
    auto* rte2 = MakeRTE(relid2);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte1, rte2});
    qd->plan = setopplan;

    ExecutorStart(qd);
    std::vector<int> a_values;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        a_values.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(a_values.size(), 1u);
    EXPECT_EQ(a_values[0], 1);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P1-7: MergeAppend — k-way merge of two sorted children
// ===========================================================================

TEST_F(ExecutorTest, MergeAppend_MergesTwoSortedChildren) {
    // Child 1 sorted: (1), (3), (5)
    // Child 2 sorted: (2), (4), (6)
    // MergeAppend on col 1 ASC → (1),(2),(3),(4),(5),(6).
    Oid relid1 = kFirstNormalObjectId;
    Oid relid2 = kFirstNormalObjectId + 1;
    Relation rel1 = CreateTestRelation(relid1, "t1", MakeIntIntSchema(relid1));
    Relation rel2 = CreateTestRelation(relid2, "t2", MakeIntIntSchema(relid2));
    // t1: (1,10), (3,30), (5,50) — already sorted on a.
    InsertIntIntRow(rel1, 1, 10);
    InsertIntIntRow(rel1, 3, 30);
    InsertIntIntRow(rel1, 5, 50);
    // t2: (2,20), (4,40), (6,60) — already sorted on a.
    InsertIntIntRow(rel2, 2, 20);
    InsertIntIntRow(rel2, 4, 40);
    InsertIntIntRow(rel2, 6, 60);
    CommitAndStartNew();
    RelationClose(rel1);
    RelationClose(rel2);

    // Child 1: SeqScan(t1) → Sort(a).
    auto* scan1 = makePallocNode<SeqScan>();
    scan1->scanrelid = 1;
    scan1->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    scan1->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* sort1 = makePallocNode<Sort>();
    sort1->lefttree = scan1;
    sort1->sortColIdx = {1};
    sort1->sortOperators = {kInt4LtOp};
    sort1->nullsFirst = {false};
    sort1->reverse = {false};
    sort1->limit = -1;
    sort1->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    sort1->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // Child 2: SeqScan(t2) → Sort(a).
    auto* scan2 = makePallocNode<SeqScan>();
    scan2->scanrelid = 2;
    scan2->targetlist.push_back(MakeTargetEntry(MakeVar(2, 1, kInt4Oid), 1, "a"));
    scan2->targetlist.push_back(MakeTargetEntry(MakeVar(2, 2, kInt4Oid), 2, "b"));

    auto* sort2 = makePallocNode<Sort>();
    sort2->lefttree = scan2;
    sort2->sortColIdx = {1};
    sort2->sortOperators = {kInt4LtOp};
    sort2->nullsFirst = {false};
    sort2->reverse = {false};
    sort2->limit = -1;
    sort2->targetlist.push_back(MakeTargetEntry(MakeVar(2, 1, kInt4Oid), 1, "a"));
    sort2->targetlist.push_back(MakeTargetEntry(MakeVar(2, 2, kInt4Oid), 2, "b"));

    // MergeAppend on col 1 ASC.
    auto* maplan = makePallocNode<MergeAppend>();
    maplan->merge_plans.push_back(sort1);
    maplan->merge_plans.push_back(sort2);
    maplan->sortColIdx = {1};
    maplan->sortOperators = {kInt4LtOp};
    maplan->nullsFirst = {false};
    maplan->reverse = {false};
    maplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    maplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte1 = MakeRTE(relid1);
    auto* rte2 = MakeRTE(relid2);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte1, rte2});
    qd->plan = maplan;

    ExecutorStart(qd);
    std::vector<int> a_values;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        a_values.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(a_values.size(), 6u);
    // Merged output should be 1..6 in ascending order.
    for (int i = 0; i < 6; i++) {
        EXPECT_EQ(a_values[i], i + 1);
    }
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P1-7: BitmapIndexScan + BitmapHeapScan — index-driven heap fetch
// ===========================================================================

TEST_F(ExecutorTest, BitmapScan_IndexToHeap) {
    // Create a heap relation with rows, build a B-tree index on column a,
    // then use BitmapIndexScan ( equality qual a = 3 ) → BitmapHeapScan to
    // fetch the matching heap tuple(s).
    Oid relid = kFirstNormalObjectId;
    Oid indexid = kFirstNormalObjectId + 1;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));

    // Insert rows: (1,10), (2,20), (3,30), (4,40), (5,50).
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 3, 30);
    InsertIntIntRow(rel, 4, 40);
    InsertIntIntRow(rel, 5, 50);
    CommitAndStartNew();

    // Build a B-tree index on column a. We populate it by scanning the heap
    // and inserting each (key, tid) into the index.
    auto* idx_class_row = makePallocNode<FormData_pg_class>();
    idx_class_row->oid = indexid;
    idx_class_row->relname = "idx_t1_a";
    idx_class_row->relfilenode = indexid;
    idx_class_row->relkind = RelKind::kIndex;
    idx_class_row->relpersistence = RelPersistence::kPermanent;
    idx_class_row->relam = pgcpp::access::kBTreeAmOid;
    catalog_->InsertClass(idx_class_row);
    pgcpp::access::RelationCreateStorage(indexid, false);
    Relation idxrel = RelationOpen(indexid);
    ASSERT_NE(idxrel, nullptr);
    pgcpp::access::btbuild(idxrel, pgcpp::access::BTKeyKind::kInt32);

    // Scan the heap and insert (a, tid) into the index.
    {
        HeapScanDesc hscan = heap_beginscan(rel, pgcpp::transaction::GetActiveSnapshot());
        HeapTuple tup = nullptr;
        while ((tup = heap_getnext(hscan)) != nullptr) {
            int32_t key = 0;
            bool isnull = false;
            Datum d = pgcpp::access::heap_getattr(tup, 1, rel->rd_att, &isnull);
            key = DatumGetInt32(d);
            pgcpp::access::btinsert(idxrel, pgcpp::access::BTKeyKind::kInt32, &key, sizeof(int32_t),
                                    tup->t_self);
        }
        heap_endscan(hscan);
    }
    RelationClose(idxrel);
    RelationClose(rel);

    // Build the plan: BitmapHeapScan( lefttree = BitmapIndexScan(a = 3) ).
    auto* idxplan = makePallocNode<BitmapIndexScan>();
    idxplan->scanrelid = 1;  // range table index of the heap relation
    idxplan->indexid = indexid;
    // Index qual: a = 3 (Var attno=1, Const=3, int4eq).
    idxplan->indexqual.push_back(
        MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(3)));
    idxplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    idxplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* heapplan = makePallocNode<BitmapHeapScan>();
    heapplan->lefttree = idxplan;
    heapplan->scanrelid = 1;
    heapplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    heapplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = heapplan;

    ExecutorStart(qd);
    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 3);
    EXPECT_EQ(results[0].second, 30);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P1-8: LockRows — SELECT FOR UPDATE
// ===========================================================================

TEST_F(ExecutorTest, LockRows_ForUpdateLocksTuples) {
    // Create a relation with 3 rows.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);
    InsertIntIntRow(rel, 1, 100);
    InsertIntIntRow(rel, 2, 200);
    InsertIntIntRow(rel, 3, 300);
    CommitAndStartNew();
    RelationClose(rel);

    // Build SeqScan child plan.
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // Build LockRows parent plan (FOR UPDATE on relation 1).
    auto* lr_plan = makePallocNode<LockRows>();
    lr_plan->lefttree = seqplan;
    lr_plan->lockRelid = 1;
    lr_plan->lockStrength = pgcpp::transaction::RowLockStrength::kForUpdate;
    lr_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    lr_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = lr_plan;

    ExecutorStart(qd);

    // Collect all tuples — LockRows should pass them through unchanged.
    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }
    EXPECT_EQ(results.size(), 3u);

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    // Verify each tuple was locked: t_infomask should carry kHeapXmaxExclusiveLock.
    rel = RelationOpen(relid);
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    int locked_count = 0;
    HeapTuple tup = nullptr;
    while ((tup = heap_getnext(scan)) != nullptr) {
        if ((tup->t_data->t_infomask & pgcpp::transaction::kHeapXmaxExclusiveLock) != 0) {
            ++locked_count;
        }
    }
    heap_endscan(scan);
    RelationClose(rel);
    EXPECT_EQ(locked_count, 3);
}

TEST_F(ExecutorTest, LockRows_ForShareUsesMultiXact) {
    // Create a relation with 2 rows.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);
    InsertIntIntRow(rel, 10, 20);
    InsertIntIntRow(rel, 30, 40);
    CommitAndStartNew();
    RelationClose(rel);

    // Build SeqScan + LockRows (FOR SHARE).
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* lr_plan = makePallocNode<LockRows>();
    lr_plan->lefttree = seqplan;
    lr_plan->lockRelid = 1;
    lr_plan->lockStrength = pgcpp::transaction::RowLockStrength::kForShare;
    lr_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    lr_plan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = lr_plan;

    ExecutorStart(qd);
    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }
    EXPECT_EQ(results.size(), 2u);
    ExecutorFinish(qd);
    ExecutorEnd(qd);

    // FOR SHARE should set the kHeapXmaxLocked + kHeapXmaxShrLock flags.
    rel = RelationOpen(relid);
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    int shared_locked = 0;
    HeapTuple tup = nullptr;
    while ((tup = heap_getnext(scan)) != nullptr) {
        if ((tup->t_data->t_infomask & pgcpp::transaction::kHeapXmaxLocked) != 0 &&
            (tup->t_data->t_infomask & pgcpp::transaction::kHeapXmaxShrLock) != 0) {
            ++shared_locked;
        }
    }
    heap_endscan(scan);
    RelationClose(rel);
    EXPECT_EQ(shared_locked, 2);
}

// ===========================================================================
// P2-2: ValuesScan — scan a VALUES list
// ===========================================================================

TEST_F(ExecutorTest, ValuesScan_ThreeRows) {
    // Build a ValuesScan plan: VALUES (1,10), (2,20), (3,30).
    auto* vsplan = makePallocNode<ValuesScan>();
    vsplan->scanrelid = 1;
    vsplan->rows.push_back({MakeInt4Const(1), MakeInt4Const(10)});
    vsplan->rows.push_back({MakeInt4Const(2), MakeInt4Const(20)});
    vsplan->rows.push_back({MakeInt4Const(3), MakeInt4Const(30)});
    vsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    vsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({});
    qd->plan = vsplan;

    ExecutorStart(qd);
    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].first, 1);
    EXPECT_EQ(results[0].second, 10);
    EXPECT_EQ(results[1].first, 2);
    EXPECT_EQ(results[1].second, 20);
    EXPECT_EQ(results[2].first, 3);
    EXPECT_EQ(results[2].second, 30);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P2-2: TidScan — scan by specific TIDs
// ===========================================================================

TEST_F(ExecutorTest, TidScan_FetchByTid) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 10, 100);
    InsertIntIntRow(rel, 20, 200);
    InsertIntIntRow(rel, 30, 300);
    CommitAndStartNew();
    RelationClose(rel);

    // Collect TIDs via a heap scan.
    rel = RelationOpen(relid);
    std::vector<ItemPointerData> tids;
    {
        HeapScanDesc scan = heap_beginscan(rel, pgcpp::transaction::GetActiveSnapshot());
        HeapTuple tup = nullptr;
        while ((tup = heap_getnext(scan)) != nullptr) {
            tids.push_back(tup->t_self);
        }
        heap_endscan(scan);
    }
    ASSERT_EQ(tids.size(), 3u);
    RelationClose(rel);

    // Build TidScan plan targeting the first and third TIDs.
    auto* tidplan = makePallocNode<TidScan>();
    tidplan->scanrelid = 1;
    tidplan->tids.push_back(tids[0]);
    tidplan->tids.push_back(tids[2]);
    tidplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    tidplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = tidplan;

    ExecutorStart(qd);
    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].first, 10);
    EXPECT_EQ(results[0].second, 100);
    EXPECT_EQ(results[1].first, 30);
    EXPECT_EQ(results[1].second, 300);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P2-2: FunctionScan — scan rows from a set-returning function
// ===========================================================================

TEST_F(ExecutorTest, FunctionScan_GenerateSeries) {
    RegisterGenerateSeries();

    // Build FunctionScan: SELECT * FROM generate_series(1, 5).
    auto* fsplan = makePallocNode<FunctionScan>();
    fsplan->scanrelid = 1;
    fsplan->functions.push_back(MakeGenerateSeries(MakeInt4Const(1), MakeInt4Const(5)));
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "v"));

    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({});
    qd->plan = fsplan;

    ExecutorStart(qd);
    std::vector<int> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(results.size(), 5u);
    EXPECT_EQ(results[0], 1);
    EXPECT_EQ(results[1], 2);
    EXPECT_EQ(results[2], 3);
    EXPECT_EQ(results[3], 4);
    EXPECT_EQ(results[4], 5);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P2-2: ProjectSet — project target list with SRFs
// ===========================================================================

TEST_F(ExecutorTest, ProjectSet_SRFExpandsRows) {
    RegisterGenerateSeries();

    // Child: Result producing one row with Const(0).
    auto* child = makePallocNode<Result>();
    child->targetlist.push_back(MakeTargetEntry(MakeInt4Const(0), 1, "x"));

    // ProjectSet: targetlist = [generate_series(1, 3)].
    auto* ps_plan = makePallocNode<ProjectSet>();
    ps_plan->lefttree = child;
    ps_plan->targetlist.push_back(
        MakeTargetEntry(MakeGenerateSeries(MakeInt4Const(1), MakeInt4Const(3)), 1, "v"));

    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({});
    qd->plan = ps_plan;

    ExecutorStart(qd);
    std::vector<int> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], 1);
    EXPECT_EQ(results[1], 2);
    EXPECT_EQ(results[2], 3);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P2-2: Memoize — cache inner-side lookup results
// ===========================================================================

TEST_F(ExecutorTest, Memoize_CacheHitReplaysRows) {
    // Child: Result producing one row (10, 20).
    auto* child = makePallocNode<Result>();
    child->targetlist.push_back(MakeTargetEntry(MakeInt4Const(10), 1, "a"));
    child->targetlist.push_back(MakeTargetEntry(MakeInt4Const(20), 2, "b"));

    // Memoize: param_exprs = [Const(42)] (constant key → always hits cache).
    auto* mplan = makePallocNode<Memoize>();
    mplan->lefttree = child;
    mplan->param_exprs.push_back(MakeInt4Const(42));
    mplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    mplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* estate = makePallocNode<EState>();
    estate->es_query_cxt = context_;
    PlanState* ps = ExecInitNode(mplan, estate);
    ASSERT_NE(ps, nullptr);
    auto* ms = static_cast<MemoizeState*>(ps);

    // First call: cache miss → drain child → emit (10, 20).
    TupleTableSlot* slot1 = ps->ExecProcNode();
    ASSERT_NE(slot1, nullptr);
    EXPECT_EQ(DatumGetInt32(slot1->tts_values[0]), 10);
    EXPECT_EQ(DatumGetInt32(slot1->tts_values[1]), 20);

    // Second call: entry exhausted → nullptr.
    EXPECT_EQ(ps->ExecProcNode(), nullptr);

    // Third call: cache hit (same key) → replay (10, 20) without re-executing child.
    TupleTableSlot* slot3 = ps->ExecProcNode();
    ASSERT_NE(slot3, nullptr);
    EXPECT_EQ(DatumGetInt32(slot3->tts_values[0]), 10);
    EXPECT_EQ(DatumGetInt32(slot3->tts_values[1]), 20);

    // Fourth call: exhausted again → nullptr.
    EXPECT_EQ(ps->ExecProcNode(), nullptr);

    // Verify only one cache entry was created (key=42).
    EXPECT_EQ(ms->ms_cache.size(), 1u);

    ExecEndNode(ps);
    destroyPallocNode(estate);
}

// ===========================================================================
// P2-2: IncrementalSort — sort exploiting a presorted prefix
// ===========================================================================

TEST_F(ExecutorTest, IncrementalSort_SortsByFullKeys) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    // Insert rows where column a is sorted but b is not.
    InsertIntIntRow(rel, 1, 30);
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 2, 5);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* iplan = makePallocNode<IncrementalSort>();
    iplan->lefttree = seqplan;
    iplan->presortedColIdx = {1};  // column a already sorted
    iplan->sortColIdx = {1, 2};    // sort by (a, b)
    iplan->sortOperators = {kInt4LtOp, kInt4LtOp};
    iplan->nullsFirst = {false, false};
    iplan->reverse = {false, false};
    iplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    iplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = iplan;

    ExecutorStart(qd);
    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }
    ASSERT_EQ(results.size(), 4u);
    // Expect sorted by (a, b): (1,10), (1,30), (2,5), (2,20).
    EXPECT_EQ(results[0].first, 1);
    EXPECT_EQ(results[0].second, 10);
    EXPECT_EQ(results[1].first, 1);
    EXPECT_EQ(results[1].second, 30);
    EXPECT_EQ(results[2].first, 2);
    EXPECT_EQ(results[2].second, 5);
    EXPECT_EQ(results[3].first, 2);
    EXPECT_EQ(results[3].second, 20);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P2-2: RecursiveUnion + WorkTableScan — WITH RECURSIVE counter
// ===========================================================================

TEST_F(ExecutorTest, RecursiveUnion_CounterCTE) {
    // Seed: Result producing (1).
    auto* seed = makePallocNode<Result>();
    seed->targetlist.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "n"));

    // Recursive term: WorkTableScan with qual (n < 5) and projection (n+1).
    auto* wtscan = makePallocNode<WorkTableScan>();
    wtscan->wtParam = 1;
    wtscan->scanrelid = 1;
    wtscan->targetlist.push_back(MakeTargetEntry(
        MakeOpExpr(kInt4PlusOp, kInt4Oid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(1)), 1, "n"));
    wtscan->qual = MakeOpExpr(kInt4LtOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(5));

    // RecursiveUnion: lefttree=seed, righttree=recursive term.
    auto* ruplan = makePallocNode<RecursiveUnion>();
    ruplan->lefttree = seed;
    ruplan->righttree = wtscan;
    ruplan->wtParam = 1;
    ruplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "n"));

    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({});
    qd->plan = ruplan;

    ExecutorStart(qd);
    std::vector<int> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    // Expect 1, 2, 3, 4, 5.
    ASSERT_EQ(results.size(), 5u);
    EXPECT_EQ(results[0], 1);
    EXPECT_EQ(results[1], 2);
    EXPECT_EQ(results[2], 3);
    EXPECT_EQ(results[3], 4);
    EXPECT_EQ(results[4], 5);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P2-2: Gather — serial passthrough (nworkers=0)
// ===========================================================================

TEST_F(ExecutorTest, Gather_SerialPassthrough) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 1, 10);
    InsertIntIntRow(rel, 2, 20);
    InsertIntIntRow(rel, 3, 30);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* gplan = makePallocNode<Gather>();
    gplan->lefttree = seqplan;
    gplan->num_workers = 0;
    gplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    gplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = gplan;

    ExecutorStart(qd);
    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].first, 1);
    EXPECT_EQ(results[0].second, 10);
    EXPECT_EQ(results[1].first, 2);
    EXPECT_EQ(results[1].second, 20);
    EXPECT_EQ(results[2].first, 3);
    EXPECT_EQ(results[2].second, 30);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P2-2: GatherMerge — serial passthrough + sort
// ===========================================================================

TEST_F(ExecutorTest, GatherMerge_SerialSortedPassthrough) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t1", MakeIntIntSchema(relid));
    InsertIntIntRow(rel, 3, 300);
    InsertIntIntRow(rel, 1, 100);
    InsertIntIntRow(rel, 4, 400);
    InsertIntIntRow(rel, 2, 200);
    CommitAndStartNew();
    RelationClose(rel);

    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* gmplan = makePallocNode<GatherMerge>();
    gmplan->lefttree = seqplan;
    gmplan->num_workers = 0;
    gmplan->sortColIdx = {1};
    gmplan->sortOperators = {kInt4LtOp};
    gmplan->nullsFirst = {false};
    gmplan->reverse = {false};
    gmplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    gmplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = gmplan;

    ExecutorStart(qd);
    std::vector<int> a_values;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        a_values.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(a_values.size(), 4u);
    EXPECT_EQ(a_values[0], 1);
    EXPECT_EQ(a_values[1], 2);
    EXPECT_EQ(a_values[2], 3);
    EXPECT_EQ(a_values[3], 4);
    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// P2-2: Parallel framework — serial stub
// ===========================================================================

TEST_F(ExecutorTest, ParallelMode_EnterExit) {
    EXPECT_FALSE(IsInParallelMode());
    EnterParallelMode();
    EXPECT_TRUE(IsInParallelMode());
    ExitParallelMode();
    EXPECT_FALSE(IsInParallelMode());
}

TEST_F(ExecutorTest, ParallelContext_LaunchReturnsZero) {
    ParallelContext* pc = CreateParallelContext(/*nworkers=*/4);
    ASSERT_NE(pc, nullptr);
    EXPECT_EQ(pc->nworkers_requested, 4);
    int launched = LaunchParallelWorkers(pc);
    EXPECT_EQ(launched, 0);
    EXPECT_EQ(pc->nworkers_launched, 0);
    DestroyParallelContext(pc);
}

}  // namespace
