#include "mytoydb/types/datetime.h"

#include <gtest/gtest.h>

#include <string>

#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/alloc_set.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/types/datum.h"

namespace {

using mytoydb::error::ErrorData;
using mytoydb::error::LogLevel;
using mytoydb::memory::AllocSetContext;
using mytoydb::types::Date;
using mytoydb::types::date_cmp;
using mytoydb::types::date_in;
using mytoydb::types::date_out;
using mytoydb::types::date_trunc;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::extract;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::Int64GetDatum;
using mytoydb::types::PartsToTimestamp;
using mytoydb::types::Timestamp;
using mytoydb::types::timestamp_cmp;
using mytoydb::types::timestamp_in;
using mytoydb::types::timestamp_out;
using mytoydb::types::TimestampParts;
using mytoydb::types::TimestampToParts;

class DatetimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("datetime_test_context");
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

// Returns true if the given callable ereports(ERROR).
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

// ===========================================================================
// timestamp input / output
// ===========================================================================

TEST_F(DatetimeTest, TimestampInBasic) {
    Datum d = timestamp_in("2013-07-15 10:30:45");
    Timestamp ts = DatumGetInt64(d);
    TimestampParts p = TimestampToParts(ts);
    EXPECT_EQ(p.year, 2013);
    EXPECT_EQ(p.month, 7);
    EXPECT_EQ(p.day, 15);
    EXPECT_EQ(p.hour, 10);
    EXPECT_EQ(p.minute, 30);
    EXPECT_EQ(p.second, 45);
    EXPECT_EQ(p.microsec, 0);
}

TEST_F(DatetimeTest, TimestampInWithMicroseconds) {
    Datum d = timestamp_in("2013-07-15 10:30:45.123456");
    Timestamp ts = DatumGetInt64(d);
    TimestampParts p = TimestampToParts(ts);
    EXPECT_EQ(p.year, 2013);
    EXPECT_EQ(p.month, 7);
    EXPECT_EQ(p.day, 15);
    EXPECT_EQ(p.hour, 10);
    EXPECT_EQ(p.minute, 30);
    EXPECT_EQ(p.second, 45);
    EXPECT_EQ(p.microsec, 123456);
}

TEST_F(DatetimeTest, TimestampOutBasic) {
    Datum d = timestamp_in("2013-07-15 10:30:45");
    char* out = timestamp_out(d);
    EXPECT_STREQ(out, "2013-07-15 10:30:45");
}

TEST_F(DatetimeTest, TimestampRoundTrip) {
    const char* inputs[] = {
        "2000-01-01 00:00:00",     // epoch
        "2013-07-15 23:59:59",     // end of day
        "1999-12-31 23:59:59",     // before epoch
        "2020-02-29 12:00:00",     // leap day
        "1900-01-01 00:00:00",     // far past
        "2100-01-01 00:00:00",     // far future (non-leap century)
    };
    for (const char* input : inputs) {
        Datum d = timestamp_in(input);
        char* out = timestamp_out(d);
        EXPECT_STREQ(out, input) << "Round-trip failed for: " << input;
    }
}

TEST_F(DatetimeTest, TimestampInInvalid) {
    EXPECT_TRUE(RaisesError([] { timestamp_in("not a date"); }));
    EXPECT_TRUE(RaisesError([] { timestamp_in("2013-13-01 00:00:00"); }));  // bad month
    EXPECT_TRUE(RaisesError([] { timestamp_in("2013-00-01 00:00:00"); }));  // bad month
    EXPECT_TRUE(RaisesError([] { timestamp_in("2013-02-30 00:00:00"); }));  // bad day
    EXPECT_TRUE(RaisesError([] { timestamp_in("2013-07-15 25:00:00"); }));  // bad hour
}

TEST_F(DatetimeTest, TimestampEpochIsZero) {
    Datum d = timestamp_in("2000-01-01 00:00:00");
    EXPECT_EQ(DatumGetInt64(d), 0);
}

// ===========================================================================
// date input / output
// ===========================================================================

TEST_F(DatetimeTest, DateInBasic) {
    Datum d = date_in("2013-07-15");
    // Verify by converting back.
    char* out = date_out(d);
    EXPECT_STREQ(out, "2013-07-15");
}

TEST_F(DatetimeTest, DateEpochIsZero) {
    Datum d = date_in("2000-01-01");
    EXPECT_EQ(DatumGetInt32(d), 0);
}

TEST_F(DatetimeTest, DateRoundTrip) {
    const char* inputs[] = {
        "2000-01-01",  // epoch
        "2013-07-15",
        "1999-12-31",
        "2020-02-29",  // leap day
        "1900-01-01",
        "2100-01-01",
    };
    for (const char* input : inputs) {
        Datum d = date_in(input);
        char* out = date_out(d);
        EXPECT_STREQ(out, input) << "Round-trip failed for: " << input;
    }
}

TEST_F(DatetimeTest, DateInInvalid) {
    EXPECT_TRUE(RaisesError([] { date_in("not a date"); }));
    EXPECT_TRUE(RaisesError([] { date_in("2013-13-01"); }));
    EXPECT_TRUE(RaisesError([] { date_in("2013-02-30"); }));
}

// ===========================================================================
// comparisons
// ===========================================================================

TEST_F(DatetimeTest, TimestampCmp) {
    Datum a = timestamp_in("2013-07-15 10:00:00");
    Datum b = timestamp_in("2013-07-15 10:00:01");
    EXPECT_LT(timestamp_cmp(a, b), 0);
    EXPECT_GT(timestamp_cmp(b, a), 0);
    EXPECT_EQ(timestamp_cmp(a, a), 0);
}

TEST_F(DatetimeTest, DateCmp) {
    Datum a = date_in("2013-07-01");
    Datum b = date_in("2013-07-31");
    EXPECT_LT(date_cmp(a, b), 0);
    EXPECT_GT(date_cmp(b, a), 0);
    EXPECT_EQ(date_cmp(a, a), 0);
}

// ===========================================================================
// date_trunc
// ===========================================================================

TEST_F(DatetimeTest, DateTruncMinute) {
    Datum ts = timestamp_in("2013-07-15 10:30:45.123456");
    Datum truncated = date_trunc("minute", ts);
    char* out = timestamp_out(truncated);
    EXPECT_STREQ(out, "2013-07-15 10:30:00");
}

TEST_F(DatetimeTest, DateTruncHour) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum truncated = date_trunc("hour", ts);
    char* out = timestamp_out(truncated);
    EXPECT_STREQ(out, "2013-07-15 10:00:00");
}

TEST_F(DatetimeTest, DateTruncDay) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum truncated = date_trunc("day", ts);
    char* out = timestamp_out(truncated);
    EXPECT_STREQ(out, "2013-07-15 00:00:00");
}

TEST_F(DatetimeTest, DateTruncMonth) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum truncated = date_trunc("month", ts);
    char* out = timestamp_out(truncated);
    EXPECT_STREQ(out, "2013-07-01 00:00:00");
}

TEST_F(DatetimeTest, DateTruncYear) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum truncated = date_trunc("year", ts);
    char* out = timestamp_out(truncated);
    EXPECT_STREQ(out, "2013-01-01 00:00:00");
}

TEST_F(DatetimeTest, DateTruncQuarter) {
    Datum ts = timestamp_in("2013-08-15 10:30:45");
    Datum truncated = date_trunc("quarter", ts);
    char* out = timestamp_out(truncated);
    EXPECT_STREQ(out, "2013-07-01 00:00:00");
}

TEST_F(DatetimeTest, DateTruncSecond) {
    Datum ts = timestamp_in("2013-07-15 10:30:45.123456");
    Datum truncated = date_trunc("second", ts);
    char* out = timestamp_out(truncated);
    EXPECT_STREQ(out, "2013-07-15 10:30:45");
}

TEST_F(DatetimeTest, DateTruncCaseInsensitive) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum truncated = date_trunc("MINUTE", ts);
    char* out = timestamp_out(truncated);
    EXPECT_STREQ(out, "2013-07-15 10:30:00");
}

TEST_F(DatetimeTest, DateTruncInvalidField) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    EXPECT_TRUE(RaisesError([&] { date_trunc("invalid", ts); }));
}

// ===========================================================================
// extract
// ===========================================================================

TEST_F(DatetimeTest, ExtractMinute) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum result = extract("minute", ts);
    EXPECT_DOUBLE_EQ(mytoydb::types::DatumGetFloat8(result), 30.0);
}

TEST_F(DatetimeTest, ExtractHour) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum result = extract("hour", ts);
    EXPECT_DOUBLE_EQ(mytoydb::types::DatumGetFloat8(result), 10.0);
}

TEST_F(DatetimeTest, ExtractDay) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum result = extract("day", ts);
    EXPECT_DOUBLE_EQ(mytoydb::types::DatumGetFloat8(result), 15.0);
}

TEST_F(DatetimeTest, ExtractMonth) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum result = extract("month", ts);
    EXPECT_DOUBLE_EQ(mytoydb::types::DatumGetFloat8(result), 7.0);
}

TEST_F(DatetimeTest, ExtractYear) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum result = extract("year", ts);
    EXPECT_DOUBLE_EQ(mytoydb::types::DatumGetFloat8(result), 2013.0);
}

TEST_F(DatetimeTest, ExtractSecond) {
    Datum ts = timestamp_in("2013-07-15 10:30:45.500000");
    Datum result = extract("second", ts);
    EXPECT_DOUBLE_EQ(mytoydb::types::DatumGetFloat8(result), 45.5);
}

TEST_F(DatetimeTest, ExtractQuarter) {
    Datum ts = timestamp_in("2013-08-15 10:30:45");
    Datum result = extract("quarter", ts);
    EXPECT_DOUBLE_EQ(mytoydb::types::DatumGetFloat8(result), 3.0);
}

TEST_F(DatetimeTest, ExtractDow) {
    // 2013-07-15 is a Monday (dow=1)
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum result = extract("dow", ts);
    EXPECT_DOUBLE_EQ(mytoydb::types::DatumGetFloat8(result), 1.0);
}

TEST_F(DatetimeTest, ExtractDoy) {
    // 2013-07-15 is day 196 of the year
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum result = extract("doy", ts);
    EXPECT_DOUBLE_EQ(mytoydb::types::DatumGetFloat8(result), 196.0);
}

TEST_F(DatetimeTest, ExtractInvalidField) {
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    EXPECT_TRUE(RaisesError([&] { extract("invalid", ts); }));
}

// ===========================================================================
// TimestampParts / PartsToTimestamp
// ===========================================================================

TEST_F(DatetimeTest, TimestampPartsRoundTrip) {
    TimestampParts p{};
    p.year = 2013;
    p.month = 7;
    p.day = 15;
    p.hour = 10;
    p.minute = 30;
    p.second = 45;
    p.microsec = 123456;

    Timestamp ts = PartsToTimestamp(p);
    TimestampParts p2 = TimestampToParts(ts);

    EXPECT_EQ(p.year, p2.year);
    EXPECT_EQ(p.month, p2.month);
    EXPECT_EQ(p.day, p2.day);
    EXPECT_EQ(p.hour, p2.hour);
    EXPECT_EQ(p.minute, p2.minute);
    EXPECT_EQ(p.second, p2.second);
    EXPECT_EQ(p.microsec, p2.microsec);
}

TEST_F(DatetimeTest, TimestampNegativeBeforeEpoch) {
    Datum d = timestamp_in("1999-12-31 23:59:59");
    Timestamp ts = DatumGetInt64(d);
    EXPECT_LT(ts, 0);  // Before 2000-01-01
    TimestampParts p = TimestampToParts(ts);
    EXPECT_EQ(p.year, 1999);
    EXPECT_EQ(p.month, 12);
    EXPECT_EQ(p.day, 31);
    EXPECT_EQ(p.hour, 23);
    EXPECT_EQ(p.minute, 59);
    EXPECT_EQ(p.second, 59);
}

// ===========================================================================
// ClickBench-specific scenarios
// ===========================================================================

TEST_F(DatetimeTest, ClickBenchDateComparison) {
    // Query 37: EventDate >= '2013-07-01' AND EventDate <= '2013-07-31'
    Datum start = date_in("2013-07-01");
    Datum end = date_in("2013-07-31");
    Datum test = date_in("2013-07-15");
    EXPECT_GE(date_cmp(test, start), 0);
    EXPECT_LE(date_cmp(test, end), 0);
}

TEST_F(DatetimeTest, ClickBenchDateTruncMinute) {
    // Query 43: DATE_TRUNC('minute', EventTime)
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum truncated = date_trunc("minute", ts);
    char* out = timestamp_out(truncated);
    EXPECT_STREQ(out, "2013-07-15 10:30:00");
}

TEST_F(DatetimeTest, ClickBenchExtractMinute) {
    // Query 19: extract(minute FROM EventTime)
    Datum ts = timestamp_in("2013-07-15 10:30:45");
    Datum result = extract("minute", ts);
    EXPECT_DOUBLE_EQ(mytoydb::types::DatumGetFloat8(result), 30.0);
}

}  // namespace
