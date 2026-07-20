// exec_eval_expr_test.cpp — Unit tests for Task 6 / Task 11 expression
// evaluation: the new kCoalesceExpr / kMinMaxExpr / kNullIfExpr /
// kSQLValueFunction switch cases in ExecEvalExpr, plus the kFuncExpr
// dispatch path for the new math / string / NULL-handling builtins.
//
// The fixture sets up the catalog (via BootstrapCatalog) so that proc lookups
// by name succeed, but does not need the buffer pool or transaction system
// — ExecEvalExpr dispatches directly to the C++ math/string functions
// without touching storage.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_proc.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "parser/primnodes.hpp"
#include "types/builtins.hpp"
#include "types/datum.hpp"
#include "types/string_funcs.hpp"

using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_proc;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kFirstNormalObjectId;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::executor::CreateExprContext;
using pgcpp::executor::ExecEvalExpr;
using pgcpp::executor::ExprContext;
using pgcpp::executor::ResetExprContext;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::CoalesceExpr;
using pgcpp::parser::Const;
using pgcpp::parser::FuncExpr;
using pgcpp::parser::MinMaxExpr;
using pgcpp::parser::MinMaxOp;
using pgcpp::parser::Node;
using pgcpp::parser::NullIfExpr;
using pgcpp::parser::SQLValueFunction;
using pgcpp::parser::SQLValueFunctionOp;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::Float8GetDatum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::Int64GetDatum;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::MakeTextDatum;
using pgcpp::types::TextDatumToString;
using pgcpp::types::VARDATA;
using pgcpp::types::VARSIZE_DATA;

namespace {

// Operator OIDs (from bootstrap_catalog.cpp).
constexpr Oid kInt4EqOp = 96;   // int4 = int4
constexpr Oid kInt4GtOp = 521;  // int4 > int4
constexpr Oid kInt4LtOp = 97;   // int4 < int4

// Proc OIDs with stable assignments in BootstrapCatalog (used to bypass
// name-based lookup for tests that need a specific overload).
constexpr Oid kAbsInt4Oid = 1398;
constexpr Oid kAbsInt8Oid = 1796;
constexpr Oid kAbsFloat8Oid = 1346;
constexpr Oid kCeilOid = 2308;
constexpr Oid kFloorOid = 2311;
constexpr Oid kRoundOid = 1700;
constexpr Oid kSqrtOid = 1340;
constexpr Oid kPowerOid = 1368;
constexpr Oid kModInt4Oid = 941;
constexpr Oid kModInt8Oid = 947;
constexpr Oid kLogOid = 1342;  // natural log
constexpr Oid kLog10Oid = 1343;
constexpr Oid kExpOid = 1341;
constexpr Oid kSignOid = 1345;
constexpr Oid kTruncOid = 1344;

class ExecEvalExprTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("exec_eval_expr_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);
    }

    void TearDown() override {
        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: look up a proc OID by name. Returns the OID of the first proc
    // with the given name (BootstrapCatalog registers most overloads with
    // kInvalidOid, so the catalog auto-assigns a fresh OID at insert time).
    Oid ProcOidByName(const std::string& name) {
        auto matches = GetCatalog()->GetProcsByName(name);
        if (matches.empty()) {
            return kInvalidOid;
        }
        return matches[0]->oid;
    }

    // Helper: build an int4 Const node.
    Const* MakeInt4Const(int32_t value, bool is_null = false) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(value);
        con->constisnull = is_null;
        con->constbyval = true;
        con->constlen = 4;
        return con;
    }

    // Helper: build an int8 Const node.
    Const* MakeInt8Const(int64_t value, bool is_null = false) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt8Oid;
        con->constvalue = Int64GetDatum(value);
        con->constisnull = is_null;
        con->constbyval = true;
        con->constlen = 8;
        return con;
    }

    // Helper: build a float8 Const node.
    Const* MakeFloat8Const(double value, bool is_null = false) {
        auto* con = makePallocNode<Const>();
        con->consttype = kFloat8Oid;
        con->constvalue = Float8GetDatum(value);
        con->constisnull = is_null;
        con->constbyval = true;
        con->constlen = 8;
        return con;
    }

    // Helper: build a text Const node.
    Const* MakeTextConst(const std::string& s, bool is_null = false) {
        auto* con = makePallocNode<Const>();
        con->consttype = kTextOid;
        con->constvalue = MakeTextDatum(s);
        con->constisnull = is_null;
        con->constbyval = false;
        con->constlen = -1;
        return con;
    }

    // Helper: build a FuncExpr from a proc name (looked up in the catalog)
    // and a list of argument nodes.
    FuncExpr* MakeFuncExprByName(const std::string& name, std::vector<Node*> args) {
        auto* fn = makePallocNode<FuncExpr>();
        fn->funcid = ProcOidByName(name);
        fn->funcresulttype = kInt4Oid;  // overridden per-test as needed
        for (auto* a : args) {
            fn->args.push_back(a);
        }
        return fn;
    }

    // Helper: build a FuncExpr from a known proc OID and argument list.
    FuncExpr* MakeFuncExprByOid(Oid funcid, Oid result_type, std::vector<Node*> args) {
        auto* fn = makePallocNode<FuncExpr>();
        fn->funcid = funcid;
        fn->funcresulttype = result_type;
        for (auto* a : args) {
            fn->args.push_back(a);
        }
        return fn;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

// Returns true if the given callable ereports(ERROR).
template<typename F>
bool RaisesError(F&& fn) {
    bool caught = false;
    PG_TRY() {
        fn();
    }
    PG_CATCH() {
        caught = true;
        pgcpp::error::ErrorData* err = pgcpp::error::GetErrorData();
        EXPECT_EQ(err->elevel, pgcpp::error::LogLevel::kError);
    }
    PG_END_TRY();
    return caught;
}

// ===========================================================================
// kCoalesceExpr
// ===========================================================================

TEST_F(ExecEvalExprTest, Coalesce_FirstArgNonNull) {
    ExprContext* econtext = CreateExprContext();
    auto* ce = makePallocNode<CoalesceExpr>();
    ce->coalescetype = kInt4Oid;
    ce->args = {MakeInt4Const(1), MakeInt4Const(2), MakeInt4Const(3)};

    bool is_null = false;
    Datum result = ExecEvalExpr(ce, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 1);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, Coalesce_SkipsNulls) {
    ExprContext* econtext = CreateExprContext();
    auto* ce = makePallocNode<CoalesceExpr>();
    ce->coalescetype = kInt4Oid;
    ce->args = {MakeInt4Const(0, /*is_null=*/true), MakeInt4Const(0, /*is_null=*/true),
                MakeInt4Const(42)};

    bool is_null = false;
    Datum result = ExecEvalExpr(ce, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 42);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, Coalesce_AllNullReturnsNull) {
    ExprContext* econtext = CreateExprContext();
    auto* ce = makePallocNode<CoalesceExpr>();
    ce->coalescetype = kInt4Oid;
    ce->args = {MakeInt4Const(0, /*is_null=*/true), MakeInt4Const(0, /*is_null=*/true)};

    bool is_null = false;
    ExecEvalExpr(ce, econtext, &is_null);
    EXPECT_TRUE(is_null);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, Coalesce_EmptyArgsReturnsNull) {
    ExprContext* econtext = CreateExprContext();
    auto* ce = makePallocNode<CoalesceExpr>();
    ce->coalescetype = kInt4Oid;

    bool is_null = false;
    ExecEvalExpr(ce, econtext, &is_null);
    EXPECT_TRUE(is_null);
    ResetExprContext(econtext);
}

// ===========================================================================
// kMinMaxExpr (GREATEST / LEAST)
// ===========================================================================

TEST_F(ExecEvalExprTest, MinMax_GreatestNormal) {
    ExprContext* econtext = CreateExprContext();
    auto* mm = makePallocNode<MinMaxExpr>();
    mm->minmaxtype = MinMaxOp::kIsGreatest;
    mm->args = {MakeInt4Const(10), MakeInt4Const(20), MakeInt4Const(5)};

    bool is_null = false;
    Datum result = ExecEvalExpr(mm, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 20);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, MinMax_LeastNormal) {
    ExprContext* econtext = CreateExprContext();
    auto* mm = makePallocNode<MinMaxExpr>();
    mm->minmaxtype = MinMaxOp::kIsLeast;
    mm->args = {MakeInt4Const(10), MakeInt4Const(20), MakeInt4Const(5)};

    bool is_null = false;
    Datum result = ExecEvalExpr(mm, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 5);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, MinMax_SkipsNulls) {
    ExprContext* econtext = CreateExprContext();
    auto* mm = makePallocNode<MinMaxExpr>();
    mm->minmaxtype = MinMaxOp::kIsGreatest;
    mm->args = {MakeInt4Const(0, /*is_null=*/true), MakeInt4Const(7),
                MakeInt4Const(0, /*is_null=*/true), MakeInt4Const(3)};

    bool is_null = false;
    Datum result = ExecEvalExpr(mm, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 7);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, MinMax_AllNullReturnsNull) {
    ExprContext* econtext = CreateExprContext();
    auto* mm = makePallocNode<MinMaxExpr>();
    mm->minmaxtype = MinMaxOp::kIsGreatest;
    mm->args = {MakeInt4Const(0, /*is_null=*/true), MakeInt4Const(0, /*is_null=*/true)};

    bool is_null = false;
    ExecEvalExpr(mm, econtext, &is_null);
    EXPECT_TRUE(is_null);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, MinMax_EmptyArgsReturnsNull) {
    ExprContext* econtext = CreateExprContext();
    auto* mm = makePallocNode<MinMaxExpr>();
    mm->minmaxtype = MinMaxOp::kIsGreatest;

    bool is_null = false;
    ExecEvalExpr(mm, econtext, &is_null);
    EXPECT_TRUE(is_null);
    ResetExprContext(econtext);
}

// ===========================================================================
// kNullIfExpr
// ===========================================================================

TEST_F(ExecEvalExprTest, NullIf_EqualArgsReturnsNull) {
    ExprContext* econtext = CreateExprContext();
    auto* ni = makePallocNode<NullIfExpr>();
    ni->opno = kInt4EqOp;
    ni->opresulttype = kInt4Oid;
    ni->args = {MakeInt4Const(5), MakeInt4Const(5)};

    bool is_null = false;
    ExecEvalExpr(ni, econtext, &is_null);
    EXPECT_TRUE(is_null);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, NullIf_DifferentArgsReturnsFirst) {
    ExprContext* econtext = CreateExprContext();
    auto* ni = makePallocNode<NullIfExpr>();
    ni->opno = kInt4EqOp;
    ni->opresulttype = kInt4Oid;
    ni->args = {MakeInt4Const(5), MakeInt4Const(10)};

    bool is_null = false;
    Datum result = ExecEvalExpr(ni, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 5);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, NullIf_FirstArgNullReturnsNull) {
    ExprContext* econtext = CreateExprContext();
    auto* ni = makePallocNode<NullIfExpr>();
    ni->opno = kInt4EqOp;
    ni->opresulttype = kInt4Oid;
    ni->args = {MakeInt4Const(0, /*is_null=*/true), MakeInt4Const(10)};

    bool is_null = false;
    ExecEvalExpr(ni, econtext, &is_null);
    EXPECT_TRUE(is_null);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, NullIf_SecondArgNullReturnsFirst) {
    // NULLIF(x, NULL) — second arg NULL, returns x.
    ExprContext* econtext = CreateExprContext();
    auto* ni = makePallocNode<NullIfExpr>();
    ni->opno = kInt4EqOp;
    ni->opresulttype = kInt4Oid;
    ni->args = {MakeInt4Const(7), MakeInt4Const(0, /*is_null=*/true)};

    bool is_null = false;
    Datum result = ExecEvalExpr(ni, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 7);
    ResetExprContext(econtext);
}

// ===========================================================================
// kSQLValueFunction (CURRENT_DATE / CURRENT_TIMESTAMP)
// ===========================================================================

TEST_F(ExecEvalExprTest, SqlValueFunction_CurrentDate) {
    ExprContext* econtext = CreateExprContext();
    auto* svf = makePallocNode<SQLValueFunction>();
    svf->op = SQLValueFunctionOp::kCurrentDate;

    bool is_null = false;
    Datum result = ExecEvalExpr(svf, econtext, &is_null);
    EXPECT_FALSE(is_null);
    // Days since PostgreSQL epoch (2000-01-01). Should be > 9000 days
    // (well past 2025 by now).
    int32_t days = DatumGetInt32(result);
    EXPECT_GT(days, 9000);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, SqlValueFunction_CurrentTimestamp) {
    ExprContext* econtext = CreateExprContext();
    auto* svf = makePallocNode<SQLValueFunction>();
    svf->op = SQLValueFunctionOp::kCurrentTimestamp;

    bool is_null = false;
    Datum result = ExecEvalExpr(svf, econtext, &is_null);
    EXPECT_FALSE(is_null);
    // Microseconds since PostgreSQL epoch (2000-01-01).
    int64_t microsecs = DatumGetInt64(result);
    EXPECT_GT(microsecs, 0);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, SqlValueFunction_LocalTimestamp) {
    ExprContext* econtext = CreateExprContext();
    auto* svf = makePallocNode<SQLValueFunction>();
    svf->op = SQLValueFunctionOp::kLocalTimestamp;

    bool is_null = false;
    Datum result = ExecEvalExpr(svf, econtext, &is_null);
    EXPECT_FALSE(is_null);
    // Should match the same calculation as CurrentDate (days since epoch).
    int32_t days = DatumGetInt32(result);
    EXPECT_GT(days, 9000);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, SqlValueFunction_LocalTime) {
    ExprContext* econtext = CreateExprContext();
    auto* svf = makePallocNode<SQLValueFunction>();
    svf->op = SQLValueFunctionOp::kLocalTime;

    bool is_null = false;
    Datum result = ExecEvalExpr(svf, econtext, &is_null);
    EXPECT_FALSE(is_null);
    int64_t microsecs = DatumGetInt64(result);
    EXPECT_GT(microsecs, 0);
    ResetExprContext(econtext);
}

// ===========================================================================
// kFuncExpr dispatch — Math functions (Task 9)
// ===========================================================================

TEST_F(ExecEvalExprTest, FuncExpr_AbsInt4) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kAbsInt4Oid, kInt4Oid, {MakeInt4Const(-42)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 42);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_AbsInt8) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kAbsInt8Oid, kInt8Oid, {MakeInt8Const(-123456789012LL)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt64(result), 123456789012LL);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_AbsFloat8) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kAbsFloat8Oid, kFloat8Oid, {MakeFloat8Const(-3.5)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 3.5);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Ceil) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kCeilOid, kFloat8Oid, {MakeFloat8Const(1.4)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 2.0);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Floor) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kFloorOid, kFloat8Oid, {MakeFloat8Const(1.7)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 1.0);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Round) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kRoundOid, kFloat8Oid, {MakeFloat8Const(1.5)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 2.0);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Sqrt) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kSqrtOid, kFloat8Oid, {MakeFloat8Const(16.0)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 4.0);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Power) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn =
        MakeFuncExprByOid(kPowerOid, kFloat8Oid, {MakeFloat8Const(2.0), MakeFloat8Const(10.0)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 1024.0);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Log) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kLogOid, kFloat8Oid, {MakeFloat8Const(std::exp(1.0))});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_NEAR(DatumGetFloat8(result), 1.0, 1e-9);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Log10) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kLog10Oid, kFloat8Oid, {MakeFloat8Const(100.0)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 2.0);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Exp) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kExpOid, kFloat8Oid, {MakeFloat8Const(0.0)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 1.0);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_ModInt4) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kModInt4Oid, kInt4Oid, {MakeInt4Const(10), MakeInt4Const(3)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 1);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_ModInt8) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kModInt8Oid, kInt8Oid,
                                     {MakeInt8Const(1000000000000LL), MakeInt8Const(7)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt64(result), 1000000000000LL % 7);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Sign) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn_pos = MakeFuncExprByOid(kSignOid, kInt4Oid, {MakeInt4Const(42)});
    FuncExpr* fn_neg = MakeFuncExprByOid(kSignOid, kInt4Oid, {MakeInt4Const(-42)});
    FuncExpr* fn_zero = MakeFuncExprByOid(kSignOid, kInt4Oid, {MakeInt4Const(0)});

    bool is_null = false;
    EXPECT_EQ(DatumGetInt32(ExecEvalExpr(fn_pos, econtext, &is_null)), 1);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(ExecEvalExpr(fn_neg, econtext, &is_null)), -1);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(ExecEvalExpr(fn_zero, econtext, &is_null)), 0);
    EXPECT_FALSE(is_null);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Trunc1Arg) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kTruncOid, kFloat8Oid, {MakeFloat8Const(1.9)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(result), 1.0);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Trunc2ArgByName) {
    // 2-arg trunc has no stable OID; look up by name.
    ExprContext* econtext = CreateExprContext();
    Oid trunc2_oid = ProcOidByName("trunc");
    ASSERT_NE(trunc2_oid, kInvalidOid);
    FuncExpr* fn =
        MakeFuncExprByOid(trunc2_oid, kFloat8Oid, {MakeFloat8Const(3.14159), MakeInt4Const(2)});

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_NEAR(DatumGetFloat8(result), 3.14, 1e-6);
    ResetExprContext(econtext);
}

// ===========================================================================
// kFuncExpr dispatch — NULL handling (Task 11)
// ===========================================================================

TEST_F(ExecEvalExprTest, FuncExpr_CoalesceFirstNonNull) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName(
        "coalesce", {MakeInt4Const(0, /*is_null=*/true), MakeInt4Const(11), MakeInt4Const(22)});
    fn->funcresulttype = kInt4Oid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 11);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_CoalesceAllNullReturnsNull) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName(
        "coalesce", {MakeInt4Const(0, /*is_null=*/true), MakeInt4Const(0, /*is_null=*/true)});
    fn->funcresulttype = kInt4Oid;

    bool is_null = false;
    ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_TRUE(is_null);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_NullIfEqualReturnsNull) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName("nullif", {MakeInt4Const(5), MakeInt4Const(5)});
    fn->funcresulttype = kInt4Oid;

    bool is_null = false;
    ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_TRUE(is_null);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_NullIfDifferentReturnsFirst) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName("nullif", {MakeInt4Const(5), MakeInt4Const(10)});
    fn->funcresulttype = kInt4Oid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 5);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Greatest) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn =
        MakeFuncExprByName("greatest", {MakeInt4Const(10), MakeInt4Const(20), MakeInt4Const(5)});
    fn->funcresulttype = kInt4Oid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 20);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Least) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn =
        MakeFuncExprByName("least", {MakeInt4Const(10), MakeInt4Const(20), MakeInt4Const(5)});
    fn->funcresulttype = kInt4Oid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 5);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_CurrentDate) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName("current_date", {});
    fn->funcresulttype = pgcpp::types::kDateOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_GT(DatumGetInt32(result), 9000);  // days since 2000-01-01
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_CurrentTimestamp) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName("current_timestamp", {});
    fn->funcresulttype = pgcpp::types::kTimestamptzOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_GT(DatumGetInt64(result), 0);  // microsecs since 2000-01-01
    ResetExprContext(econtext);
}

// ===========================================================================
// kFuncExpr dispatch — String functions (Task 10)
// ===========================================================================

TEST_F(ExecEvalExprTest, FuncExpr_Substr2Arg) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName("substr", {MakeTextConst("hello world"), MakeInt4Const(7)});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(TextDatumToString(result), "world");
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Substr3Arg) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName(
        "substr", {MakeTextConst("hello world"), MakeInt4Const(1), MakeInt4Const(5)});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(TextDatumToString(result), "hello");
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Trim) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName("trim", {MakeTextConst("  hello  ")});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(TextDatumToString(result), "hello");
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Btrim) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName("btrim", {MakeTextConst("  hello  ")});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(TextDatumToString(result), "hello");
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Ltrim) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName("ltrim", {MakeTextConst("  hello  ")});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(TextDatumToString(result), "hello  ");
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Rtrim) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName("rtrim", {MakeTextConst("  hello  ")});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(TextDatumToString(result), "  hello");
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Replace) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName(
        "replace", {MakeTextConst("hello world"), MakeTextConst("o"), MakeTextConst("0")});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(TextDatumToString(result), "hell0 w0rld");
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Position) {
    ExprContext* econtext = CreateExprContext();
    // PostgreSQL's `position(substr IN str)` parser form emits a FuncExpr
    // with args ordered as (string, substring) — i.e. the function takes
    // (haystack, needle). text_position follows that same convention.
    FuncExpr* fn =
        MakeFuncExprByName("position", {MakeTextConst("hello world"), MakeTextConst("world")});
    fn->funcresulttype = kInt4Oid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 7);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Lpad3Arg) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn =
        MakeFuncExprByName("lpad", {MakeTextConst("hi"), MakeInt4Const(8), MakeTextConst("xy")});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(TextDatumToString(result), "xyxyxyhi");
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Rpad3Arg) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn =
        MakeFuncExprByName("rpad", {MakeTextConst("hi"), MakeInt4Const(8), MakeTextConst("xy")});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(TextDatumToString(result), "hixyxyxy");
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_SplitPart) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName(
        "split_part", {MakeTextConst("a-b-c"), MakeTextConst("-"), MakeInt4Const(2)});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(TextDatumToString(result), "b");
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_Length) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName("length", {MakeTextConst("hello")});
    fn->funcresulttype = kInt4Oid;

    bool is_null = false;
    Datum result = ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(result), 5);
    ResetExprContext(econtext);
}

// ===========================================================================
// NULL-handling semantics for kFuncExpr dispatch
// ===========================================================================

TEST_F(ExecEvalExprTest, FuncExpr_AbsNullArgReturnsNull) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByOid(kAbsInt4Oid, kInt4Oid, {MakeInt4Const(0, /*is_null=*/true)});

    bool is_null = false;
    ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_TRUE(is_null);
    ResetExprContext(econtext);
}

TEST_F(ExecEvalExprTest, FuncExpr_SubstrNullArgReturnsNull) {
    ExprContext* econtext = CreateExprContext();
    FuncExpr* fn = MakeFuncExprByName(
        "substr", {MakeTextConst("", /*is_null=*/true), MakeInt4Const(1), MakeInt4Const(3)});
    fn->funcresulttype = kTextOid;

    bool is_null = false;
    ExecEvalExpr(fn, econtext, &is_null);
    EXPECT_TRUE(is_null);
    ResetExprContext(econtext);
}

}  // namespace
