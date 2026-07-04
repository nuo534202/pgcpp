#pragma once

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

// Forward declare memory::MemoryContext to avoid full include in the header.
namespace pgcpp::memory {
class MemoryContext;
}

namespace pgcpp::error {

// Error severity levels (PostgreSQL enums, kept as enum class for C++ safety).
enum class LogLevel : int {
    kDebug5 = 10,
    kDebug4 = 11,
    kDebug3 = 12,
    kDebug2 = 13,
    kDebug1 = 14,
    kLog = 15,
    kInfo = 17,
    kNotice = 18,
    kWarning = 19,
    kError = 20,
    kFatal = 21,
    kPanic = 22,
};

// ErrorData — the structured error record.
// PostgreSQL C version uses char* fields; C++ version uses std::string.
struct ErrorData {
    LogLevel elevel = LogLevel::kNotice;
    int sqlerrcode = 0;
    std::string message;
    std::string detail;
    std::string hint;
    std::string context;
    const char* filename = nullptr;
    const char* funcname = nullptr;
    int lineno = 0;

    bool IsError() const { return static_cast<int>(elevel) >= static_cast<int>(LogLevel::kError); }
};

// PgException — the exception type thrown by ereport(ERROR/FATAL/PANIC).
// Inherits std::exception so it can be caught by generic catch handlers.
// The error data is available via GetErrorData() inside a PG_CATCH block.
class PgException : public std::exception {
public:
    PgException() = default;
    const char* what() const noexcept override { return "pgcpp ereport error"; }
};

// Get the last recorded ErrorData (valid inside a PG_CATCH block).
ErrorData* GetErrorData();

// Set up the error subsystem. Creates the ErrorContext used to hold error
// data across exception throws. Must be called once before any ereport(ERROR).
void InitErrorSubsystem();

// ereport macro: the primary error reporting mechanism.
// Usage: ereport(LogLevel::kError, "error message");
#define ereport(elevel, ...) \
    pgcpp::error::EreportImpl(elevel, __FILE__, __func__, __LINE__, __VA_ARGS__)

// elog macro: legacy alias for ereport (same behavior).
#define elog(elevel, ...) \
    pgcpp::error::EreportImpl(elevel, __FILE__, __func__, __LINE__, __VA_ARGS__)

// Implementation function (do not call directly; use ereport/elog macros).
// For ERROR/FATAL/PANIC: never returns (throws PgException to nearest PG_CATCH).
// For lower levels: records the message and returns normally.
//
// Takes std::string by value (not string_view) so that temporary strings
// constructed at call sites (e.g. "msg: " + name) are moved into the
// parameter. With C++ exceptions, the parameter's destructor runs normally
// during stack unwinding — no explicit destructor call or manual cleanup needed.
void EreportImpl(LogLevel elevel, const char* filename, const char* funcname, int lineno,
                 std::string message);

// PG_TRY / PG_CATCH / PG_END_TRY — exception-based error recovery.
//
// Usage:
//   PG_TRY() {
//       ... code that may ereport(ERROR) ...
//   } PG_CATCH() {
//       ... handle the error ...
//   } PG_END_TRY();
//
// PG_CATCH is required — a try block without a catch is ill-formed.
// To re-throw from PG_CATCH, use PG_RE_THROW().
//
// NOTE: C++ exceptions now power this mechanism. The macros expand to
// try/catch blocks. Stack unwinding automatically calls destructors for
// std::string/std::vector locals, fixing the longjmp UB/leak issue.
#define PG_TRY() try {
#define PG_CATCH() \
    }              \
    catch (const pgcpp::error::PgException&) {
#define PG_END_TRY() }

// PG_RE_THROW — re-throw the current error (use inside PG_CATCH).
#define PG_RE_THROW() throw pgcpp::error::PgException()

}  // namespace pgcpp::error
