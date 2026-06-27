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

#include "mytoydb/access/heapam.hpp"
#include "mytoydb/access/rel.hpp"
#include "mytoydb/catalog/bootstrap_catalog.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_attribute.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/catalog/pg_operator.hpp"
#include "mytoydb/catalog/pg_proc.hpp"
#include "mytoydb/catalog/syscache.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/executor/estate.hpp"
#include "mytoydb/executor/exec_expr.hpp"
#include "mytoydb/executor/exec_main.hpp"
#include "mytoydb/executor/exec_utils.hpp"
#include "mytoydb/executor/node_exec.hpp"
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/executor/tupletable.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/parser/primnodes.hpp"
#include "mytoydb/storage/bufmgr.hpp"
#include "mytoydb/storage/smgr.hpp"
#include "mytoydb/transaction/heap_tuple.hpp"
#include "mytoydb/transaction/snapshot.hpp"
#include "mytoydb/transaction/transam.hpp"
#include "mytoydb/transaction/xact.hpp"
#include "mytoydb/types/datetime.hpp"
#include "mytoydb/types/datum.hpp"

using mytoydb::access::CreateTupleDesc;
using mytoydb::access::heap_beginscan;
using mytoydb::access::heap_endscan;
using mytoydb::access::heap_form_tuple;
using mytoydb::access::heap_freetuple;
using mytoydb::access::heap_getnext;
using mytoydb::access::heap_insert;
using mytoydb::access::HeapScanDesc;
using mytoydb::access::InitializeRelcache;
using mytoydb::access::Relation;
using mytoydb::access::RelationClose;
using mytoydb::access::RelationCreateStorage;
using mytoydb::access::RelationOpen;
using mytoydb::access::ResetRelcache;
using mytoydb::access::TupleDesc;
using mytoydb::catalog::AttAlign;
using mytoydb::catalog::AttStorage;
using mytoydb::catalog::BootstrapCatalog;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_attribute;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::kFirstNormalObjectId;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::catalog::RelKind;
using mytoydb::catalog::RelPersistence;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::executor::Agg;
using mytoydb::executor::BuildTupleDescFromTargetList;
using mytoydb::executor::CreateExprContext;
using mytoydb::executor::EState;
using mytoydb::executor::ExecEndNode;
using mytoydb::executor::ExecEvalExpr;
using mytoydb::executor::ExecInitNode;
using mytoydb::executor::ExecProject;
using mytoydb::executor::ExecQual;
using mytoydb::executor::ExecutorEnd;
using mytoydb::executor::ExecutorFinish;
using mytoydb::executor::ExecutorRun;
using mytoydb::executor::ExecutorStart;
using mytoydb::executor::ExprContext;
using mytoydb::executor::HashJoin;
using mytoydb::executor::MakeTupleTableSlot;
using mytoydb::executor::ModifyTable;
using mytoydb::executor::NestLoop;
using mytoydb::executor::Plan;
using mytoydb::executor::PlanState;
using mytoydb::executor::PlanType;
using mytoydb::executor::QueryDesc;
using mytoydb::executor::ResetExprContext;
using mytoydb::executor::Result;
using mytoydb::executor::SeqScan;
using mytoydb::executor::Sort;
using mytoydb::executor::TupleTableSlot;
using mytoydb::memory::AllocSetContext;
using mytoydb::memory::palloc;
using mytoydb::memory::pfree;
using mytoydb::nodes::destroyPallocNode;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::AArrayExpr;
using mytoydb::parser::Aggref;
using mytoydb::parser::CaseExpr;
using mytoydb::parser::CaseWhen;
using mytoydb::parser::CmdType;
using mytoydb::parser::Const;
using mytoydb::parser::JoinType;
using mytoydb::parser::kInnerVar;
using mytoydb::parser::kOuterVar;
using mytoydb::parser::Node;
using mytoydb::parser::OpExpr;
using mytoydb::parser::Query;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::RTEKind;
using mytoydb::parser::ScalarArrayOpExpr;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::Var;
using mytoydb::storage::InitBufferPool;
using mytoydb::storage::SetStorageBaseDir;
using mytoydb::storage::ShutdownBufferPool;
using mytoydb::storage::smgrcloseall;
using mytoydb::transaction::AllocateNextTransactionId;
using mytoydb::transaction::BeginTransactionBlock;
using mytoydb::transaction::EndTransactionBlock;
using mytoydb::transaction::HeapTuple;
using mytoydb::transaction::InitializeTransactionSystem;
using mytoydb::transaction::MakeSnapshot;
using mytoydb::transaction::ResetTransactionState;
using mytoydb::transaction::SnapshotData;
using mytoydb::transaction::TransactionIdCommit;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetBool;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::Int64GetDatum;
using mytoydb::types::kBoolOid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;

namespace {

using mytoydb::nodes::makePallocNode;

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

class ExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("executor_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        mytoydb::transaction::InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/mytoydb_executor_test_" + std::to_string(getpid());
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
        mytoydb::transaction::InitializeSnapshotManager();

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: commit and start a new transaction so inserted tuples are visible.
    void CommitAndStartNew() {
        EndTransactionBlock();
        // Reset the cached snapshot so the new transaction gets a fresh one.
        mytoydb::transaction::InitializeSnapshotManager();
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

    static void RunShell(const std::string& cmd) { std::system(cmd.c_str()); }

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
    int64_t minute_trunc = DatumGetInt64(mytoydb::types::date_trunc("minute", Int64GetDatum(ts)));
    EXPECT_EQ(minute_trunc % 60000000, 0);
    EXPECT_GT(minute_trunc, 0);
    EXPECT_LE(minute_trunc, ts);
    EXPECT_GT(minute_trunc, ts - 60000000);

    // Truncating to hour zeroes minutes/seconds/microseconds (3,600s = 3.6e9 μs).
    int64_t hour_trunc = DatumGetInt64(mytoydb::types::date_trunc("hour", Int64GetDatum(ts)));
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
    auto* hash_plan = makePallocNode<mytoydb::executor::Hash>();
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

}  // namespace
