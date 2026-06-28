// extended_types_test.cpp — unit tests for M4 extended types (Task 15.19).
//
// Covers in/out/compare/operations for: char, name, xid, xid8, tid, pg_lsn,
// bytea, oid family, inet/cidr, macaddr, macaddr8, uuid, bit/varbit, array,
// json/jsonb, range types, rowtypes, geo types, enum types, acl, window
// functions, ordered-set aggregates, tsvector/tsquery, xml, selfuncs,
// ruleutils.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/types/acl.hpp"
#include "pgcpp/types/array.hpp"
#include "pgcpp/types/builtins.hpp"
#include "pgcpp/types/datum.hpp"
#include "pgcpp/types/enum_types.hpp"
#include "pgcpp/types/geo.hpp"
#include "pgcpp/types/json.hpp"
#include "pgcpp/types/misc_types.hpp"
#include "pgcpp/types/network_types.hpp"
#include "pgcpp/types/oid_types.hpp"
#include "pgcpp/types/orderedsetaggs.hpp"
#include "pgcpp/types/range.hpp"
#include "pgcpp/types/rowtypes.hpp"
#include "pgcpp/types/ruleutils.hpp"
#include "pgcpp/types/selfuncs.hpp"
#include "pgcpp/types/ts_types.hpp"
#include "pgcpp/types/uuid.hpp"
#include "pgcpp/types/varbit.hpp"
#include "pgcpp/types/windowfunctions.hpp"
#include "pgcpp/types/xml.hpp"

namespace {

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::types::BoolGetDatum;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::Float8GetDatum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::Int64GetDatum;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::MakeTextDatum;
using pgcpp::types::TextDatumToString;
using pgcpp::types::VARDATA;
using pgcpp::types::VARSIZE_DATA;

class ExtendedTypesTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
};

template<typename F>
bool RaisesError(F&& fn) {
    bool caught = false;
    PG_TRY() {
        fn();
    }
    PG_CATCH() {
        caught = true;
        ErrorData* err = pgcpp::error::GetErrorData();
        EXPECT_EQ(err->elevel, LogLevel::kError);
    }
    PG_END_TRY();
    return caught;
}

}  // namespace

// ===========================================================================
// char / name
// ===========================================================================

TEST_F(ExtendedTypesTest, CharRoundTrip) {
    auto d = pgcpp::types::char_in("a");
    EXPECT_STREQ(pgcpp::types::char_out(d), "a");
}

TEST_F(ExtendedTypesTest, CharCmp) {
    EXPECT_EQ(pgcpp::types::char_cmp(pgcpp::types::char_in("a"), pgcpp::types::char_in("b")), -1);
    EXPECT_TRUE(DatumGetBool(
        pgcpp::types::char_eq(pgcpp::types::char_in("x"), pgcpp::types::char_in("x"))));
}

TEST_F(ExtendedTypesTest, NameRoundTrip) {
    auto d = pgcpp::types::name_in("pg_class");
    EXPECT_STREQ(pgcpp::types::name_out(d), "pg_class");
}

TEST_F(ExtendedTypesTest, NameTruncatesTo63Chars) {
    std::string long_name(100, 'a');
    auto d = pgcpp::types::name_in(long_name.c_str());
    EXPECT_EQ(std::strlen(pgcpp::types::NameDatumToCString(d)), 63);
}

TEST_F(ExtendedTypesTest, NameCmp) {
    auto a = pgcpp::types::name_in("alpha");
    auto b = pgcpp::types::name_in("beta");
    EXPECT_EQ(pgcpp::types::name_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::name_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::name_ne(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::name_lt(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::name_le(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::name_gt(b, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::name_ge(b, a)));
}

// ===========================================================================
// xid / xid8
// ===========================================================================

TEST_F(ExtendedTypesTest, XidRoundTrip) {
    auto d = pgcpp::types::xid_in("12345");
    EXPECT_STREQ(pgcpp::types::xid_out(d), "12345");
}

TEST_F(ExtendedTypesTest, XidCmp) {
    auto a = pgcpp::types::xid_in("100");
    auto b = pgcpp::types::xid_in("200");
    EXPECT_EQ(pgcpp::types::xid_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::xid_eq(a, a)));
}

TEST_F(ExtendedTypesTest, Xid8RoundTrip) {
    auto d = pgcpp::types::xid8_in("18446744073709551615");
    EXPECT_STREQ(pgcpp::types::xid8_out(d), "18446744073709551615");
}

TEST_F(ExtendedTypesTest, XidInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::xid_in("notanumber"); }));
}

// ===========================================================================
// tid
// ===========================================================================

TEST_F(ExtendedTypesTest, TidRoundTrip) {
    auto d = pgcpp::types::tid_in("(5,10)");
    EXPECT_STREQ(pgcpp::types::tid_out(d), "(5,10)");
}

TEST_F(ExtendedTypesTest, TidCmp) {
    auto a = pgcpp::types::tid_in("(1,1)");
    auto b = pgcpp::types::tid_in("(1,2)");
    auto c = pgcpp::types::tid_in("(2,1)");
    EXPECT_EQ(pgcpp::types::tid_cmp(a, b), -1);
    EXPECT_EQ(pgcpp::types::tid_cmp(b, c), -1);
    EXPECT_EQ(pgcpp::types::tid_cmp(a, a), 0);
}

TEST_F(ExtendedTypesTest, TidInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::tid_in("5,10"); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::tid_in("(5)"); }));
}

// ===========================================================================
// pg_lsn
// ===========================================================================

TEST_F(ExtendedTypesTest, PgLsnRoundTrip) {
    auto d = pgcpp::types::pg_lsn_in("1/ABCDEF");
    EXPECT_STREQ(pgcpp::types::pg_lsn_out(d), "1/ABCDEF");
}

TEST_F(ExtendedTypesTest, PgLsnCmp) {
    auto a = pgcpp::types::pg_lsn_in("0/1");
    auto b = pgcpp::types::pg_lsn_in("0/2");
    EXPECT_EQ(pgcpp::types::pg_lsn_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::pg_lsn_lt(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::pg_lsn_eq(a, a)));
}

TEST_F(ExtendedTypesTest, PgLsnAdd) {
    auto a = pgcpp::types::pg_lsn_in("0/100");
    auto b = pgcpp::types::pg_lsn_add(a, Int64GetDatum(0x10));
    EXPECT_STREQ(pgcpp::types::pg_lsn_out(b), "0/110");
}

TEST_F(ExtendedTypesTest, PgLsnInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::pg_lsn_in("not/valid"); }));
}

// ===========================================================================
// bytea
// ===========================================================================

TEST_F(ExtendedTypesTest, ByteaRoundTripAscii) {
    auto d = pgcpp::types::bytea_in("hello");
    EXPECT_STREQ(pgcpp::types::bytea_out(d), "hello");
}

TEST_F(ExtendedTypesTest, ByteaEscapedRoundTrip) {
    auto d = pgcpp::types::bytea_in("\\000\\001\\377\\\\");
    char* out = pgcpp::types::bytea_out(d);
    EXPECT_STREQ(out, "\\000\\001\\377\\\\");
}

TEST_F(ExtendedTypesTest, ByteaCmp) {
    auto a = pgcpp::types::bytea_in("aaa");
    auto b = pgcpp::types::bytea_in("aab");
    EXPECT_EQ(pgcpp::types::bytea_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bytea_eq(a, a)));
}

TEST_F(ExtendedTypesTest, ByteaConcat) {
    auto a = pgcpp::types::bytea_in("foo");
    auto b = pgcpp::types::bytea_in("bar");
    auto c = pgcpp::types::bytea_concat(a, b);
    EXPECT_STREQ(pgcpp::types::bytea_out(c), "foobar");
}

TEST_F(ExtendedTypesTest, ByteaLength) {
    auto d = pgcpp::types::bytea_in("hello");
    EXPECT_EQ(DatumGetInt32(pgcpp::types::bytea_length(d)), 5);
}

// ===========================================================================
// oid family
// ===========================================================================

TEST_F(ExtendedTypesTest, OidRoundTrip) {
    auto d = pgcpp::types::oid_in("12345");
    EXPECT_STREQ(pgcpp::types::oid_out(d), "12345");
}

TEST_F(ExtendedTypesTest, OidCmp) {
    auto a = pgcpp::types::oid_in("100");
    auto b = pgcpp::types::oid_in("200");
    EXPECT_EQ(pgcpp::types::oid_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::oid_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::oid_lt(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::oid_gt(b, a)));
}

TEST_F(ExtendedTypesTest, RegprocNumericAndName) {
    pgcpp::types::ResetRegCatalogs();
    pgcpp::types::RegisterRegName(pgcpp::types::RegCatalog::kProc, 1242, "now");
    auto d1 = pgcpp::types::regproc_in("1242");
    EXPECT_STREQ(pgcpp::types::regproc_out(d1), "now");
    auto d2 = pgcpp::types::regproc_in("now");
    EXPECT_EQ(static_cast<uint32_t>(d2), 1242u);
}

TEST_F(ExtendedTypesTest, RegclassRoundTrip) {
    pgcpp::types::ResetRegCatalogs();
    pgcpp::types::RegisterRegName(pgcpp::types::RegCatalog::kClass, 1259, "pg_class");
    auto d = pgcpp::types::regclass_in("pg_class");
    EXPECT_STREQ(pgcpp::types::regclass_out(d), "pg_class");
}

TEST_F(ExtendedTypesTest, RegtypeUnknownFallsBackToOid) {
    pgcpp::types::ResetRegCatalogs();
    auto d = pgcpp::types::regtype_in("23");
    EXPECT_STREQ(pgcpp::types::regtype_out(d), "23");
}

TEST_F(ExtendedTypesTest, OidInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::oid_in("not_an_oid"); }));
}

// ===========================================================================
// inet / cidr
// ===========================================================================

TEST_F(ExtendedTypesTest, InetRoundTrip) {
    auto d = pgcpp::types::inet_in("192.168.1.1");
    EXPECT_STREQ(pgcpp::types::inet_out(d), "192.168.1.1");
}

TEST_F(ExtendedTypesTest, InetWithNetmask) {
    auto d = pgcpp::types::inet_in("192.168.1.0/24");
    EXPECT_STREQ(pgcpp::types::inet_out(d), "192.168.1.0/24");
}

TEST_F(ExtendedTypesTest, InetIpv6RoundTrip) {
    auto d = pgcpp::types::inet_in("::1");
    EXPECT_STREQ(pgcpp::types::inet_out(d), "0:0:0:0:0:0:0:1");
}

TEST_F(ExtendedTypesTest, InetCmp) {
    auto a = pgcpp::types::inet_in("10.0.0.1");
    auto b = pgcpp::types::inet_in("10.0.0.2");
    EXPECT_EQ(pgcpp::types::inet_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::inet_lt(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::inet_le(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::inet_gt(b, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::inet_ge(b, a)));
}

TEST_F(ExtendedTypesTest, CidrRejectsHostBits) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::cidr_in("192.168.1.5/24"); }));
    auto d = pgcpp::types::cidr_in("192.168.1.0/24");
    EXPECT_STREQ(pgcpp::types::cidr_out(d), "192.168.1.0/24");
}

TEST_F(ExtendedTypesTest, InetInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::inet_in("not_an_ip"); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::inet_in("999.1.1.1"); }));
}

// ===========================================================================
// macaddr / macaddr8
// ===========================================================================

TEST_F(ExtendedTypesTest, MacaddrRoundTrip) {
    auto d = pgcpp::types::macaddr_in("08:00:2b:01:02:03");
    EXPECT_STREQ(pgcpp::types::macaddr_out(d), "08:00:2b:01:02:03");
}

TEST_F(ExtendedTypesTest, MacaddrCmp) {
    auto a = pgcpp::types::macaddr_in("08:00:2b:01:02:03");
    auto b = pgcpp::types::macaddr_in("08:00:2b:01:02:04");
    EXPECT_EQ(pgcpp::types::macaddr_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::macaddr_eq(a, a)));
}

TEST_F(ExtendedTypesTest, Macaddr8RoundTrip) {
    auto d = pgcpp::types::macaddr8_in("08:00:2b:ff:fe:01:02:03");
    EXPECT_STREQ(pgcpp::types::macaddr8_out(d), "08:00:2b:ff:fe:01:02:03");
}

TEST_F(ExtendedTypesTest, Macaddr8Cmp) {
    auto a = pgcpp::types::macaddr8_in("08:00:2b:ff:fe:01:02:03");
    auto b = pgcpp::types::macaddr8_in("08:00:2b:ff:fe:01:02:04");
    EXPECT_EQ(pgcpp::types::macaddr8_cmp(a, b), -1);
}

TEST_F(ExtendedTypesTest, MacaddrInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::macaddr_in("08:00:2b:01:02"); }));
}

// ===========================================================================
// uuid
// ===========================================================================

TEST_F(ExtendedTypesTest, UuidRoundTrip) {
    auto d = pgcpp::types::uuid_in("550e8400-e29b-41d4-a716-446655440000");
    EXPECT_STREQ(pgcpp::types::uuid_out(d), "550e8400-e29b-41d4-a716-446655440000");
}

TEST_F(ExtendedTypesTest, UuidCmpAndOps) {
    auto a = pgcpp::types::uuid_in("00000000-0000-0000-0000-000000000001");
    auto b = pgcpp::types::uuid_in("00000000-0000-0000-0000-000000000002");
    EXPECT_EQ(pgcpp::types::uuid_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::uuid_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::uuid_ne(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::uuid_lt(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::uuid_le(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::uuid_gt(b, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::uuid_ge(b, a)));
}

TEST_F(ExtendedTypesTest, UuidInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::uuid_in("not-a-uuid"); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::uuid_in("550e8400-e29b-41d4-a716"); }));
}

// ===========================================================================
// bit / varbit
// ===========================================================================

TEST_F(ExtendedTypesTest, VarbitRoundTrip) {
    auto d = pgcpp::types::varbit_in("10110", -1);
    EXPECT_STREQ(pgcpp::types::varbit_out(d), "10110");
}

TEST_F(ExtendedTypesTest, BitRoundTrip) {
    auto d = pgcpp::types::bit_in("1011", 4);
    EXPECT_STREQ(pgcpp::types::bit_out(d), "1011");
}

TEST_F(ExtendedTypesTest, VarbitLength) {
    auto d = pgcpp::types::varbit_in_default("10110");
    EXPECT_EQ(DatumGetInt32(pgcpp::types::bit_length(d)), 5);
}

TEST_F(ExtendedTypesTest, VarbitLogicalOps) {
    auto a = pgcpp::types::varbit_in_default("10110");
    auto b = pgcpp::types::varbit_in_default("11001");
    EXPECT_STREQ(pgcpp::types::bit_out(pgcpp::types::bit_and(a, b)), "10000");
    EXPECT_STREQ(pgcpp::types::bit_out(pgcpp::types::bit_or(a, b)), "11111");
    EXPECT_STREQ(pgcpp::types::bit_out(pgcpp::types::bit_xor(a, b)), "01111");
    EXPECT_STREQ(pgcpp::types::bit_out(pgcpp::types::bit_not(a)), "01001");
}

TEST_F(ExtendedTypesTest, VarbitCmp) {
    auto a = pgcpp::types::varbit_in_default("0100");
    auto b = pgcpp::types::varbit_in_default("1100");
    EXPECT_EQ(pgcpp::types::varbit_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::varbit_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::varbit_lt(a, b)));
}

TEST_F(ExtendedTypesTest, VarbitInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::varbit_in("10a", -1); }));
}

// ===========================================================================
// array
// ===========================================================================

TEST_F(ExtendedTypesTest, ArrayInt4RoundTrip) {
    auto d = pgcpp::types::array_in("{1,2,3}", kInt4Oid, -1);
    EXPECT_STREQ(pgcpp::types::array_out(d), "{1,2,3}");
}

TEST_F(ExtendedTypesTest, ArrayTextRoundTrip) {
    auto d = pgcpp::types::array_in("{abc,def,ghi}", kTextOid, -1);
    EXPECT_STREQ(pgcpp::types::array_out(d), "{abc,def,ghi}");
}

TEST_F(ExtendedTypesTest, ArrayWithNulls) {
    auto d = pgcpp::types::array_in("{1,NULL,3}", kInt4Oid, -1);
    EXPECT_STREQ(pgcpp::types::array_out(d), "{1,NULL,3}");
}

TEST_F(ExtendedTypesTest, ArrayWithQuotedElements) {
    auto d = pgcpp::types::array_in("{\"a,b\",\"c\"}", kTextOid, -1);
    EXPECT_STREQ(pgcpp::types::array_out(d), "{\"a,b\",c}");
}

TEST_F(ExtendedTypesTest, ArrayEmpty) {
    auto d = pgcpp::types::array_in("{}", kInt4Oid, -1);
    EXPECT_STREQ(pgcpp::types::array_out(d), "{}");
}

TEST_F(ExtendedTypesTest, ArrayLengthAndNdims) {
    auto d = pgcpp::types::array_in("{1,2,3,4,5}", kInt4Oid, -1);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::array_length(d, Int32GetDatum(1))), 5);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::array_ndims(d)), 1);
}

TEST_F(ExtendedTypesTest, ArrayAppend) {
    auto d = pgcpp::types::array_in("{1,2}", kInt4Oid, -1);
    auto e = pgcpp::types::array_append(d, Int32GetDatum(3));
    EXPECT_STREQ(pgcpp::types::array_out(e), "{1,2,3}");
}

TEST_F(ExtendedTypesTest, ArrayCmp) {
    auto a = pgcpp::types::array_in("{1,2,3}", kInt4Oid, -1);
    auto b = pgcpp::types::array_in("{1,2,4}", kInt4Oid, -1);
    EXPECT_EQ(pgcpp::types::array_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::array_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::array_lt(a, b)));
}

// ===========================================================================
// json / jsonb
// ===========================================================================

TEST_F(ExtendedTypesTest, JsonRoundTripScalar) {
    auto d = pgcpp::types::json_in("true");
    EXPECT_STREQ(pgcpp::types::json_out(d), "true");
}

TEST_F(ExtendedTypesTest, JsonRoundTripString) {
    auto d = pgcpp::types::json_in("\"hello\"");
    EXPECT_STREQ(pgcpp::types::json_out(d), "\"hello\"");
}

TEST_F(ExtendedTypesTest, JsonRoundTripNumber) {
    auto d = pgcpp::types::json_in("42");
    EXPECT_STREQ(pgcpp::types::json_out(d), "42");
}

TEST_F(ExtendedTypesTest, JsonRoundTripArray) {
    auto d = pgcpp::types::json_in("[1,2,3]");
    EXPECT_STREQ(pgcpp::types::json_out(d), "[1,2,3]");
}

TEST_F(ExtendedTypesTest, JsonRoundTripObject) {
    auto d = pgcpp::types::json_in("{\"a\":1,\"b\":\"x\"}");
    EXPECT_STREQ(pgcpp::types::json_out(d), "{\"a\":1,\"b\":\"x\"}");
}

TEST_F(ExtendedTypesTest, JsonEscapes) {
    auto d = pgcpp::types::json_in("\"\\n\\t\\\"\\\\\"");
    EXPECT_STREQ(pgcpp::types::json_out(d), "\"\\n\\t\\\"\\\\\"");
}

TEST_F(ExtendedTypesTest, JsonbRoundTrip) {
    auto d = pgcpp::types::jsonb_in("{\"x\":[1,2]}");
    EXPECT_STREQ(pgcpp::types::jsonb_out(d), "{\"x\":[1,2]}");
}

TEST_F(ExtendedTypesTest, JsonInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::json_in("not json"); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::json_in("[1,2"); }));
}

// ===========================================================================
// range types
// ===========================================================================

TEST_F(ExtendedTypesTest, Int4RangeRoundTrip) {
    // pgcpp does not implement PostgreSQL's canonical-form transformation
    // for discrete range types, so [1,10] round-trips as [1,10] (not [1,11)).
    auto d = pgcpp::types::int4range_in("[1,10]");
    EXPECT_STREQ(pgcpp::types::int4range_out(d), "[1,10]");
}

TEST_F(ExtendedTypesTest, Int4RangeExclusiveUpper) {
    auto d = pgcpp::types::int4range_in("(5,15)");
    EXPECT_STREQ(pgcpp::types::int4range_out(d), "(5,15)");
}

TEST_F(ExtendedTypesTest, Int4RangeUnbounded) {
    auto d = pgcpp::types::int4range_in("[1,)");
    EXPECT_STREQ(pgcpp::types::int4range_out(d), "[1,)");
}

TEST_F(ExtendedTypesTest, Int4RangeEmpty) {
    auto d = pgcpp::types::int4range_in("empty");
    EXPECT_STREQ(pgcpp::types::int4range_out(d), "empty");
}

TEST_F(ExtendedTypesTest, Int8RangeRoundTrip) {
    // See Int4RangeRoundTrip: no canonical-form transformation is applied.
    auto d = pgcpp::types::int8range_in("[10000000000,20000000000]");
    EXPECT_STREQ(pgcpp::types::int8range_out(d), "[10000000000,20000000000]");
}

TEST_F(ExtendedTypesTest, RangeCmpAndOps) {
    auto a = pgcpp::types::int4range_in("[1,5]");
    auto b = pgcpp::types::int4range_in("[1,5]");
    auto c = pgcpp::types::int4range_in("[6,10]");
    EXPECT_TRUE(DatumGetBool(pgcpp::types::range_eq(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::range_ne(a, c)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::range_lt(a, c)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::range_le(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::range_gt(c, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::range_ge(c, c)));
}

TEST_F(ExtendedTypesTest, RangeContainsElem) {
    auto r = pgcpp::types::int4range_in("[1,10]");
    EXPECT_TRUE(DatumGetBool(pgcpp::types::range_contains_elem(r, Int32GetDatum(5))));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::range_contains_elem(r, Int32GetDatum(11))));
}

TEST_F(ExtendedTypesTest, RangeInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int4range_in("[1,5"); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int4range_in("1,5]"); }));
}

// ===========================================================================
// rowtypes
// ===========================================================================

TEST_F(ExtendedTypesTest, RowRoundTrip) {
    auto d = pgcpp::types::row_in("(1,2,3)", 0, -1);
    EXPECT_STREQ(pgcpp::types::row_out(d), "(1,2,3)");
}

TEST_F(ExtendedTypesTest, RowWithNulls) {
    auto d = pgcpp::types::row_in("(1,,3)", 0, -1);
    EXPECT_STREQ(pgcpp::types::row_out(d), "(1,,3)");
}

TEST_F(ExtendedTypesTest, RowWithQuotedStrings) {
    auto d = pgcpp::types::row_in("(hello,world)", 0, -1);
    EXPECT_STREQ(pgcpp::types::row_out(d), "(hello,world)");
}

TEST_F(ExtendedTypesTest, RowCmp) {
    auto a = pgcpp::types::row_in("(1,2,3)", 0, -1);
    auto b = pgcpp::types::row_in("(1,2,4)", 0, -1);
    EXPECT_EQ(pgcpp::types::row_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::row_eq(a, a)));
}

// ===========================================================================
// geometric types
// ===========================================================================

TEST_F(ExtendedTypesTest, PointRoundTrip) {
    auto d = pgcpp::types::point_in("(1.5,2.5)");
    EXPECT_STREQ(pgcpp::types::point_out(d), "(1.5,2.5)");
}

TEST_F(ExtendedTypesTest, PointCmpAndDistance) {
    auto a = pgcpp::types::point_in("(0,0)");
    auto b = pgcpp::types::point_in("(3,4)");
    EXPECT_EQ(pgcpp::types::point_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::point_eq(a, a)));
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::point_distance(a, b)), 5.0, 1e-9);
}

TEST_F(ExtendedTypesTest, LsegRoundTripAndLength) {
    auto d = pgcpp::types::lseg_in("[(0,0),(3,4)]");
    EXPECT_STREQ(pgcpp::types::lseg_out(d), "[(0,0),(3,4)]");
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::lseg_length(d)), 5.0, 1e-9);
}

TEST_F(ExtendedTypesTest, BoxRoundTrip) {
    auto d = pgcpp::types::box_in("((0,0),(2,2))");
    EXPECT_STREQ(pgcpp::types::box_out(d), "((2,2),(0,0))");
}

TEST_F(ExtendedTypesTest, BoxArea) {
    auto d = pgcpp::types::box_in("((0,0),(3,4))");
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::box_area(d)), 12.0, 1e-9);
}

TEST_F(ExtendedTypesTest, BoxWidthHeight) {
    auto d = pgcpp::types::box_in("((0,0),(3,4))");
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::box_width(d)), 3.0, 1e-9);
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::box_height(d)), 4.0, 1e-9);
}

TEST_F(ExtendedTypesTest, LineRoundTrip) {
    auto d = pgcpp::types::line_in("{1,2,3}");
    EXPECT_STREQ(pgcpp::types::line_out(d), "{1,2,3}");
}

TEST_F(ExtendedTypesTest, PathRoundTripAndLength) {
    auto d = pgcpp::types::path_in("((0,0),(1,0),(1,1),(0,1))");
    EXPECT_STREQ(pgcpp::types::path_out(d), "((0,0),(1,0),(1,1),(0,1))");
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::path_length(d)), 4.0, 1e-9);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::path_npoints(d)), 4);
}

TEST_F(ExtendedTypesTest, CircleRoundTripAndArea) {
    auto d = pgcpp::types::circle_in("<(0,0),5>");
    EXPECT_STREQ(pgcpp::types::circle_out(d), "<(0,0),5>");
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::circle_area(d)), M_PI * 25.0, 1e-6);
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::circle_radius(d)), 5.0, 1e-9);
}

// ===========================================================================
// enum types
// ===========================================================================

TEST_F(ExtendedTypesTest, EnumRegistration) {
    pgcpp::types::EnumResetRegistry();
    pgcpp::types::EnumRegisterLabel(1000, "small");
    pgcpp::types::EnumRegisterLabel(1000, "medium");
    pgcpp::types::EnumRegisterLabel(1000, "large");
    EXPECT_EQ(pgcpp::types::EnumLookupLabel(1000, "medium"), 2);
    EXPECT_STREQ(pgcpp::types::EnumLookupSortorder(1000, 3), "large");
}

TEST_F(ExtendedTypesTest, EnumInRoundTrip) {
    pgcpp::types::EnumResetRegistry();
    pgcpp::types::EnumRegisterLabel(1000, "low");
    pgcpp::types::EnumRegisterLabel(1000, "mid");
    pgcpp::types::EnumRegisterLabel(1000, "high");
    auto d = pgcpp::types::enum_in("mid", 1000);
    EXPECT_EQ(DatumGetInt32(d), 2);
    EXPECT_STREQ(pgcpp::types::enum_out(d, 1000), "mid");
}

TEST_F(ExtendedTypesTest, EnumCmp) {
    auto a = Int32GetDatum(1);
    auto b = Int32GetDatum(2);
    EXPECT_EQ(pgcpp::types::enum_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(pgcpp::types::enum_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::enum_lt(a, b)));
}

TEST_F(ExtendedTypesTest, EnumInvalidRaises) {
    pgcpp::types::EnumResetRegistry();
    pgcpp::types::EnumRegisterLabel(1000, "low");
    EXPECT_TRUE(RaisesError([] { pgcpp::types::enum_in("nonexistent", 1000); }));
}

// ===========================================================================
// acl
// ===========================================================================

TEST_F(ExtendedTypesTest, AclRoundTrip) {
    auto d = pgcpp::types::acl_in("{1=arwd/2}");
    EXPECT_STREQ(pgcpp::types::acl_out(d), "{1=arwd/2}");
}

TEST_F(ExtendedTypesTest, AclMultipleItems) {
    auto d = pgcpp::types::acl_in("{1=ar/2,3=rw/4}");
    EXPECT_STREQ(pgcpp::types::acl_out(d), "{1=ar/2,3=rw/4}");
}

TEST_F(ExtendedTypesTest, AclItemConstruction) {
    auto d =
        pgcpp::types::MakeAclItemDatum(10, 20, pgcpp::types::kAclSelect | pgcpp::types::kAclInsert);
    EXPECT_STREQ(pgcpp::types::acl_out(d), "{10=ar/20}");
}

TEST_F(ExtendedTypesTest, AclInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::acl_in("{1=q/2}"); }));
}

// ===========================================================================
// window functions
// ===========================================================================

TEST_F(ExtendedTypesTest, RowNumber) {
    auto s = pgcpp::types::row_number_reset();
    EXPECT_EQ(DatumGetInt64(pgcpp::types::row_number_next(s)), 1);
    EXPECT_EQ(DatumGetInt64(pgcpp::types::row_number_next(Int64GetDatum(1))), 2);
}

TEST_F(ExtendedTypesTest, Rank) {
    auto s = pgcpp::types::rank_init();
    s = pgcpp::types::rank_advance(s, Int32GetDatum(1));
    EXPECT_EQ(DatumGetInt64(pgcpp::types::rank_value(s)), 1);
    s = pgcpp::types::rank_advance(s, Int32GetDatum(1));
    EXPECT_EQ(DatumGetInt64(pgcpp::types::rank_value(s)), 1);
    s = pgcpp::types::rank_advance(s, Int32GetDatum(2));
    EXPECT_EQ(DatumGetInt64(pgcpp::types::rank_value(s)), 3);
}

TEST_F(ExtendedTypesTest, DenseRank) {
    auto s = pgcpp::types::dense_rank_init();
    s = pgcpp::types::dense_rank_advance(s, Int32GetDatum(1));
    EXPECT_EQ(DatumGetInt64(pgcpp::types::dense_rank_value(s)), 1);
    s = pgcpp::types::dense_rank_advance(s, Int32GetDatum(1));
    EXPECT_EQ(DatumGetInt64(pgcpp::types::dense_rank_value(s)), 1);
    s = pgcpp::types::dense_rank_advance(s, Int32GetDatum(2));
    EXPECT_EQ(DatumGetInt64(pgcpp::types::dense_rank_value(s)), 2);
}

TEST_F(ExtendedTypesTest, LagAndLead) {
    std::vector<Datum> v = {Int32GetDatum(10), Int32GetDatum(20), Int32GetDatum(30)};
    EXPECT_EQ(DatumGetInt32(pgcpp::types::lag_compute(v, 0, 1, Int32GetDatum(-1))), -1);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::lag_compute(v, 1, 1, Int32GetDatum(-1))), 10);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::lead_compute(v, 1, 1, Int32GetDatum(-1))), 30);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::lead_compute(v, 2, 1, Int32GetDatum(-1))), -1);
}

TEST_F(ExtendedTypesTest, FirstLastNth) {
    std::vector<Datum> v = {Int32GetDatum(10), Int32GetDatum(20), Int32GetDatum(30)};
    EXPECT_EQ(DatumGetInt32(pgcpp::types::first_value(v)), 10);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::last_value(v)), 30);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::nth_value(v, 2)), 20);
}

// ===========================================================================
// ordered-set aggregates
// ===========================================================================

TEST_F(ExtendedTypesTest, OrderedSetMode) {
    std::vector<Datum> v = {Int32GetDatum(1), Int32GetDatum(2), Int32GetDatum(2), Int32GetDatum(2),
                            Int32GetDatum(3)};
    EXPECT_EQ(DatumGetInt32(pgcpp::types::ordered_set_mode(v, pgcpp::types::int4_cmp)), 2);
}

TEST_F(ExtendedTypesTest, OrderedSetPercentileDisc) {
    std::vector<Datum> v = {Int32GetDatum(1), Int32GetDatum(2), Int32GetDatum(3), Int32GetDatum(4),
                            Int32GetDatum(5)};
    EXPECT_EQ(DatumGetInt32(pgcpp::types::ordered_set_percentile_disc(v, Float8GetDatum(0.5),
                                                                      pgcpp::types::int4_cmp)),
              3);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::ordered_set_percentile_disc(v, Float8GetDatum(1.0),
                                                                      pgcpp::types::int4_cmp)),
              5);
}

TEST_F(ExtendedTypesTest, OrderedSetPercentileContInt4) {
    std::vector<Datum> v = {Int32GetDatum(10), Int32GetDatum(20), Int32GetDatum(30),
                            Int32GetDatum(40), Int32GetDatum(50)};
    EXPECT_NEAR(
        DatumGetFloat8(pgcpp::types::ordered_set_percentile_cont_int4(v, Float8GetDatum(0.5))),
        30.0, 1e-9);
}

TEST_F(ExtendedTypesTest, OrderedSetPercentileContFloat8) {
    std::vector<Datum> v = {Float8GetDatum(10.0), Float8GetDatum(20.0), Float8GetDatum(30.0)};
    EXPECT_NEAR(
        DatumGetFloat8(pgcpp::types::ordered_set_percentile_cont_float8(v, Float8GetDatum(0.25))),
        15.0, 1e-9);
}

// ===========================================================================
// tsvector / tsquery
// ===========================================================================

TEST_F(ExtendedTypesTest, TsVectorRoundTrip) {
    auto d = pgcpp::types::tsvector_in("a quick brown fox");
    char* out = pgcpp::types::tsvector_out(d);
    // Sorted alphabetically.
    EXPECT_STREQ(out, "a brown fox quick");
}

TEST_F(ExtendedTypesTest, TsVectorWithPositions) {
    auto d = pgcpp::types::tsvector_in("hello:1,2 world:3");
    char* out = pgcpp::types::tsvector_out(d);
    EXPECT_NE(std::string(out).find("hello:1,2"), std::string::npos);
}

TEST_F(ExtendedTypesTest, TsQueryAndMatch) {
    auto v = pgcpp::types::tsvector_in("hello world");
    auto q = pgcpp::types::tsquery_in("hello & world");
    EXPECT_TRUE(DatumGetBool(pgcpp::types::ts_match(v, q)));
    auto q2 = pgcpp::types::tsquery_in("hello | foo");
    EXPECT_TRUE(DatumGetBool(pgcpp::types::ts_match(v, q2)));
    auto q3 = pgcpp::types::tsquery_in("hello & foo");
    EXPECT_FALSE(DatumGetBool(pgcpp::types::ts_match(v, q3)));
}

// ===========================================================================
// xml
// ===========================================================================

TEST_F(ExtendedTypesTest, XmlRoundTrip) {
    auto d = pgcpp::types::xml_in("<root>hello</root>");
    EXPECT_STREQ(pgcpp::types::xml_out(d), "<root>hello</root>");
}

TEST_F(ExtendedTypesTest, XmlValidate) {
    EXPECT_TRUE(DatumGetBool(
        pgcpp::types::xml_validate(pgcpp::types::xml_in("<root><a>1</a><b>2</b></root>"))));
    EXPECT_FALSE(
        DatumGetBool(pgcpp::types::xml_validate(pgcpp::types::xml_in("<root><a>1</a></b>"))));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::xml_validate(pgcpp::types::xml_in("<root>1</a>"))));
}

TEST_F(ExtendedTypesTest, XmlConcat) {
    auto a = pgcpp::types::xml_in("<a/>");
    auto b = pgcpp::types::xml_in("<b/>");
    auto c = pgcpp::types::xml_concat(a, b);
    EXPECT_STREQ(pgcpp::types::xml_out(c), "<a/><b/>");
}

TEST_F(ExtendedTypesTest, XpathExistsSubstring) {
    auto xml = pgcpp::types::xml_in("<root>hello world</root>");
    auto xpath = MakeTextDatum("world");
    EXPECT_TRUE(DatumGetBool(pgcpp::types::xpath_exists(xml, xpath)));
}

// ===========================================================================
// selfuncs
// ===========================================================================

TEST_F(ExtendedTypesTest, SelfuncsEqsel) {
    double sel = pgcpp::types::eqsel(0.1, 100, true);
    EXPECT_NEAR(sel, 0.9 / 100.0, 1e-9);
}

TEST_F(ExtendedTypesTest, SelfuncsScalarLtGt) {
    EXPECT_NEAR(pgcpp::types::scalarltsel(0.5), 0.5, 1e-9);
    EXPECT_NEAR(pgcpp::types::scalargtsel(0.5), 0.5, 1e-9);
}

TEST_F(ExtendedTypesTest, SelfuncsEqjoin) {
    EXPECT_NEAR(pgcpp::types::eqjoinsel_inner(10, 100), 0.01, 1e-9);
}

// ===========================================================================
// ruleutils
// ===========================================================================

TEST_F(ExtendedTypesTest, RuleutilsQuoteIdentifier) {
    EXPECT_EQ(pgcpp::types::QuoteIdentifier("foo"), "foo");
    EXPECT_EQ(pgcpp::types::QuoteIdentifier("select"), "\"select\"");
    EXPECT_EQ(pgcpp::types::QuoteIdentifier("123abc"), "\"123abc\"");
    EXPECT_EQ(pgcpp::types::QuoteIdentifier("has space"), "\"has space\"");
}

TEST_F(ExtendedTypesTest, RuleutilsDeparseLiteral) {
    EXPECT_EQ(pgcpp::types::DeparseLiteral(kInt4Oid, Int32GetDatum(42), false), "42");
    EXPECT_EQ(pgcpp::types::DeparseLiteral(kInt4Oid, 0, true), "NULL");
    EXPECT_EQ(pgcpp::types::DeparseLiteral(kTextOid, MakeTextDatum("it's"), false), "'it''s'");
}

TEST_F(ExtendedTypesTest, RuleutilsFormatOperatorName) {
    EXPECT_EQ(pgcpp::types::FormatOperatorName("", "+"), "+");
    EXPECT_EQ(pgcpp::types::FormatOperatorName("pg_catalog", "="), "pg_catalog.=");
}
