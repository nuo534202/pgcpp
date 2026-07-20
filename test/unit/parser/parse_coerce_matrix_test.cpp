// parse_coerce_matrix_test.cpp — Matrix tests for coerce_type conversions.
//
// Verifies the type conversion paths added to parse_coerce.cpp:
//   - int4 <-> bool (Const-folding, boundary at 0)
//   - text <-> int4 / int8 / float8 / bool (I/O cast with Const-folding)
//   - int4 / int8 / float8 / bool -> text (I/O cast with Const-folding)
//   - timestamp <-> date (arithmetic conversion with floor-toward-(-inf))
//
// Coverage requirements (per task spec):
//   - Normal inputs for each path
//   - NULL input handling (constisnull preserved, type metadata updated)
//   - Boundary values (e.g., int4->bool at 0 and non-zero)
//   - Error inputs (e.g., text->int4 on non-numeric string)
//   - Implicit vs explicit coercion (can_coerce_type gatekeeping)
//   - SQL-level integration via :: casts (kExplicit context)

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_type.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/analyze.hpp"
#include "parser/parse_coerce.hpp"
#include "parser/parse_node.hpp"
#include "parser/parse_type.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"
#include "parser/primnodes.hpp"
#include "types/builtins.hpp"
#include "types/datetime.hpp"
#include "types/datum.hpp"

using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::FormData_pg_type;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::GetSysCache;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::nodeTag;
using pgcpp::parser::can_coerce_type;
using pgcpp::parser::CmdType;
using pgcpp::parser::coerce_type;
using pgcpp::parser::CoercionContext;
using pgcpp::parser::CoercionForm;
using pgcpp::parser::Const;
using pgcpp::parser::makeConst;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::ParseState;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
using pgcpp::parser::RTEKind;
using pgcpp::parser::TargetEntry;
using pgcpp::types::BoolGetDatum;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::Float8GetDatum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::Int64GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kDateOid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kMicrosecsPerSec;
using pgcpp::types::kSecsPerDay;
using pgcpp::types::kTextOid;
using pgcpp::types::kTimestampOid;
using pgcpp::types::text_in;
using pgcpp::types::text_out;
using pgcpp::types::TextDatumToString;

namespace {

// kUnknownOid — PostgreSQL's OID for the "unknown" pseudo-type (705).
constexpr Oid kUnknownOid = 705;

// Test fixture: provides a memory context, catalog (with built-in types,
// operators, functions), and syscache. Mirrors ParseCoerceVarcharTest.
class ParseCoerceMatrixTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("parse_coerce_matrix_test");
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

    // Helper: parse and analyze a SQL string, returning the first Query.
    // Takes const char* to avoid a temporary std::string leaking across
    // an ereport(ERROR) longjmp.
    Query* AnalyzeSingle(const char* sql) {
        auto stmts = raw_parser(sql);
        if (stmts.empty())
            return nullptr;
        auto queries = parse_analyze(stmts, sql);
        if (queries.empty())
            return nullptr;
        return queries[0];
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

// Helper: check if a callable ereports(ERROR).
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

// --- Const builders for direct coerce_type tests ---

Const* MakeInt4Const(int32_t v) {
    return makeConst(kInt4Oid, -1, 0, 4, Int32GetDatum(v), /*isnull=*/false,
                     /*byval=*/true, -1);
}

Const* MakeInt8Const(int64_t v) {
    return makeConst(kInt8Oid, -1, 0, 8, Int64GetDatum(v), false, true, -1);
}

Const* MakeFloat8Const(double v) {
    return makeConst(kFloat8Oid, -1, 0, 8, Float8GetDatum(v), false, true, -1);
}

Const* MakeBoolConst(bool v) {
    return makeConst(kBoolOid, -1, 0, 1, BoolGetDatum(v), false, true, -1);
}

Const* MakeTextConst(const char* s) {
    Datum d = text_in(s);
    return makeConst(kTextOid, -1, 0, -1, d, false, false, -1);
}

Const* MakeDateConst(int32_t days) {
    return makeConst(kDateOid, -1, 0, 4, Int32GetDatum(days), false, true, -1);
}

Const* MakeTimestampConst(int64_t micros) {
    return makeConst(kTimestampOid, -1, 0, 8, Int64GetDatum(micros), false, true, -1);
}

// Make a NULL Const of the given type. constvalue is 0, constisnull is true.
Const* MakeNullConst(Oid type_oid) {
    return makeConst(type_oid, -1, 0, 0, 0, /*isnull=*/true, /*byval=*/false, -1);
}

// Wrap a coerce_type call as a functor for RaisesError. Uses pstate=nullptr
// because coerce_type only uses pstate for error messages (which we don't
// verify here) and the ccontext=cformat=kExplicit context matches what
// transformTypeCast passes.
Node* CoerceExplicit(Node* node, Oid from, Oid to) {
    return coerce_type(/*pstate=*/nullptr, node, from, to, -1, CoercionContext::kExplicit,
                       CoercionForm::kExplicit, -1);
}

// First target-list Const from a SELECT, or nullptr if not a Const.
const Const* FirstTargetConst(const Query* qry) {
    if (qry == nullptr || qry->target_list.empty())
        return nullptr;
    Node* tle_node = qry->target_list[0];
    if (nodeTag(tle_node) != NodeTag::kTargetEntry)
        return nullptr;
    auto* tle = static_cast<TargetEntry*>(tle_node);
    if (tle->expr == nullptr || nodeTag(tle->expr) != NodeTag::kConst)
        return nullptr;
    return static_cast<Const*>(tle->expr);
}

// can_coerce_type for a single (input, target) pair.
bool CanCoerceOne(Oid input, Oid target, CoercionContext ctx) {
    return can_coerce_type(1, &input, &target, ctx);
}

}  // namespace

// ===========================================================================
// int4 -> bool (Bool <-> Numeric branch, Const-folding)
// ===========================================================================

TEST_F(ParseCoerceMatrixTest, Int4ToBoolZeroIsFalse) {
    auto* con = MakeInt4Const(0);
    Node* result = CoerceExplicit(con, kInt4Oid, kBoolOid);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result, con);  // Const mutated in place
    EXPECT_EQ(con->consttype, kBoolOid);
    EXPECT_FALSE(DatumGetBool(con->constvalue));
}

TEST_F(ParseCoerceMatrixTest, Int4ToBoolOneIsTrue) {
    auto* con = MakeInt4Const(1);
    Node* result = CoerceExplicit(con, kInt4Oid, kBoolOid);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(DatumGetBool(con->constvalue));
}

TEST_F(ParseCoerceMatrixTest, Int4ToBoolNonzeroIsTrue) {
    // Boundary: any non-zero value (positive or negative) maps to true.
    auto* con_pos = MakeInt4Const(42);
    CoerceExplicit(con_pos, kInt4Oid, kBoolOid);
    EXPECT_TRUE(DatumGetBool(con_pos->constvalue));

    auto* con_neg = MakeInt4Const(-7);
    CoerceExplicit(con_neg, kInt4Oid, kBoolOid);
    EXPECT_TRUE(DatumGetBool(con_neg->constvalue));

    auto* con_max = MakeInt4Const(INT32_MAX);
    CoerceExplicit(con_max, kInt4Oid, kBoolOid);
    EXPECT_TRUE(DatumGetBool(con_max->constvalue));
}

// ===========================================================================
// bool -> int4 (Bool <-> Numeric branch, Const-folding)
// ===========================================================================

TEST_F(ParseCoerceMatrixTest, BoolToInt4FalseIsZero) {
    auto* con = MakeBoolConst(false);
    Node* result = CoerceExplicit(con, kBoolOid, kInt4Oid);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(con->consttype, kInt4Oid);
    EXPECT_EQ(DatumGetInt32(con->constvalue), 0);
}

TEST_F(ParseCoerceMatrixTest, BoolToInt4TrueIsOne) {
    auto* con = MakeBoolConst(true);
    CoerceExplicit(con, kBoolOid, kInt4Oid);
    EXPECT_EQ(con->consttype, kInt4Oid);
    EXPECT_EQ(DatumGetInt32(con->constvalue), 1);
}

// ===========================================================================
// text -> int4 / int8 / float8 (String <-> non-String branch, Const-folding)
// ===========================================================================

TEST_F(ParseCoerceMatrixTest, TextToInt4Valid) {
    auto* con = MakeTextConst("42");
    Node* result = CoerceExplicit(con, kTextOid, kInt4Oid);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(con->consttype, kInt4Oid);
    EXPECT_EQ(DatumGetInt32(con->constvalue), 42);
}

TEST_F(ParseCoerceMatrixTest, TextToInt4Negative) {
    auto* con = MakeTextConst("-12345");
    CoerceExplicit(con, kTextOid, kInt4Oid);
    EXPECT_EQ(DatumGetInt32(con->constvalue), -12345);
}

TEST_F(ParseCoerceMatrixTest, TextToInt4InvalidErrors) {
    auto* con = MakeTextConst("abc");
    EXPECT_TRUE(RaisesError([&] { CoerceExplicit(con, kTextOid, kInt4Oid); }));
}

TEST_F(ParseCoerceMatrixTest, TextToInt4TrailingCharsError) {
    auto* con = MakeTextConst("42abc");
    EXPECT_TRUE(RaisesError([&] { CoerceExplicit(con, kTextOid, kInt4Oid); }));
}

TEST_F(ParseCoerceMatrixTest, TextToInt8Valid) {
    auto* con = MakeTextConst("9223372036854775807");  // INT64_MAX
    CoerceExplicit(con, kTextOid, kInt8Oid);
    EXPECT_EQ(con->consttype, kInt8Oid);
    EXPECT_EQ(DatumGetInt64(con->constvalue), INT64_MAX);
}

TEST_F(ParseCoerceMatrixTest, TextToInt8Negative) {
    auto* con = MakeTextConst("-9223372036854775807");
    CoerceExplicit(con, kTextOid, kInt8Oid);
    EXPECT_EQ(DatumGetInt64(con->constvalue), INT64_MIN + 1);
}

TEST_F(ParseCoerceMatrixTest, TextToFloat8Valid) {
    auto* con = MakeTextConst("3.14");
    CoerceExplicit(con, kTextOid, kFloat8Oid);
    EXPECT_EQ(con->consttype, kFloat8Oid);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(con->constvalue), 3.14);
}

TEST_F(ParseCoerceMatrixTest, TextToFloat8Negative) {
    auto* con = MakeTextConst("-2.5");
    CoerceExplicit(con, kTextOid, kFloat8Oid);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(con->constvalue), -2.5);
}

TEST_F(ParseCoerceMatrixTest, TextToFloat8InvalidErrors) {
    auto* con = MakeTextConst("not_a_number");
    EXPECT_TRUE(RaisesError([&] { CoerceExplicit(con, kTextOid, kFloat8Oid); }));
}

// ===========================================================================
// int4 / int8 / float8 -> text (X -> String branch, Const-folding)
// ===========================================================================

TEST_F(ParseCoerceMatrixTest, Int4ToTextBasic) {
    auto* con = MakeInt4Const(42);
    CoerceExplicit(con, kInt4Oid, kTextOid);
    EXPECT_EQ(con->consttype, kTextOid);
    EXPECT_EQ(TextDatumToString(con->constvalue), "42");
}

TEST_F(ParseCoerceMatrixTest, Int4ToTextNegative) {
    auto* con = MakeInt4Const(-7);
    CoerceExplicit(con, kInt4Oid, kTextOid);
    EXPECT_EQ(TextDatumToString(con->constvalue), "-7");
}

TEST_F(ParseCoerceMatrixTest, Int8ToTextBasic) {
    auto* con = MakeInt8Const(INT64_MAX);
    CoerceExplicit(con, kInt8Oid, kTextOid);
    EXPECT_EQ(TextDatumToString(con->constvalue), "9223372036854775807");
}

TEST_F(ParseCoerceMatrixTest, Float8ToTextBasic) {
    auto* con = MakeFloat8Const(3.5);
    CoerceExplicit(con, kFloat8Oid, kTextOid);
    EXPECT_EQ(con->consttype, kTextOid);
    // float8_out uses shortest round-trip representation, so 3.5 -> "3.5".
    EXPECT_EQ(TextDatumToString(con->constvalue), "3.5");
}

// ===========================================================================
// text <-> bool (I/O cast with Const-folding)
// ===========================================================================

TEST_F(ParseCoerceMatrixTest, TextToBoolTIsTrue) {
    auto* con = MakeTextConst("t");
    CoerceExplicit(con, kTextOid, kBoolOid);
    EXPECT_EQ(con->consttype, kBoolOid);
    EXPECT_TRUE(DatumGetBool(con->constvalue));
}

TEST_F(ParseCoerceMatrixTest, TextToBoolFIsFalse) {
    auto* con = MakeTextConst("f");
    CoerceExplicit(con, kTextOid, kBoolOid);
    EXPECT_FALSE(DatumGetBool(con->constvalue));
}

TEST_F(ParseCoerceMatrixTest, TextToBoolFullWords) {
    auto* t = MakeTextConst("true");
    CoerceExplicit(t, kTextOid, kBoolOid);
    EXPECT_TRUE(DatumGetBool(t->constvalue));

    auto* f = MakeTextConst("false");
    CoerceExplicit(f, kTextOid, kBoolOid);
    EXPECT_FALSE(DatumGetBool(f->constvalue));
}

TEST_F(ParseCoerceMatrixTest, TextToBoolOneZero) {
    auto* one = MakeTextConst("1");
    CoerceExplicit(one, kTextOid, kBoolOid);
    EXPECT_TRUE(DatumGetBool(one->constvalue));

    auto* zero = MakeTextConst("0");
    CoerceExplicit(zero, kTextOid, kBoolOid);
    EXPECT_FALSE(DatumGetBool(zero->constvalue));
}

TEST_F(ParseCoerceMatrixTest, TextToBoolCaseInsensitive) {
    // bool_in uses IStringEq (case-insensitive), so "TRUE", "Yes", "On" work.
    auto* upper = MakeTextConst("TRUE");
    CoerceExplicit(upper, kTextOid, kBoolOid);
    EXPECT_TRUE(DatumGetBool(upper->constvalue));

    auto* yes = MakeTextConst("Yes");
    CoerceExplicit(yes, kTextOid, kBoolOid);
    EXPECT_TRUE(DatumGetBool(yes->constvalue));
}

TEST_F(ParseCoerceMatrixTest, TextToBoolInvalidErrors) {
    auto* con = MakeTextConst("maybe");
    EXPECT_TRUE(RaisesError([&] { CoerceExplicit(con, kTextOid, kBoolOid); }));
}

TEST_F(ParseCoerceMatrixTest, BoolToTextTrueIsLowercaseT) {
    auto* con = MakeBoolConst(true);
    CoerceExplicit(con, kBoolOid, kTextOid);
    EXPECT_EQ(con->consttype, kTextOid);
    EXPECT_EQ(TextDatumToString(con->constvalue), "t");
}

TEST_F(ParseCoerceMatrixTest, BoolToTextFalseIsLowercaseF) {
    auto* con = MakeBoolConst(false);
    CoerceExplicit(con, kBoolOid, kTextOid);
    EXPECT_EQ(TextDatumToString(con->constvalue), "f");
}

// ===========================================================================
// timestamp <-> date (Datetime <-> Datetime branch, arithmetic conversion)
// ===========================================================================

TEST_F(ParseCoerceMatrixTest, DateToTimestampIsMidnight) {
    // Day 0 = 2000-01-01. Date 0 -> timestamp 0 (2000-01-01 00:00:00).
    auto* con = MakeDateConst(0);
    CoerceExplicit(con, kDateOid, kTimestampOid);
    EXPECT_EQ(con->consttype, kTimestampOid);
    EXPECT_EQ(DatumGetInt64(con->constvalue), 0);
}

TEST_F(ParseCoerceMatrixTest, DateToTimestampOneDay) {
    // Date 1 -> 86400 * 1000000 microseconds.
    auto* con = MakeDateConst(1);
    CoerceExplicit(con, kDateOid, kTimestampOid);
    int64_t expected = kSecsPerDay * kMicrosecsPerSec;
    EXPECT_EQ(DatumGetInt64(con->constvalue), expected);
}

TEST_F(ParseCoerceMatrixTest, TimestampToDateTruncatesTime) {
    // 2023-01-15 10:30:00 -> 2023-01-15 00:00:00.
    // Compute the day number for 2023-01-15 and verify truncation.
    // 2023-01-15 is day 8405 since 2000-01-01 (computed via date_in).
    Datum d = pgcpp::types::date_in("2023-01-15");
    int32_t target_day = DatumGetInt32(d);

    Datum ts = pgcpp::types::timestamp_in("2023-01-15 10:30:00");
    auto* con = MakeTimestampConst(DatumGetInt64(ts));
    CoerceExplicit(con, kTimestampOid, kDateOid);
    EXPECT_EQ(con->consttype, kDateOid);
    EXPECT_EQ(DatumGetInt32(con->constvalue), target_day);
}

TEST_F(ParseCoerceMatrixTest, TimestampToDateExactMidnight) {
    // Timestamp exactly at midnight -> same day (no truncation needed).
    Datum ts = pgcpp::types::timestamp_in("2023-01-15 00:00:00");
    Datum d = pgcpp::types::date_in("2023-01-15");
    auto* con = MakeTimestampConst(DatumGetInt64(ts));
    CoerceExplicit(con, kTimestampOid, kDateOid);
    EXPECT_EQ(DatumGetInt32(con->constvalue), DatumGetInt32(d));
}

TEST_F(ParseCoerceMatrixTest, TimestampToDateNegativeTimestampFloors) {
    // Boundary: negative timestamps (before 2000-01-01) must floor toward
    // negative infinity, not truncate toward zero. A timestamp one microsecond
    // before 2000-01-01 00:00:00 should map to 1999-12-31 (day -1), not
    // 2000-01-01 (day 0).
    int64_t micros_per_day = kSecsPerDay * kMicrosecsPerSec;
    int64_t neg_one_micros = -1;  // 1 microsecond before epoch
    auto* con = MakeTimestampConst(neg_one_micros);
    CoerceExplicit(con, kTimestampOid, kDateOid);
    EXPECT_EQ(DatumGetInt32(con->constvalue), -1);

    // -micros_per_day + 1 -> day -1 (still in the day before epoch).
    auto* con2 = MakeTimestampConst(-micros_per_day + 1);
    CoerceExplicit(con2, kTimestampOid, kDateOid);
    EXPECT_EQ(DatumGetInt32(con2->constvalue), -1);

    // -micros_per_day -> day -1 (exactly midnight of day -1).
    auto* con3 = MakeTimestampConst(-micros_per_day);
    CoerceExplicit(con3, kTimestampOid, kDateOid);
    EXPECT_EQ(DatumGetInt32(con3->constvalue), -1);

    // -micros_per_day - 1 -> day -2.
    auto* con4 = MakeTimestampConst(-micros_per_day - 1);
    CoerceExplicit(con4, kTimestampOid, kDateOid);
    EXPECT_EQ(DatumGetInt32(con4->constvalue), -2);
}

TEST_F(ParseCoerceMatrixTest, DateToTimestampRoundTrip) {
    // Date -> Timestamp -> Date should be identity.
    auto* con = MakeDateConst(8405);
    CoerceExplicit(con, kDateOid, kTimestampOid);
    EXPECT_EQ(con->consttype, kTimestampOid);
    auto* con2 = MakeTimestampConst(DatumGetInt64(con->constvalue));
    CoerceExplicit(con2, kTimestampOid, kDateOid);
    EXPECT_EQ(DatumGetInt32(con2->constvalue), 8405);
}

// ===========================================================================
// NULL handling: constisnull preserved, type metadata updated
// ===========================================================================

TEST_F(ParseCoerceMatrixTest, NullInt4ToBoolPreservesNull) {
    auto* con = MakeNullConst(kInt4Oid);
    CoerceExplicit(con, kInt4Oid, kBoolOid);
    EXPECT_EQ(con->consttype, kBoolOid);
    EXPECT_TRUE(con->constisnull);
}

TEST_F(ParseCoerceMatrixTest, NullBoolToInt4PreservesNull) {
    auto* con = MakeNullConst(kBoolOid);
    CoerceExplicit(con, kBoolOid, kInt4Oid);
    EXPECT_EQ(con->consttype, kInt4Oid);
    EXPECT_TRUE(con->constisnull);
}

TEST_F(ParseCoerceMatrixTest, NullTextToInt4PreservesNull) {
    auto* con = MakeNullConst(kTextOid);
    CoerceExplicit(con, kTextOid, kInt4Oid);
    EXPECT_EQ(con->consttype, kInt4Oid);
    EXPECT_TRUE(con->constisnull);
}

TEST_F(ParseCoerceMatrixTest, NullInt4ToTextPreservesNull) {
    auto* con = MakeNullConst(kInt4Oid);
    CoerceExplicit(con, kInt4Oid, kTextOid);
    EXPECT_EQ(con->consttype, kTextOid);
    EXPECT_TRUE(con->constisnull);
}

TEST_F(ParseCoerceMatrixTest, NullTimestampToDatePreservesNull) {
    auto* con = MakeNullConst(kTimestampOid);
    CoerceExplicit(con, kTimestampOid, kDateOid);
    EXPECT_EQ(con->consttype, kDateOid);
    EXPECT_TRUE(con->constisnull);
}

TEST_F(ParseCoerceMatrixTest, NullDateToTimestampPreservesNull) {
    auto* con = MakeNullConst(kDateOid);
    CoerceExplicit(con, kDateOid, kTimestampOid);
    EXPECT_EQ(con->consttype, kTimestampOid);
    EXPECT_TRUE(con->constisnull);
}

TEST_F(ParseCoerceMatrixTest, NullFloat8ToInt4PreservesNull) {
    auto* con = MakeNullConst(kFloat8Oid);
    CoerceExplicit(con, kFloat8Oid, kInt4Oid);
    EXPECT_EQ(con->consttype, kInt4Oid);
    EXPECT_TRUE(con->constisnull);
}

// ===========================================================================
// NULL literal (kUnknownOid, constisnull=true) → target type.
//
// Regression for the bug where `INSERT INTO t VALUES (NULL)` failed with
// "unsupported expression type in ExecEvalExpr". The NULL literal is parsed
// as AConst(isnull=true), transformed by make_const into
// Const(kUnknownOid, constisnull=true, constvalue=0). In coerce_type's
// kUnknownOid branch, every string-conversion sub-branch checks
// `if (str != nullptr)` and skips when constvalue is 0, then the function
// fell through to building a CoerceViaIO node — which ExecEvalExpr does
// not support. The fix short-circuits NULL constants at the top of the
// kUnknownOid branch by just fixing up type metadata and returning the
// original Const (no CoerceViaIO wrapper).
// ===========================================================================

TEST_F(ParseCoerceMatrixTest, NullUnknownToInt4PreservesNullNoCoerceViaIO) {
    auto* con = MakeNullConst(kUnknownOid);
    Node* result = CoerceExplicit(con, kUnknownOid, kInt4Oid);
    // Must return the same node (in-place fix-up), NOT a CoerceViaIO wrapper.
    ASSERT_EQ(result, con);
    EXPECT_EQ(con->consttype, kInt4Oid);
    EXPECT_TRUE(con->constisnull);
    EXPECT_EQ(con->constvalue, static_cast<pgcpp::types::Datum>(0));
    // int4 is 4 bytes, pass-by-value.
    EXPECT_EQ(con->constlen, 4);
    EXPECT_TRUE(con->constbyval);
}

TEST_F(ParseCoerceMatrixTest, NullUnknownToTextPreservesNullNoCoerceViaIO) {
    auto* con = MakeNullConst(kUnknownOid);
    Node* result = CoerceExplicit(con, kUnknownOid, kTextOid);
    ASSERT_EQ(result, con);
    EXPECT_EQ(con->consttype, kTextOid);
    EXPECT_TRUE(con->constisnull);
    EXPECT_EQ(con->constvalue, static_cast<pgcpp::types::Datum>(0));
    // text is varlena: length -1, pass-by-reference.
    EXPECT_EQ(con->constlen, -1);
    EXPECT_FALSE(con->constbyval);
}

TEST_F(ParseCoerceMatrixTest, NullUnknownToBoolPreservesNullNoCoerceViaIO) {
    auto* con = MakeNullConst(kUnknownOid);
    Node* result = CoerceExplicit(con, kUnknownOid, kBoolOid);
    ASSERT_EQ(result, con);
    EXPECT_EQ(con->consttype, kBoolOid);
    EXPECT_TRUE(con->constisnull);
    EXPECT_EQ(con->constlen, 1);
    EXPECT_TRUE(con->constbyval);
}

TEST_F(ParseCoerceMatrixTest, NullUnknownToFloat8PreservesNullNoCoerceViaIO) {
    auto* con = MakeNullConst(kUnknownOid);
    Node* result = CoerceExplicit(con, kUnknownOid, kFloat8Oid);
    ASSERT_EQ(result, con);
    EXPECT_EQ(con->consttype, kFloat8Oid);
    EXPECT_TRUE(con->constisnull);
    EXPECT_EQ(con->constlen, 8);
    EXPECT_TRUE(con->constbyval);
}

TEST_F(ParseCoerceMatrixTest, NullUnknownToDatePreservesNullNoCoerceViaIO) {
    auto* con = MakeNullConst(kUnknownOid);
    Node* result = CoerceExplicit(con, kUnknownOid, kDateOid);
    ASSERT_EQ(result, con);
    EXPECT_EQ(con->consttype, kDateOid);
    EXPECT_TRUE(con->constisnull);
    EXPECT_EQ(con->constlen, 4);
    EXPECT_TRUE(con->constbyval);
}

TEST_F(ParseCoerceMatrixTest, NullUnknownToTimestampPreservesNullNoCoerceViaIO) {
    auto* con = MakeNullConst(kUnknownOid);
    Node* result = CoerceExplicit(con, kUnknownOid, kTimestampOid);
    ASSERT_EQ(result, con);
    EXPECT_EQ(con->consttype, kTimestampOid);
    EXPECT_TRUE(con->constisnull);
    EXPECT_EQ(con->constlen, 8);
    EXPECT_TRUE(con->constbyval);
}

// CoerceViaIO is the wrong node type for a NULL literal — verify the
// result is NOT a CoerceViaIO for any of the common target types. This
// catches regressions where the NULL short-circuit is removed and the
// function falls through to building a CoerceViaIO wrapper.
TEST_F(ParseCoerceMatrixTest, NullUnknownToAnyTargetNeverProducesCoerceViaIO) {
    const Oid targets[] = {kInt4Oid, kInt8Oid, kFloat8Oid,   kBoolOid,
                           kTextOid, kDateOid, kTimestampOid};
    for (Oid target : targets) {
        auto* con = MakeNullConst(kUnknownOid);
        Node* result = CoerceExplicit(con, kUnknownOid, target);
        ASSERT_NE(result, nullptr);
        EXPECT_NE(result->GetTag(), pgcpp::nodes::NodeTag::kCoerceViaIO)
            << "NULL literal to type " << target
            << " must not produce a CoerceViaIO (ExecEvalExpr does not support it)";
        EXPECT_EQ(result, con) << "NULL literal coercion must return the original Const in-place";
    }
}

// ===========================================================================
// can_coerce_type: implicit vs explicit context gatekeeping
// ===========================================================================

// --- int4 <-> bool: explicit only (PostgreSQL pg_cast: int4->bool = explicit,
//     bool->int4 = explicit).

TEST_F(ParseCoerceMatrixTest, Int4ToBoolImplicitNotAllowed) {
    EXPECT_FALSE(CanCoerceOne(kInt4Oid, kBoolOid, CoercionContext::kImplicit));
}

TEST_F(ParseCoerceMatrixTest, Int4ToBoolAssignmentNotAllowed) {
    EXPECT_FALSE(CanCoerceOne(kInt4Oid, kBoolOid, CoercionContext::kAssignment));
}

TEST_F(ParseCoerceMatrixTest, Int4ToBoolExplicitAllowed) {
    EXPECT_TRUE(CanCoerceOne(kInt4Oid, kBoolOid, CoercionContext::kExplicit));
}

TEST_F(ParseCoerceMatrixTest, BoolToInt4ExplicitAllowed) {
    EXPECT_TRUE(CanCoerceOne(kBoolOid, kInt4Oid, CoercionContext::kExplicit));
}

TEST_F(ParseCoerceMatrixTest, BoolToInt4ImplicitNotAllowed) {
    EXPECT_FALSE(CanCoerceOne(kBoolOid, kInt4Oid, CoercionContext::kImplicit));
}

// --- text <-> bool: explicit only.

TEST_F(ParseCoerceMatrixTest, TextToBoolExplicitAllowed) {
    EXPECT_TRUE(CanCoerceOne(kTextOid, kBoolOid, CoercionContext::kExplicit));
}

TEST_F(ParseCoerceMatrixTest, TextToBoolImplicitNotAllowed) {
    EXPECT_FALSE(CanCoerceOne(kTextOid, kBoolOid, CoercionContext::kImplicit));
}

TEST_F(ParseCoerceMatrixTest, BoolToTextExplicitAllowed) {
    EXPECT_TRUE(CanCoerceOne(kBoolOid, kTextOid, CoercionContext::kExplicit));
}

TEST_F(ParseCoerceMatrixTest, BoolToTextImplicitNotAllowed) {
    EXPECT_FALSE(CanCoerceOne(kBoolOid, kTextOid, CoercionContext::kImplicit));
}

// --- text <-> numeric: explicit only.

TEST_F(ParseCoerceMatrixTest, TextToInt4ExplicitAllowed) {
    EXPECT_TRUE(CanCoerceOne(kTextOid, kInt4Oid, CoercionContext::kExplicit));
}

TEST_F(ParseCoerceMatrixTest, TextToInt4ImplicitNotAllowed) {
    EXPECT_FALSE(CanCoerceOne(kTextOid, kInt4Oid, CoercionContext::kImplicit));
}

TEST_F(ParseCoerceMatrixTest, Int4ToTextExplicitAllowed) {
    EXPECT_TRUE(CanCoerceOne(kInt4Oid, kTextOid, CoercionContext::kExplicit));
}

TEST_F(ParseCoerceMatrixTest, Int4ToTextImplicitNotAllowed) {
    EXPECT_FALSE(CanCoerceOne(kInt4Oid, kTextOid, CoercionContext::kImplicit));
}

// --- date <-> timestamp: allowed in implicit/assignment AND explicit contexts
//     (PostgreSQL pg_cast: date->timestamp = assignment, timestamp->date =
//     assignment).

TEST_F(ParseCoerceMatrixTest, DateToTimestampImplicitAllowed) {
    EXPECT_TRUE(CanCoerceOne(kDateOid, kTimestampOid, CoercionContext::kImplicit));
}

TEST_F(ParseCoerceMatrixTest, DateToTimestampAssignmentAllowed) {
    EXPECT_TRUE(CanCoerceOne(kDateOid, kTimestampOid, CoercionContext::kAssignment));
}

TEST_F(ParseCoerceMatrixTest, DateToTimestampExplicitAllowed) {
    EXPECT_TRUE(CanCoerceOne(kDateOid, kTimestampOid, CoercionContext::kExplicit));
}

TEST_F(ParseCoerceMatrixTest, TimestampToDateImplicitAllowed) {
    EXPECT_TRUE(CanCoerceOne(kTimestampOid, kDateOid, CoercionContext::kImplicit));
}

TEST_F(ParseCoerceMatrixTest, TimestampToDateAssignmentAllowed) {
    EXPECT_TRUE(CanCoerceOne(kTimestampOid, kDateOid, CoercionContext::kAssignment));
}

TEST_F(ParseCoerceMatrixTest, TimestampToDateExplicitAllowed) {
    EXPECT_TRUE(CanCoerceOne(kTimestampOid, kDateOid, CoercionContext::kExplicit));
}

// --- float8 -> int4: not directly castable in PostgreSQL (no pg_cast entry
//     for float8->int4 with a CoercionContext other than explicit via
//     CAST()). Our can_coerce_type only allows numeric->numeric widening in
//     implicit context; narrowing is explicit-only via CoerceViaIO/Const-folding.
//     Since float8->int4 is narrowing (not binary-coercible), implicit is
//     NOT allowed; explicit IS allowed via the Numeric branch.

TEST_F(ParseCoerceMatrixTest, Float8ToInt4ImplicitNotAllowed) {
    EXPECT_FALSE(CanCoerceOne(kFloat8Oid, kInt4Oid, CoercionContext::kImplicit));
}

TEST_F(ParseCoerceMatrixTest, Float8ToInt4ExplicitNotAllowed) {
    // can_coerce_type's explicit branch does not have a generic
    // numeric->numeric narrowing rule; only String<->X and Bool<->X and
    // Datetime<->Datetime are gated as explicit. Narrowing within Numeric
    // is handled by coerce_type's Numeric branch directly (which does
    // Const-folding without consulting can_coerce_type for explicit casts).
    EXPECT_FALSE(CanCoerceOne(kFloat8Oid, kInt4Oid, CoercionContext::kExplicit));
}

// Direct coerce_type still performs the narrowing for explicit casts when
// called directly (transformTypeCast bypasses can_coerce_type).
TEST_F(ParseCoerceMatrixTest, Float8ToInt4DirectCoerceSucceeds) {
    auto* con = MakeFloat8Const(3.7);
    CoerceExplicit(con, kFloat8Oid, kInt4Oid);
    EXPECT_EQ(con->consttype, kInt4Oid);
    // float -> int truncates toward zero (C++ static_cast semantics).
    EXPECT_EQ(DatumGetInt32(con->constvalue), 3);
}

TEST_F(ParseCoerceMatrixTest, Float8ToInt4DirectCoerceNegative) {
    auto* con = MakeFloat8Const(-3.7);
    CoerceExplicit(con, kFloat8Oid, kInt4Oid);
    EXPECT_EQ(DatumGetInt32(con->constvalue), -3);
}

// ===========================================================================
// SQL-level integration tests (SELECT ... ::type casts)
// ===========================================================================

TEST_F(ParseCoerceMatrixTest, SqlSelectOneAsBoolean) {
    Query* qry = AnalyzeSingle("SELECT 1::BOOLEAN AS b");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kBoolOid);
    EXPECT_TRUE(DatumGetBool(con->constvalue));
}

TEST_F(ParseCoerceMatrixTest, SqlSelectZeroAsBoolean) {
    Query* qry = AnalyzeSingle("SELECT 0::BOOLEAN AS b");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kBoolOid);
    EXPECT_FALSE(DatumGetBool(con->constvalue));
}

TEST_F(ParseCoerceMatrixTest, SqlSelect42AsText) {
    Query* qry = AnalyzeSingle("SELECT 42::TEXT AS t");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kTextOid);
    EXPECT_EQ(TextDatumToString(con->constvalue), "42");
}

TEST_F(ParseCoerceMatrixTest, SqlSelectNegativeIntAsText) {
    // Parenthesize the literal so the parser sees (-7) as an int4 Const
    // before applying ::TEXT. Without parens, -7::TEXT parses as -(7::TEXT),
    // which would try to negate a text value (unsupported).
    Query* qry = AnalyzeSingle("SELECT (-7)::TEXT AS t");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kTextOid);
    EXPECT_EQ(TextDatumToString(con->constvalue), "-7");
}

TEST_F(ParseCoerceMatrixTest, SqlSelectTextCastToInt4) {
    // '42'::TEXT::INT4 — exercises both unknown->text and text->int4 paths.
    Query* qry = AnalyzeSingle("SELECT '42'::TEXT::INT4 AS i");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kInt4Oid);
    EXPECT_EQ(DatumGetInt32(con->constvalue), 42);
}

TEST_F(ParseCoerceMatrixTest, SqlSelectInvalidTextCastToInt4Errors) {
    EXPECT_TRUE(RaisesError([&] { AnalyzeSingle("SELECT 'abc'::TEXT::INT4 AS i"); }));
}

TEST_F(ParseCoerceMatrixTest, SqlSelectTextCastToFloat8) {
    Query* qry = AnalyzeSingle("SELECT '3.14'::TEXT::FLOAT8 AS f");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kFloat8Oid);
    EXPECT_DOUBLE_EQ(DatumGetFloat8(con->constvalue), 3.14);
}

TEST_F(ParseCoerceMatrixTest, SqlSelectTextCastToBoolT) {
    Query* qry = AnalyzeSingle("SELECT 't'::TEXT::BOOLEAN AS b");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kBoolOid);
    EXPECT_TRUE(DatumGetBool(con->constvalue));
}

TEST_F(ParseCoerceMatrixTest, SqlSelectTextCastToBoolF) {
    Query* qry = AnalyzeSingle("SELECT 'f'::TEXT::BOOLEAN AS b");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_FALSE(DatumGetBool(con->constvalue));
}

TEST_F(ParseCoerceMatrixTest, SqlSelectBoolCastToText) {
    Query* qry = AnalyzeSingle("SELECT TRUE::TEXT AS t");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kTextOid);
    EXPECT_EQ(TextDatumToString(con->constvalue), "t");
}

TEST_F(ParseCoerceMatrixTest, SqlSelectTimestampCastToDate) {
    // '2023-01-15 10:30:00'::TIMESTAMP::DATE — truncates time portion.
    Query* qry = AnalyzeSingle("SELECT '2023-01-15 10:30:00'::TIMESTAMP::DATE AS d");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kDateOid);
    // Verify by converting back to a date string.
    char* out = pgcpp::types::date_out(con->constvalue);
    EXPECT_STREQ(out, "2023-01-15");
}

TEST_F(ParseCoerceMatrixTest, SqlSelectDateCastToTimestamp) {
    Query* qry = AnalyzeSingle("SELECT '2023-01-15'::DATE::TIMESTAMP AS ts");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kTimestampOid);
    // Verify the timestamp is midnight of 2023-01-15.
    char* out = pgcpp::types::timestamp_out(con->constvalue);
    EXPECT_STREQ(out, "2023-01-15 00:00:00");
}

TEST_F(ParseCoerceMatrixTest, SqlSelectInt8CastToText) {
    // 9223372036854775807::INT8::TEXT — exercises int8->text via Const-folding.
    Query* qry = AnalyzeSingle("SELECT 9223372036854775807::INT8::TEXT AS t");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kTextOid);
    EXPECT_EQ(TextDatumToString(con->constvalue), "9223372036854775807");
}

TEST_F(ParseCoerceMatrixTest, SqlSelectTextCastToInt8) {
    Query* qry = AnalyzeSingle("SELECT '123456789012345'::TEXT::INT8 AS i");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kInt8Oid);
    EXPECT_EQ(DatumGetInt64(con->constvalue), 123456789012345LL);
}
