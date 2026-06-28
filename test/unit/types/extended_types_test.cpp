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

using mytoydb::error::ErrorData;
using mytoydb::error::LogLevel;
using mytoydb::memory::AllocSetContext;
using mytoydb::types::BoolGetDatum;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetBool;
using mytoydb::types::DatumGetFloat8;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::DatumGetTextP;
using mytoydb::types::Float8GetDatum;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::Int64GetDatum;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::MakeTextDatum;
using mytoydb::types::TextDatumToString;
using mytoydb::types::VARDATA;
using mytoydb::types::VARSIZE_DATA;

class ExtendedTypesTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
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
        ErrorData* err = mytoydb::error::GetErrorData();
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
    auto d = mytoydb::types::char_in("a");
    EXPECT_STREQ(mytoydb::types::char_out(d), "a");
}

TEST_F(ExtendedTypesTest, CharCmp) {
    EXPECT_EQ(mytoydb::types::char_cmp(mytoydb::types::char_in("a"), mytoydb::types::char_in("b")),
              -1);
    EXPECT_TRUE(DatumGetBool(
        mytoydb::types::char_eq(mytoydb::types::char_in("x"), mytoydb::types::char_in("x"))));
}

TEST_F(ExtendedTypesTest, NameRoundTrip) {
    auto d = mytoydb::types::name_in("pg_class");
    EXPECT_STREQ(mytoydb::types::name_out(d), "pg_class");
}

TEST_F(ExtendedTypesTest, NameTruncatesTo63Chars) {
    std::string long_name(100, 'a');
    auto d = mytoydb::types::name_in(long_name.c_str());
    EXPECT_EQ(std::strlen(mytoydb::types::NameDatumToCString(d)), 63);
}

TEST_F(ExtendedTypesTest, NameCmp) {
    auto a = mytoydb::types::name_in("alpha");
    auto b = mytoydb::types::name_in("beta");
    EXPECT_EQ(mytoydb::types::name_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::name_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::name_ne(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::name_lt(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::name_le(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::name_gt(b, a)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::name_ge(b, a)));
}

// ===========================================================================
// xid / xid8
// ===========================================================================

TEST_F(ExtendedTypesTest, XidRoundTrip) {
    auto d = mytoydb::types::xid_in("12345");
    EXPECT_STREQ(mytoydb::types::xid_out(d), "12345");
}

TEST_F(ExtendedTypesTest, XidCmp) {
    auto a = mytoydb::types::xid_in("100");
    auto b = mytoydb::types::xid_in("200");
    EXPECT_EQ(mytoydb::types::xid_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::xid_eq(a, a)));
}

TEST_F(ExtendedTypesTest, Xid8RoundTrip) {
    auto d = mytoydb::types::xid8_in("18446744073709551615");
    EXPECT_STREQ(mytoydb::types::xid8_out(d), "18446744073709551615");
}

TEST_F(ExtendedTypesTest, XidInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::xid_in("notanumber"); }));
}

// ===========================================================================
// tid
// ===========================================================================

TEST_F(ExtendedTypesTest, TidRoundTrip) {
    auto d = mytoydb::types::tid_in("(5,10)");
    EXPECT_STREQ(mytoydb::types::tid_out(d), "(5,10)");
}

TEST_F(ExtendedTypesTest, TidCmp) {
    auto a = mytoydb::types::tid_in("(1,1)");
    auto b = mytoydb::types::tid_in("(1,2)");
    auto c = mytoydb::types::tid_in("(2,1)");
    EXPECT_EQ(mytoydb::types::tid_cmp(a, b), -1);
    EXPECT_EQ(mytoydb::types::tid_cmp(b, c), -1);
    EXPECT_EQ(mytoydb::types::tid_cmp(a, a), 0);
}

TEST_F(ExtendedTypesTest, TidInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::tid_in("5,10"); }));
    EXPECT_TRUE(RaisesError([] { mytoydb::types::tid_in("(5)"); }));
}

// ===========================================================================
// pg_lsn
// ===========================================================================

TEST_F(ExtendedTypesTest, PgLsnRoundTrip) {
    auto d = mytoydb::types::pg_lsn_in("1/ABCDEF");
    EXPECT_STREQ(mytoydb::types::pg_lsn_out(d), "1/ABCDEF");
}

TEST_F(ExtendedTypesTest, PgLsnCmp) {
    auto a = mytoydb::types::pg_lsn_in("0/1");
    auto b = mytoydb::types::pg_lsn_in("0/2");
    EXPECT_EQ(mytoydb::types::pg_lsn_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::pg_lsn_lt(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::pg_lsn_eq(a, a)));
}

TEST_F(ExtendedTypesTest, PgLsnAdd) {
    auto a = mytoydb::types::pg_lsn_in("0/100");
    auto b = mytoydb::types::pg_lsn_add(a, Int64GetDatum(0x10));
    EXPECT_STREQ(mytoydb::types::pg_lsn_out(b), "0/110");
}

TEST_F(ExtendedTypesTest, PgLsnInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::pg_lsn_in("not/valid"); }));
}

// ===========================================================================
// bytea
// ===========================================================================

TEST_F(ExtendedTypesTest, ByteaRoundTripAscii) {
    auto d = mytoydb::types::bytea_in("hello");
    EXPECT_STREQ(mytoydb::types::bytea_out(d), "hello");
}

TEST_F(ExtendedTypesTest, ByteaEscapedRoundTrip) {
    auto d = mytoydb::types::bytea_in("\\000\\001\\377\\\\");
    char* out = mytoydb::types::bytea_out(d);
    EXPECT_STREQ(out, "\\000\\001\\377\\\\");
}

TEST_F(ExtendedTypesTest, ByteaCmp) {
    auto a = mytoydb::types::bytea_in("aaa");
    auto b = mytoydb::types::bytea_in("aab");
    EXPECT_EQ(mytoydb::types::bytea_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::bytea_eq(a, a)));
}

TEST_F(ExtendedTypesTest, ByteaConcat) {
    auto a = mytoydb::types::bytea_in("foo");
    auto b = mytoydb::types::bytea_in("bar");
    auto c = mytoydb::types::bytea_concat(a, b);
    EXPECT_STREQ(mytoydb::types::bytea_out(c), "foobar");
}

TEST_F(ExtendedTypesTest, ByteaLength) {
    auto d = mytoydb::types::bytea_in("hello");
    EXPECT_EQ(DatumGetInt32(mytoydb::types::bytea_length(d)), 5);
}

// ===========================================================================
// oid family
// ===========================================================================

TEST_F(ExtendedTypesTest, OidRoundTrip) {
    auto d = mytoydb::types::oid_in("12345");
    EXPECT_STREQ(mytoydb::types::oid_out(d), "12345");
}

TEST_F(ExtendedTypesTest, OidCmp) {
    auto a = mytoydb::types::oid_in("100");
    auto b = mytoydb::types::oid_in("200");
    EXPECT_EQ(mytoydb::types::oid_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::oid_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::oid_lt(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::oid_gt(b, a)));
}

TEST_F(ExtendedTypesTest, RegprocNumericAndName) {
    mytoydb::types::ResetRegCatalogs();
    mytoydb::types::RegisterRegName(mytoydb::types::RegCatalog::kProc, 1242, "now");
    auto d1 = mytoydb::types::regproc_in("1242");
    EXPECT_STREQ(mytoydb::types::regproc_out(d1), "now");
    auto d2 = mytoydb::types::regproc_in("now");
    EXPECT_EQ(static_cast<uint32_t>(d2), 1242u);
}

TEST_F(ExtendedTypesTest, RegclassRoundTrip) {
    mytoydb::types::ResetRegCatalogs();
    mytoydb::types::RegisterRegName(mytoydb::types::RegCatalog::kClass, 1259, "pg_class");
    auto d = mytoydb::types::regclass_in("pg_class");
    EXPECT_STREQ(mytoydb::types::regclass_out(d), "pg_class");
}

TEST_F(ExtendedTypesTest, RegtypeUnknownFallsBackToOid) {
    mytoydb::types::ResetRegCatalogs();
    auto d = mytoydb::types::regtype_in("23");
    EXPECT_STREQ(mytoydb::types::regtype_out(d), "23");
}

TEST_F(ExtendedTypesTest, OidInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::oid_in("not_an_oid"); }));
}

// ===========================================================================
// inet / cidr
// ===========================================================================

TEST_F(ExtendedTypesTest, InetRoundTrip) {
    auto d = mytoydb::types::inet_in("192.168.1.1");
    EXPECT_STREQ(mytoydb::types::inet_out(d), "192.168.1.1");
}

TEST_F(ExtendedTypesTest, InetWithNetmask) {
    auto d = mytoydb::types::inet_in("192.168.1.0/24");
    EXPECT_STREQ(mytoydb::types::inet_out(d), "192.168.1.0/24");
}

TEST_F(ExtendedTypesTest, InetIpv6RoundTrip) {
    auto d = mytoydb::types::inet_in("::1");
    EXPECT_STREQ(mytoydb::types::inet_out(d), "0:0:0:0:0:0:0:1");
}

TEST_F(ExtendedTypesTest, InetCmp) {
    auto a = mytoydb::types::inet_in("10.0.0.1");
    auto b = mytoydb::types::inet_in("10.0.0.2");
    EXPECT_EQ(mytoydb::types::inet_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::inet_lt(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::inet_le(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::inet_gt(b, a)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::inet_ge(b, a)));
}

TEST_F(ExtendedTypesTest, CidrRejectsHostBits) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::cidr_in("192.168.1.5/24"); }));
    auto d = mytoydb::types::cidr_in("192.168.1.0/24");
    EXPECT_STREQ(mytoydb::types::cidr_out(d), "192.168.1.0/24");
}

TEST_F(ExtendedTypesTest, InetInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::inet_in("not_an_ip"); }));
    EXPECT_TRUE(RaisesError([] { mytoydb::types::inet_in("999.1.1.1"); }));
}

// ===========================================================================
// macaddr / macaddr8
// ===========================================================================

TEST_F(ExtendedTypesTest, MacaddrRoundTrip) {
    auto d = mytoydb::types::macaddr_in("08:00:2b:01:02:03");
    EXPECT_STREQ(mytoydb::types::macaddr_out(d), "08:00:2b:01:02:03");
}

TEST_F(ExtendedTypesTest, MacaddrCmp) {
    auto a = mytoydb::types::macaddr_in("08:00:2b:01:02:03");
    auto b = mytoydb::types::macaddr_in("08:00:2b:01:02:04");
    EXPECT_EQ(mytoydb::types::macaddr_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::macaddr_eq(a, a)));
}

TEST_F(ExtendedTypesTest, Macaddr8RoundTrip) {
    auto d = mytoydb::types::macaddr8_in("08:00:2b:ff:fe:01:02:03");
    EXPECT_STREQ(mytoydb::types::macaddr8_out(d), "08:00:2b:ff:fe:01:02:03");
}

TEST_F(ExtendedTypesTest, Macaddr8Cmp) {
    auto a = mytoydb::types::macaddr8_in("08:00:2b:ff:fe:01:02:03");
    auto b = mytoydb::types::macaddr8_in("08:00:2b:ff:fe:01:02:04");
    EXPECT_EQ(mytoydb::types::macaddr8_cmp(a, b), -1);
}

TEST_F(ExtendedTypesTest, MacaddrInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::macaddr_in("08:00:2b:01:02"); }));
}

// ===========================================================================
// uuid
// ===========================================================================

TEST_F(ExtendedTypesTest, UuidRoundTrip) {
    auto d = mytoydb::types::uuid_in("550e8400-e29b-41d4-a716-446655440000");
    EXPECT_STREQ(mytoydb::types::uuid_out(d), "550e8400-e29b-41d4-a716-446655440000");
}

TEST_F(ExtendedTypesTest, UuidCmpAndOps) {
    auto a = mytoydb::types::uuid_in("00000000-0000-0000-0000-000000000001");
    auto b = mytoydb::types::uuid_in("00000000-0000-0000-0000-000000000002");
    EXPECT_EQ(mytoydb::types::uuid_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::uuid_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::uuid_ne(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::uuid_lt(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::uuid_le(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::uuid_gt(b, a)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::uuid_ge(b, a)));
}

TEST_F(ExtendedTypesTest, UuidInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::uuid_in("not-a-uuid"); }));
    EXPECT_TRUE(RaisesError([] { mytoydb::types::uuid_in("550e8400-e29b-41d4-a716"); }));
}

// ===========================================================================
// bit / varbit
// ===========================================================================

TEST_F(ExtendedTypesTest, VarbitRoundTrip) {
    auto d = mytoydb::types::varbit_in("10110", -1);
    EXPECT_STREQ(mytoydb::types::varbit_out(d), "10110");
}

TEST_F(ExtendedTypesTest, BitRoundTrip) {
    auto d = mytoydb::types::bit_in("1011", 4);
    EXPECT_STREQ(mytoydb::types::bit_out(d), "1011");
}

TEST_F(ExtendedTypesTest, VarbitLength) {
    auto d = mytoydb::types::varbit_in_default("10110");
    EXPECT_EQ(DatumGetInt32(mytoydb::types::bit_length(d)), 5);
}

TEST_F(ExtendedTypesTest, VarbitLogicalOps) {
    auto a = mytoydb::types::varbit_in_default("10110");
    auto b = mytoydb::types::varbit_in_default("11001");
    EXPECT_STREQ(mytoydb::types::bit_out(mytoydb::types::bit_and(a, b)), "10000");
    EXPECT_STREQ(mytoydb::types::bit_out(mytoydb::types::bit_or(a, b)), "11111");
    EXPECT_STREQ(mytoydb::types::bit_out(mytoydb::types::bit_xor(a, b)), "01111");
    EXPECT_STREQ(mytoydb::types::bit_out(mytoydb::types::bit_not(a)), "01001");
}

TEST_F(ExtendedTypesTest, VarbitCmp) {
    auto a = mytoydb::types::varbit_in_default("0100");
    auto b = mytoydb::types::varbit_in_default("1100");
    EXPECT_EQ(mytoydb::types::varbit_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::varbit_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::varbit_lt(a, b)));
}

TEST_F(ExtendedTypesTest, VarbitInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::varbit_in("10a", -1); }));
}

// ===========================================================================
// array
// ===========================================================================

TEST_F(ExtendedTypesTest, ArrayInt4RoundTrip) {
    auto d = mytoydb::types::array_in("{1,2,3}", kInt4Oid, -1);
    EXPECT_STREQ(mytoydb::types::array_out(d), "{1,2,3}");
}

TEST_F(ExtendedTypesTest, ArrayTextRoundTrip) {
    auto d = mytoydb::types::array_in("{abc,def,ghi}", kTextOid, -1);
    EXPECT_STREQ(mytoydb::types::array_out(d), "{abc,def,ghi}");
}

TEST_F(ExtendedTypesTest, ArrayWithNulls) {
    auto d = mytoydb::types::array_in("{1,NULL,3}", kInt4Oid, -1);
    EXPECT_STREQ(mytoydb::types::array_out(d), "{1,NULL,3}");
}

TEST_F(ExtendedTypesTest, ArrayWithQuotedElements) {
    auto d = mytoydb::types::array_in("{\"a,b\",\"c\"}", kTextOid, -1);
    EXPECT_STREQ(mytoydb::types::array_out(d), "{\"a,b\",c}");
}

TEST_F(ExtendedTypesTest, ArrayEmpty) {
    auto d = mytoydb::types::array_in("{}", kInt4Oid, -1);
    EXPECT_STREQ(mytoydb::types::array_out(d), "{}");
}

TEST_F(ExtendedTypesTest, ArrayLengthAndNdims) {
    auto d = mytoydb::types::array_in("{1,2,3,4,5}", kInt4Oid, -1);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::array_length(d, Int32GetDatum(1))), 5);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::array_ndims(d)), 1);
}

TEST_F(ExtendedTypesTest, ArrayAppend) {
    auto d = mytoydb::types::array_in("{1,2}", kInt4Oid, -1);
    auto e = mytoydb::types::array_append(d, Int32GetDatum(3));
    EXPECT_STREQ(mytoydb::types::array_out(e), "{1,2,3}");
}

TEST_F(ExtendedTypesTest, ArrayCmp) {
    auto a = mytoydb::types::array_in("{1,2,3}", kInt4Oid, -1);
    auto b = mytoydb::types::array_in("{1,2,4}", kInt4Oid, -1);
    EXPECT_EQ(mytoydb::types::array_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::array_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::array_lt(a, b)));
}

// ===========================================================================
// json / jsonb
// ===========================================================================

TEST_F(ExtendedTypesTest, JsonRoundTripScalar) {
    auto d = mytoydb::types::json_in("true");
    EXPECT_STREQ(mytoydb::types::json_out(d), "true");
}

TEST_F(ExtendedTypesTest, JsonRoundTripString) {
    auto d = mytoydb::types::json_in("\"hello\"");
    EXPECT_STREQ(mytoydb::types::json_out(d), "\"hello\"");
}

TEST_F(ExtendedTypesTest, JsonRoundTripNumber) {
    auto d = mytoydb::types::json_in("42");
    EXPECT_STREQ(mytoydb::types::json_out(d), "42");
}

TEST_F(ExtendedTypesTest, JsonRoundTripArray) {
    auto d = mytoydb::types::json_in("[1,2,3]");
    EXPECT_STREQ(mytoydb::types::json_out(d), "[1,2,3]");
}

TEST_F(ExtendedTypesTest, JsonRoundTripObject) {
    auto d = mytoydb::types::json_in("{\"a\":1,\"b\":\"x\"}");
    EXPECT_STREQ(mytoydb::types::json_out(d), "{\"a\":1,\"b\":\"x\"}");
}

TEST_F(ExtendedTypesTest, JsonEscapes) {
    auto d = mytoydb::types::json_in("\"\\n\\t\\\"\\\\\"");
    EXPECT_STREQ(mytoydb::types::json_out(d), "\"\\n\\t\\\"\\\\\"");
}

TEST_F(ExtendedTypesTest, JsonbRoundTrip) {
    auto d = mytoydb::types::jsonb_in("{\"x\":[1,2]}");
    EXPECT_STREQ(mytoydb::types::jsonb_out(d), "{\"x\":[1,2]}");
}

TEST_F(ExtendedTypesTest, JsonInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::json_in("not json"); }));
    EXPECT_TRUE(RaisesError([] { mytoydb::types::json_in("[1,2"); }));
}

// ===========================================================================
// range types
// ===========================================================================

TEST_F(ExtendedTypesTest, Int4RangeRoundTrip) {
    // MyToyDB does not implement PostgreSQL's canonical-form transformation
    // for discrete range types, so [1,10] round-trips as [1,10] (not [1,11)).
    auto d = mytoydb::types::int4range_in("[1,10]");
    EXPECT_STREQ(mytoydb::types::int4range_out(d), "[1,10]");
}

TEST_F(ExtendedTypesTest, Int4RangeExclusiveUpper) {
    auto d = mytoydb::types::int4range_in("(5,15)");
    EXPECT_STREQ(mytoydb::types::int4range_out(d), "(5,15)");
}

TEST_F(ExtendedTypesTest, Int4RangeUnbounded) {
    auto d = mytoydb::types::int4range_in("[1,)");
    EXPECT_STREQ(mytoydb::types::int4range_out(d), "[1,)");
}

TEST_F(ExtendedTypesTest, Int4RangeEmpty) {
    auto d = mytoydb::types::int4range_in("empty");
    EXPECT_STREQ(mytoydb::types::int4range_out(d), "empty");
}

TEST_F(ExtendedTypesTest, Int8RangeRoundTrip) {
    // See Int4RangeRoundTrip: no canonical-form transformation is applied.
    auto d = mytoydb::types::int8range_in("[10000000000,20000000000]");
    EXPECT_STREQ(mytoydb::types::int8range_out(d), "[10000000000,20000000000]");
}

TEST_F(ExtendedTypesTest, RangeCmpAndOps) {
    auto a = mytoydb::types::int4range_in("[1,5]");
    auto b = mytoydb::types::int4range_in("[1,5]");
    auto c = mytoydb::types::int4range_in("[6,10]");
    EXPECT_TRUE(DatumGetBool(mytoydb::types::range_eq(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::range_ne(a, c)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::range_lt(a, c)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::range_le(a, b)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::range_gt(c, a)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::range_ge(c, c)));
}

TEST_F(ExtendedTypesTest, RangeContainsElem) {
    auto r = mytoydb::types::int4range_in("[1,10]");
    EXPECT_TRUE(DatumGetBool(mytoydb::types::range_contains_elem(r, Int32GetDatum(5))));
    EXPECT_FALSE(DatumGetBool(mytoydb::types::range_contains_elem(r, Int32GetDatum(11))));
}

TEST_F(ExtendedTypesTest, RangeInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::int4range_in("[1,5"); }));
    EXPECT_TRUE(RaisesError([] { mytoydb::types::int4range_in("1,5]"); }));
}

// ===========================================================================
// rowtypes
// ===========================================================================

TEST_F(ExtendedTypesTest, RowRoundTrip) {
    auto d = mytoydb::types::row_in("(1,2,3)", 0, -1);
    EXPECT_STREQ(mytoydb::types::row_out(d), "(1,2,3)");
}

TEST_F(ExtendedTypesTest, RowWithNulls) {
    auto d = mytoydb::types::row_in("(1,,3)", 0, -1);
    EXPECT_STREQ(mytoydb::types::row_out(d), "(1,,3)");
}

TEST_F(ExtendedTypesTest, RowWithQuotedStrings) {
    auto d = mytoydb::types::row_in("(hello,world)", 0, -1);
    EXPECT_STREQ(mytoydb::types::row_out(d), "(hello,world)");
}

TEST_F(ExtendedTypesTest, RowCmp) {
    auto a = mytoydb::types::row_in("(1,2,3)", 0, -1);
    auto b = mytoydb::types::row_in("(1,2,4)", 0, -1);
    EXPECT_EQ(mytoydb::types::row_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::row_eq(a, a)));
}

// ===========================================================================
// geometric types
// ===========================================================================

TEST_F(ExtendedTypesTest, PointRoundTrip) {
    auto d = mytoydb::types::point_in("(1.5,2.5)");
    EXPECT_STREQ(mytoydb::types::point_out(d), "(1.5,2.5)");
}

TEST_F(ExtendedTypesTest, PointCmpAndDistance) {
    auto a = mytoydb::types::point_in("(0,0)");
    auto b = mytoydb::types::point_in("(3,4)");
    EXPECT_EQ(mytoydb::types::point_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::point_eq(a, a)));
    EXPECT_NEAR(DatumGetFloat8(mytoydb::types::point_distance(a, b)), 5.0, 1e-9);
}

TEST_F(ExtendedTypesTest, LsegRoundTripAndLength) {
    auto d = mytoydb::types::lseg_in("[(0,0),(3,4)]");
    EXPECT_STREQ(mytoydb::types::lseg_out(d), "[(0,0),(3,4)]");
    EXPECT_NEAR(DatumGetFloat8(mytoydb::types::lseg_length(d)), 5.0, 1e-9);
}

TEST_F(ExtendedTypesTest, BoxRoundTrip) {
    auto d = mytoydb::types::box_in("((0,0),(2,2))");
    EXPECT_STREQ(mytoydb::types::box_out(d), "((2,2),(0,0))");
}

TEST_F(ExtendedTypesTest, BoxArea) {
    auto d = mytoydb::types::box_in("((0,0),(3,4))");
    EXPECT_NEAR(DatumGetFloat8(mytoydb::types::box_area(d)), 12.0, 1e-9);
}

TEST_F(ExtendedTypesTest, BoxWidthHeight) {
    auto d = mytoydb::types::box_in("((0,0),(3,4))");
    EXPECT_NEAR(DatumGetFloat8(mytoydb::types::box_width(d)), 3.0, 1e-9);
    EXPECT_NEAR(DatumGetFloat8(mytoydb::types::box_height(d)), 4.0, 1e-9);
}

TEST_F(ExtendedTypesTest, LineRoundTrip) {
    auto d = mytoydb::types::line_in("{1,2,3}");
    EXPECT_STREQ(mytoydb::types::line_out(d), "{1,2,3}");
}

TEST_F(ExtendedTypesTest, PathRoundTripAndLength) {
    auto d = mytoydb::types::path_in("((0,0),(1,0),(1,1),(0,1))");
    EXPECT_STREQ(mytoydb::types::path_out(d), "((0,0),(1,0),(1,1),(0,1))");
    EXPECT_NEAR(DatumGetFloat8(mytoydb::types::path_length(d)), 4.0, 1e-9);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::path_npoints(d)), 4);
}

TEST_F(ExtendedTypesTest, CircleRoundTripAndArea) {
    auto d = mytoydb::types::circle_in("<(0,0),5>");
    EXPECT_STREQ(mytoydb::types::circle_out(d), "<(0,0),5>");
    EXPECT_NEAR(DatumGetFloat8(mytoydb::types::circle_area(d)), M_PI * 25.0, 1e-6);
    EXPECT_NEAR(DatumGetFloat8(mytoydb::types::circle_radius(d)), 5.0, 1e-9);
}

// ===========================================================================
// enum types
// ===========================================================================

TEST_F(ExtendedTypesTest, EnumRegistration) {
    mytoydb::types::EnumResetRegistry();
    mytoydb::types::EnumRegisterLabel(1000, "small");
    mytoydb::types::EnumRegisterLabel(1000, "medium");
    mytoydb::types::EnumRegisterLabel(1000, "large");
    EXPECT_EQ(mytoydb::types::EnumLookupLabel(1000, "medium"), 2);
    EXPECT_STREQ(mytoydb::types::EnumLookupSortorder(1000, 3), "large");
}

TEST_F(ExtendedTypesTest, EnumInRoundTrip) {
    mytoydb::types::EnumResetRegistry();
    mytoydb::types::EnumRegisterLabel(1000, "low");
    mytoydb::types::EnumRegisterLabel(1000, "mid");
    mytoydb::types::EnumRegisterLabel(1000, "high");
    auto d = mytoydb::types::enum_in("mid", 1000);
    EXPECT_EQ(DatumGetInt32(d), 2);
    EXPECT_STREQ(mytoydb::types::enum_out(d, 1000), "mid");
}

TEST_F(ExtendedTypesTest, EnumCmp) {
    auto a = Int32GetDatum(1);
    auto b = Int32GetDatum(2);
    EXPECT_EQ(mytoydb::types::enum_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(mytoydb::types::enum_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::enum_lt(a, b)));
}

TEST_F(ExtendedTypesTest, EnumInvalidRaises) {
    mytoydb::types::EnumResetRegistry();
    mytoydb::types::EnumRegisterLabel(1000, "low");
    EXPECT_TRUE(RaisesError([] { mytoydb::types::enum_in("nonexistent", 1000); }));
}

// ===========================================================================
// acl
// ===========================================================================

TEST_F(ExtendedTypesTest, AclRoundTrip) {
    auto d = mytoydb::types::acl_in("{1=arwd/2}");
    EXPECT_STREQ(mytoydb::types::acl_out(d), "{1=arwd/2}");
}

TEST_F(ExtendedTypesTest, AclMultipleItems) {
    auto d = mytoydb::types::acl_in("{1=ar/2,3=rw/4}");
    EXPECT_STREQ(mytoydb::types::acl_out(d), "{1=ar/2,3=rw/4}");
}

TEST_F(ExtendedTypesTest, AclItemConstruction) {
    auto d = mytoydb::types::MakeAclItemDatum(
        10, 20, mytoydb::types::kAclSelect | mytoydb::types::kAclInsert);
    EXPECT_STREQ(mytoydb::types::acl_out(d), "{10=ar/20}");
}

TEST_F(ExtendedTypesTest, AclInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::acl_in("{1=q/2}"); }));
}

// ===========================================================================
// window functions
// ===========================================================================

TEST_F(ExtendedTypesTest, RowNumber) {
    auto s = mytoydb::types::row_number_reset();
    EXPECT_EQ(DatumGetInt64(mytoydb::types::row_number_next(s)), 1);
    EXPECT_EQ(DatumGetInt64(mytoydb::types::row_number_next(Int64GetDatum(1))), 2);
}

TEST_F(ExtendedTypesTest, Rank) {
    auto s = mytoydb::types::rank_init();
    s = mytoydb::types::rank_advance(s, Int32GetDatum(1));
    EXPECT_EQ(DatumGetInt64(mytoydb::types::rank_value(s)), 1);
    s = mytoydb::types::rank_advance(s, Int32GetDatum(1));
    EXPECT_EQ(DatumGetInt64(mytoydb::types::rank_value(s)), 1);
    s = mytoydb::types::rank_advance(s, Int32GetDatum(2));
    EXPECT_EQ(DatumGetInt64(mytoydb::types::rank_value(s)), 3);
}

TEST_F(ExtendedTypesTest, DenseRank) {
    auto s = mytoydb::types::dense_rank_init();
    s = mytoydb::types::dense_rank_advance(s, Int32GetDatum(1));
    EXPECT_EQ(DatumGetInt64(mytoydb::types::dense_rank_value(s)), 1);
    s = mytoydb::types::dense_rank_advance(s, Int32GetDatum(1));
    EXPECT_EQ(DatumGetInt64(mytoydb::types::dense_rank_value(s)), 1);
    s = mytoydb::types::dense_rank_advance(s, Int32GetDatum(2));
    EXPECT_EQ(DatumGetInt64(mytoydb::types::dense_rank_value(s)), 2);
}

TEST_F(ExtendedTypesTest, LagAndLead) {
    std::vector<Datum> v = {Int32GetDatum(10), Int32GetDatum(20), Int32GetDatum(30)};
    EXPECT_EQ(DatumGetInt32(mytoydb::types::lag_compute(v, 0, 1, Int32GetDatum(-1))), -1);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::lag_compute(v, 1, 1, Int32GetDatum(-1))), 10);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::lead_compute(v, 1, 1, Int32GetDatum(-1))), 30);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::lead_compute(v, 2, 1, Int32GetDatum(-1))), -1);
}

TEST_F(ExtendedTypesTest, FirstLastNth) {
    std::vector<Datum> v = {Int32GetDatum(10), Int32GetDatum(20), Int32GetDatum(30)};
    EXPECT_EQ(DatumGetInt32(mytoydb::types::first_value(v)), 10);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::last_value(v)), 30);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::nth_value(v, 2)), 20);
}

// ===========================================================================
// ordered-set aggregates
// ===========================================================================

TEST_F(ExtendedTypesTest, OrderedSetMode) {
    std::vector<Datum> v = {Int32GetDatum(1), Int32GetDatum(2), Int32GetDatum(2), Int32GetDatum(2),
                            Int32GetDatum(3)};
    EXPECT_EQ(DatumGetInt32(mytoydb::types::ordered_set_mode(v, mytoydb::types::int4_cmp)), 2);
}

TEST_F(ExtendedTypesTest, OrderedSetPercentileDisc) {
    std::vector<Datum> v = {Int32GetDatum(1), Int32GetDatum(2), Int32GetDatum(3), Int32GetDatum(4),
                            Int32GetDatum(5)};
    EXPECT_EQ(DatumGetInt32(mytoydb::types::ordered_set_percentile_disc(v, Float8GetDatum(0.5),
                                                                        mytoydb::types::int4_cmp)),
              3);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::ordered_set_percentile_disc(v, Float8GetDatum(1.0),
                                                                        mytoydb::types::int4_cmp)),
              5);
}

TEST_F(ExtendedTypesTest, OrderedSetPercentileContInt4) {
    std::vector<Datum> v = {Int32GetDatum(10), Int32GetDatum(20), Int32GetDatum(30),
                            Int32GetDatum(40), Int32GetDatum(50)};
    EXPECT_NEAR(
        DatumGetFloat8(mytoydb::types::ordered_set_percentile_cont_int4(v, Float8GetDatum(0.5))),
        30.0, 1e-9);
}

TEST_F(ExtendedTypesTest, OrderedSetPercentileContFloat8) {
    std::vector<Datum> v = {Float8GetDatum(10.0), Float8GetDatum(20.0), Float8GetDatum(30.0)};
    EXPECT_NEAR(
        DatumGetFloat8(mytoydb::types::ordered_set_percentile_cont_float8(v, Float8GetDatum(0.25))),
        15.0, 1e-9);
}

// ===========================================================================
// tsvector / tsquery
// ===========================================================================

TEST_F(ExtendedTypesTest, TsVectorRoundTrip) {
    auto d = mytoydb::types::tsvector_in("a quick brown fox");
    char* out = mytoydb::types::tsvector_out(d);
    // Sorted alphabetically.
    EXPECT_STREQ(out, "a brown fox quick");
}

TEST_F(ExtendedTypesTest, TsVectorWithPositions) {
    auto d = mytoydb::types::tsvector_in("hello:1,2 world:3");
    char* out = mytoydb::types::tsvector_out(d);
    EXPECT_NE(std::string(out).find("hello:1,2"), std::string::npos);
}

TEST_F(ExtendedTypesTest, TsQueryAndMatch) {
    auto v = mytoydb::types::tsvector_in("hello world");
    auto q = mytoydb::types::tsquery_in("hello & world");
    EXPECT_TRUE(DatumGetBool(mytoydb::types::ts_match(v, q)));
    auto q2 = mytoydb::types::tsquery_in("hello | foo");
    EXPECT_TRUE(DatumGetBool(mytoydb::types::ts_match(v, q2)));
    auto q3 = mytoydb::types::tsquery_in("hello & foo");
    EXPECT_FALSE(DatumGetBool(mytoydb::types::ts_match(v, q3)));
}

// ===========================================================================
// xml
// ===========================================================================

TEST_F(ExtendedTypesTest, XmlRoundTrip) {
    auto d = mytoydb::types::xml_in("<root>hello</root>");
    EXPECT_STREQ(mytoydb::types::xml_out(d), "<root>hello</root>");
}

TEST_F(ExtendedTypesTest, XmlValidate) {
    EXPECT_TRUE(DatumGetBool(
        mytoydb::types::xml_validate(mytoydb::types::xml_in("<root><a>1</a><b>2</b></root>"))));
    EXPECT_FALSE(
        DatumGetBool(mytoydb::types::xml_validate(mytoydb::types::xml_in("<root><a>1</a></b>"))));
    EXPECT_FALSE(DatumGetBool(mytoydb::types::xml_validate(mytoydb::types::xml_in("<root>1</a>"))));
}

TEST_F(ExtendedTypesTest, XmlConcat) {
    auto a = mytoydb::types::xml_in("<a/>");
    auto b = mytoydb::types::xml_in("<b/>");
    auto c = mytoydb::types::xml_concat(a, b);
    EXPECT_STREQ(mytoydb::types::xml_out(c), "<a/><b/>");
}

TEST_F(ExtendedTypesTest, XpathExistsSubstring) {
    auto xml = mytoydb::types::xml_in("<root>hello world</root>");
    auto xpath = MakeTextDatum("world");
    EXPECT_TRUE(DatumGetBool(mytoydb::types::xpath_exists(xml, xpath)));
}

// ===========================================================================
// selfuncs
// ===========================================================================

TEST_F(ExtendedTypesTest, SelfuncsEqsel) {
    double sel = mytoydb::types::eqsel(0.1, 100, true);
    EXPECT_NEAR(sel, 0.9 / 100.0, 1e-9);
}

TEST_F(ExtendedTypesTest, SelfuncsScalarLtGt) {
    EXPECT_NEAR(mytoydb::types::scalarltsel(0.5), 0.5, 1e-9);
    EXPECT_NEAR(mytoydb::types::scalargtsel(0.5), 0.5, 1e-9);
}

TEST_F(ExtendedTypesTest, SelfuncsEqjoin) {
    EXPECT_NEAR(mytoydb::types::eqjoinsel_inner(10, 100), 0.01, 1e-9);
}

// ===========================================================================
// ruleutils
// ===========================================================================

TEST_F(ExtendedTypesTest, RuleutilsQuoteIdentifier) {
    EXPECT_EQ(mytoydb::types::QuoteIdentifier("foo"), "foo");
    EXPECT_EQ(mytoydb::types::QuoteIdentifier("select"), "\"select\"");
    EXPECT_EQ(mytoydb::types::QuoteIdentifier("123abc"), "\"123abc\"");
    EXPECT_EQ(mytoydb::types::QuoteIdentifier("has space"), "\"has space\"");
}

TEST_F(ExtendedTypesTest, RuleutilsDeparseLiteral) {
    EXPECT_EQ(mytoydb::types::DeparseLiteral(kInt4Oid, Int32GetDatum(42), false), "42");
    EXPECT_EQ(mytoydb::types::DeparseLiteral(kInt4Oid, 0, true), "NULL");
    EXPECT_EQ(mytoydb::types::DeparseLiteral(kTextOid, MakeTextDatum("it's"), false), "'it''s'");
}

TEST_F(ExtendedTypesTest, RuleutilsFormatOperatorName) {
    EXPECT_EQ(mytoydb::types::FormatOperatorName("", "+"), "+");
    EXPECT_EQ(mytoydb::types::FormatOperatorName("pg_catalog", "="), "pg_catalog.=");
}
