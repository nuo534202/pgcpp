// functioncmds_test.cpp — Tests for CREATE FUNCTION command.
//
// Verifies that CreateFunction parses DefElem options (LANGUAGE, AS,
// volatility, strict), builds a FormData_pg_proc row, and persists it
// in the catalog. Also tests fmgr_info lookup on user-created functions.
#include "commands/functioncmds.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_proc.hpp"
#include "catalog/pg_type.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "types/datum.hpp"
#include "utils/fmgr.hpp"

namespace {

using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_proc;
using pgcpp::catalog::FormData_pg_type;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::ProKind;
using pgcpp::catalog::ProVolatile;
using pgcpp::catalog::SetCatalog;
using pgcpp::commands::CreateFunction;
using pgcpp::error::InitErrorSubsystem;
using pgcpp::fmgr::fmgr_info;
using pgcpp::fmgr::FmgrInfo;
using pgcpp::fmgr::kCLanguageOid;
using pgcpp::fmgr::kSqlLanguageOid;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makeInteger;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::makeString;
using pgcpp::nodes::Node;
using pgcpp::parser::CreateFunctionStmt;
using pgcpp::parser::DefElem;
using pgcpp::parser::TypeName;
using pgcpp::types::kInt4Oid;

// Helper: create a String Value node (for funcname, type names, etc.).
Node* StrNode(const std::string& s) {
    return makeString(s);
}

// Helper: create a DefElem with a String arg.
DefElem* MakeStrDefElem(const std::string& name, const std::string& val) {
    auto* de = makePallocNode<DefElem>();
    de->defname = name;
    de->arg = makeString(val);
    return de;
}

// Helper: create a DefElem with an Integer arg.
DefElem* MakeIntDefElem(const std::string& name, int64_t val) {
    auto* de = makePallocNode<DefElem>();
    de->defname = name;
    de->arg = makeInteger(val);
    return de;
}

// Helper: create a TypeName for a simple unqualified type name.
TypeName* MakeTypeName(const std::string& name) {
    auto* tn = makePallocNode<TypeName>();
    tn->names.push_back(makeString(name));
    return tn;
}

class FunctionCmdsTest : public ::testing::Test {
protected:
    void SetUp() override {
        InitErrorSubsystem();
        context_ = AllocSetContext::Create("functioncmds_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);

        // Register the int4 type so ResolveTypeOid can find it by name.
        // The bootstrap catalog does not yet populate pg_type.
        auto* int4_type = makePallocNode<FormData_pg_type>();
        int4_type->oid = kInt4Oid;
        int4_type->typname = "integer";
        int4_type->typlen = 4;
        int4_type->typbyval = true;
        catalog_->InsertType(int4_type);
    }

    void TearDown() override {
        SetCatalog(nullptr);
        delete catalog_;

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
};

// --- Basic CREATE FUNCTION ---

TEST_F(FunctionCmdsTest, CreateSqlFunction) {
    auto* stmt = makePallocNode<CreateFunctionStmt>();
    stmt->is_procedure = false;
    stmt->funcname.push_back(StrNode("my_sql_func"));
    stmt->return_type = MakeTypeName("integer");
    stmt->parameters.push_back(MakeTypeName("integer"));
    stmt->options.push_back(MakeStrDefElem("language", "sql"));
    stmt->options.push_back(MakeStrDefElem("as", "SELECT $1 + 1"));

    std::string tag = CreateFunction(stmt);
    EXPECT_EQ(tag, "CREATE FUNCTION");

    // Verify the pg_proc row was inserted.
    auto procs = GetCatalog()->GetProcsByName("my_sql_func");
    ASSERT_EQ(procs.size(), 1u);
    EXPECT_EQ(procs[0]->proname, "my_sql_func");
    EXPECT_EQ(procs[0]->prolang, kSqlLanguageOid);
    EXPECT_EQ(procs[0]->prosrc, "SELECT $1 + 1");
    EXPECT_EQ(procs[0]->prorettype, kInt4Oid);
    EXPECT_EQ(procs[0]->pronargs, 1);
    EXPECT_EQ(procs[0]->proargtypes.size(), 1u);
    EXPECT_EQ(procs[0]->proargtypes[0], kInt4Oid);
    EXPECT_EQ(procs[0]->prokind, ProKind::kFunction);
}

TEST_F(FunctionCmdsTest, CreateCFunction) {
    auto* stmt = makePallocNode<CreateFunctionStmt>();
    stmt->funcname.push_back(StrNode("my_c_func"));
    stmt->return_type = MakeTypeName("integer");
    stmt->parameters.push_back(MakeTypeName("integer"));
    stmt->options.push_back(MakeStrDefElem("language", "c"));
    stmt->options.push_back(MakeStrDefElem("as", "abs"));

    std::string tag = CreateFunction(stmt);
    EXPECT_EQ(tag, "CREATE FUNCTION");

    auto procs = GetCatalog()->GetProcsByName("my_c_func");
    ASSERT_EQ(procs.size(), 1u);
    EXPECT_EQ(procs[0]->prolang, kCLanguageOid);
    EXPECT_EQ(procs[0]->prosrc, "abs");

    // fmgr_info should resolve the C function pointer via prosrc lookup.
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(procs[0]->oid, &finfo));
    EXPECT_EQ(finfo.fn_name, "my_c_func");
    EXPECT_EQ(finfo.fn_language, kCLanguageOid);
    EXPECT_TRUE(finfo.fn_addr != nullptr);
}

TEST_F(FunctionCmdsTest, CreateProcedure) {
    auto* stmt = makePallocNode<CreateFunctionStmt>();
    stmt->is_procedure = true;
    stmt->funcname.push_back(StrNode("my_proc"));
    stmt->options.push_back(MakeStrDefElem("language", "sql"));
    stmt->options.push_back(MakeStrDefElem("as", "SELECT 1"));

    std::string tag = CreateFunction(stmt);
    EXPECT_EQ(tag, "CREATE PROCEDURE");

    auto procs = GetCatalog()->GetProcsByName("my_proc");
    ASSERT_EQ(procs.size(), 1u);
    EXPECT_EQ(procs[0]->prokind, ProKind::kProcedure);
}

// --- Option parsing ---

TEST_F(FunctionCmdsTest, VolatilityOption) {
    auto* stmt = makePallocNode<CreateFunctionStmt>();
    stmt->funcname.push_back(StrNode("vol_test"));
    stmt->return_type = MakeTypeName("integer");
    stmt->options.push_back(MakeStrDefElem("language", "sql"));
    stmt->options.push_back(MakeStrDefElem("as", "SELECT 1"));
    stmt->options.push_back(MakeStrDefElem("volatility", "stable"));

    CreateFunction(stmt);

    auto procs = GetCatalog()->GetProcsByName("vol_test");
    ASSERT_EQ(procs.size(), 1u);
    EXPECT_EQ(procs[0]->provolatile, ProVolatile::kStable);
}

TEST_F(FunctionCmdsTest, StrictOption) {
    auto* stmt = makePallocNode<CreateFunctionStmt>();
    stmt->funcname.push_back(StrNode("strict_test"));
    stmt->return_type = MakeTypeName("integer");
    stmt->options.push_back(MakeStrDefElem("language", "sql"));
    stmt->options.push_back(MakeStrDefElem("as", "SELECT 1"));

    // STRICT is an Integer DefElem with value 1.
    stmt->options.push_back(MakeIntDefElem("strict", 1));

    CreateFunction(stmt);

    auto procs = GetCatalog()->GetProcsByName("strict_test");
    ASSERT_EQ(procs.size(), 1u);
    EXPECT_TRUE(procs[0]->proisstrict);
}

TEST_F(FunctionCmdsTest, DefaultVolatilityIsVolatile) {
    auto* stmt = makePallocNode<CreateFunctionStmt>();
    stmt->funcname.push_back(StrNode("default_vol"));
    stmt->return_type = MakeTypeName("integer");
    stmt->options.push_back(MakeStrDefElem("language", "sql"));
    stmt->options.push_back(MakeStrDefElem("as", "SELECT 1"));

    CreateFunction(stmt);

    auto procs = GetCatalog()->GetProcsByName("default_vol");
    ASSERT_EQ(procs.size(), 1u);
    EXPECT_EQ(procs[0]->provolatile, ProVolatile::kVolatile);
}

// --- fmgr_info on user-created function ---

TEST_F(FunctionCmdsTest, FmgrInfoOnUserCreatedSqlFunction) {
    auto* stmt = makePallocNode<CreateFunctionStmt>();
    stmt->funcname.push_back(StrNode("user_sql_fn"));
    stmt->return_type = MakeTypeName("integer");
    stmt->options.push_back(MakeStrDefElem("language", "sql"));
    stmt->options.push_back(MakeStrDefElem("as", "SELECT 42"));

    CreateFunction(stmt);

    auto procs = GetCatalog()->GetProcsByName("user_sql_fn");
    ASSERT_EQ(procs.size(), 1u);

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(procs[0]->oid, &finfo));
    EXPECT_EQ(finfo.fn_name, "user_sql_fn");
    EXPECT_EQ(finfo.fn_language, kSqlLanguageOid);
    EXPECT_EQ(finfo.fn_addr, nullptr);  // SQL functions have no C handler
    EXPECT_FALSE(finfo.has_handler());
}

TEST_F(FunctionCmdsTest, FmgrInfoOnUserCreatedCFunction) {
    auto* stmt = makePallocNode<CreateFunctionStmt>();
    stmt->funcname.push_back(StrNode("user_c_fn"));
    stmt->return_type = MakeTypeName("integer");
    stmt->parameters.push_back(MakeTypeName("integer"));
    stmt->options.push_back(MakeStrDefElem("language", "c"));
    stmt->options.push_back(MakeStrDefElem("as", "abs"));

    CreateFunction(stmt);

    auto procs = GetCatalog()->GetProcsByName("user_c_fn");
    ASSERT_EQ(procs.size(), 1u);

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(procs[0]->oid, &finfo));
    EXPECT_EQ(finfo.fn_language, kCLanguageOid);
    // "abs" is in the builtin table → fn_addr should be resolved.
    EXPECT_TRUE(finfo.fn_addr != nullptr);
    EXPECT_TRUE(finfo.has_handler());
}

// --- Null stmt ---

TEST_F(FunctionCmdsTest, NullStmtReturnsCreateFunction) {
    std::string tag = CreateFunction(nullptr);
    EXPECT_EQ(tag, "CREATE FUNCTION");
}

}  // namespace
