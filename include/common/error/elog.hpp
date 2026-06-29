#pragma once

#include <csetjmp>
#include <cstddef>
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

// Get the last recorded ErrorData (valid inside a PG_CATCH block).
ErrorData* GetErrorData();

// Set up the error subsystem. Creates the ErrorContext used to hold error
// data across longjmp. Must be called once before any ereport(ERROR).
void InitErrorSubsystem();

// The setjmp/longjmp-based error recovery mechanism.
struct JmpBufEntry {
    std::jmp_buf buf;
    bool rethrow = false;
};

// Push a jump buffer onto the exception stack. Returns a pointer to the entry.
JmpBufEntry* PushExceptionStack();
// Pop a jump buffer from the exception stack.
void PopExceptionStack(JmpBufEntry* entry);

// ereport macro: the primary error reporting mechanism.
// Usage: ereport(LogLevel::kError, "error message");
#define ereport(elevel, ...) \
    pgcpp::error::EreportImpl(elevel, __FILE__, __func__, __LINE__, __VA_ARGS__)

// elog macro: legacy alias for ereport (same behavior).
#define elog(elevel, ...) \
    pgcpp::error::EreportImpl(elevel, __FILE__, __func__, __LINE__, __VA_ARGS__)

// Implementation function (do not call directly; use ereport/elog macros).
// For ERROR/FATAL/PANIC: never returns (longjmps to nearest PG_CATCH).
// For lower levels: records the message and returns normally.
//
// Takes std::string by value (not string_view) so that temporary strings
// constructed at call sites (e.g. "msg: " + name) are moved into the
// parameter. For error levels, the parameter's destructor is called
// explicitly before longjmp (since longjmp bypasses C++ destructors, the
// automatic destructor call is skipped — no double-free).
void EreportImpl(LogLevel elevel, const char* filename, const char* funcname, int lineno,
                 std::string message);

// PG_TRY / PG_CATCH / PG_END_TRY — setjmp-based error recovery.
//
// Usage:
//   PG_TRY() {
//       ... code that may ereport(ERROR) ...
//   } PG_CATCH() {
//       ... handle the error ...
//   } PG_END_TRY();
//
// NOTE: A C++ RAII wrapper (PgErrorScope) is fundamentally incompatible with
// setjmp/longjmp because the constructor returns before any error occurs,
// invalidating the jump buffer's stack frame. Use the PG_TRY/PG_CATCH macros
// instead. This is the same constraint as PostgreSQL's C code.
#define PG_TRY()                                                                          \
    do {                                                                                  \
        pgcpp::error::JmpBufEntry* _pgcpp_jmp_entry = pgcpp::error::PushExceptionStack(); \
        if (setjmp(_pgcpp_jmp_entry->buf) == 0) {
#define PG_CATCH()                                     \
    pgcpp::error::PopExceptionStack(_pgcpp_jmp_entry); \
    }                                                  \
    else {                                             \
        pgcpp::error::PopExceptionStack(_pgcpp_jmp_entry);

#define PG_END_TRY() \
    }                \
    }                \
    while (0)

}  // namespace pgcpp::error
