// window_func_test.cpp — Unit tests for the WindowAgg executor node.
//
// Validates window function execution (OVER clause) at the executor level:
//   - Full-partition aggregates (no ORDER BY): every row in a partition
//     receives the same aggregate value computed over the whole partition.
//   - Running aggregates (ORDER BY present): each row sees the running
//     total from the start of the partition up to and including itself.
//
// Aggregate functions covered: COUNT, SUM, AVG, MIN, MAX.
//
// Note: ROW_NUMBER / RANK / LAG / LEAD require a WindowFunc node type that
// pgcpp does not yet model; only Aggref-based window aggregates are tested
// here. The parser (OVER clause grammar) and planner (WindowAgg path
// generation) wiring is verified separately via the regression test
// query_cte_window.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <tuple>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_proc.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/estate.hpp"
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
#include "transaction/lock.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

using pgcpp::access::CreateTupleDesc;
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
using pgcpp::executor::EState;
using pgcpp::executor::ExecutorEnd;
using pgcpp::executor::ExecutorFinish;
using pgcpp::executor::ExecutorRun;
using pgcpp::executor::ExecutorStart;
using pgcpp::executor::Plan;
using pgcpp::executor::QueryDesc;
using pgcpp::executor::SeqScan;
using pgcpp::executor::Sort;
using pgcpp::executor::TupleTableSlot;
using pgcpp::executor::WindowAgg;
using pgcpp::memory::AllocSetContext;
using pgcpp::memory::palloc;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::Aggref;
using pgcpp::parser::CmdType;
using pgcpp::parser::Node;
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
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::Float8GetDatum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;

namespace {

// Operator OIDs (from bootstrap_catalog.cpp).
constexpr Oid kInt4LtOp = 97;  // int4 < int4

// Aggregate function OIDs (from bootstrap_catalog.cpp).
constexpr Oid kCountInt4Oid = 2147;
constexpr Oid kSumInt4Oid = 2108;
constexpr Oid kAvgInt4Oid = 2107;
constexpr Oid kMinInt4Oid = 2131;
constexpr Oid kMaxInt4Oid = 2116;

class WindowFuncTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("window_func_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_window_func_test_" + std::to_string(getpid());
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
        InitializeSnapshotManager();
        BeginTransactionBlock();
    }

    // Build a 2-column int4 schema (k, v) — k = partition key, v = value.
    std::vector<FormData_pg_attribute> MakeKVSchema(Oid relid) {
        FormData_pg_attribute k;
        k.attrelid = relid;
        k.attname = "k";
        k.attnum = 1;
        k.atttypid = kInt4Oid;
        k.attlen = 4;
        k.attbyval = true;
        k.attalign = AttAlign::kInt;
        k.attstorage = AttStorage::kPlain;

        FormData_pg_attribute v;
        v.attrelid = relid;
        v.attname = "v";
        v.attnum = 2;
        v.atttypid = kInt4Oid;
        v.attlen = 4;
        v.attbyval = true;
        v.attalign = AttAlign::kInt;
        v.attstorage = AttStorage::kPlain;

        return {k, v};
    }

    Relation CreateTestRelation(Oid relid, const std::string& name,
                                const std::vector<FormData_pg_attribute>& attrs) {
        auto* class_row = makePallocNode<FormData_pg_class>();
        class_row->oid = relid;
        class_row->relname = name;
        class_row->relfilenode = relid;
        class_row->relkind = RelKind::kRelation;
        class_row->relpersistence = RelPersistence::kPermanent;
        catalog_->InsertClass(class_row);
        for (const auto& attr : attrs) {
            auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
            catalog_->InsertAttribute(attr_row);
        }
        RelationCreateStorage(relid, false);
        return RelationOpen(relid);
    }

    void InsertKVRow(Relation rel, int32_t k, int32_t v) {
        TupleDesc tupdesc = rel->rd_att;
        Datum values[2] = {Int32GetDatum(k), Int32GetDatum(v)};
        bool isnull[2] = {false, false};
        HeapTuple tup = heap_form_tuple(tupdesc, values, isnull);
        heap_insert(rel, tup);
        heap_freetuple(tup);
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

    Aggref* MakeAggref(Oid aggfnoid, Oid aggtype, bool aggstar = false) {
        auto* agg = makePallocNode<Aggref>();
        agg->aggfnoid = aggfnoid;
        agg->aggtype = aggtype;
        agg->aggstar = aggstar;
        return agg;
    }

    RangeTblEntry* MakeRTE(Oid relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = static_cast<int>(relid);
        return rte;
    }

    Query* MakeSelectQuery(std::vector<RangeTblEntry*> rtable) {
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kSelect;
        for (auto* rte : rtable) {
            query->rtable.push_back(rte);
        }
        return query;
    }

    // Build a SeqScan → Sort plan that outputs (k, v) sorted by the given
    // column indices. The Sort is needed because WindowAgg assumes the
    // child produces rows sorted on PARTITION BY then ORDER BY so rows in
    // the same partition are contiguous.
    Plan* BuildSortedScan(Oid /*relid*/, const std::vector<int>& sort_col_idx) {
        auto* seqplan = makePallocNode<SeqScan>();
        seqplan->scanrelid = 1;
        seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "k"));
        seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "v"));

        if (sort_col_idx.empty()) {
            return seqplan;
        }

        auto* sortplan = makePallocNode<Sort>();
        sortplan->lefttree = seqplan;
        sortplan->limit = -1;
        sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "k"));
        sortplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "v"));
        for (int col : sort_col_idx) {
            sortplan->sortColIdx.push_back(col);
            sortplan->sortOperators.push_back(kInt4LtOp);
            sortplan->nullsFirst.push_back(false);
            sortplan->reverse.push_back(false);
        }
        return sortplan;
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
// SubTask 13.5: SELECT sum(v) OVER (PARTITION BY k) FROM t
// Full-partition sum (no ORDER BY → every row gets the partition total).
// ===========================================================================

TEST_F(WindowFuncTest, SumOverPartition_FullPartition) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t", MakeKVSchema(relid));
    // Partition k=1: values 10, 20, 50 → sum = 80
    // Partition k=2: values 30, 40      → sum = 70
    InsertKVRow(rel, 1, 10);
    InsertKVRow(rel, 1, 20);
    InsertKVRow(rel, 1, 50);
    InsertKVRow(rel, 2, 30);
    InsertKVRow(rel, 2, 40);
    CommitAndStartNew();
    RelationClose(rel);

    // Sort by PARTITION BY (k) only — no ORDER BY → full-partition mode.
    Plan* child = BuildSortedScan(relid, /*sort_col_idx=*/{1});

    auto* wplan = makePallocNode<WindowAgg>();
    wplan->lefttree = child;
    wplan->partColIdx = {1};  // PARTITION BY k
    wplan->ordColIdx = {};    // no ORDER BY → full-partition mode
    wplan->ordReverse = {};

    // Target: k, v, SUM(v) OVER (PARTITION BY k)
    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "k"));
    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "v"));
    Aggref* sum_agg = MakeAggref(kSumInt4Oid, kInt8Oid, false);
    sum_agg->args.push_back(MakeVar(1, 2, kInt4Oid));
    wplan->targetlist.push_back(MakeTargetEntry(sum_agg, 3, "part_sum"));

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
    ASSERT_EQ(results.size(), 5u);

    // Partition 1: every row gets sum = 80.
    EXPECT_EQ(std::get<0>(results[0]), 1);
    EXPECT_EQ(std::get<1>(results[0]), 10);
    EXPECT_EQ(std::get<2>(results[0]), 80);
    EXPECT_EQ(std::get<0>(results[1]), 1);
    EXPECT_EQ(std::get<1>(results[1]), 20);
    EXPECT_EQ(std::get<2>(results[1]), 80);
    EXPECT_EQ(std::get<0>(results[2]), 1);
    EXPECT_EQ(std::get<1>(results[2]), 50);
    EXPECT_EQ(std::get<2>(results[2]), 80);
    // Partition 2: every row gets sum = 70.
    EXPECT_EQ(std::get<0>(results[3]), 2);
    EXPECT_EQ(std::get<1>(results[3]), 30);
    EXPECT_EQ(std::get<2>(results[3]), 70);
    EXPECT_EQ(std::get<0>(results[4]), 2);
    EXPECT_EQ(std::get<1>(results[4]), 40);
    EXPECT_EQ(std::get<2>(results[4]), 70);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// SubTask 13.5: SELECT count(*) OVER () FROM t
// Total count over the whole input (no PARTITION BY, no ORDER BY).
// ===========================================================================

TEST_F(WindowFuncTest, CountStarOver_Empty_FullTable) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t", MakeKVSchema(relid));
    InsertKVRow(rel, 1, 10);
    InsertKVRow(rel, 1, 20);
    InsertKVRow(rel, 2, 30);
    InsertKVRow(rel, 2, 40);
    CommitAndStartNew();
    RelationClose(rel);

    // No PARTITION BY, no ORDER BY → single partition over all rows,
    // full-partition mode (every row gets the total count = 4).
    Plan* child = BuildSortedScan(relid, /*sort_col_idx=*/{});

    auto* wplan = makePallocNode<WindowAgg>();
    wplan->lefttree = child;
    wplan->partColIdx = {};
    wplan->ordColIdx = {};
    wplan->ordReverse = {};

    // Target: k, v, COUNT(*) OVER ()
    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "k"));
    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "v"));
    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, true);
    wplan->targetlist.push_back(MakeTargetEntry(count_agg, 3, "total_count"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = wplan;

    ExecutorStart(qd);
    std::vector<std::pair<int, int64_t>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[1]),   // v
                             DatumGetInt64(slot->tts_values[2]));  // count
    }
    ASSERT_EQ(results.size(), 4u);
    // Every row gets count = 4 (the whole table is one partition, full mode).
    for (const auto& r : results) {
        EXPECT_EQ(r.second, 4);
    }

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// SubTask 13.5: SELECT count(v) OVER (PARTITION BY k) FROM t
// Full-partition count of non-null v per partition.
// ===========================================================================

TEST_F(WindowFuncTest, CountOverPartition_FullPartition) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t", MakeKVSchema(relid));
    InsertKVRow(rel, 1, 10);
    InsertKVRow(rel, 1, 20);
    InsertKVRow(rel, 1, 50);
    InsertKVRow(rel, 2, 30);
    InsertKVRow(rel, 2, 40);
    CommitAndStartNew();
    RelationClose(rel);

    Plan* child = BuildSortedScan(relid, /*sort_col_idx=*/{1});

    auto* wplan = makePallocNode<WindowAgg>();
    wplan->lefttree = child;
    wplan->partColIdx = {1};
    wplan->ordColIdx = {};
    wplan->ordReverse = {};

    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "k"));
    Aggref* count_agg = MakeAggref(kCountInt4Oid, kInt8Oid, false);
    count_agg->args.push_back(MakeVar(1, 2, kInt4Oid));
    wplan->targetlist.push_back(MakeTargetEntry(count_agg, 2, "part_count"));

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
    ASSERT_EQ(results.size(), 5u);
    // Partition 1 has 3 rows → count = 3 for each.
    EXPECT_EQ(results[0].first, 1);
    EXPECT_EQ(results[0].second, 3);
    EXPECT_EQ(results[1].first, 1);
    EXPECT_EQ(results[1].second, 3);
    EXPECT_EQ(results[2].first, 1);
    EXPECT_EQ(results[2].second, 3);
    // Partition 2 has 2 rows → count = 2 for each.
    EXPECT_EQ(results[3].first, 2);
    EXPECT_EQ(results[3].second, 2);
    EXPECT_EQ(results[4].first, 2);
    EXPECT_EQ(results[4].second, 2);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// SubTask 13.5: SELECT avg(v) OVER (PARTITION BY k ORDER BY v) FROM t
// Running average (ORDER BY present → running mode).
// ===========================================================================

TEST_F(WindowFuncTest, AvgOverPartitionOrderBy_Running) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t", MakeKVSchema(relid));
    // Partition k=1: values 10, 20, 50
    //   running avg = 10.0, 15.0, 26.666...
    // Partition k=2: values 30, 40
    //   running avg = 30.0, 35.0
    InsertKVRow(rel, 1, 10);
    InsertKVRow(rel, 1, 20);
    InsertKVRow(rel, 1, 50);
    InsertKVRow(rel, 2, 30);
    InsertKVRow(rel, 2, 40);
    CommitAndStartNew();
    RelationClose(rel);

    // Sort by PARTITION BY (k) then ORDER BY (v).
    Plan* child = BuildSortedScan(relid, /*sort_col_idx=*/{1, 2});

    auto* wplan = makePallocNode<WindowAgg>();
    wplan->lefttree = child;
    wplan->partColIdx = {1};  // PARTITION BY k
    wplan->ordColIdx = {2};   // ORDER BY v → running mode
    wplan->ordReverse = {false};

    // Target: k, v, AVG(v) OVER (PARTITION BY k ORDER BY v)
    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "k"));
    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "v"));
    Aggref* avg_agg = MakeAggref(kAvgInt4Oid, kFloat8Oid, false);
    avg_agg->args.push_back(MakeVar(1, 2, kInt4Oid));
    wplan->targetlist.push_back(MakeTargetEntry(avg_agg, 3, "running_avg"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = wplan;

    ExecutorStart(qd);
    std::vector<std::tuple<int, int, double>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]), DatumGetInt32(slot->tts_values[1]),
                             DatumGetFloat8(slot->tts_values[2]));
    }
    ASSERT_EQ(results.size(), 5u);

    // Partition 1: running avg = 10.0, 15.0, 26.666...
    EXPECT_EQ(std::get<0>(results[0]), 1);
    EXPECT_EQ(std::get<1>(results[0]), 10);
    EXPECT_DOUBLE_EQ(std::get<2>(results[0]), 10.0);
    EXPECT_EQ(std::get<0>(results[1]), 1);
    EXPECT_EQ(std::get<1>(results[1]), 20);
    EXPECT_DOUBLE_EQ(std::get<2>(results[1]), 15.0);
    EXPECT_EQ(std::get<0>(results[2]), 1);
    EXPECT_EQ(std::get<1>(results[2]), 50);
    EXPECT_NEAR(std::get<2>(results[2]), 26.6666667, 1e-6);
    // Partition 2: running avg = 30.0, 35.0 (resets at partition boundary)
    EXPECT_EQ(std::get<0>(results[3]), 2);
    EXPECT_EQ(std::get<1>(results[3]), 30);
    EXPECT_DOUBLE_EQ(std::get<2>(results[3]), 30.0);
    EXPECT_EQ(std::get<0>(results[4]), 2);
    EXPECT_EQ(std::get<1>(results[4]), 40);
    EXPECT_DOUBLE_EQ(std::get<2>(results[4]), 35.0);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// SubTask 13.5: SELECT sum(v) OVER (PARTITION BY k ORDER BY v) FROM t
// Running sum (ORDER BY present → running mode). Verifies the existing
// behavior is preserved when the new full-partition mode flag is false.
// ===========================================================================

TEST_F(WindowFuncTest, SumOverPartitionOrderBy_Running) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t", MakeKVSchema(relid));
    InsertKVRow(rel, 1, 10);
    InsertKVRow(rel, 1, 20);
    InsertKVRow(rel, 1, 50);
    InsertKVRow(rel, 2, 30);
    InsertKVRow(rel, 2, 40);
    CommitAndStartNew();
    RelationClose(rel);

    Plan* child = BuildSortedScan(relid, /*sort_col_idx=*/{1, 2});

    auto* wplan = makePallocNode<WindowAgg>();
    wplan->lefttree = child;
    wplan->partColIdx = {1};
    wplan->ordColIdx = {2};
    wplan->ordReverse = {false};

    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "k"));
    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "v"));
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
    ASSERT_EQ(results.size(), 5u);
    // Partition 1: running sum = 10, 30, 80
    EXPECT_EQ(std::get<2>(results[0]), 10);
    EXPECT_EQ(std::get<2>(results[1]), 30);
    EXPECT_EQ(std::get<2>(results[2]), 80);
    // Partition 2: running sum = 30, 70 (resets)
    EXPECT_EQ(std::get<2>(results[3]), 30);
    EXPECT_EQ(std::get<2>(results[4]), 70);

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

// ===========================================================================
// SubTask 13.5: SELECT min(v), max(v) OVER (PARTITION BY k) FROM t
// Full-partition min/max (no ORDER BY → full-partition mode).
// ===========================================================================

TEST_F(WindowFuncTest, MinMaxOverPartition_FullPartition) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "t", MakeKVSchema(relid));
    InsertKVRow(rel, 1, 50);
    InsertKVRow(rel, 1, 10);
    InsertKVRow(rel, 1, 20);
    InsertKVRow(rel, 2, 40);
    InsertKVRow(rel, 2, 30);
    CommitAndStartNew();
    RelationClose(rel);

    // Sort by PARTITION BY (k) only. Within-partition row order doesn't
    // matter for full-partition min/max.
    Plan* child = BuildSortedScan(relid, /*sort_col_idx=*/{1});

    auto* wplan = makePallocNode<WindowAgg>();
    wplan->lefttree = child;
    wplan->partColIdx = {1};
    wplan->ordColIdx = {};
    wplan->ordReverse = {};

    wplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "k"));
    Aggref* min_agg = MakeAggref(kMinInt4Oid, kInt4Oid, false);
    min_agg->args.push_back(MakeVar(1, 2, kInt4Oid));
    wplan->targetlist.push_back(MakeTargetEntry(min_agg, 2, "part_min"));
    Aggref* max_agg = MakeAggref(kMaxInt4Oid, kInt4Oid, false);
    max_agg->args.push_back(MakeVar(1, 2, kInt4Oid));
    wplan->targetlist.push_back(MakeTargetEntry(max_agg, 3, "part_max"));

    auto* rte = MakeRTE(relid);
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = MakeSelectQuery({rte});
    qd->plan = wplan;

    ExecutorStart(qd);
    std::vector<std::tuple<int, int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]), DatumGetInt32(slot->tts_values[1]),
                             DatumGetInt32(slot->tts_values[2]));
    }
    ASSERT_EQ(results.size(), 5u);
    // Partition 1 (values 50, 10, 20): min=10, max=50 for every row.
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(std::get<0>(results[i]), 1);
        EXPECT_EQ(std::get<1>(results[i]), 10);
        EXPECT_EQ(std::get<2>(results[i]), 50);
    }
    // Partition 2 (values 40, 30): min=30, max=40 for every row.
    for (int i = 3; i < 5; ++i) {
        EXPECT_EQ(std::get<0>(results[i]), 2);
        EXPECT_EQ(std::get<1>(results[i]), 30);
        EXPECT_EQ(std::get<2>(results[i]), 40);
    }

    ExecutorFinish(qd);
    ExecutorEnd(qd);
}

}  // namespace
