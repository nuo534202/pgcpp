// datetime.cpp — timestamp and date type implementation.
//
// Converts PostgreSQL's utils/adt/timestamp.c and utils/adt/date.c to C++20.
// Timestamps are int64 microseconds since 2000-01-01 UTC (PostgreSQL convention).
// Dates are int32 days since 2000-01-01.

#include "mytoydb/types/datetime.hpp"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"

namespace mytoydb::types {

using mytoydb::error::LogLevel;
using mytoydb::memory::palloc;

namespace {

// Allocate a palloc'd C string copy (null-terminated).
char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

// Case-insensitive ASCII string comparison.
bool IStringEq(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Julian date conversions (standard algorithm).
// Julian day 2451545 = 2000-01-01 (PostgreSQL epoch).

int32_t DateToJulian(int year, int month, int day) {
    int a = (14 - month) / 12;
    int y = year + 4800 - a;
    int m = month + 12 * a - 3;
    return day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
}

void JulianToDate(int32_t jdn, int* year, int* month, int* day) {
    int a = jdn + 32044;
    int b = (4 * a + 3) / 146097;
    int c = a - (146097 * b) / 4;
    int d = (4 * c + 3) / 1461;
    int e = c - (1461 * d) / 4;
    int m = (5 * e + 2) / 153;
    *day = e - (153 * m + 2) / 5 + 1;
    *month = m + 3 - 12 * (m / 10);
    *year = 100 * b + d - 4800 + m / 10;
}

// Days from 2000-01-01 to the given year/month/day.
int32_t DateToDays(int year, int month, int day) {
    return DateToJulian(year, month, day) - static_cast<int32_t>(kTimestampEpochJdate);
}

// Days since 2000-01-01 to year/month/day.
void DaysToDateParts(int32_t days, int* year, int* month, int* day) {
    JulianToDate(days + static_cast<int32_t>(kTimestampEpochJdate), year, month, day);
}

// Is the given year a leap year?
bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

// Days in each month (non-leap year).
constexpr int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

int DaysInMonth(int year, int month) {
    if (month == 2 && IsLeapYear(year))
        return 29;
    return kDaysInMonth[month - 1];
}

}  // namespace

// --- TimestampParts conversion ---

TimestampParts TimestampToParts(Timestamp ts) {
    TimestampParts parts{};
    // Split into days and microseconds-within-day.
    int64_t days = ts / (kSecsPerDay * kMicrosecsPerSec);
    int64_t micros_in_day = ts % (kSecsPerDay * kMicrosecsPerSec);
    if (micros_in_day < 0) {
        // Negative timestamp: borrow from days.
        days--;
        micros_in_day += kSecsPerDay * kMicrosecsPerSec;
    }

    int year, month, day;
    DaysToDateParts(static_cast<int32_t>(days), &year, &month, &day);
    parts.year = year;
    parts.month = month;
    parts.day = day;

    int64_t secs = micros_in_day / kMicrosecsPerSec;
    parts.microsec = static_cast<int>(micros_in_day % kMicrosecsPerSec);
    parts.hour = static_cast<int>(secs / 3600);
    secs %= 3600;
    parts.minute = static_cast<int>(secs / 60);
    parts.second = static_cast<int>(secs % 60);
    return parts;
}

Timestamp PartsToTimestamp(const TimestampParts& p) {
    int32_t days = DateToDays(p.year, p.month, p.day);
    int64_t secs = static_cast<int64_t>(p.hour) * 3600 + p.minute * 60 + p.second;
    return days * kSecsPerDay * kMicrosecsPerSec + secs * kMicrosecsPerSec + p.microsec;
}

// --- timestamp input/output ---

Datum timestamp_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type timestamp: NULL");
    }
    // Parse "YYYY-MM-DD HH:MM:SS[.ffffff]" or "YYYY-MM-DDTHH:MM:SS[.ffffff]".
    int year = 0, month = 0, day = 0;
    int hour = 0, minute = 0, second = 0;
    int microsec = 0;
    char sep = '\0';

    int n = std::sscanf(str, "%d-%d-%d%c%d:%d:%d.%d", &year, &month, &day, &sep, &hour, &minute,
                        &second, &microsec);
    if (n < 3) {
        // Try date-only format.
        n = std::sscanf(str, "%d-%d-%d", &year, &month, &day);
        if (n != 3) {
            ereport(LogLevel::kError,
                    "invalid input syntax for type timestamp: \"" + std::string(str) + "\"");
        }
    }
    if (month < 1 || month > 12) {
        ereport(LogLevel::kError, "timestamp field value out of range: month");
    }
    if (day < 1 || day > DaysInMonth(year, month)) {
        ereport(LogLevel::kError, "timestamp field value out of range: day");
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        ereport(LogLevel::kError, "timestamp field value out of range: time");
    }

    TimestampParts parts{};
    parts.year = year;
    parts.month = month;
    parts.day = day;
    parts.hour = hour;
    parts.minute = minute;
    parts.second = second;
    parts.microsec = microsec;
    Timestamp ts = PartsToTimestamp(parts);
    return Int64GetDatum(ts);
}

char* timestamp_out(Datum value) {
    Timestamp ts = DatumGetInt64(value);
    TimestampParts p = TimestampToParts(ts);
    char buf[64];
    if (p.microsec > 0) {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d", p.year, p.month,
                      p.day, p.hour, p.minute, p.second, p.microsec);
    } else {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", p.year, p.month, p.day,
                      p.hour, p.minute, p.second);
    }
    return PallocCString(buf);
}

// --- date input/output ---

Datum date_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type date: NULL");
    }
    int year = 0, month = 0, day = 0;
    int n = std::sscanf(str, "%d-%d-%d", &year, &month, &day);
    if (n != 3) {
        ereport(LogLevel::kError,
                "invalid input syntax for type date: \"" + std::string(str) + "\"");
    }
    if (month < 1 || month > 12) {
        ereport(LogLevel::kError, "date field value out of range: month");
    }
    if (day < 1 || day > DaysInMonth(year, month)) {
        ereport(LogLevel::kError, "date field value out of range: day");
    }
    int32_t days = DateToDays(year, month, day);
    return Int32GetDatum(days);
}

char* date_out(Datum value) {
    int32_t days = DatumGetInt32(value);
    int year, month, day;
    DaysToDateParts(days, &year, &month, &day);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
    return PallocCString(buf);
}

// --- comparisons ---

int timestamp_cmp(Datum a, Datum b) {
    Timestamp x = DatumGetInt64(a);
    Timestamp y = DatumGetInt64(b);
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

int date_cmp(Datum a, Datum b) {
    int32_t x = DatumGetInt32(a);
    int32_t y = DatumGetInt32(b);
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

// --- date_trunc ---

Datum date_trunc(const char* field, Datum timestamp) {
    if (field == nullptr) {
        ereport(LogLevel::kError, "date_trunc: field is null");
    }
    Timestamp ts = DatumGetInt64(timestamp);
    TimestampParts p = TimestampToParts(ts);

    std::string_view f(field);
    if (IStringEq(f, "microseconds")) {
        // Already at microsecond precision.
    } else if (IStringEq(f, "milliseconds")) {
        p.microsec = (p.microsec / 1000) * 1000;
    } else if (IStringEq(f, "second")) {
        p.microsec = 0;
    } else if (IStringEq(f, "minute")) {
        p.microsec = 0;
        p.second = 0;
    } else if (IStringEq(f, "hour")) {
        p.microsec = 0;
        p.second = 0;
        p.minute = 0;
    } else if (IStringEq(f, "day")) {
        p.microsec = 0;
        p.second = 0;
        p.minute = 0;
        p.hour = 0;
    } else if (IStringEq(f, "week")) {
        // Truncate to Monday (day of week 1).
        p.microsec = 0;
        p.second = 0;
        p.minute = 0;
        p.hour = 0;
        // Compute day of week: 0=Sunday, 1=Monday, ...
        int32_t days = DateToDays(p.year, p.month, p.day);
        // 2000-01-01 was a Saturday (day of week 6).
        int dow = (days + 6) % 7;  // 0=Sunday
        if (dow != 1) {
            // Go back to Monday.
            int delta = (dow == 0) ? -6 : (1 - dow);
            days += delta;
            DaysToDateParts(days, &p.year, &p.month, &p.day);
        }
    } else if (IStringEq(f, "month")) {
        p.microsec = 0;
        p.second = 0;
        p.minute = 0;
        p.hour = 0;
        p.day = 1;
    } else if (IStringEq(f, "quarter")) {
        p.microsec = 0;
        p.second = 0;
        p.minute = 0;
        p.hour = 0;
        p.day = 1;
        p.month = ((p.month - 1) / 3) * 3 + 1;
    } else if (IStringEq(f, "year")) {
        p.microsec = 0;
        p.second = 0;
        p.minute = 0;
        p.hour = 0;
        p.day = 1;
        p.month = 1;
    } else if (IStringEq(f, "decade")) {
        p.microsec = 0;
        p.second = 0;
        p.minute = 0;
        p.hour = 0;
        p.day = 1;
        p.month = 1;
        p.year = (p.year / 10) * 10;
    } else if (IStringEq(f, "century")) {
        p.microsec = 0;
        p.second = 0;
        p.minute = 0;
        p.hour = 0;
        p.day = 1;
        p.month = 1;
        p.year = ((p.year - 1) / 100) * 100 + 1;
    } else if (IStringEq(f, "millennium")) {
        p.microsec = 0;
        p.second = 0;
        p.minute = 0;
        p.hour = 0;
        p.day = 1;
        p.month = 1;
        p.year = ((p.year - 1) / 1000) * 1000 + 1;
    } else {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "date_trunc: unknown field \"%s\"", field);
        ereport(LogLevel::kError, errbuf);
    }

    return Int64GetDatum(PartsToTimestamp(p));
}

// --- extract ---

Datum extract(const char* field, Datum timestamp) {
    if (field == nullptr) {
        ereport(LogLevel::kError, "extract: field is null");
    }
    Timestamp ts = DatumGetInt64(timestamp);
    TimestampParts p = TimestampToParts(ts);
    std::string_view f(field);
    double result = 0.0;

    if (IStringEq(f, "microsecond")) {
        result = static_cast<double>(p.second) * 1000000.0 + p.microsec;
    } else if (IStringEq(f, "millisecond")) {
        result = static_cast<double>(p.second) * 1000.0 + p.microsec / 1000.0;
    } else if (IStringEq(f, "second")) {
        result = p.second + p.microsec / 1000000.0;
    } else if (IStringEq(f, "minute")) {
        result = p.minute;
    } else if (IStringEq(f, "hour")) {
        result = p.hour;
    } else if (IStringEq(f, "day")) {
        result = p.day;
    } else if (IStringEq(f, "dow")) {
        // Day of week: 0=Sunday, 1=Monday, ..., 6=Saturday.
        int32_t days = DateToDays(p.year, p.month, p.day);
        result = (days + 6) % 7;
    } else if (IStringEq(f, "doy")) {
        // Day of year: 1-366.
        int32_t days = DateToDays(p.year, p.month, p.day);
        int32_t year_start = DateToDays(p.year, 1, 1);
        result = days - year_start + 1;
    } else if (IStringEq(f, "week")) {
        // ISO week number (simplified).
        int32_t days = DateToDays(p.year, p.month, p.day);
        int32_t year_start = DateToDays(p.year, 1, 1);
        int doy = days - year_start + 1;
        result = (doy - 1) / 7 + 1;
    } else if (IStringEq(f, "month")) {
        result = p.month;
    } else if (IStringEq(f, "quarter")) {
        result = (p.month - 1) / 3 + 1;
    } else if (IStringEq(f, "year")) {
        result = p.year;
    } else if (IStringEq(f, "decade")) {
        result = p.year / 10;
    } else if (IStringEq(f, "century")) {
        result = (p.year - 1) / 100 + 1;
    } else if (IStringEq(f, "millennium")) {
        result = (p.year - 1) / 1000 + 1;
    } else if (IStringEq(f, "epoch")) {
        // Seconds since Unix epoch (1970-01-01).
        // PostgreSQL epoch (2000-01-01) is 10957 days after Unix epoch.
        result = static_cast<double>(ts) / kMicrosecsPerSec + 10957.0 * kSecsPerDay;
    } else {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "extract: unknown field \"%s\"", field);
        ereport(LogLevel::kError, errbuf);
    }

    return Float8GetDatum(result);
}

// --- helpers ---

int32_t DateToDays(Date d) {
    return d;
}

Date DaysToDate(int32_t days) {
    return days;
}

}  // namespace mytoydb::types
