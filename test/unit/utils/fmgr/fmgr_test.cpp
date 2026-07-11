// fmgr_test.cpp — Tests for the function manager (fmgr).
//
// Verifies fmgr_info lookup, FunctionCall dispatch, strict-NULL
// short-circuiting, Direct/OidFunctionCall wrappers, and the builtin
// function table.
#include "utils/fmgr.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_proc.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "types/builtins.hpp"
#include "types/datum.hpp"
#include "types/string_funcs.hpp"

namespace {

using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_proc;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::SetCatalog;
using pgcpp::error::InitErrorSubsystem;
using pgcpp::fmgr::DirectFunctionCall1;
using pgcpp::fmgr::DirectFunctionCall2;
using pgcpp::fmgr::fmgr_info;
using pgcpp::fmgr::FmgrInfo;
using pgcpp::fmgr::FunctionCall;
using pgcpp::fmgr::FunctionCallInfo;
using pgcpp::fmgr::FunctionCallWithNulls;
using pgcpp::fmgr::kInternalLanguageOid;
using pgcpp::fmgr::kSqlLanguageOid;
using pgcpp::fmgr::LookupBuiltinFunction;
using pgcpp::fmgr::OidFunctionCall1;
using pgcpp::fmgr::OidFunctionCall2;
using pgcpp::fmgr::PgFunction;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Float8GetDatum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::MakeTextDatum;
using pgcpp::types::TextDatumToString;

// Builtin function OIDs (from bootstrap_catalog.cpp).
constexpr Oid kAbsOid = 1398;       // abs(int4)
constexpr Oid kRoundOid = 1700;     // round(float8)
constexpr Oid kCeilOid = 2308;      // ceil(float8)
constexpr Oid kFloorOid = 2311;     // floor(float8)
constexpr Oid kModOid = 941;        // mod(int4, int4)
constexpr Oid kLengthOid = 1311;    // length(text)

class FmgrTest : public ::testing::Test {
protected:
    void SetUp() override {
        InitErrorSubsystem();
        context_ = AllocSetContext::Create("fmgr_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
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

// --- fmgr_info tests ---

TEST_F(FmgrTest, FmgrInfoBuiltinAbs) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kAbsOid, &finfo));
    EXPECT_EQ(finfo.fn_oid, kAbsOid);
    EXPECT_EQ(finfo.fn_name, "abs");
    EXPECT_TRUE(finfo.fn_addr != nullptr);
    EXPECT_TRUE(finfo.fn_strict);
    // Builtins don't set prolang → treated as internal.
    EXPECT_EQ(finfo.fn_language, kInternalLanguageOid);
}

TEST_F(FmgrTest, FmgrInfoBuiltinRound) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kRoundOid, &finfo));
    EXPECT_EQ(finfo.fn_name, "round");
    EXPECT_TRUE(finfo.fn_addr != nullptr);
}

TEST_F(FmgrTest, FmgrInfoInvalidOid) {
    FmgrInfo finfo;
    EXPECT_FALSE(fmgr_info(999999, &finfo));
}

TEST_F(FmgrTest, FmgrInfoNullFinfo) {
    EXPECT_FALSE(fmgr_info(kAbsOid, nullptr));
}

// --- FunctionCall tests ---

TEST_F(FmgrTest, FunctionCallInt4Abs) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kAbsOid, &finfo));
    Datum result = FunctionCall(&finfo, {Int32GetDatum(-42)});
    EXPECT_EQ(DatumGetInt32(result), 42);
}

TEST_F(FmgrTest, FunctionCallFloat8Round) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kRoundOid, &finfo));
    Datum result = FunctionCall(&finfo, {Float8GetDatum(3.6)});
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 4.0);
}

TEST_F(FmgrTest, FunctionCallFloat8Ceil) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kCeilOid, &finfo));
    Datum result = FunctionCall(&finfo, {Float8GetDatum(2.1)});
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 3.0);
}

TEST_F(FmgrTest, FunctionCallFloat8Floor) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kFloorOid, &finfo));
    Datum result = FunctionCall(&finfo, {Float8GetDatum(2.9)});
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 2.0);
}

TEST_F(FmgrTest, FunctionCallInt4Mod) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kModOid, &finfo));
    Datum result = FunctionCall(&finfo, {Int32GetDatum(17), Int32GetDatum(5)});
    EXPECT_EQ(DatumGetInt32(result), 2);
}

TEST_F(FmgrTest, FunctionCallTextLength) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kLengthOid, &finfo));
    Datum text_datum = MakeTextDatum("hello");
    Datum result = FunctionCall(&finfo, {text_datum});
    EXPECT_EQ(DatumGetInt32(result), 5);
}

// --- Strict NULL short-circuiting ---

TEST_F(FmgrTest, StrictFunctionNullArgReturnsNull) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kAbsOid, &finfo));
    EXPECT_TRUE(finfo.fn_strict);

    bool isnull = false;
    Datum result = FunctionCallWithNulls(&finfo,
                                          {Int32GetDatum(0)},
                                          {true},  // arg[0] is NULL
                                          &isnull);
    EXPECT_TRUE(isnull);
    (void)result;
}

TEST_F(FmgrTest, StrictFunctionNoNullArgsReturnsValue) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kAbsOid, &finfo));

    bool isnull = false;
    Datum result = FunctionCallWithNulls(&finfo,
                                          {Int32GetDatum(-7)},
                                          {false},
                                          &isnull);
    EXPECT_FALSE(isnull);
    EXPECT_EQ(DatumGetInt32(result), 7);
}

// --- DirectFunctionCallN tests ---

TEST_F(FmgrTest, DirectFunctionCall1Abs) {
    PgFunction func = LookupBuiltinFunction("abs");
    ASSERT_TRUE(func != nullptr);
    Datum result = DirectFunctionCall1(func, Int32GetDatum(-99));
    EXPECT_EQ(DatumGetInt32(result), 99);
}

TEST_F(FmgrTest, DirectFunctionCall2Mod) {
    PgFunction func = LookupBuiltinFunction("mod");
    ASSERT_TRUE(func != nullptr);
    Datum result = DirectFunctionCall2(func, Int32GetDatum(20), Int32GetDatum(6));
    EXPECT_EQ(DatumGetInt32(result), 2);
}

// --- OidFunctionCallN tests ---

TEST_F(FmgrTest, OidFunctionCall1Abs) {
    Datum result = OidFunctionCall1(kAbsOid, Int32GetDatum(-55));
    EXPECT_EQ(DatumGetInt32(result), 55);
}

TEST_F(FmgrTest, OidFunctionCall2Mod) {
    Datum result = OidFunctionCall2(kModOid, Int32GetDatum(23), Int32GetDatum(7));
    EXPECT_EQ(DatumGetInt32(result), 2);
}

// --- LookupBuiltinFunction tests ---

TEST_F(FmgrTest, LookupKnownFunction) {
    PgFunction func = LookupBuiltinFunction("abs");
    EXPECT_TRUE(func != nullptr);
}

TEST_F(FmgrTest, LookupUnknownFunction) {
    PgFunction func = LookupBuiltinFunction("nonexistent_function");
    EXPECT_EQ(func, nullptr);
}

TEST_F(FmgrTest, LookupRound) {
    PgFunction func = LookupBuiltinFunction("round");
    EXPECT_TRUE(func != nullptr);
}

TEST_F(FmgrTest, LookupLength) {
    PgFunction func = LookupBuiltinFunction("length");
    EXPECT_TRUE(func != nullptr);
}

// --- SQL-language function has no handler ---

TEST_F(FmgrTest, SqlLanguageFunctionReturnsNull) {
    // Insert a SQL-language function into the catalog.
    auto* proc = makePallocNode<FormData_pg_proc>();
    proc->oid = 50000;
    proc->proname = "my_sql_func";
    proc->prolang = kSqlLanguageOid;
    proc->proisstrict = false;
    proc->pronargs = 0;
    proc->prorettype = pgcpp::types::kInt4Oid;
    proc->prosrc = "SELECT 1";
    GetCatalog()->InsertProc(proc);

    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(50000, &finfo));
    EXPECT_EQ(finfo.fn_language, kSqlLanguageOid);
    EXPECT_EQ(finfo.fn_addr, nullptr);
    EXPECT_FALSE(finfo.has_handler());

    bool isnull = false;
    Datum result = FunctionCall(&finfo, {}, &isnull);
    EXPECT_TRUE(isnull);
    (void)result;
}

// --- has_handler test ---

TEST_F(FmgrTest, BuiltinHasHandler) {
    FmgrInfo finfo;
    ASSERT_TRUE(fmgr_info(kAbsOid, &finfo));
    EXPECT_TRUE(finfo.has_handler());
}

}  // namespace
