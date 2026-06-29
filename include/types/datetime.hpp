#pragma once

#include <cstdint>

#include "types/datum.hpp"

namespace pgcpp::types {

// PostgreSQL timestamp representation: int64 microseconds since 2000-01-01
// 00:00:00 UTC. This matches PostgreSQL's Timestamp type.
using Timestamp = int64_t;

// PostgreSQL date representation: int32 days since 2000-01-01.
using Date = int32_t;

// Epoch constants (PostgreSQL uses 2000-01-01 as the epoch).
constexpr int64_t kTimestampEpochJdate = 2451545;  // Julian day for 2000-01-01
constexpr int64_t kPostgresEpochDate = 0;          // day 0 = 2000-01-01
constexpr int64_t kSecsPerDay = 86400;
constexpr int64_t kMicrosecsPerSec = 1000000;

// --- timestamp input/output ---

// Parse a timestamp string "YYYY-MM-DD HH:MM:SS[.ffffff]" into a Timestamp.
Datum timestamp_in(const char* str);
// Format a Timestamp datum as "YYYY-MM-DD HH:MM:SS[.ffffff]".
char* timestamp_out(Datum value);

// --- date input/output ---

// Parse a date string "YYYY-MM-DD" into a Date (days since 2000-01-01).
Datum date_in(const char* str);
// Format a Date datum as "YYYY-MM-DD".
char* date_out(Datum value);

// --- timestamp comparison ---
int timestamp_cmp(Datum a, Datum b);
int date_cmp(Datum a, Datum b);

// --- timestamp/date comparison operators (return bool Datum) ---
Datum timestamp_eq(Datum a, Datum b);
Datum timestamp_ne(Datum a, Datum b);
Datum timestamp_lt(Datum a, Datum b);
Datum timestamp_le(Datum a, Datum b);
Datum timestamp_gt(Datum a, Datum b);
Datum timestamp_ge(Datum a, Datum b);

Datum date_eq(Datum a, Datum b);
Datum date_ne(Datum a, Datum b);
Datum date_lt(Datum a, Datum b);
Datum date_le(Datum a, Datum b);
Datum date_gt(Datum a, Datum b);
Datum date_ge(Datum a, Datum b);

// --- date_trunc ---
// Truncate a timestamp to the specified precision.
// Supported fields: 'microseconds', 'milliseconds', 'second', 'minute',
// 'hour', 'day', 'week', 'month', 'quarter', 'year', 'decade', 'century',
// 'millennium'. (ClickBench primarily uses 'minute'.)
Datum date_trunc(const char* field, Datum timestamp);

// --- extract ---
// Extract a numeric component from a timestamp.
// Supported fields: 'microsecond', 'millisecond', 'second', 'minute', 'hour',
// 'day', 'dow' (day of week), 'doy' (day of year), 'week', 'month', 'quarter',
// 'year', 'decade', 'century', 'millennium', 'epoch'.
// Returns the extracted value as a float8 Datum.
Datum extract(const char* field, Datum timestamp);

// --- helpers ---
// Convert a Timestamp to broken-down components (year, month, day, etc.).
struct TimestampParts {
    int year;
    int month;     // 1-12
    int day;       // 1-31
    int hour;      // 0-23
    int minute;    // 0-59
    int second;    // 0-59 (integer part)
    int microsec;  // 0-999999
};
TimestampParts TimestampToParts(Timestamp ts);

// Convert broken-down components to a Timestamp.
Timestamp PartsToTimestamp(const TimestampParts& parts);

// Convert a Date to days since 2000-01-01 (identity, for API clarity).
int32_t DateToDays(Date d);
Date DaysToDate(int32_t days);

}  // namespace pgcpp::types
