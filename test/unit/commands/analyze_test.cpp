// analyze_test.cpp — Unit tests for P1-5 optimizer minimal closed loop.
//
// Verifies the full pipeline: ANALYZE collects statistics → pg_statistic
// stores them → EstimateSelectivity uses them → CreateIndexPaths generates
// IndexPath candidates → standard_planner selects IndexScan when cheaper.
//
// The fixture sets up the full storage stack (Catalog, SysCache, buffer
// pool, relcache, transaction) so ANALYZE can scan real heap tuples.

#include "commands/analyze.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_index.hpp"
#include "catalog/pg_statistic.hpp"
#include "catalog/syscache.hpp"
#include "commands/indexcmds.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/plannodes.hpp"
#include "optimizer/cost.hpp"
#include "optimizer/path.hpp"
#include "optimizer/path/indxpath.hpp"
#include "optimizer/plan/init_splan.hpp"
#include "optimizer/planner.hpp"
#include "optimizer/util/pathnode.hpp"
#include "optimizer/util/relnode.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_form_tuple;
using pgcpp::access::heap_freetuple;
using pgcpp::access::heap_getattr;
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
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::FormData_pg_statistic;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::kStatisticKindHistogram;
using pgcpp::catalog::kStatisticKindMcv;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::commands::AnalyzeCommand;
using pgcpp::commands::DefineIndex;
using pgcpp::executor::IndexScan;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanType;
using pgcpp::executor::SeqScan;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::optimizer::CreateIndexPaths;
using pgcpp::optimizer::EstimateSelectivity;
using pgcpp::optimizer::PlannerInfo;
using pgcpp::optimizer::RelOptInfo;
using pgcpp::optimizer::standard_planner;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::FromExpr;
using pgcpp::parser::IndexElem;
using pgcpp::parser::IndexStmt;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RangeTblRef;
using pgcpp::parser::RangeVar;
using pgcpp::parser::RTEKind;
using pgcpp::parser::SortGroupClause;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::VacuumStmt;
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
using pgcpp::transaction::ItemPointerData;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::types::Datum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;

namespace {

class AnalyzeTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("analyze_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_analyze_test_" + std::to_string(getpid());
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
        InitializeSnapshotManager();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    void CommitAndStartNew() {
        EndTransactionBlock();
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

    std::vector<FormData_pg_attribute> MakeIntSchema(Oid relid) {
        FormData_pg_attribute a;
        a.attrelid = relid;
        a.attname = "a";
        a.attnum = 1;
        a.atttypid = kInt4Oid;
        a.attlen = 4;
        a.attbyval = true;
        a.attalign = AttAlign::kInt;
        a.attstorage = AttStorage::kPlain;
        return {a};
    }

    ItemPointerData InsertTuple(Relation rel, int32_t a) {
        Datum values[1] = {Int32GetDatum(a)};
        bool isnull[1] = {false};
        HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
        ItemPointerData tid = heap_insert(rel, tup);
        heap_freetuple(tup);
        return tid;
    }

    // Build a VacuumStmt for ANALYZE on the given relation name.
    VacuumStmt* MakeAnalyzeStmt(const std::string& relname) {
        auto* stmt = makePallocNode<VacuumStmt>();
        auto* rv = makePallocNode<RangeVar>();
        rv->relname = relname;
        stmt->rels.push_back(rv);
        stmt->is_vacuumcmd = false;  // ANALYZE, not VACUUM
        return stmt;
    }

    // Build an IndexStmt for CREATE INDEX ON table (col).
    IndexStmt* MakeIndexStmt(const std::string& table, const std::string& column,
                             const std::string& idxname) {
        auto* stmt = makePallocNode<IndexStmt>();
        stmt->idxname = idxname;
        auto* rv = makePallocNode<RangeVar>();
        rv->relname = table;
        stmt->relation = rv;
        auto* elem = makePallocNode<IndexElem>();
        elem->name = column;
        stmt->index_params.push_back(elem);
        return stmt;
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

    OpExpr* MakeOpExpr(pgcpp::catalog::Oid opno, Node* left, Node* right) {
        auto* op = makePallocNode<OpExpr>();
        op->opno = opno;
        op->opresulttype = kBoolOid;
        op->args.push_back(left);
        op->args.push_back(right);
        return op;
    }

    TargetEntry* MakeTargetEntry(Node* expr, int resno, const std::string& resname = "") {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = resname;
        return te;
    }

    RangeTblEntry* MakeRTE(Oid relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = relid;
        return rte;
    }

    FromExpr* MakeFromExpr(int rtindex, Node* quals = nullptr) {
        auto* from = makePallocNode<FromExpr>();
        auto* ref = makePallocNode<RangeTblRef>();
        ref->rtindex = rtindex;
        from->fromlist.push_back(ref);
        from->quals = quals;
        return from;
    }

    Query* MakeSelectQuery() {
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kSelect;
        return query;
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

constexpr Oid kInt4EqOp = 96;   // int4 = int4
constexpr Oid kInt4LtOp = 97;   // int4 < int4
constexpr Oid kInt4GtOp = 521;  // int4 > int4

// --- ANALYZE populates pg_statistic ---

// ANALYZE on a table with int4 values writes pg_statistic with MCV + histogram.
TEST_F(AnalyzeTest, AnalyzePopulatesPgStatistic) {
    constexpr Oid kRelid = 4001;
    auto attrs = MakeIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "analyze_t1", attrs);

    // Insert 10 rows: values 1..5 each appear twice (so they're all MCV).
    for (int i = 1; i <= 5; i++) {
        InsertTuple(rel, i);
        InsertTuple(rel, i);
    }
    CommitAndStartNew();

    // Run ANALYZE.
    std::string result = AnalyzeCommand(MakeAnalyzeStmt("analyze_t1"));
    EXPECT_EQ(result, "ANALYZE");

    // Verify pg_statistic was populated for column 1.
    const FormData_pg_statistic* stat = GetCatalog()->GetStatistic(kRelid, 1);
    ASSERT_NE(stat, nullptr);
    EXPECT_EQ(stat->starelid, kRelid);
    EXPECT_EQ(stat->staattnum, 1);
    EXPECT_EQ(stat->stanullfrac, 0.0F);  // no nulls
    EXPECT_EQ(stat->stawidth, 4);        // int4 = 4 bytes
    EXPECT_EQ(stat->stadistinct, 5);     // 5 distinct values (1..5)

    // MCV slot should be populated.
    EXPECT_EQ(stat->stakind1, kStatisticKindMcv);
    EXPECT_FALSE(stat->stavalues1.empty());
    // MCV string format: "v1:f1,v2:f2,..."
    EXPECT_NE(stat->stavalues1.find(':'), std::string::npos);

    // Histogram slot should be populated.
    EXPECT_EQ(stat->stakind2, kStatisticKindHistogram);

    // pg_class.relpages and reltuples should be updated.
    const FormData_pg_class* cls = GetCatalog()->GetClassByOid(kRelid);
    ASSERT_NE(cls, nullptr);
    EXPECT_GE(cls->relpages, 1);
    EXPECT_FLOAT_EQ(cls->reltuples, 10.0F);

    RelationClose(rel);
}

// ANALYZE on an empty table writes a pg_statistic row with zero stats.
TEST_F(AnalyzeTest, AnalyzeEmptyTable) {
    constexpr Oid kRelid = 4002;
    auto attrs = MakeIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "analyze_empty", attrs);
    CommitAndStartNew();

    std::string result = AnalyzeCommand(MakeAnalyzeStmt("analyze_empty"));
    EXPECT_EQ(result, "ANALYZE");

    const FormData_pg_statistic* stat = GetCatalog()->GetStatistic(kRelid, 1);
    ASSERT_NE(stat, nullptr);
    EXPECT_EQ(stat->stanullfrac, 0.0F);
    EXPECT_EQ(stat->stadistinct, 0);

    RelationClose(rel);
}

// --- EstimateSelectivity uses pg_statistic ---

// EstimateSelectivity for col = const uses MCV frequency from pg_statistic.
TEST_F(AnalyzeTest, EstimateSelectivity_EqUsesMcv) {
    constexpr Oid kRelid = 4003;
    auto attrs = MakeIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "sel_eq", attrs);

    // Insert 10 rows: value 5 appears 5 times (freq 0.5), 5 other distinct
    // values (6,7,8,9,10) once each.
    for (int i = 1; i <= 5; i++)
        InsertTuple(rel, 5);
    for (int i = 6; i <= 10; i++)
        InsertTuple(rel, i);
    CommitAndStartNew();

    AnalyzeCommand(MakeAnalyzeStmt("sel_eq"));

    // col = 5 should have selectivity ~0.5 (from MCV).
    Node* qual = MakeOpExpr(kInt4EqOp, MakeVar(1, 1), MakeInt4Const(5));
    double selec = EstimateSelectivity(qual, 10, kRelid);
    EXPECT_NEAR(selec, 0.5, 0.05);

    // col = 99 (not in MCV) should fall back to 1/ndistinct.
    Node* qual2 = MakeOpExpr(kInt4EqOp, MakeVar(1, 1), MakeInt4Const(99));
    double selec2 = EstimateSelectivity(qual2, 10, kRelid);
    EXPECT_GT(selec2, 0.0);
    EXPECT_LT(selec2, 0.2);

    RelationClose(rel);
}

// EstimateSelectivity for col < const uses histogram interpolation.
TEST_F(AnalyzeTest, EstimateSelectivity_LtUsesHistogram) {
    constexpr Oid kRelid = 4004;
    auto attrs = MakeIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "sel_lt", attrs);

    // Insert 20 rows with values 1..20.
    for (int i = 1; i <= 20; i++)
        InsertTuple(rel, i);
    CommitAndStartNew();

    AnalyzeCommand(MakeAnalyzeStmt("sel_lt"));

    // col < 15 should give selectivity > 0.5 (most values are below 15).
    Node* qual = MakeOpExpr(kInt4LtOp, MakeVar(1, 1), MakeInt4Const(15));
    double selec = EstimateSelectivity(qual, 20, kRelid);
    EXPECT_GT(selec, 0.5);
    EXPECT_LT(selec, 1.0);

    RelationClose(rel);
}

// EstimateSelectivity without stats falls back to heuristics.
TEST_F(AnalyzeTest, EstimateSelectivity_NoStatsUsesHeuristic) {
    constexpr Oid kRelid = 4005;
    auto attrs = MakeIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "sel_nostat", attrs);
    InsertTuple(rel, 1);
    CommitAndStartNew();

    // No ANALYZE → no pg_statistic → heuristic selectivity.
    Node* qual = MakeOpExpr(kInt4EqOp, MakeVar(1, 1), MakeInt4Const(1));
    double selec = EstimateSelectivity(qual, 1000, kInvalidOid);
    EXPECT_NEAR(selec, 0.1, 0.01);  // default eq heuristic

    RelationClose(rel);
}

// --- CreateIndexPaths generates IndexPath candidates ---

// CreateIndexPaths creates an IndexPath when an index matches a qual clause.
TEST_F(AnalyzeTest, CreateIndexPaths_GeneratesIndexPath) {
    constexpr Oid kRelid = 4006;
    auto attrs = MakeIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "idx_path_t1", attrs);

    // Insert 100 rows.
    for (int i = 1; i <= 100; i++)
        InsertTuple(rel, i);
    CommitAndStartNew();

    // Create an index on column "a".
    DefineIndex(MakeIndexStmt("idx_path_t1", "a", "idx_path_t1_a_idx"));

    // Build a PlannerInfo with a qual clause (a = 50).
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(kRelid));
    Node* qual = MakeOpExpr(kInt4EqOp, MakeVar(1, 1), MakeInt4Const(50));
    query->jointree = MakeFromExpr(1, qual);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));
    root->parse = query;

    // Initialize planner state (builds RelOptInfos, distributes quals).
    pgcpp::optimizer::query_planner_init(root, query);

    // Find the base rel.
    RelOptInfo* relinfo = pgcpp::optimizer::find_base_rel(root, 1);
    ASSERT_NE(relinfo, nullptr);

    // Manually run CreateIndexPaths (query_planner would also call it).
    // First add a SeqScanPath so we have a baseline.
    pgcpp::optimizer::SeqScanPath* seqpath = pgcpp::optimizer::create_seqscan_path(root, relinfo);
    pgcpp::optimizer::add_path(relinfo, seqpath);

    CreateIndexPaths(root, relinfo);

    // The pathlist should now contain an IndexPath.
    bool has_index_path = false;
    for (auto* p : relinfo->pathlist) {
        if (p->type == pgcpp::optimizer::PathType::kIndexScan) {
            has_index_path = true;
            break;
        }
    }
    EXPECT_TRUE(has_index_path);

    RelationClose(rel);
}

// --- standard_planner selects IndexScan when index is cheaper ---

// standard_planner produces an IndexScan plan for a point lookup on an
// indexed column when the table is large enough that IndexScan is cheaper
// than SeqScan.
TEST_F(AnalyzeTest, StandardPlanner_SelectsWithIndex_ForPointLookup) {
    constexpr Oid kRelid = 4007;
    auto attrs = MakeIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "plan_idx_t1", attrs);

    // Insert 2000 rows so SeqScan cost is significant (multiple pages).
    for (int i = 1; i <= 2000; i++)
        InsertTuple(rel, i);
    CommitAndStartNew();

    // Create an index on column "a".
    DefineIndex(MakeIndexStmt("plan_idx_t1", "a", "plan_idx_t1_a_idx"));

    // ANALYZE to populate statistics (so selectivity is accurate).
    AnalyzeCommand(MakeAnalyzeStmt("plan_idx_t1"));

    // Build a SELECT query: SELECT a FROM plan_idx_t1 WHERE a = 50.
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(kRelid));
    Node* qual = MakeOpExpr(kInt4EqOp, MakeVar(1, 1), MakeInt4Const(50));
    query->jointree = MakeFromExpr(1, qual);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));

    Plan* plan = standard_planner(query);
    ASSERT_NE(plan, nullptr);
    // With 2000 rows and a point lookup (selectivity ~1/2000), IndexScan
    // should be cheaper than SeqScan.
    EXPECT_EQ(plan->type, PlanType::kIndexScan);
    auto* scan = static_cast<IndexScan*>(plan);
    EXPECT_EQ(scan->scanrelid, 1);
    EXPECT_NE(scan->indexid, kInvalidOid);
    ASSERT_EQ(scan->indexqual.size(), 1u);

    RelationClose(rel);
}

// standard_planner falls back to SeqScan when no index matches the qual.
TEST_F(AnalyzeTest, StandardPlanner_UsesSeqScan_WithoutIndex) {
    constexpr Oid kRelid = 4008;
    auto attrs = MakeIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "plan_seq_t1", attrs);

    for (int i = 1; i <= 10; i++)
        InsertTuple(rel, i);
    CommitAndStartNew();

    // No index created.

    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(kRelid));
    Node* qual = MakeOpExpr(kInt4EqOp, MakeVar(1, 1), MakeInt4Const(5));
    query->jointree = MakeFromExpr(1, qual);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));

    Plan* plan = standard_planner(query);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSeqScan);

    RelationClose(rel);
}

// standard_planner uses SeqScan when no qual clause is present (full scan).
TEST_F(AnalyzeTest, StandardPlanner_UsesSeqScan_NoQual) {
    constexpr Oid kRelid = 4009;
    auto attrs = MakeIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "plan_scan_t1", attrs);

    for (int i = 1; i <= 10; i++)
        InsertTuple(rel, i);
    CommitAndStartNew();

    DefineIndex(MakeIndexStmt("plan_scan_t1", "a", "plan_scan_t1_a_idx"));

    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(kRelid));
    query->jointree = MakeFromExpr(1);  // no qual
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));

    Plan* plan = standard_planner(query);
    ASSERT_NE(plan, nullptr);
    // No qual → no indexable clause → SeqScan.
    EXPECT_EQ(plan->type, PlanType::kSeqScan);

    RelationClose(rel);
}

}  // namespace
