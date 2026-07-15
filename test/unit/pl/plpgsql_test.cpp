// plpgsql_test.cpp — Tests for PL/pgSQL + PL language framework (P3-7).
//
// Covers:
//   1. PL handler registry (RegisterPlHandler/LookupPlHandler/LookupByName)
//   2. pg_language catalog operations (insert/get-by-oid/get-by-name/delete)
//   3. PL/pgSQL lexer (basic token classification)
//   4. PL/pgSQL parser (block/declarations/statements/expressions)
//   5. PL/pgSQL executor (assignment, arithmetic, IF, LOOP, WHILE, FOR, RETURN)
//   6. End-to-end function call via fmgr + PL handler dispatch
//   7. CREATE LANGUAGE / DROP LANGUAGE / DO command dispatch
#include "pl/plpgsql/plpgsql.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_language.hpp"
#include "catalog/pg_proc.hpp"
#include "catalog/pg_type.hpp"
#include "commands/languagecmds.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "pl/pl_handler.hpp"
#include "types/datum.hpp"
#include "utils/fmgr.hpp"

namespace {

using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_language;
using pgcpp::catalog::FormData_pg_proc;
using pgcpp::catalog::FormData_pg_type;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::ProKind;
using pgcpp::catalog::SetCatalog;
using pgcpp::commands::CreateLanguage;
using pgcpp::commands::DoBlock;
using pgcpp::commands::DropLanguage;
using pgcpp::error::InitErrorSubsystem;
using pgcpp::fmgr::fmgr_info;
using pgcpp::fmgr::FmgrInfo;
using pgcpp::fmgr::FunctionCall;
using pgcpp::fmgr::FunctionCallInfo;
using pgcpp::fmgr::kPlPgsqlLanguageOid;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::makeString;
using pgcpp::nodes::Node;
using pgcpp::parser::CreateLanguageStmt;
using pgcpp::parser::DoStmt;
using pgcpp::parser::DropLanguageStmt;
using pgcpp::pl::ClearPlHandlers;
using pgcpp::pl::LookupPlHandler;
using pgcpp::pl::LookupPlHandlerByName;
using pgcpp::pl::PlHandler;
using pgcpp::pl::plpgsql::RegisterPlPgsqlHandler;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kInt4Oid;

class PlPgsqlTest : public ::testing::Test {
protected:
    void SetUp() override {
        InitErrorSubsystem();
        context_ = AllocSetContext::Create("plpgsql_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        // Register int4 type for type resolution.
        auto* int4_type = makePallocNode<FormData_pg_type>();
        int4_type->oid = kInt4Oid;
        int4_type->typname = "integer";
        int4_type->typlen = 4;
        int4_type->typbyval = true;
        catalog_->InsertType(int4_type);

        // Reset the PL handler registry for test isolation.
        ClearPlHandlers();
        RegisterPlPgsqlHandler();
    }

    void TearDown() override {
        ClearPlHandlers();
        SetCatalog(nullptr);
        delete catalog_;

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: insert a PL/pgSQL function into pg_proc and return its OID.
    Oid InsertPlPgsqlFunction(const std::string& name, const std::string& body,
                              Oid rettype = kInt4Oid) {
        auto* proc = makePallocNode<FormData_pg_proc>();
        proc->proname = name;
        proc->prorettype = rettype;
        proc->prolang = kPlPgsqlLanguageOid;
        proc->prosrc = body;
        proc->prokind = ProKind::kFunction;
        proc->proisstrict = false;
        return catalog_->InsertProc(proc);
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
};

// ===========================================================================
// 1. PL handler registry
// ===========================================================================

TEST_F(PlPgsqlTest, HandlerRegistryLookupByOid) {
    const PlHandler* h = LookupPlHandler(kPlPgsqlLanguageOid);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->language_oid, kPlPgsqlLanguageOid);
    EXPECT_EQ(h->language_name, "plpgsql");
    EXPECT_NE(h->call_cb, nullptr);
    EXPECT_NE(h->inline_cb, nullptr);
}

TEST_F(PlPgsqlTest, HandlerRegistryLookupByName) {
    const PlHandler* h = LookupPlHandlerByName("plpgsql");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->language_oid, kPlPgsqlLanguageOid);
}

TEST_F(PlPgsqlTest, HandlerRegistryLookupUnknown) {
    EXPECT_EQ(LookupPlHandler(9999), nullptr);
    EXPECT_EQ(LookupPlHandlerByName("nosuchlang"), nullptr);
}

TEST_F(PlPgsqlTest, HandlerRegistryClear) {
    ClearPlHandlers();
    EXPECT_EQ(LookupPlHandler(kPlPgsqlLanguageOid), nullptr);
    // Re-register for subsequent tests.
    RegisterPlPgsqlHandler();
    EXPECT_NE(LookupPlHandler(kPlPgsqlLanguageOid), nullptr);
}

// ===========================================================================
// 2. pg_language catalog
// ===========================================================================

TEST_F(PlPgsqlTest, BootstrapLanguagesPresent) {
    // BootstrapCatalog registers 4 languages: internal, c, sql, plpgsql.
    EXPECT_NE(catalog_->GetLanguageByName("internal"), nullptr);
    EXPECT_NE(catalog_->GetLanguageByName("c"), nullptr);
    EXPECT_NE(catalog_->GetLanguageByName("sql"), nullptr);
    EXPECT_NE(catalog_->GetLanguageByName("plpgsql"), nullptr);
}

TEST_F(PlPgsqlTest, LanguageByOid) {
    const auto* lang = catalog_->GetLanguageByOid(kPlPgsqlLanguageOid);
    ASSERT_NE(lang, nullptr);
    EXPECT_EQ(lang->lanname, "plpgsql");
    EXPECT_TRUE(lang->lanpltrusted);
    EXPECT_TRUE(lang->lanispl);
}

TEST_F(PlPgsqlTest, LanguageInsertAndDelete) {
    auto* row = makePallocNode<FormData_pg_language>();
    row->lanname = "testlang";
    row->lanispl = true;
    row->lanpltrusted = false;
    Oid oid = catalog_->InsertLanguage(row);
    EXPECT_NE(oid, 0u);

    const auto* fetched = catalog_->GetLanguageByName("testlang");
    ASSERT_NE(fetched, nullptr);
    EXPECT_EQ(fetched->oid, oid);
    EXPECT_EQ(fetched->lanname, "testlang");

    EXPECT_TRUE(catalog_->DeleteLanguage(oid));
    EXPECT_EQ(catalog_->GetLanguageByName("testlang"), nullptr);
    EXPECT_EQ(catalog_->GetLanguageByOid(oid), nullptr);
}

TEST_F(PlPgsqlTest, LanguageDeleteNonexistent) {
    EXPECT_FALSE(catalog_->DeleteLanguage(9999));
}

// ===========================================================================
// 3. PL/pgSQL parser: ParsePlPgsql returns a non-null PlBlock on valid input
// ===========================================================================

TEST_F(PlPgsqlTest, ParseEmptyBlock) {
    using pgcpp::pl::plpgsql::ParsePlPgsql;
    auto* block = ParsePlPgsql("BEGIN END");
    ASSERT_NE(block, nullptr);
    EXPECT_TRUE(block->declarations.empty());
    EXPECT_TRUE(block->body.empty());
    delete block;
}

TEST_F(PlPgsqlTest, ParseDeclareAndAssignment) {
    using pgcpp::pl::plpgsql::ParsePlPgsql;
    auto* block = ParsePlPgsql(
        "DECLARE\n"
        "  x integer := 10;\n"
        "BEGIN\n"
        "  x := x + 5;\n"
        "END");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->declarations.size(), 1u);
    ASSERT_EQ(block->body.size(), 1u);
    delete block;
}

TEST_F(PlPgsqlTest, ParseIfElsifElse) {
    using pgcpp::pl::plpgsql::ParsePlPgsql;
    auto* block = ParsePlPgsql(
        "BEGIN\n"
        "  IF x > 0 THEN\n"
        "    x := 1;\n"
        "  ELSIF x < 0 THEN\n"
        "    x := -1;\n"
        "  ELSE\n"
        "    x := 0;\n"
        "  END IF;\n"
        "END");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->body.size(), 1u);
    delete block;
}

TEST_F(PlPgsqlTest, ParseLoopAndExit) {
    using pgcpp::pl::plpgsql::ParsePlPgsql;
    auto* block = ParsePlPgsql(
        "BEGIN\n"
        "  LOOP\n"
        "    EXIT WHEN x > 10;\n"
        "    x := x + 1;\n"
        "  END LOOP;\n"
        "END");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->body.size(), 1u);
    delete block;
}

TEST_F(PlPgsqlTest, ParseWhileLoop) {
    using pgcpp::pl::plpgsql::ParsePlPgsql;
    auto* block = ParsePlPgsql(
        "BEGIN\n"
        "  WHILE x < 10 LOOP\n"
        "    x := x + 1;\n"
        "  END LOOP;\n"
        "END");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->body.size(), 1u);
    delete block;
}

TEST_F(PlPgsqlTest, ParseForLoop) {
    using pgcpp::pl::plpgsql::ParsePlPgsql;
    auto* block = ParsePlPgsql(
        "BEGIN\n"
        "  FOR i IN 1..10 LOOP\n"
        "    x := x + i;\n"
        "  END LOOP;\n"
        "END");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->body.size(), 1u);
    delete block;
}

TEST_F(PlPgsqlTest, ParseForLoopReverse) {
    using pgcpp::pl::plpgsql::ParsePlPgsql;
    auto* block = ParsePlPgsql(
        "BEGIN\n"
        "  FOR i IN REVERSE 10..1 LOOP\n"
        "    x := x + i;\n"
        "  END LOOP;\n"
        "END");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->body.size(), 1u);
    delete block;
}

TEST_F(PlPgsqlTest, ParseReturnExpr) {
    using pgcpp::pl::plpgsql::ParsePlPgsql;
    auto* block = ParsePlPgsql(
        "BEGIN\n"
        "  RETURN x + 1;\n"
        "END");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->body.size(), 1u);
    delete block;
}

// ===========================================================================
// 4. End-to-end: PL/pgSQL function call via fmgr
// ===========================================================================

TEST_F(PlPgsqlTest, FunctionCallReturnConstant) {
    Oid oid = InsertPlPgsqlFunction("ret_const",
                                    "BEGIN\n"
                                    "  RETURN 42;\n"
                                    "END");

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(oid, &finfo));
    EXPECT_EQ(finfo.fn_language, kPlPgsqlLanguageOid);
    ASSERT_NE(finfo.fn_pl_handler, nullptr);

    Datum result = FunctionCall(&finfo, std::vector<Datum>{});
    EXPECT_EQ(DatumGetInt32(result), 42);
}

TEST_F(PlPgsqlTest, FunctionCallAssignmentAndReturn) {
    Oid oid = InsertPlPgsqlFunction("assign_ret",
                                    "DECLARE\n"
                                    "  x integer := 10;\n"
                                    "BEGIN\n"
                                    "  x := x + 5;\n"
                                    "  RETURN x;\n"
                                    "END");

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(oid, &finfo));

    Datum result = FunctionCall(&finfo, std::vector<Datum>{});
    EXPECT_EQ(DatumGetInt32(result), 15);
}

TEST_F(PlPgsqlTest, FunctionCallWithArg) {
    Oid oid = InsertPlPgsqlFunction("inc_arg",
                                    "BEGIN\n"
                                    "  RETURN $1 + 1;\n"
                                    "END");
    // Set the argument count on the proc row for fmgr strictness handling.
    auto procs = catalog_->GetProcsByName("inc_arg");
    ASSERT_FALSE(procs.empty());
    const_cast<FormData_pg_proc*>(procs[0])->pronargs = 1;
    const_cast<FormData_pg_proc*>(procs[0])->proargtypes = {kInt4Oid};
    const_cast<FormData_pg_proc*>(procs[0])->proisstrict = false;

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(oid, &finfo));

    Datum result = FunctionCall(&finfo, std::vector<Datum>{Int32GetDatum(41)});
    EXPECT_EQ(DatumGetInt32(result), 42);
}

TEST_F(PlPgsqlTest, FunctionCallIfElse) {
    Oid oid = InsertPlPgsqlFunction("sign_fn",
                                    "BEGIN\n"
                                    "  IF $1 > 0 THEN\n"
                                    "    RETURN 1;\n"
                                    "  ELSIF $1 < 0 THEN\n"
                                    "    RETURN -1;\n"
                                    "  ELSE\n"
                                    "    RETURN 0;\n"
                                    "  END IF;\n"
                                    "END");
    auto procs = catalog_->GetProcsByName("sign_fn");
    ASSERT_FALSE(procs.empty());
    const_cast<FormData_pg_proc*>(procs[0])->pronargs = 1;
    const_cast<FormData_pg_proc*>(procs[0])->proargtypes = {kInt4Oid};
    const_cast<FormData_pg_proc*>(procs[0])->proisstrict = false;

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(oid, &finfo));

    EXPECT_EQ(DatumGetInt32(FunctionCall(&finfo, std::vector<Datum>{Int32GetDatum(5)})), 1);
    EXPECT_EQ(DatumGetInt32(FunctionCall(&finfo, std::vector<Datum>{Int32GetDatum(-5)})), -1);
    EXPECT_EQ(DatumGetInt32(FunctionCall(&finfo, std::vector<Datum>{Int32GetDatum(0)})), 0);
}

TEST_F(PlPgsqlTest, FunctionCallForLoopSum) {
    Oid oid = InsertPlPgsqlFunction("sum_loop",
                                    "DECLARE\n"
                                    "  s integer := 0;\n"
                                    "BEGIN\n"
                                    "  FOR i IN 1..10 LOOP\n"
                                    "    s := s + i;\n"
                                    "  END LOOP;\n"
                                    "  RETURN s;\n"
                                    "END");

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(oid, &finfo));

    Datum result = FunctionCall(&finfo, std::vector<Datum>{});
    EXPECT_EQ(DatumGetInt32(result), 55);  // 1+2+...+10 = 55
}

TEST_F(PlPgsqlTest, FunctionCallWhileLoop) {
    Oid oid = InsertPlPgsqlFunction("count_down",
                                    "DECLARE\n"
                                    "  n integer := 5;\n"
                                    "  s integer := 0;\n"
                                    "BEGIN\n"
                                    "  WHILE n > 0 LOOP\n"
                                    "    s := s + n;\n"
                                    "    n := n - 1;\n"
                                    "  END LOOP;\n"
                                    "  RETURN s;\n"
                                    "END");

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(oid, &finfo));

    Datum result = FunctionCall(&finfo, std::vector<Datum>{});
    EXPECT_EQ(DatumGetInt32(result), 15);  // 5+4+3+2+1
}

TEST_F(PlPgsqlTest, FunctionCallExitWhen) {
    Oid oid = InsertPlPgsqlFunction("exit_when",
                                    "DECLARE\n"
                                    "  i integer := 0;\n"
                                    "BEGIN\n"
                                    "  LOOP\n"
                                    "    i := i + 1;\n"
                                    "    EXIT WHEN i >= 7;\n"
                                    "  END LOOP;\n"
                                    "  RETURN i;\n"
                                    "END");

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(oid, &finfo));

    Datum result = FunctionCall(&finfo, std::vector<Datum>{});
    EXPECT_EQ(DatumGetInt32(result), 7);
}

TEST_F(PlPgsqlTest, FunctionCallContinueWhen) {
    // Sum odd numbers 1..9: 1+3+5+7+9 = 25
    Oid oid = InsertPlPgsqlFunction("sum_odd",
                                    "DECLARE\n"
                                    "  s integer := 0;\n"
                                    "BEGIN\n"
                                    "  FOR i IN 1..9 LOOP\n"
                                    "    CONTINUE WHEN i % 2 = 0;\n"
                                    "    s := s + i;\n"
                                    "  END LOOP;\n"
                                    "  RETURN s;\n"
                                    "END");

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(oid, &finfo));

    Datum result = FunctionCall(&finfo, std::vector<Datum>{});
    EXPECT_EQ(DatumGetInt32(result), 25);
}

TEST_F(PlPgsqlTest, FunctionCallForReverseLoop) {
    // Sum 5..1 in reverse: 5+4+3+2+1 = 15
    Oid oid = InsertPlPgsqlFunction("sum_rev",
                                    "DECLARE\n"
                                    "  s integer := 0;\n"
                                    "BEGIN\n"
                                    "  FOR i IN REVERSE 5..1 LOOP\n"
                                    "    s := s + i;\n"
                                    "  END LOOP;\n"
                                    "  RETURN s;\n"
                                    "END");

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(oid, &finfo));

    Datum result = FunctionCall(&finfo, std::vector<Datum>{});
    EXPECT_EQ(DatumGetInt32(result), 15);
}

TEST_F(PlPgsqlTest, FunctionCallNestedBlock) {
    Oid oid = InsertPlPgsqlFunction("nested",
                                    "DECLARE\n"
                                    "  x integer := 0;\n"
                                    "BEGIN\n"
                                    "  x := 1;\n"
                                    "  BEGIN\n"
                                    "    x := x + 10;\n"
                                    "  END;\n"
                                    "  RETURN x;\n"
                                    "END");

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(oid, &finfo));

    Datum result = FunctionCall(&finfo, std::vector<Datum>{});
    EXPECT_EQ(DatumGetInt32(result), 11);
}

// ===========================================================================
// 5. CREATE LANGUAGE / DROP LANGUAGE / DO command dispatch
// ===========================================================================

TEST_F(PlPgsqlTest, CreateLanguageCommand) {
    auto* stmt = makePallocNode<CreateLanguageStmt>();
    stmt->lanname = "mypl";
    stmt->trusted = true;

    std::string tag = CreateLanguage(stmt);
    EXPECT_EQ(tag, "CREATE LANGUAGE");

    const auto* lang = catalog_->GetLanguageByName("mypl");
    ASSERT_NE(lang, nullptr);
    EXPECT_EQ(lang->lanname, "mypl");
    EXPECT_TRUE(lang->lanpltrusted);
    EXPECT_TRUE(lang->lanispl);
}

TEST_F(PlPgsqlTest, CreateLanguageDuplicateRejected) {
    auto* stmt = makePallocNode<CreateLanguageStmt>();
    stmt->lanname = "duplang";
    CreateLanguage(stmt);

    auto* stmt2 = makePallocNode<CreateLanguageStmt>();
    stmt2->lanname = "duplang";
    // The second CREATE LANGUAGE without REPLACE should ereport(ERROR).
    EXPECT_THROW({ CreateLanguage(stmt2); }, pgcpp::error::PgException);
}

TEST_F(PlPgsqlTest, CreateLanguageReplace) {
    auto* stmt = makePallocNode<CreateLanguageStmt>();
    stmt->lanname = "replacelang";
    stmt->trusted = false;
    CreateLanguage(stmt);

    auto* stmt2 = makePallocNode<CreateLanguageStmt>();
    stmt2->lanname = "replacelang";
    stmt2->replace = true;
    stmt2->trusted = true;
    std::string tag = CreateLanguage(stmt2);
    EXPECT_EQ(tag, "CREATE LANGUAGE");

    const auto* lang = catalog_->GetLanguageByName("replacelang");
    ASSERT_NE(lang, nullptr);
    EXPECT_TRUE(lang->lanpltrusted);
}

TEST_F(PlPgsqlTest, DropLanguageCommand) {
    auto* stmt = makePallocNode<CreateLanguageStmt>();
    stmt->lanname = "droppable";
    CreateLanguage(stmt);
    ASSERT_NE(catalog_->GetLanguageByName("droppable"), nullptr);

    auto* drop = makePallocNode<DropLanguageStmt>();
    drop->lanname = "droppable";
    std::string tag = DropLanguage(drop);
    EXPECT_EQ(tag, "DROP LANGUAGE");
    EXPECT_EQ(catalog_->GetLanguageByName("droppable"), nullptr);
}

TEST_F(PlPgsqlTest, DropLanguageMissingOk) {
    auto* drop = makePallocNode<DropLanguageStmt>();
    drop->lanname = "nonexistent";
    drop->missing_ok = true;
    std::string tag = DropLanguage(drop);
    EXPECT_EQ(tag, "DROP LANGUAGE");
}

TEST_F(PlPgsqlTest, DropLanguageMissingError) {
    auto* drop = makePallocNode<DropLanguageStmt>();
    drop->lanname = "alsnonexistent";
    drop->missing_ok = false;
    EXPECT_THROW({ DropLanguage(drop); }, pgcpp::error::PgException);
}

TEST_F(PlPgsqlTest, DoBlockExecutesInlineCode) {
    // A DO block with no RETURN: should execute without error.
    auto* stmt = makePallocNode<DoStmt>();
    stmt->lanname = "plpgsql";
    stmt->code =
        "BEGIN\n"
        "  DECLARE\n"
        "    x integer := 1;\n"
        "  BEGIN\n"
        "    x := x + 1;\n"
        "  END;\n"
        "END";
    std::string tag = DoBlock(stmt);
    EXPECT_EQ(tag, "DO");
}

TEST_F(PlPgsqlTest, DoBlockUnknownLanguageErrors) {
    auto* stmt = makePallocNode<DoStmt>();
    stmt->lanname = "nosuchlang";
    stmt->code = "BEGIN END";
    EXPECT_THROW({ DoBlock(stmt); }, pgcpp::error::PgException);
}

}  // namespace
