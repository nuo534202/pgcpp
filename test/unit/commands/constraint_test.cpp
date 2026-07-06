// constraint_test.cpp — Unit tests for P1-1 constraint implementation.
//
// Verifies that CREATE TABLE with NOT NULL / DEFAULT / CHECK / PRIMARY KEY /
// UNIQUE / FOREIGN KEY constraints creates the correct catalog entries
// (pg_attribute.attnotnull, pg_attrdef, pg_constraint, pg_index) and that
// runtime enforcement (ExecConstraints) rejects violating rows.
//
// DDL path: raw_parser -> parse_analyze -> ProcessUtility -> DefineRelation.
// DML path: build a ModifyTable plan directly and run it through the executor.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attrdef.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_constraint.hpp"
#include "catalog/pg_index.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/estate.hpp"
#include "executor/exec_main.hpp"
#include "executor/node_exec.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "parser/analyze.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"
#include "parser/primnodes.hpp"
#include "protocol/pqformat.hpp"
#include "protocol/utility.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_getnext;
using pgcpp::access::HeapScanDesc;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::ConstraintType;
using pgcpp::catalog::FormData_pg_attrdef;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::FormData_pg_constraint;
using pgcpp::catalog::FormData_pg_index;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::executor::ExecutorEnd;
using pgcpp::executor::ExecutorFinish;
using pgcpp::executor::ExecutorRun;
using pgcpp::executor::ExecutorStart;
using pgcpp::executor::ModifyTable;
using pgcpp::executor::Plan;
using pgcpp::executor::QueryDesc;
using pgcpp::executor::Result;
using pgcpp::executor::TupleTableSlot;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::Node;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
using pgcpp::parser::RTEKind;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;
using pgcpp::protocol::ProcessUtility;
using pgcpp::protocol::StringSink;
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
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kInt4Oid;

namespace {

class ConstraintTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("constraint_test_context");
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

        test_dir_ = "/tmp/pgcpp_constraint_test_" + std::to_string(getpid());
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

    // Parse + analyze + execute a utility statement (DDL).
    std::string RunUtility(const std::string& sql) {
        std::vector<RawStmt*> raw = raw_parser(sql);
        if (raw.empty())
            return "";
        std::vector<Query*> queries = parse_analyze(raw, sql.c_str());
        if (queries.empty())
            return "";
        return ProcessUtility(queries[0]->utility_stmt, &sink_);
    }

    // Helper to check if a function ereports an error.
    template<typename F>
    bool RaisesError(F&& fn) {
        bool caught = false;
        PG_TRY() {
            fn();
        }
        PG_CATCH() {
            caught = true;
        }
        PG_END_TRY();
        return caught;
    }

    // Build a Const int4 node.
    Const* MakeInt4Const(int32_t value) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(value);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 4;
        return con;
    }

    // Build a NULL Const int4 node (for testing NOT NULL violations).
    Const* MakeNullInt4Const() {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(0);
        con->constisnull = true;
        con->constbyval = true;
        con->constlen = 4;
        return con;
    }

    // Build a Result plan producing one (a) int4 row with NULL value.
    Result* MakeNullRowPlan1() {
        auto* rp = makePallocNode<Result>();
        rp->targetlist.push_back(MakeTargetEntry(MakeNullInt4Const(), 1, "a"));
        return rp;
    }

    // Build a TargetEntry.
    TargetEntry* MakeTargetEntry(Node* expr, int resno, const std::string& resname = "") {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = resname;
        return te;
    }

    // Build a Var node.
    Var* MakeVar(int varno, int varattno, Oid vartype) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = vartype;
        return var;
    }

    // Build a RangeTblEntry for a relation.
    RangeTblEntry* MakeRTE(Oid relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = static_cast<int>(relid);
        return rte;
    }

    // Build a Result plan producing one (a) int4 row.
    Result* MakeResultRowPlan1(int32_t a) {
        auto* rp = makePallocNode<Result>();
        rp->targetlist.push_back(MakeTargetEntry(MakeInt4Const(a), 1, "a"));
        return rp;
    }

    // Build a Result plan producing one (a, b) int4 row.
    Result* MakeResultRowPlan2(int32_t a, int32_t b) {
        auto* rp = makePallocNode<Result>();
        rp->targetlist.push_back(MakeTargetEntry(MakeInt4Const(a), 1, "a"));
        rp->targetlist.push_back(MakeTargetEntry(MakeInt4Const(b), 2, "b"));
        return rp;
    }

    // Build a ModifyTable plan for INSERT on RT entry 1.
    ModifyTable* MakeInsertPlan(Plan* child, int natts) {
        auto* mt = makePallocNode<ModifyTable>();
        mt->operation = CmdType::kInsert;
        mt->resultRelid = 1;
        mt->lefttree = child;
        for (int i = 1; i <= natts; ++i) {
            mt->targetlist.push_back(MakeTargetEntry(MakeVar(1, i, kInt4Oid), i, ""));
        }
        return mt;
    }

    // Build a QueryDesc for an INSERT on the given relation.
    QueryDesc* MakeInsertQueryDesc(Oid relid, Plan* plan) {
        auto* rte = MakeRTE(relid);
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kInsert;
        query->result_relation = 1;
        query->rtable.push_back(rte);
        auto* qd = makePallocNode<QueryDesc>();
        qd->query = query;
        qd->plan = plan;
        return qd;
    }

    // Run a DML plan to completion. If ExecutorRun raises an error
    // (e.g. a constraint violation), ExecutorEnd must still be called so
    // that es_query_cxt and other per-query resources are released —
    // otherwise LeakSanitizer reports the per-query memory context as
    // leaked.
    void RunDml(QueryDesc* qd) {
        ExecutorStart(qd);
        PG_TRY() {
            TupleTableSlot* slot = nullptr;
            while ((slot = ExecutorRun(qd)) != nullptr) {
                // drain
            }
            ExecutorFinish(qd);
        }
        PG_CATCH() {
            ExecutorEnd(qd);
            PG_RE_THROW();
        }
        PG_END_TRY();
        ExecutorEnd(qd);
    }

    // Commit and start a new transaction so inserted rows are visible.
    void CommitAndStartNew() {
        EndTransactionBlock();
        InitializeSnapshotManager();
        BeginTransactionBlock();
    }

    // Scan a relation and return all (a) int4 values from the first column.
    std::vector<int32_t> ScanFirstColumn(Oid relid) {
        Relation rel = RelationOpen(relid);
        HeapScanDesc scan = heap_beginscan(rel, nullptr);
        std::vector<int32_t> rows;
        HeapTuple tup = nullptr;
        while ((tup = heap_getnext(scan)) != nullptr) {
            TupleTableSlot* slot = TupleTableSlot::Make(rel->rd_att);
            slot->StoreTuple(tup, false);
            int32_t v = slot->tts_isnull[0] ? 0 : DatumGetInt32(slot->tts_values[0]);
            rows.push_back(v);
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
    StringSink sink_;
};

// Find the pg_class OID for a relation by name.
Oid GetRelidByName(const std::string& name) {
    auto* cat = GetCatalog();
    const FormData_pg_class* row = cat->GetClassByName(name);
    return row ? row->oid : kInvalidOid;
}

}  // namespace

// ===========================================================================
// Catalog creation: NOT NULL
// ===========================================================================

TEST_F(ConstraintTest, NotNullSetsAttributeFlag) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 NOT NULL);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    ASSERT_NE(relid, kInvalidOid);

    auto attrs = catalog_->GetAttributes(relid);
    ASSERT_EQ(attrs.size(), 1u);
    EXPECT_TRUE(attrs[0]->attnotnull);
}

TEST_F(ConstraintTest, NotNullPopulatesTupleConstrHasNotNull) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 NOT NULL);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    Relation rel = RelationOpen(relid);
    ASSERT_NE(rel, nullptr);
    EXPECT_TRUE(rel->rd_att->constr.has_not_null);
    RelationClose(rel);
}

// ===========================================================================
// Catalog creation: DEFAULT
// ===========================================================================

TEST_F(ConstraintTest, DefaultCreatesAttrdefEntry) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 DEFAULT 42);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    ASSERT_NE(relid, kInvalidOid);

    auto attrdefs = catalog_->GetAttrdefsByRelid(relid);
    ASSERT_EQ(attrdefs.size(), 1u);
    EXPECT_EQ(attrdefs[0]->adnum, 1);
    EXPECT_FALSE(attrdefs[0]->adbin.empty());
}

TEST_F(ConstraintTest, DefaultMarksAttributeHasDef) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 DEFAULT 42);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    auto attrs = catalog_->GetAttributes(relid);
    ASSERT_EQ(attrs.size(), 1u);
    EXPECT_TRUE(attrs[0]->atthasdef);
}

TEST_F(ConstraintTest, DefaultPopulatesTupleConstrDefval) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 DEFAULT 42);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    Relation rel = RelationOpen(relid);
    ASSERT_NE(rel, nullptr);
    ASSERT_EQ(rel->rd_att->constr.defval.size(), 1u);
    EXPECT_EQ(rel->rd_att->constr.defval[0].adnum, 1);
    EXPECT_FALSE(rel->rd_att->constr.defval[0].adbin.empty());
    RelationClose(rel);
}

// ===========================================================================
// Catalog creation: CHECK
// ===========================================================================

TEST_F(ConstraintTest, CheckCreatesConstraintEntry) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 CHECK (a > 0));"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    ASSERT_NE(relid, kInvalidOid);

    auto cons = catalog_->GetConstraintsByRelid(relid);
    ASSERT_EQ(cons.size(), 1u);
    EXPECT_EQ(cons[0]->contype, ConstraintType::kCheck);
    EXPECT_EQ(cons[0]->conrelid, relid);
    EXPECT_FALSE(cons[0]->conbin.empty());
    EXPECT_EQ(cons[0]->conkey.size(), 1u);
    EXPECT_EQ(cons[0]->conkey[0], 1);
}

TEST_F(ConstraintTest, CheckPopulatesTupleConstrCheck) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 CHECK (a > 0));"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    Relation rel = RelationOpen(relid);
    ASSERT_NE(rel, nullptr);
    ASSERT_EQ(rel->rd_att->constr.check.size(), 1u);
    EXPECT_FALSE(rel->rd_att->constr.check[0].ccbin.empty());
    RelationClose(rel);
}

TEST_F(ConstraintTest, TableLevelCheckCreatesConstraintEntry) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4, b int4, CHECK (b > a));"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    auto cons = catalog_->GetConstraintsByRelid(relid);
    ASSERT_EQ(cons.size(), 1u);
    EXPECT_EQ(cons[0]->contype, ConstraintType::kCheck);
    EXPECT_FALSE(cons[0]->conbin.empty());
}

// ===========================================================================
// Catalog creation: PRIMARY KEY
// ===========================================================================

TEST_F(ConstraintTest, PrimaryKeyCreatesConstraintAndIndex) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 PRIMARY KEY);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    ASSERT_NE(relid, kInvalidOid);

    auto cons = catalog_->GetConstraintsByRelid(relid);
    ASSERT_EQ(cons.size(), 1u);
    EXPECT_EQ(cons[0]->contype, ConstraintType::kPrimaryKey);
    EXPECT_NE(cons[0]->conindid, kInvalidOid);

    auto indexes = catalog_->GetIndexesByRelid(relid);
    ASSERT_EQ(indexes.size(), 1u);
    EXPECT_TRUE(indexes[0]->indisprimary);
    EXPECT_TRUE(indexes[0]->indisunique);
}

TEST_F(ConstraintTest, PrimaryKeyImpliesNotNull) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 PRIMARY KEY);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    auto attrs = catalog_->GetAttributes(relid);
    ASSERT_EQ(attrs.size(), 1u);
    EXPECT_TRUE(attrs[0]->attnotnull);
}

TEST_F(ConstraintTest, TableLevelPrimaryKeyCreatesConstraint) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4, b int4, PRIMARY KEY (a, b));"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    auto cons = catalog_->GetConstraintsByRelid(relid);
    ASSERT_EQ(cons.size(), 1u);
    EXPECT_EQ(cons[0]->contype, ConstraintType::kPrimaryKey);
    EXPECT_EQ(cons[0]->conkey.size(), 2u);
    EXPECT_EQ(cons[0]->conkey[0], 1);
    EXPECT_EQ(cons[0]->conkey[1], 2);

    auto indexes = catalog_->GetIndexesByRelid(relid);
    ASSERT_EQ(indexes.size(), 1u);
    EXPECT_TRUE(indexes[0]->indisprimary);
}

// ===========================================================================
// Catalog creation: UNIQUE
// ===========================================================================

TEST_F(ConstraintTest, UniqueCreatesConstraintAndIndex) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 UNIQUE);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    ASSERT_NE(relid, kInvalidOid);

    auto cons = catalog_->GetConstraintsByRelid(relid);
    ASSERT_EQ(cons.size(), 1u);
    EXPECT_EQ(cons[0]->contype, ConstraintType::kUnique);

    auto indexes = catalog_->GetIndexesByRelid(relid);
    ASSERT_EQ(indexes.size(), 1u);
    EXPECT_TRUE(indexes[0]->indisunique);
    EXPECT_FALSE(indexes[0]->indisprimary);
}

TEST_F(ConstraintTest, TableLevelUniqueCreatesConstraint) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4, b int4, UNIQUE (a, b));"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    auto cons = catalog_->GetConstraintsByRelid(relid);
    ASSERT_EQ(cons.size(), 1u);
    EXPECT_EQ(cons[0]->contype, ConstraintType::kUnique);
    EXPECT_EQ(cons[0]->conkey.size(), 2u);
}

// ===========================================================================
// Catalog creation: FOREIGN KEY
// ===========================================================================

TEST_F(ConstraintTest, ForeignKeyCreatesConstraintEntry) {
    ASSERT_EQ(RunUtility("CREATE TABLE p (a int4 PRIMARY KEY);"), "CREATE TABLE");
    ASSERT_EQ(RunUtility("CREATE TABLE c (a int4 REFERENCES p);"), "CREATE TABLE");
    Oid c_relid = GetRelidByName("c");
    ASSERT_NE(c_relid, kInvalidOid);

    auto cons = catalog_->GetConstraintsByRelid(c_relid);
    ASSERT_EQ(cons.size(), 1u);
    EXPECT_EQ(cons[0]->contype, ConstraintType::kForeignKey);
    EXPECT_EQ(cons[0]->conkey.size(), 1u);
    EXPECT_EQ(cons[0]->conkey[0], 1);
    EXPECT_EQ(cons[0]->confkey.size(), 1u);
}

TEST_F(ConstraintTest, TableLevelForeignKeyCreatesConstraintEntry) {
    ASSERT_EQ(RunUtility("CREATE TABLE p (a int4 PRIMARY KEY);"), "CREATE TABLE");
    ASSERT_EQ(RunUtility("CREATE TABLE c (a int4, FOREIGN KEY (a) REFERENCES p (a));"),
              "CREATE TABLE");
    Oid c_relid = GetRelidByName("c");
    auto cons = catalog_->GetConstraintsByRelid(c_relid);
    ASSERT_EQ(cons.size(), 1u);
    EXPECT_EQ(cons[0]->contype, ConstraintType::kForeignKey);
}

// ===========================================================================
// Combined: multiple constraints on one table
// ===========================================================================

TEST_F(ConstraintTest, MultipleConstraintsOnSameTable) {
    ASSERT_EQ(RunUtility("CREATE TABLE t ("
                         "  a int4 PRIMARY KEY, "
                         "  b int4 NOT NULL DEFAULT 0, "
                         "  c int4 CHECK (c > 0), "
                         "  d int4 UNIQUE"
                         ");"),
              "CREATE TABLE");
    Oid relid = GetRelidByName("t");
    ASSERT_NE(relid, kInvalidOid);

    auto cons = catalog_->GetConstraintsByRelid(relid);
    // PK, CHECK, UNIQUE => 3 constraints
    EXPECT_EQ(cons.size(), 3u);

    auto attrdefs = catalog_->GetAttrdefsByRelid(relid);
    ASSERT_EQ(attrdefs.size(), 1u);
    EXPECT_EQ(attrdefs[0]->adnum, 2);  // column b

    auto attrs = catalog_->GetAttributes(relid);
    ASSERT_EQ(attrs.size(), 4u);
    EXPECT_TRUE(attrs[0]->attnotnull);  // PK implies NOT NULL
    EXPECT_TRUE(attrs[1]->attnotnull);  // explicit NOT NULL
    EXPECT_TRUE(attrs[1]->atthasdef);   // DEFAULT
}

// ===========================================================================
// Runtime enforcement: NOT NULL violation raises error
// ===========================================================================

TEST_F(ConstraintTest, InsertNotNullViolationRaisesError) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 NOT NULL);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");

    // INSERT a NULL value into a NOT NULL column → should ereport ERROR.
    auto* mt_plan = MakeInsertPlan(MakeNullRowPlan1(), 1);
    auto* qd = MakeInsertQueryDesc(relid, mt_plan);

    EXPECT_TRUE(RaisesError([&] { RunDml(qd); }));
}

// ===========================================================================
// Runtime enforcement: CHECK violation raises error
// ===========================================================================

TEST_F(ConstraintTest, InsertCheckViolationRaisesError) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 CHECK (a > 0));"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");

    // INSERT a row with a = -5, which violates CHECK (a > 0).
    auto* mt_plan = MakeInsertPlan(MakeResultRowPlan1(-5), 1);
    auto* qd = MakeInsertQueryDesc(relid, mt_plan);

    EXPECT_TRUE(RaisesError([&] { RunDml(qd); }));
}

TEST_F(ConstraintTest, InsertCheckPassingRowSucceeds) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 CHECK (a > 0));"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");

    auto* mt_plan = MakeInsertPlan(MakeResultRowPlan1(42), 1);
    auto* qd = MakeInsertQueryDesc(relid, mt_plan);

    EXPECT_FALSE(RaisesError([&] { RunDml(qd); }));

    CommitAndStartNew();
    auto rows = ScanFirstColumn(relid);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], 42);
}

// ===========================================================================
// Runtime enforcement: DEFAULT substitution
// ===========================================================================

TEST_F(ConstraintTest, InsertDefaultSubstitutesValue) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 DEFAULT 42);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");

    // INSERT a NULL value — ExecInsertDefault should substitute 42.
    auto* mt_plan = MakeInsertPlan(MakeNullRowPlan1(), 1);
    auto* qd = MakeInsertQueryDesc(relid, mt_plan);

    EXPECT_FALSE(RaisesError([&] { RunDml(qd); }));

    CommitAndStartNew();
    auto rows = ScanFirstColumn(relid);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], 42);
}

TEST_F(ConstraintTest, InsertExplicitValueOverridesDefault) {
    ASSERT_EQ(RunUtility("CREATE TABLE t (a int4 DEFAULT 42);"), "CREATE TABLE");
    Oid relid = GetRelidByName("t");

    // INSERT with a = 7 — default should NOT be applied.
    auto* mt_plan = MakeInsertPlan(MakeResultRowPlan1(7), 1);
    auto* qd = MakeInsertQueryDesc(relid, mt_plan);

    EXPECT_FALSE(RaisesError([&] { RunDml(qd); }));

    CommitAndStartNew();
    auto rows = ScanFirstColumn(relid);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], 7);
}
