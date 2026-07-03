#include "common/error/elog.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::error {

namespace {

// Thread-local error state. The last_error_data is populated before throwing
// PgException, so it is available inside PG_CATCH via GetErrorData().
thread_local ErrorData last_error_data;

// ErrorContext: a memory context that survives error throws so that
// ErrorData strings allocated within it are not freed prematurely.
// In PostgreSQL this is a dedicated AllocSetContext named "ErrorContext".
memory::MemoryContext* error_context = nullptr;

// Convert LogLevel to a printable string.
const char* LogLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug5:
            return "DEBUG5";
        case LogLevel::kDebug4:
            return "DEBUG4";
        case LogLevel::kDebug3:
            return "DEBUG3";
        case LogLevel::kDebug2:
            return "DEBUG2";
        case LogLevel::kDebug1:
            return "DEBUG1";
        case LogLevel::kLog:
            return "LOG";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kNotice:
            return "NOTICE";
        case LogLevel::kWarning:
            return "WARNING";
        case LogLevel::kError:
            return "ERROR";
        case LogLevel::kFatal:
            return "FATAL";
        case LogLevel::kPanic:
            return "PANIC";
    }
    return "UNKNOWN";
}

}  // namespace

ErrorData* GetErrorData() {
    return &last_error_data;
}

void InitErrorSubsystem() {
    if (error_context != nullptr) {
        return;
    }
    error_context = memory::AllocSetContext::Create("ErrorContext");
}

void EreportImpl(LogLevel elevel, const char* filename, const char* funcname, int lineno,
                 std::string message) {
    // Fill in the error data. Use assign() (not move) to avoid leaving
    // the parameter's buffer in a moved-from state before the throw —
    // the parameter's destructor will run during stack unwinding, but
    // last_error_data must own its own copy regardless.
    last_error_data.elevel = elevel;
    last_error_data.filename = filename;
    last_error_data.funcname = funcname;
    last_error_data.lineno = lineno;
    last_error_data.message.assign(message);

    if (static_cast<int>(elevel) < static_cast<int>(LogLevel::kError)) {
        // Non-error level: print to stderr and return normally.
        // message's destructor will run on return, freeing its buffer.
        std::fprintf(stderr, "[%s] %s (%s:%d:%s)\n", LogLevelToString(elevel),
                     last_error_data.message.c_str(), filename, lineno, funcname);
        return;
    }

    // ERROR/FATAL/PANIC: print, then throw PgException.
    // C++ stack unwinding will call destructors for the `message` parameter
    // and any std::string/std::vector locals in calling frames — no manual
    // cleanup needed (unlike the old longjmp approach).
    std::fprintf(stderr, "[%s] %s (%s:%d:%s)\n", LogLevelToString(elevel),
                 last_error_data.message.c_str(), filename, lineno, funcname);

    throw PgException();
}

}  // namespace pgcpp::error
