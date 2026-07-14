// foreign_test.cpp — Unit tests for the FDW framework (P3-5).
//
// Tests three layers of the Foreign Data Wrapper implementation:
//   1. FDW catalog CRUD (foreign servers, user mappings, foreign tables)
//   2. FDW handler registry (RegisterFdw / LookupFdw / file_fdw registration)
//   3. file_fdw end-to-end executor tests (ForeignScan node driving CSV reads)

#include "foreign/foreign.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
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
#include "foreign/fdwapi.hpp"
#include "foreign/file_fdw.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/builtins.hpp"
#include "types/datum.hpp"

using pgcpp::access::InitializeRelcache;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::kFirstNormalObjectId;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::error::PgException;
using pgcpp::executor::ExecutorEnd;
using pgcpp::executor::ExecutorFinish;
using pgcpp::executor::ExecutorRun;
using pgcpp::executor::ExecutorStart;
using pgcpp::executor::ForeignScan;
using pgcpp::executor::QueryDesc;
using pgcpp::executor::TupleTableSlot;
using pgcpp::foreign::ClearFdwRegistry;
using pgcpp::foreign::CreateForeignServer;
using pgcpp::foreign::CreateForeignTable;
using pgcpp::foreign::CreateUserMapping;
using pgcpp::foreign::DropForeignServer;
using pgcpp::foreign::DropForeignTable;
using pgcpp::foreign::DropUserMapping;
using pgcpp::foreign::FdwOption;
using pgcpp::foreign::FdwRoutine;
using pgcpp::foreign::GetOption;
using pgcpp::foreign::LookupFdw;
using pgcpp::foreign::LookupForeignServerByName;
using pgcpp::foreign::LookupForeignServerByOid;
using pgcpp::foreign::LookupForeignTable;
using pgcpp::foreign::LookupUserMapping;
using pgcpp::foreign::NumForeignServers;
using pgcpp::foreign::NumForeignTables;
using pgcpp::foreign::NumRegisteredFdws;
using pgcpp::foreign::NumUserMappings;
using pgcpp::foreign::RegisterFdw;
using pgcpp::foreign::RegisterFileFdw;
using pgcpp::foreign::ResetForeignCatalog;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
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
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::TextDatumToString;

namespace {

// Operator OID for int4 equality (from bootstrap_catalog.cpp).
constexpr Oid kInt4EqOp = 96;

// ===========================================================================
// Part 1+2: FDW Catalog CRUD + Handler Registry Tests
// ===========================================================================
//
// These tests use a minimal fixture: error subsystem + memory context (for
// ereport/palloc) plus ResetForeignCatalog/ClearFdwRegistry in SetUp/TearDown.

class ForeignCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("foreign_catalog_test");
        pgcpp::memory::SetCurrentMemoryContext(context_);
        ResetForeignCatalog();
        ClearFdwRegistry();
    }

    void TearDown() override {
        ResetForeignCatalog();
        ClearFdwRegistry();
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
};

// --- Foreign Server ---

TEST_F(ForeignCatalogTest, CreateAndLookupServer) {
    Oid sid = CreateForeignServer("fs1", "file_fdw");
    EXPECT_NE(sid, kInvalidOid);

    const auto* s = LookupForeignServerByName("fs1");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->servername, "fs1");
    EXPECT_EQ(s->fdwname, "file_fdw");
    EXPECT_EQ(s->serverid, sid);

    EXPECT_EQ(LookupForeignServerByOid(sid), s);
    EXPECT_EQ(LookupForeignServerByName("nonexistent"), nullptr);
    EXPECT_EQ(LookupForeignServerByOid(99999), nullptr);
}

TEST_F(ForeignCatalogTest, CreateServerWithOptions) {
    std::vector<FdwOption> opts = {{"host", "localhost"}, {"port", "5432"}};
    CreateForeignServer("fs_opts", "postgres_fdw", 10, opts);
    const auto* s = LookupForeignServerByName("fs_opts");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->owner, Oid(10));
    EXPECT_EQ(s->options.size(), 2u);
    EXPECT_EQ(s->options[0].optname, "host");
    EXPECT_EQ(s->options[0].optvalue, "localhost");
}

TEST_F(ForeignCatalogTest, DuplicateServerThrows) {
    CreateForeignServer("dup", "file_fdw");
    EXPECT_THROW(CreateForeignServer("dup", "file_fdw"), PgException);
    EXPECT_EQ(NumForeignServers(), 1u);
}

TEST_F(ForeignCatalogTest, DropServer) {
    Oid sid = CreateForeignServer("todrop", "file_fdw");
    EXPECT_EQ(NumForeignServers(), 1u);
    EXPECT_TRUE(DropForeignServer(sid));
    EXPECT_EQ(NumForeignServers(), 0u);
    EXPECT_EQ(LookupForeignServerByOid(sid), nullptr);
}

TEST_F(ForeignCatalogTest, DropNonexistentServer) {
    EXPECT_FALSE(DropForeignServer(99999));
}

TEST_F(ForeignCatalogTest, DropServerCascadesToMappingsAndTables) {
    Oid sid = CreateForeignServer("cascade_srv", "file_fdw");
    Oid umid = CreateUserMapping(sid, 10);
    Oid ftrelid = 5000;
    CreateForeignTable(ftrelid, sid, {{"filename", "/tmp/x.csv"}});

    EXPECT_EQ(NumForeignServers(), 1u);
    EXPECT_EQ(NumUserMappings(), 1u);
    EXPECT_EQ(NumForeignTables(), 1u);

    EXPECT_TRUE(DropForeignServer(sid));

    EXPECT_EQ(NumForeignServers(), 0u);
    EXPECT_EQ(NumUserMappings(), 0u);
    EXPECT_EQ(NumForeignTables(), 0u);
    EXPECT_EQ(LookupUserMapping(sid, 10), nullptr);
    EXPECT_EQ(LookupForeignTable(ftrelid), nullptr);
    (void)umid;
}

// --- User Mapping ---

TEST_F(ForeignCatalogTest, CreateAndLookupUserMapping) {
    Oid sid = CreateForeignServer("fs_um", "file_fdw");
    Oid umid = CreateUserMapping(sid, 42, {{"username", "alice"}});

    const auto* m = LookupUserMapping(sid, 42);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->umid, umid);
    EXPECT_EQ(m->serverid, sid);
    EXPECT_EQ(m->userid, Oid(42));
    EXPECT_EQ(m->options.size(), 1u);
    EXPECT_EQ(m->options[0].optvalue, "alice");
}

TEST_F(ForeignCatalogTest, DuplicateUserMappingThrows) {
    Oid sid = CreateForeignServer("fs_dup_um", "file_fdw");
    CreateUserMapping(sid, 1);
    EXPECT_THROW(CreateUserMapping(sid, 1), PgException);
    EXPECT_EQ(NumUserMappings(), 1u);
}

TEST_F(ForeignCatalogTest, CreateUserMappingInvalidServerThrows) {
    EXPECT_THROW(CreateUserMapping(99999, 1), PgException);
    EXPECT_EQ(NumUserMappings(), 0u);
}

TEST_F(ForeignCatalogTest, DropUserMapping) {
    Oid sid = CreateForeignServer("fs_drop_um", "file_fdw");
    Oid umid = CreateUserMapping(sid, 7);
    EXPECT_TRUE(DropUserMapping(umid));
    EXPECT_EQ(LookupUserMapping(sid, 7), nullptr);
    EXPECT_FALSE(DropUserMapping(umid));
}

// --- Foreign Table ---

TEST_F(ForeignCatalogTest, CreateAndLookupForeignTable) {
    Oid sid = CreateForeignServer("fs_ft", "file_fdw");
    Oid relid = 6000;
    CreateForeignTable(relid, sid, {{"filename", "/tmp/data.csv"}});

    const auto* ft = LookupForeignTable(relid);
    ASSERT_NE(ft, nullptr);
    EXPECT_EQ(ft->relid, relid);
    EXPECT_EQ(ft->serverid, sid);
    EXPECT_EQ(ft->options.size(), 1u);
    EXPECT_EQ(ft->options[0].optname, "filename");
    EXPECT_EQ(ft->options[0].optvalue, "/tmp/data.csv");
}

TEST_F(ForeignCatalogTest, DuplicateForeignTableThrows) {
    Oid sid = CreateForeignServer("fs_dup_ft", "file_fdw");
    Oid relid = 7000;
    CreateForeignTable(relid, sid);
    EXPECT_THROW(CreateForeignTable(relid, sid), PgException);
    EXPECT_EQ(NumForeignTables(), 1u);
}

TEST_F(ForeignCatalogTest, CreateForeignTableInvalidServerThrows) {
    EXPECT_THROW(CreateForeignTable(8000, 99999), PgException);
    EXPECT_EQ(NumForeignTables(), 0u);
}

TEST_F(ForeignCatalogTest, DropForeignTable) {
    Oid sid = CreateForeignServer("fs_drop_ft", "file_fdw");
    Oid relid = 9000;
    CreateForeignTable(relid, sid);
    EXPECT_TRUE(DropForeignTable(relid));
    EXPECT_EQ(LookupForeignTable(relid), nullptr);
    EXPECT_FALSE(DropForeignTable(relid));
}

// --- GetOption helper ---

TEST_F(ForeignCatalogTest, GetOptionHelper) {
    std::vector<FdwOption> opts = {{"a", "1"}, {"b", "2"}, {"c", "3"}};
    const auto* val = GetOption(opts, "b");
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(*val, "2");
    EXPECT_EQ(GetOption(opts, "z"), nullptr);
}

// --- ResetForeignCatalog ---

TEST_F(ForeignCatalogTest, ResetClearsAll) {
    Oid sid = CreateForeignServer("fs_reset", "file_fdw");
    CreateUserMapping(sid, 1);
    CreateForeignTable(10000, sid);
    EXPECT_EQ(NumForeignServers(), 1u);
    EXPECT_EQ(NumUserMappings(), 1u);
    EXPECT_EQ(NumForeignTables(), 1u);

    ResetForeignCatalog();
    EXPECT_EQ(NumForeignServers(), 0u);
    EXPECT_EQ(NumUserMappings(), 0u);
    EXPECT_EQ(NumForeignTables(), 0u);

    // OID counter should restart from kFirstNormalObjectId.
    Oid sid2 = CreateForeignServer("after_reset", "file_fdw");
    EXPECT_EQ(sid2, kFirstNormalObjectId);
}

// --- FDW Handler Registry ---

namespace {

// Dummy FdwRoutine for registry tests.
const FdwRoutine kDummyRoutine = {nullptr, nullptr, nullptr, nullptr};
const FdwRoutine* DummyFactory() {
    return &kDummyRoutine;
}

}  // namespace

TEST_F(ForeignCatalogTest, RegisterAndLookupFdw) {
    RegisterFdw("dummy_fdw", &DummyFactory);
    EXPECT_EQ(NumRegisteredFdws(), 1u);

    const FdwRoutine* r = LookupFdw("dummy_fdw");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r, &kDummyRoutine);

    EXPECT_EQ(LookupFdw("unregistered"), nullptr);
}

TEST_F(ForeignCatalogTest, DuplicateFdwRegistrationThrows) {
    RegisterFdw("dup_fdw", &DummyFactory);
    EXPECT_THROW(RegisterFdw("dup_fdw", &DummyFactory), PgException);
    EXPECT_EQ(NumRegisteredFdws(), 1u);
}

TEST_F(ForeignCatalogTest, ClearFdwRegistry) {
    RegisterFdw("a", &DummyFactory);
    RegisterFdw("b", &DummyFactory);
    EXPECT_EQ(NumRegisteredFdws(), 2u);

    ClearFdwRegistry();
    EXPECT_EQ(NumRegisteredFdws(), 0u);
    EXPECT_EQ(LookupFdw("a"), nullptr);
}

TEST_F(ForeignCatalogTest, RegisterFileFdw) {
    RegisterFileFdw();
    EXPECT_EQ(NumRegisteredFdws(), 1u);

    const FdwRoutine* r = LookupFdw("file_fdw");
    ASSERT_NE(r, nullptr);
    EXPECT_NE(r->BeginForeignScan, nullptr);
    EXPECT_NE(r->IterateForeignScan, nullptr);
    EXPECT_NE(r->ReScanForeignScan, nullptr);
    EXPECT_NE(r->EndForeignScan, nullptr);
}

// ===========================================================================
// Part 3: file_fdw End-to-End Executor Tests
// ===========================================================================
//
// These tests set up the full executor stack (matching ExecutorTest) and drive
// a ForeignScan plan node through the executor interface.

class FileFdwExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("file_fdw_test");
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

        test_dir_ = "/tmp/pgcpp_fdw_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();

        // FDW-specific setup.
        ResetForeignCatalog();
        ClearFdwRegistry();
        RegisterFileFdw();
    }

    void TearDown() override {
        ResetForeignCatalog();
        ClearFdwRegistry();

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

    // Helper: run a shell command (ignores exit code).
    static void RunShell(const std::string& cmd) {
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

    // Helper: create a temp file with the given content.
    static std::string MakeTempFile(const std::string& suffix, const std::string& content) {
        std::string path = "/tmp/pgcpp_fdw_test_data_" + std::to_string(getpid()) + "_" + suffix;
        std::ofstream out(path);
        out << content;
        out.flush();
        out.close();
        return path;
    }

    // Helper: build a pg_class row for a foreign table.
    FormData_pg_class* MakeForeignClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kForeignTable;
        row->relpersistence = RelPersistence::kPermanent;
        return row;
    }

    // Helper: build a 2-column int4 schema (a, b).
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

    // Helper: build a (int4, text) schema.
    std::vector<FormData_pg_attribute> MakeIntTextSchema(Oid relid) {
        FormData_pg_attribute a1;
        a1.attrelid = relid;
        a1.attname = "id";
        a1.attnum = 1;
        a1.atttypid = kInt4Oid;
        a1.attlen = 4;
        a1.attbyval = true;
        a1.attalign = AttAlign::kInt;
        a1.attstorage = AttStorage::kPlain;

        FormData_pg_attribute a2;
        a2.attrelid = relid;
        a2.attname = "name";
        a2.attnum = 2;
        a2.atttypid = kTextOid;
        a2.attlen = -1;
        a2.attbyval = false;
        a2.attalign = AttAlign::kDouble;
        a2.attstorage = AttStorage::kPlain;

        return {a1, a2};
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

    // Helper: create a RangeTblEntry.
    RangeTblEntry* MakeRTE(Oid relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = static_cast<int>(relid);
        return rte;
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

    // Helper: register a foreign table in both the main catalog (pg_class +
    // pg_attribute with relkind='f') and the FDW catalog (with filename
    // option). Does NOT call RelationCreateStorage (foreign tables have no
    // heap file).
    void SetupForeignTable(Oid relid, const std::string& tablename,
                           const std::vector<FormData_pg_attribute>& attrs,
                           const std::string& filename) {
        // pg_class row with relkind = 'f'.
        catalog_->InsertClass(MakeForeignClassRow(tablename, relid));
        // pg_attribute rows.
        for (const auto& attr : attrs) {
            auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
            catalog_->InsertAttribute(attr_row);
        }
        // FDW catalog: foreign server + foreign table.
        Oid sid = CreateForeignServer("fs_" + tablename, "file_fdw");
        CreateForeignTable(relid, sid, {{"filename", filename}});
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// --- Basic file_fdw scan (int4, int4) ---

TEST_F(FileFdwExecutorTest, SimpleScan) {
    // Create a CSV file with 3 rows.
    std::string csv = MakeTempFile("simple.csv", "1,100\n2,200\n3,300\n");

    // Set up foreign table.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    SetupForeignTable(relid, "ft1", attrs, csv);

    // Build a ForeignScan plan.
    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    // Build a QueryDesc.
    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

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

    RunShell("rm -f " + csv);
}

// --- Scan with qual filter (WHERE a = 2) ---

TEST_F(FileFdwExecutorTest, ScanWithQual) {
    std::string csv = MakeTempFile("qual.csv", "1,10\n2,20\n3,30\n4,40\n5,50\n");

    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    SetupForeignTable(relid, "ft_qual", attrs, csv);

    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));
    // WHERE a = 2
    fsplan->qual = MakeOpExpr(kInt4EqOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(2));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

    ExecutorStart(qd);

    std::vector<std::pair<int, int>> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 2);
    EXPECT_EQ(results[0].second, 20);

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    RunShell("rm -f " + csv);
}

// --- ReScan (rewind and read again) ---

TEST_F(FileFdwExecutorTest, ReScan) {
    std::string csv = MakeTempFile("rescan.csv", "10,11\n20,21\n");

    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    SetupForeignTable(relid, "ft_rescan", attrs, csv);

    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

    ExecutorStart(qd);

    // Read first tuple.
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 10);

    // ReScan — rewind to beginning.
    qd->planstate->ExecReScan();

    // Read all tuples again.
    std::vector<std::pair<int, int>> results;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.emplace_back(DatumGetInt32(slot->tts_values[0]),
                             DatumGetInt32(slot->tts_values[1]));
    }
    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].first, 10);
    EXPECT_EQ(results[1].first, 20);

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    RunShell("rm -f " + csv);
}

// --- Empty file ---

TEST_F(FileFdwExecutorTest, EmptyFile) {
    std::string csv = MakeTempFile("empty.csv", "");

    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    SetupForeignTable(relid, "ft_empty", attrs, csv);

    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

    ExecutorStart(qd);

    int count = 0;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        count++;
    }
    EXPECT_EQ(count, 0);

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    RunShell("rm -f " + csv);
}

// --- NULL fields (empty CSV fields → NULL values) ---

TEST_F(FileFdwExecutorTest, NullFields) {
    // Second row has empty fields for both columns → NULL.
    std::string csv = MakeTempFile("nulls.csv", "1,100\n,\n3,300\n");

    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    SetupForeignTable(relid, "ft_nulls", attrs, csv);

    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

    ExecutorStart(qd);

    // Row 1: (1, 100)
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_FALSE(slot->tts_isnull[1]);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 1);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[1]), 100);

    // Row 2: (NULL, NULL)
    slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_TRUE(slot->tts_isnull[0]);
    EXPECT_TRUE(slot->tts_isnull[1]);

    // Row 3: (3, 300)
    slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 3);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[1]), 300);

    // EOF
    slot = ExecutorRun(qd);
    EXPECT_EQ(slot, nullptr);

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    RunShell("rm -f " + csv);
}

// --- Text columns ---

TEST_F(FileFdwExecutorTest, TextColumns) {
    std::string csv = MakeTempFile("text.csv", "1,alice\n2,bob\n3,charlie\n");

    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntTextSchema(relid);
    SetupForeignTable(relid, "ft_text", attrs, csv);

    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "id"));
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kTextOid), 2, "name"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

    ExecutorStart(qd);

    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 1);
    EXPECT_EQ(TextDatumToString(slot->tts_values[1]), "alice");

    slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 2);
    EXPECT_EQ(TextDatumToString(slot->tts_values[1]), "bob");

    slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 3);
    EXPECT_EQ(TextDatumToString(slot->tts_values[1]), "charlie");

    slot = ExecutorRun(qd);
    EXPECT_EQ(slot, nullptr);

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    RunShell("rm -f " + csv);
}

// --- Missing filename option throws ---

TEST_F(FileFdwExecutorTest, MissingFilenameOptionThrows) {
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);

    // Set up foreign table WITHOUT filename option.
    catalog_->InsertClass(MakeForeignClassRow("ft_nofile", relid));
    for (const auto& attr : attrs) {
        auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
        catalog_->InsertAttribute(attr_row);
    }
    Oid sid = CreateForeignServer("fs_nofile", "file_fdw");
    CreateForeignTable(relid, sid);  // no options

    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

    // ExecutorStart calls ExecInit → BeginForeignScan → ereport(ERROR).
    EXPECT_THROW(ExecutorStart(qd), PgException);

    // ExecutorEnd is safe to call even if Start failed.
    ExecutorEnd(qd);
}

// --- Foreign table not found in FDW catalog throws ---

TEST_F(FileFdwExecutorTest, ForeignTableNotFoundThrows) {
    Oid relid = kFirstNormalObjectId;

    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;  // not registered in FDW catalog
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

    EXPECT_THROW(ExecutorStart(qd), PgException);
    ExecutorEnd(qd);
}

// --- Projection: SELECT a only (subset of columns) ---

TEST_F(FileFdwExecutorTest, ProjectionSubset) {
    std::string csv = MakeTempFile("proj.csv", "1,111\n2,222\n");

    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    SetupForeignTable(relid, "ft_proj", attrs, csv);

    // Target list: only column "a" (skip "b").
    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

    ExecutorStart(qd);

    std::vector<int> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        EXPECT_EQ(slot->Natts(), 1);  // result has 1 column
        results.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0], 1);
    EXPECT_EQ(results[1], 2);

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    RunShell("rm -f " + csv);
}

// --- Single-column file ---

TEST_F(FileFdwExecutorTest, SingleColumnFile) {
    std::string csv = MakeTempFile("single.csv", "42\n99\n");

    Oid relid = kFirstNormalObjectId;

    // Single int4 column.
    FormData_pg_attribute a1;
    a1.attrelid = relid;
    a1.attname = "x";
    a1.attnum = 1;
    a1.atttypid = kInt4Oid;
    a1.attlen = 4;
    a1.attbyval = true;
    a1.attalign = AttAlign::kInt;
    a1.attstorage = AttStorage::kPlain;

    SetupForeignTable(relid, "ft_single", {a1}, csv);

    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "x"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

    ExecutorStart(qd);

    std::vector<int> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(qd)) != nullptr) {
        results.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0], 42);
    EXPECT_EQ(results[1], 99);

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    RunShell("rm -f " + csv);
}

// --- File with fewer fields than columns (missing fields → NULL) ---

TEST_F(FileFdwExecutorTest, FewerFieldsThanColumns) {
    // Row 2 has only 1 field (missing column b → NULL).
    std::string csv = MakeTempFile("few.csv", "1,100\n2\n3,300\n");

    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    SetupForeignTable(relid, "ft_few", attrs, csv);

    auto* fsplan = makePallocNode<ForeignScan>();
    fsplan->fs_relid = relid;
    fsplan->scanrelid = 1;
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    fsplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* rte = MakeRTE(relid);
    auto* query = MakeSelectQuery({rte});
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = fsplan;

    ExecutorStart(qd);

    // Row 1: (1, 100)
    TupleTableSlot* slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 1);
    EXPECT_FALSE(slot->tts_isnull[1]);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[1]), 100);

    // Row 2: (2, NULL) — missing field → NULL
    slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 2);
    EXPECT_TRUE(slot->tts_isnull[1]);

    // Row 3: (3, 300)
    slot = ExecutorRun(qd);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 3);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[1]), 300);

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    RunShell("rm -f " + csv);
}

}  // namespace
